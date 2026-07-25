/*
 * engine.h — the sampler/granular DSP engine. No Win32, no VST: pure state +
 * audio so the headless CI smoke test exercises the same code the DAW runs.
 *
 * Model (milestone 2):
 *   - 3 independent sample layers, stacked and driven monophonically by MIDI.
 *   - Each layer: a loaded WAV, a loop region (start/end/overlap), a loop mode,
 *     an amplitude ADSR, and grain controls for the granular mode.
 *   - Loop modes: OneShot, Forward (crossfaded), Granular (grain cloud over the
 *     loop region with per-grain Hann windows).
 *   - "Play from start" plays the head of the sample once, then rolls into the
 *     loop / grain cloud at loopStart.
 *   - Per layer: volume and fine-tune (cents). Note pitch maps to playback rate.
 *
 * Sample ownership across threads: the editor (GUI thread) builds a Sample and
 * hands it over with publish(); the audio thread picks it up at block start.
 * One retired generation is kept alive until the next load so the audio thread
 * never reads freed memory.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <math.h>
#include <string.h>
#include <atomic>
#include <string>
#include <vector>
#include "wav.h"
#include "fft.h"

static const float ENG_TWO_PI = 6.28318530717958647692f;

enum LoopMode { LOOP_ONESHOT = 0, LOOP_FORWARD = 1, LOOP_GRANULAR = 2, LOOP_MODE_COUNT = 3 };

/* Chaos seed: bytes of a user-supplied .txt file drive the deterministic
 * bit-rearranging glitch. Handed GUI -> audio via the same retire pattern. */
struct SeedData {
    std::vector<uint8_t> bytes;
    std::string path;
    uint32_t hash = 0x811C9DC5u;   /* FNV-1a of the bytes */
};

/* One decoded sample plus a precomputed peak envelope for the waveform view. */
struct Sample {
    std::vector<float> data;      /* interleaved stereo */
    int    frames = 0;
    int    srcRate = 44100;
    std::string path;

    /* display peaks (mono), PEAKS buckets across the whole sample */
    static const int PEAKS = 1024;
    std::vector<float> peakMin, peakMax;

    void computePeaks()
    {
        peakMin.assign(PEAKS, 0.0f);
        peakMax.assign(PEAKS, 0.0f);
        if (frames <= 0) return;
        for (int p = 0; p < PEAKS; p++) {
            int a = (int)((int64_t)p * frames / PEAKS);
            int b = (int)((int64_t)(p + 1) * frames / PEAKS);
            if (b <= a) b = a + 1;
            if (b > frames) b = frames;
            float mn = 1e9f, mx = -1e9f;
            for (int i = a; i < b; i++) {
                float s = 0.5f * (data[(size_t)i * 2] + data[(size_t)i * 2 + 1]);
                if (s < mn) mn = s;
                if (s > mx) mx = s;
            }
            peakMin[p] = mn; peakMax[p] = mx;
        }
    }
};

/* Per-layer parameter snapshot, recomputed from the flat VST params each block. */
struct LayerParams {
    float volume = 0.8f;    /* linear gain 0..1                    */
    float cents  = 0.0f;    /* fine tune, -100..+100               */
    int   mode   = LOOP_FORWARD;
    bool  playFromStart = true;
    float loopStart = 0.0f; /* fraction 0..1 of sample             */
    float loopEnd   = 1.0f; /* fraction 0..1 of sample             */
    float overlapMs = 20.0f;
    float attackMs = 5.0f, decayMs = 200.0f, sustain = 0.8f, releaseMs = 300.0f;
    float grainMs  = 60.0f; /* granular grain length               */
    float density  = 20.0f; /* grains per second                   */

    /* ---- experimental granular controls (milestone 3) ---- */
    float sprayMs   = 0.0f; /* random grain start scatter, ms       */
    float pitchJit  = 0.0f; /* per-grain random detune, semitones   */
    float panSpread = 0.0f; /* per-grain stereo scatter, 0..1       */
    float revProb   = 0.0f; /* probability a grain plays reversed   */
    float scan      = 0.0f; /* grain-source drift through region -1..1 */
    float shape     = 0.5f; /* grain window skew, 0..1 (0.5 = Hann) */
    /* ---- chaos manglers, applied to the whole layer output ---- */
    float bits      = 16.0f;/* bit depth 1..16 (16 = clean)         */
    float decimate  = 1.0f; /* sample-and-hold factor 1..50         */
    float chaos     = 0.0f; /* seed-driven bit rearrange 0..1       */
    float timeJit   = 0.0f; /* grain spawn-time jitter 0..1         */

    /* ---- wav operators (milestone 4) ---- */
    float strch     = 1.0f; /* time-stretch 0.25..4 (1 = off), pitch-preserving */
    float tonal     = 0.0f; /* spectral tonal/atonal balance -1..+1  */
    float tilt      = 0.0f; /* spectral tilt dark/bright -1..+1      */
    int   shiftBins = 0;    /* spectral shift, -64..+64 bins         */
    float freeze    = 0.0f; /* spectral magnitude freeze 0..1        */
    int   specMode  = 0;    /* chaotic spectral artifact mode 0..7   */
    float specAmt   = 0.5f; /* intensity of specMode                 */
};

/* spectral artifact modes (specMode) */
enum {
    SPEC_OFF = 0, SPEC_SCRAMBLE, SPEC_ROBOT, SPEC_WHISPER,
    SPEC_HOLES, SPEC_MIRROR, SPEC_CRUSH, SPEC_SMEAR, SPEC_MODE_COUNT
};

/* glitch sequencer algorithms (per step) */
enum {
    GL_OFF = 0, GL_STUTTER, GL_STUT16, GL_REVERSE, GL_TAPESTOP,
    GL_HALF, GL_GATE, GL_SCRAMBLE, GL_ALGO_COUNT
};

/* per-block glitch sequencer settings, built from the flat VST params */
struct GlitchParams {
    int   pattern[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    int   divIdx = 1;       /* 0=1/32 1=1/16 2=1/8 3=1/4 */
    float mix = 1.0f;
};

/* ADSR stages */
enum { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Grain {
    bool   active = false;
    double pos = 0;         /* read position in source frames      */
    double inc = 1;         /* per-sample advance (may be negative)*/
    int    age = 0;         /* samples elapsed                     */
    int    len = 1;         /* total grain length in samples       */
    float  panL = 1.0f;     /* equal-power pan gains               */
    float  panR = 1.0f;
    float  shape = 0.5f;    /* window skew captured at spawn       */
};

struct Layer {
    /* ---- sample handoff (GUI -> audio) ---- */
    std::atomic<Sample*> live{nullptr};   /* what the audio thread reads   */
    std::atomic<Sample*> pending{nullptr};/* GUI parks a new one here      */
    Sample* retire = nullptr;             /* prev generation, freed on swap*/

    /* ---- runtime voice state ---- */
    double pos = 0;         /* main playhead in source frames      */
    bool   inLoop = false;  /* has the head pass reached the loop  */
    int    stage = ENV_IDLE;
    float  env = 0.0f;
    Grain  grains[64];
    double grainAccum = 0;  /* fractional grains owed              */
    double scanPhase = 0;   /* granular scan position 0..1         */
    uint32_t rng = 0x1234567u;

    /* mangler state (decimate sample-and-hold + chaos index) */
    float  holdL = 0, holdR = 0;
    int    holdCnt = 0;
    uint32_t chaosIdx = 0;

    /* stretch-stream state (grain-based time stretch in Forward/OneShot) */
    double vpos = 0;        /* slow virtual playhead                */
    int    stretchCount = 0;/* output samples since last grain      */

    /* streaming STFT state, one per channel (spectral wav operators) */
    static const int SPEC_N = 1024, SPEC_HOP = 256, SPEC_BINS = 513;
    struct SpecCh {
        float inRing[SPEC_N];
        float ola[SPEC_N + SPEC_HOP];
        float prevMag[SPEC_BINS];
        float smearMag[SPEC_BINS];
        int   inPos = 0, olaRead = 0, hopCount = 0;
    };
    SpecCh spec[2];
    uint32_t specFrame = 0;
    bool specWasActive = false;

    void resetSpectral()
    {
        for (int c = 0; c < 2; c++) {
            memset(spec[c].inRing, 0, sizeof(spec[c].inRing));
            memset(spec[c].ola, 0, sizeof(spec[c].ola));
            memset(spec[c].prevMag, 0, sizeof(spec[c].prevMag));
            memset(spec[c].smearMag, 0, sizeof(spec[c].smearMag));
            spec[c].inPos = spec[c].olaRead = spec[c].hopCount = 0;
        }
        specFrame = 0;
    }

    std::atomic<float> playhead{0.0f};    /* 0..1 for the GUI              */

    float randf()
    {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) * (1.0f / 16777216.0f);
    }

    /* GUI thread: publish a freshly loaded sample. */
    void publish(Sample* s)
    {
        Sample* old = pending.exchange(s);
        delete old;                       /* a pending one never consumed  */
    }

    /* audio thread: adopt any pending sample at block start. Reset playback
     * position for the new sample but keep the amplitude envelope, so dropping
     * a sample in the same block a note arrives doesn't swallow the note. */
    void adopt()
    {
        Sample* p = pending.exchange(nullptr);
        if (p) {
            delete retire;                /* free the generation before last */
            retire = live.exchange(p);
            resetPlayback();
        }
    }

    /* playback position only (envelope untouched) */
    void resetPlayback()
    {
        pos = 0; inLoop = false; grainAccum = 0; scanPhase = 0; holdCnt = 0;
        vpos = 0; stretchCount = 0;
        for (Grain& g : grains) g.active = false;
    }

    /* full reset incl. envelope */
    void resetVoice()
    {
        resetPlayback();
        stage = ENV_IDLE; env = 0.0f;
    }

    void retrigger();   /* defined below Engine */
};

class Engine {
public:
    static const int NLAYERS = 3;
    Layer layers[NLAYERS];
    float sampleRate = 44100.0f;

    int  note = -1;        /* current MIDI note, -1 = none */
    bool gate = false;
    int  rootNote = 60;    /* C4 plays the sample at native pitch/rate */

    /* global chaos seed (shared by all layers) */
    std::atomic<SeedData*> seedLive{nullptr};
    std::atomic<SeedData*> seedPending{nullptr};
    SeedData* seedRetire = nullptr;

    /* host transport, fed by the plugin each block before process() */
    double hostTempo = 130.0;
    double hostPpq = 0.0;
    bool   hostPpqValid = false;

    /* ---- glitch sequencer state ---- */
    static const int GL_RING = 1 << 17;         /* ~3 s capture ring    */
    static const int GL_SNAP = 1 << 16;         /* max one-step snapshot */
    std::vector<float> glRingL, glRingR, glSnapL, glSnapR;
    int    glWrite = 0;
    long   glTotalWritten = 0;
    long   glCurStep = -1000000;                /* absolute step number  */
    int    glSnapLen = 0;
    int    glAlgo = GL_OFF;
    long   glSampleInStep = 0;
    double glTapePos = 0;
    float  glGateEnv = 1.0f;
    double glInternalPpq = 0;                   /* fallback clock        */
    std::atomic<int> glitchStep{-1};            /* for the GUI highlight */

    Engine()
    {
        glRingL.assign(GL_RING, 0.0f); glRingR.assign(GL_RING, 0.0f);
        glSnapL.assign(GL_SNAP, 0.0f); glSnapR.assign(GL_SNAP, 0.0f);
    }

    ~Engine()
    {
        for (Layer& L : layers) {
            delete L.live.exchange(nullptr);
            delete L.pending.exchange(nullptr);
            delete L.retire;
        }
        delete seedLive.exchange(nullptr);
        delete seedPending.exchange(nullptr);
        delete seedRetire;
    }

    /* GUI thread: publish a new chaos seed */
    void publishSeed(SeedData* s) { delete seedPending.exchange(s); }
    /* audio thread: adopt at block start */
    void adoptSeed()
    {
        SeedData* s = seedPending.exchange(nullptr);
        if (s) { delete seedRetire; seedRetire = seedLive.exchange(s); }
    }

    void setSampleRate(float sr) { sampleRate = sr > 0 ? sr : 44100.0f; }

    void noteOn(int n)  { note = n; gate = true;  for (Layer& L : layers) L.retrigger(); }
    void noteOff(int n) { if (n == note) gate = false; }

    /* Render sampleFrames of all active layers, summed into outL/outR. */
    void process(float** outLR, int frames, const LayerParams lp[NLAYERS])
    {
        adoptSeed();
        SeedData* seed = seedLive.load();
        float* outL = outLR[0];
        float* outR = outLR[1];
        for (int i = 0; i < NLAYERS; i++)
            renderLayer(layers[i], lp[i], outL, outR, frames, seed);
    }

    /* ---- tempo-synced glitch sequencer, applied to the summed bus ----
     *
     * A 16-step pattern advances in sync with the host transport (ppqPos when
     * the DAW provides it, an internal clock at the host tempo otherwise).
     * At each step boundary the previous step's audio is snapshotted from a
     * capture ring; the step's algorithm then rearranges that snapshot in
     * real time (stutters, reverse, tapestop, half-speed, chaos-seeded slice
     * scramble) or trance-gates the live signal. */
    void processGlitch(float** outLR, int frames, const GlitchParams& gp)
    {
        float* outL = outLR[0];
        float* outR = outLR[1];
        SeedData* seed = seedLive.load();

        double tempo = hostTempo > 20.0 && hostTempo < 999.0 ? hostTempo : 130.0;
        static const double kStepPPQ[4] = { 0.125, 0.25, 0.5, 1.0 };
        double stepPPQ = kStepPPQ[gp.divIdx & 3];
        double stepSamples = stepPPQ * (60.0 / tempo) * sampleRate;
        if (stepSamples < 16) stepSamples = 16;
        double ppqPerSample = tempo / 60.0 / sampleRate;

        for (int i = 0; i < frames; i++) {
            double ppq;
            if (hostPpqValid) ppq = hostPpq + (double)i * ppqPerSample;
            else { ppq = glInternalPpq; glInternalPpq += ppqPerSample; }
            if (ppq < 0) ppq = 0;

            long stepAbs = (long)(ppq / stepPPQ);
            if (stepAbs != glCurStep) {
                glCurStep = stepAbs;
                int idx16 = (int)(stepAbs % 16);
                glitchStep.store(idx16);
                glAlgo = gp.pattern[idx16];
                glSampleInStep = 0;
                glTapePos = 0;
                /* snapshot the previous step from the capture ring */
                int n = (int)stepSamples;
                if (n > GL_SNAP) n = GL_SNAP;
                if ((long)n > glTotalWritten) n = (int)glTotalWritten;
                glSnapLen = n;
                for (int j = 0; j < n; j++) {
                    int src = (glWrite - n + j) & (GL_RING - 1);
                    glSnapL[j] = glRingL[src];
                    glSnapR[j] = glRingR[src];
                }
            }

            float dl = outL[i], dr = outR[i];

            /* always capture dry into the ring */
            glRingL[glWrite] = dl; glRingR[glWrite] = dr;
            glWrite = (glWrite + 1) & (GL_RING - 1);
            glTotalWritten++;

            float wl = dl, wr = dr;
            long s = glSampleInStep++;
            int n = glSnapLen;

            if (glAlgo != GL_OFF && (n > 15 || glAlgo == GL_GATE)) {
                switch (glAlgo) {
                case GL_STUTTER: {
                    int seg = n / 2 > 0 ? n / 2 : 1;
                    int r = (int)(s % seg);
                    wl = glSnapL[r]; wr = glSnapR[r];
                    break;
                }
                case GL_STUT16: {
                    int seg = n / 4 > 0 ? n / 4 : 1;
                    int r = (int)(s % seg);
                    wl = glSnapL[r]; wr = glSnapR[r];
                    break;
                }
                case GL_REVERSE: {
                    int r = n - 1 - (int)(s % n);
                    wl = glSnapL[r]; wr = glSnapR[r];
                    break;
                }
                case GL_TAPESTOP: {
                    double rate = 1.0 - (double)s / stepSamples;
                    if (rate < 0) rate = 0;
                    glTapePos += rate;
                    int r = (int)glTapePos; if (r >= n) r = n - 1;
                    float fade = (float)(rate * 0.3 + 0.7);
                    wl = glSnapL[r] * fade; wr = glSnapR[r] * fade;
                    break;
                }
                case GL_HALF: {
                    int r = (int)((s / 2) % n);
                    wl = glSnapL[r]; wr = glSnapR[r];
                    break;
                }
                case GL_GATE: {
                    double sub = fmod((double)s * 4.0 / stepSamples, 1.0);
                    float target = sub < 0.55 ? 1.0f : 0.0f;
                    glGateEnv += (target - glGateEnv) * 0.03f;
                    wl = dl * glGateEnv; wr = dr * glGateEnv;
                    break;
                }
                case GL_SCRAMBLE: {
                    int sliceLen = n / 8 > 0 ? n / 8 : 1;
                    int sN = (int)(s % n);
                    int sl_ = sN / sliceLen; if (sl_ > 7) sl_ = 7;
                    uint32_t v = seedValue(seed, (uint32_t)(glCurStep * 16 + sl_));
                    int src = (int)(v % 8u);
                    int r = src * sliceLen + (sN % sliceLen);
                    if (r >= n) r = n - 1;
                    wl = glSnapL[r]; wr = glSnapR[r];
                    break;
                }
                }
                outL[i] = dl + (wl - dl) * gp.mix;
                outR[i] = dr + (wr - dr) * gp.mix;
            } else {
                glGateEnv = 1.0f;
            }
        }

        if (hostPpqValid)
            hostPpq += (double)frames * ppqPerSample;  /* until next block */
    }

private:
    /* Interpolated stereo read from a sample at fractional frame p. */
    static inline void readFrame(const Sample* s, double p, float& L, float& R)
    {
        int i0 = (int)p;
        if (i0 < 0) { L = R = 0; return; }
        if (i0 >= s->frames - 1) {
            int c = s->frames - 1;
            L = s->data[(size_t)c * 2]; R = s->data[(size_t)c * 2 + 1];
            return;
        }
        float t = (float)(p - i0);
        const float* a = &s->data[(size_t)i0 * 2];
        L = a[0] + (a[2] - a[0]) * t;
        R = a[1] + (a[3] - a[1]) * t;
    }

    void renderLayer(Layer& L, const LayerParams& lp, float* outL, float* outR,
                     int frames, SeedData* seed)
    {
        L.adopt();
        Sample* s = L.live.load();
        if (!s || s->frames < 2) { L.playhead.store(0.0f); return; }

        double srcRatio = (double)s->srcRate / (double)sampleRate;
        double semis = (note >= 0 ? note - rootNote : 0) + lp.cents * 0.01;
        double pitchInc = pow(2.0, semis / 12.0) * srcRatio;

        double ls = (double)lp.loopStart * s->frames;
        double le = (double)lp.loopEnd   * s->frames;
        if (le < ls + 4) le = ls + 4;
        if (le > s->frames) le = s->frames;
        double loopLen = le - ls;
        double overlap = (double)lp.overlapMs * 0.001 * s->srcRate;
        if (overlap > loopLen * 0.5) overlap = loopLen * 0.5;   /* keep a clean core */
        if (overlap < 0) overlap = 0;

        float attStep = 1.0f / fmaxf(1.0f, lp.attackMs  * 0.001f * sampleRate);
        float decStep = 1.0f / fmaxf(1.0f, lp.decayMs   * 0.001f * sampleRate);
        float relStep = 1.0f / fmaxf(1.0f, lp.releaseMs * 0.001f * sampleRate);

        float grainLenF = fmaxf(1.0f, lp.grainMs * 0.001f * s->srcRate);
        double spawnPerSample = (double)lp.density / sampleRate;
        double scanStep = (double)lp.scan * 2.0 / sampleRate;   /* up to 2 sweeps/s */

        /* spectral wav operators: engage only when non-neutral; reset the
         * STFT state on the rising edge so stale buffers don't burp */
        bool specActive = lp.specMode != SPEC_OFF ||
                          fabsf(lp.tonal) > 0.01f || fabsf(lp.tilt) > 0.01f ||
                          lp.shiftBins != 0 || lp.freeze > 0.01f;
        if (specActive && !L.specWasActive) L.resetSpectral();
        L.specWasActive = specActive;

        for (int i = 0; i < frames; i++) {
            /* ---- amplitude ADSR ---- */
            advanceEnv(L, lp, attStep, decStep, relStep);
            bool voiceOn = L.stage != ENV_IDLE;
            if (!voiceOn && !specActive) break;
            float amp = voiceOn ? L.env * lp.volume : 0.0f;

            float sl = 0, sr = 0;

            if (voiceOn) {
                if (lp.mode == LOOP_GRANULAR) {
                    renderGranular(L, lp, s, ls, le, pitchInc, grainLenF,
                                   spawnPerSample, scanStep, sl, sr);
                } else {
                    renderSample(L, lp, s, ls, le, overlap, pitchInc, sl, sr);
                }
                mangle(L, lp, seed, sl, sr);  /* decimate -> bitcrush -> chaos */
                sl *= amp; sr *= amp;
            }

            /* spectral runs even after the voice ends so its tail flushes */
            if (specActive) processSpectral(L, lp, seed, sl, sr);

            outL[i] += sl;
            outR[i] += sr;
        }

        /* report a playhead position for the GUI */
        double ph = (lp.mode == LOOP_GRANULAR)
                    ? (ls + L.scanPhase * loopLen)
                    : L.pos;
        L.playhead.store((float)(ph / s->frames));
    }

    /* ---- output manglers: sample-rate reduction, bit crush, chaos ---- */
    void mangle(Layer& L, const LayerParams& lp, SeedData* seed,
                float& sl, float& sr)
    {
        if (lp.decimate > 1.0f) {
            if (L.holdCnt <= 0) { L.holdL = sl; L.holdR = sr; L.holdCnt = (int)lp.decimate; }
            sl = L.holdL; sr = L.holdR; L.holdCnt--;
        }
        if (lp.bits < 15.99f) {
            float levels = powf(2.0f, lp.bits);
            float step = 2.0f / levels;
            sl = floorf(sl / step + 0.5f) * step;
            sr = floorf(sr / step + 0.5f) * step;
        }
        if (lp.chaos > 0.001f) {
            sl = bitMangle(sl, seedValue(seed, L.chaosIdx++), lp.chaos);
            sr = bitMangle(sr, seedValue(seed, L.chaosIdx++), lp.chaos);
        }
    }

    /* Pull a pseudo-random 32-bit value from the seed stream at position idx.
     * With a loaded .txt the file's bytes steer the sequence; without one a
     * built-in constant keeps chaos usable. Deterministic either way. */
    static inline uint32_t seedValue(SeedData* seed, uint32_t idx)
    {
        uint32_t base = 0x9E3779B9u;
        if (seed && !seed->bytes.empty())
            base = seed->hash ^ (uint32_t)seed->bytes[idx % seed->bytes.size()];
        uint32_t v = base ^ (idx * 2246822519u);
        v ^= v << 13; v ^= v >> 17; v ^= v << 5;
        return v;
    }

    /* ---- spectral wav operators (streaming STFT, N=1024 hop=256) ----
     * Chain per frame: tonal/atonal balance -> tilt -> bin shift -> freeze
     * -> chaotic artifact mode. Runs per channel; ~17 ms latency. */
    void processSpectral(Layer& L, const LayerParams& lp, SeedData* seed,
                         float& sl, float& sr)
    {
        float in[2] = { sl, sr };
        float out[2];
        for (int c = 0; c < 2; c++) {
            Layer::SpecCh& S = L.spec[c];
            S.inRing[S.inPos] = in[c];
            S.inPos = (S.inPos + 1) & (Layer::SPEC_N - 1);
            out[c] = S.ola[S.olaRead];
            S.ola[S.olaRead] = 0.0f;
            S.olaRead = (S.olaRead + 1) % (Layer::SPEC_N + Layer::SPEC_HOP);
            if (++S.hopCount >= Layer::SPEC_HOP) {
                S.hopCount = 0;
                specFrame(L, S, lp, seed);
            }
        }
        L.specFrame++;   /* advances 2x per sample; only relative value matters */
        sl = out[0]; sr = out[1];
    }

    static const float* hannTable()
    {
        static float w[Layer::SPEC_N];
        static bool init = false;
        if (!init) {
            for (int n = 0; n < Layer::SPEC_N; n++)
                w[n] = 0.5f - 0.5f * cosf(ENG_TWO_PI * n / Layer::SPEC_N);
            init = true;
        }
        return w;
    }

    void specFrame(Layer& L, Layer::SpecCh& S, const LayerParams& lp,
                   SeedData* seed)
    {
        const int N = Layer::SPEC_N, HALF = N / 2;
        const float* w = hannTable();
        float re[Layer::SPEC_N], im[Layer::SPEC_N];
        for (int n = 0; n < N; n++) {
            re[n] = S.inRing[(S.inPos + n) & (N - 1)] * w[n];
            im[n] = 0.0f;
        }
        fft_radix2(re, im, N, 0);

        float mag[Layer::SPEC_BINS], ph[Layer::SPEC_BINS];
        for (int k = 0; k <= HALF; k++) {
            mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
            ph[k]  = atan2f(im[k], re[k]);
        }

        /* tonal/atonal balance: peaks (above 2x mean) vs residual */
        if (fabsf(lp.tonal) > 0.01f) {
            float mean = 0;
            for (int k = 1; k <= HALF; k++) mean += mag[k];
            mean /= HALF;
            float thr = 2.0f * mean;
            for (int k = 1; k <= HALF; k++) {
                bool peak = mag[k] > thr;
                if (lp.tonal > 0 && !peak) mag[k] *= 1.0f - lp.tonal;
                if (lp.tonal < 0 &&  peak) mag[k] *= 1.0f + lp.tonal;
            }
        }

        /* spectral tilt: dark (<0) .. bright (>0), +/-24 dB across */
        if (fabsf(lp.tilt) > 0.01f) {
            for (int k = 1; k <= HALF; k++) {
                float x = (float)k / HALF - 0.5f;
                mag[k] *= powf(10.0f, lp.tilt * x * 2.4f);
            }
        }

        /* bin shift (inharmonic frequency shift) */
        if (lp.shiftBins != 0) {
            float m2[Layer::SPEC_BINS], p2[Layer::SPEC_BINS];
            for (int k = 0; k <= HALF; k++) { m2[k] = 0; p2[k] = 0; }
            for (int k = 0; k <= HALF; k++) {
                int d = k + lp.shiftBins;
                if (d >= 0 && d <= HALF) { m2[d] = mag[k]; p2[d] = ph[k]; }
            }
            memcpy(mag, m2, sizeof(m2)); memcpy(ph, p2, sizeof(p2));
        }

        /* magnitude freeze (phases keep running -> shimmer) */
        if (lp.freeze > 0.01f) {
            for (int k = 0; k <= HALF; k++) {
                mag[k] = mag[k] * (1.0f - lp.freeze) + S.prevMag[k] * lp.freeze;
            }
        }
        memcpy(S.prevMag, mag, sizeof(float) * Layer::SPEC_BINS);

        /* chaotic artifact modes, seed-steered where random */
        float a = lp.specAmt;
        uint32_t fr = L.specFrame;
        switch (lp.specMode) {
        case SPEC_SCRAMBLE:
            for (int k = 1; k < HALF; k++) {
                uint32_t v = seedValue(seed, fr * 7919u + k);
                if ((v & 1023u) < (uint32_t)(a * 300.0f)) {
                    int j = 1 + (int)((v >> 10) % (uint32_t)(HALF - 1));
                    float t = mag[k]; mag[k] = mag[j]; mag[j] = t;
                }
            }
            break;
        case SPEC_ROBOT:
            for (int k = 0; k <= HALF; k++) ph[k] *= 1.0f - a;
            break;
        case SPEC_WHISPER:
            for (int k = 1; k <= HALF; k++) {
                float rp = (seedValue(seed, fr * 4099u + k) & 0xFFFF)
                           * (ENG_TWO_PI / 65536.0f);
                ph[k] += (rp - ph[k]) * a;
            }
            break;
        case SPEC_HOLES:
            for (int k = 1; k <= HALF; k++) {
                uint32_t v = seedValue(seed, (fr / 4u) * 613u + k);
                if ((v & 1023u) < (uint32_t)(a * 512.0f)) mag[k] = 0.0f;
            }
            break;
        case SPEC_MIRROR:
            for (int k = 1; k < HALF / 2; k++) {
                int j = HALF - k;
                float mk = mag[k], mj = mag[j];
                mag[k] += (mj - mk) * a;
                mag[j] += (mk - mj) * a;
            }
            break;
        case SPEC_CRUSH: {
            float inv = 2.0f + (1.0f - a) * 62.0f;
            float peak = 1e-9f;
            for (int k = 1; k <= HALF; k++) if (mag[k] > peak) peak = mag[k];
            for (int k = 1; k <= HALF; k++)
                mag[k] = floorf(mag[k] / peak * inv + 0.5f) / inv * peak;
            break;
        }
        case SPEC_SMEAR: {
            float decay = 0.55f + 0.43f * a;
            for (int k = 0; k <= HALF; k++) {
                float sm = S.smearMag[k] * decay;
                if (mag[k] > sm) sm = mag[k];
                S.smearMag[k] = sm;
                mag[k] = sm;
            }
            break;
        }
        default: break;
        }

        /* back to complex, enforce conjugate symmetry, inverse, overlap-add */
        for (int k = 0; k <= HALF; k++) {
            re[k] = mag[k] * cosf(ph[k]);
            im[k] = mag[k] * sinf(ph[k]);
        }
        im[0] = 0; im[HALF] = 0;
        for (int k = 1; k < HALF; k++) { re[N - k] = re[k]; im[N - k] = -im[k]; }
        fft_radix2(re, im, N, 1);

        const float norm = 1.0f / 1.5f;   /* Hann^2 OLA at hop N/4 sums to 1.5 */
        int base = S.olaRead;
        for (int n = 0; n < N; n++) {
            int j = (base + n) % (N + Layer::SPEC_HOP);
            S.ola[j] += re[n] * w[n] * norm;
        }
    }

    /* Rotate + partially XOR the 16-bit word of a sample: "chaotic bit
     * re-arranging." amt crossfades clean -> mangled so it stays playable. */
    static inline float bitMangle(float x, uint32_t sv, float amt)
    {
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
        uint16_t u = (uint16_t)(int16_t)lrintf(x * 32767.0f);
        int rot = sv & 15;
        uint16_t r = (uint16_t)((u << rot) | (u >> ((16 - rot) & 15)));
        uint16_t mask = (uint16_t)((sv >> 8) & (uint16_t)(0xFFFF * amt));
        r ^= mask;
        float mangled = (int16_t)r / 32767.0f;
        return x * (1.0f - amt) + mangled * amt;
    }

    void advanceEnv(Layer& L, const LayerParams& lp,
                    float attStep, float decStep, float relStep)
    {
        switch (L.stage) {
        case ENV_ATTACK:
            L.env += attStep;
            if (L.env >= 1.0f) { L.env = 1.0f; L.stage = ENV_DECAY; }
            break;
        case ENV_DECAY:
            L.env -= decStep * (1.0f - lp.sustain);
            if (L.env <= lp.sustain) { L.env = lp.sustain; L.stage = ENV_SUSTAIN; }
            break;
        case ENV_SUSTAIN:
            L.env = lp.sustain;
            break;
        case ENV_RELEASE:
            L.env -= relStep;
            if (L.env <= 0.0f) { L.env = 0.0f; L.stage = ENV_IDLE; }
            break;
        }
        if (!gate && L.stage != ENV_IDLE && L.stage != ENV_RELEASE)
            L.stage = ENV_RELEASE;
    }

    /* OneShot / Forward playback with a seamless equal-power crossfade loop.
     *
     * A single read pointer runs ls..le. Within `overlap` frames of le it
     * crossfades (cos/sin, constant power) with a second read starting at ls,
     * then wraps to ls+overlap — the exact frame the fade left off. The head
     * pass (playFromStart) flows through the same fade into the loop, so there
     * is no hard jump at the seam and no click when the overlap is large. */
    void renderSample(Layer& L, const LayerParams& lp, Sample* s,
                      double ls, double le, double overlap,
                      double inc, float& outL, float& outR)
    {
        if (lp.strch < 0.97f || lp.strch > 1.03f) {
            renderStretched(L, lp, s, ls, le, inc, outL, outR);
            return;
        }

        if (!L.inLoop) {                       /* first sample: seed position */
            L.pos = lp.playFromStart ? 0.0 : ls;
            L.inLoop = true;
        }

        float aL, aR; readFrame(s, L.pos, aL, aR);

        if (lp.mode == LOOP_ONESHOT) {
            outL = aL; outR = aR;
            L.pos += inc;
            if (L.pos >= s->frames - 1) { L.pos = s->frames - 1; L.stage = ENV_RELEASE; }
            return;
        }

        double dEnd = le - L.pos;
        if (overlap > 0 && dEnd < overlap && L.pos >= ls) {
            float t = (float)(1.0 - dEnd / overlap);       /* 0..1 into fade */
            double posB = ls + (overlap - dEnd);
            float bL, bR; readFrame(s, posB, bL, bR);
            float ga = cosf(t * 1.57079633f), gb = sinf(t * 1.57079633f);
            outL = aL * ga + bL * gb;
            outR = aR * ga + bR * gb;
        } else {
            outL = aL; outR = aR;
        }
        L.pos += inc;
        if (L.pos >= le) L.pos = ls + overlap + (L.pos - le);
    }

    /* Pitch-preserving time stretch for OneShot/Forward: a slow virtual
     * playhead (vpos) advances at inc/strch while fixed-rate Hann grains
     * (50% overlap, so they sum to constant power) read from it at the
     * normal pitch. Twice the Strch = twice the duration, same pitch. */
    void renderStretched(Layer& L, const LayerParams& lp, Sample* s,
                         double ls, double le, double inc,
                         float& outL, float& outR)
    {
        static const int GLEN = 2048, GHOP = 1024;   /* output samples */

        if (!L.inLoop) {
            L.vpos = lp.playFromStart ? 0.0 : ls;
            L.inLoop = true;
            L.stretchCount = GHOP;                   /* spawn immediately */
        }

        if (L.stretchCount >= GHOP) {
            L.stretchCount = 0;
            for (Grain& g : L.grains) {
                if (g.active) continue;
                g.pos = L.vpos; g.inc = inc; g.age = 0; g.len = GLEN;
                g.panL = g.panR = 1.0f; g.shape = 0.5f; g.active = true;
                break;
            }
        }
        L.stretchCount++;

        float mixL = 0, mixR = 0;
        for (Grain& g : L.grains) {
            if (!g.active) continue;
            float w = grainWindow(g);
            float gl, gr; readFrame(s, g.pos, gl, gr);
            mixL += gl * w; mixR += gr * w;
            g.pos += g.inc;
            if (++g.age >= g.len || g.pos >= s->frames - 1) g.active = false;
        }
        outL = mixL; outR = mixR;

        L.vpos += inc / lp.strch;
        if (lp.mode == LOOP_ONESHOT) {
            if (L.vpos >= s->frames - 1) { L.vpos = s->frames - 1; L.stage = ENV_RELEASE; }
        } else if (L.vpos >= le) {
            L.vpos = ls + (L.vpos - le);             /* grains blend the seam */
        }
        L.pos = L.vpos;                              /* playhead for the GUI */
    }

    /* Granular cloud over [ls,le] with the experimental controls: a scanning
     * source position, per-grain spray / detune / reverse / pan, a skewable
     * window, and spawn-time jitter. Head pass (playFromStart) plays normally
     * until loopStart, then hands over to the cloud. */
    void renderGranular(Layer& L, const LayerParams& lp, Sample* s,
                        double ls, double le, double inc, float grainLenF,
                        double spawnPerSample, double scanStep,
                        float& outL, float& outR)
    {
        if (!L.inLoop) {
            if (lp.playFromStart && L.pos < ls) {
                readFrame(s, L.pos, outL, outR);
                L.pos += inc;
                if (L.pos >= ls) { L.pos = ls; L.inLoop = true; }
                return;
            }
            L.inLoop = true; L.scanPhase = 0;
        }

        L.scanPhase += scanStep;
        if (L.scanPhase >= 1.0) L.scanPhase -= 1.0;
        else if (L.scanPhase < 0.0) L.scanPhase += 1.0;

        L.grainAccum += spawnPerSample;
        while (L.grainAccum >= 1.0) {
            double jit = 1.0 + (L.randf() * 2.0f - 1.0f) * lp.timeJit * 0.9;
            L.grainAccum -= jit > 0.1 ? jit : 0.1;
            spawnGrain(L, lp, s, ls, le, inc, grainLenF);
        }

        float mixL = 0, mixR = 0;
        for (Grain& g : L.grains) {
            if (!g.active) continue;
            float w = grainWindow(g);
            float gl, gr; readFrame(s, g.pos, gl, gr);
            mixL += gl * w * g.panL; mixR += gr * w * g.panR;
            g.pos += g.inc;
            if (++g.age >= g.len || g.pos < 0.0 || g.pos >= s->frames - 1)
                g.active = false;
        }
        outL = mixL; outR = mixR;
    }

    void spawnGrain(Layer& L, const LayerParams& lp, Sample* s,
                    double ls, double le, double inc, float grainLenF)
    {
        for (Grain& g : L.grains) {
            if (g.active) continue;
            double region = le - ls; if (region < 1) region = 1;
            double center = ls + L.scanPhase * region;
            double spray  = (L.randf() * 2.0f - 1.0f) * lp.sprayMs * 0.001 * s->srcRate;
            double gpos = center + spray;
            while (gpos < ls)  gpos += region;
            while (gpos >= le) gpos -= region;

            double detune = (L.randf() * 2.0f - 1.0f) * lp.pitchJit;
            double ginc = inc * pow(2.0, detune / 12.0);
            int len = (int)grainLenF; if (len < 1) len = 1;
            if (L.randf() < lp.revProb) {                 /* reverse grain */
                ginc = -ginc;
                gpos += (double)len * fabs(ginc);
                if (gpos >= le) gpos = le - 1;
            }
            float pan = 0.5f + (L.randf() - 0.5f) * lp.panSpread;
            if (pan < 0) pan = 0;
            if (pan > 1) pan = 1;

            g.pos = gpos; g.inc = ginc; g.age = 0; g.len = len; g.active = true;
            g.panL = fminf(1.0f, 2.0f * (1.0f - pan));    /* center = unity */
            g.panR = fminf(1.0f, 2.0f * pan);
            g.shape = lp.shape;
            return;
        }
    }

    /* Skewable grain window: `shape` moves the peak (0.5 = symmetric Hann,
     * <0.5 fast-attack/long-tail, >0.5 slow-attack). */
    static inline float grainWindow(const Grain& g)
    {
        float x = (g.len > 1) ? (float)g.age / (float)(g.len - 1) : 0.0f;
        float pk = g.shape; if (pk < 0.03f) pk = 0.03f; if (pk > 0.97f) pk = 0.97f;
        if (x < pk) return 0.5f * (1.0f - cosf(3.14159265f * x / pk));
        return 0.5f * (1.0f - cosf(3.14159265f * (1.0f - (x - pk) / (1.0f - pk))));
    }
};

/* Trigger this layer's envelope + playback from the top of the sample. The
 * chaos index resets so a given seed + note reproduces the same glitch; the
 * grain RNG keeps running so the granular texture stays organic across hits. */
inline void Layer::retrigger()
{
    resetVoice();
    stage = ENV_ATTACK;
    env = 0.0f;
    chaosIdx = 0;
}

#endif /* ENGINE_H */
