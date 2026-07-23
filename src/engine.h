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

static const float ENG_TWO_PI = 6.28318530717958647692f;

enum LoopMode { LOOP_ONESHOT = 0, LOOP_FORWARD = 1, LOOP_GRANULAR = 2, LOOP_MODE_COUNT = 3 };

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
};

/* ADSR stages */
enum { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Grain {
    bool   active = false;
    double pos = 0;         /* read position in source frames      */
    double inc = 1;         /* per-sample advance                  */
    int    age = 0;         /* samples elapsed                     */
    int    len = 1;         /* total grain length in samples       */
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
    Grain  grains[48];
    double grainAccum = 0;  /* fractional grains owed              */
    uint32_t rng = 0x1234567u;

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
        pos = 0; inLoop = false; grainAccum = 0;
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

    ~Engine()
    {
        for (Layer& L : layers) {
            delete L.live.exchange(nullptr);
            delete L.pending.exchange(nullptr);
            delete L.retire;
        }
    }

    void setSampleRate(float sr) { sampleRate = sr > 0 ? sr : 44100.0f; }

    void noteOn(int n)  { note = n; gate = true;  for (Layer& L : layers) L.retrigger(); }
    void noteOff(int n) { if (n == note) gate = false; }

    /* Render sampleFrames of all active layers, summed into outL/outR. */
    void process(float** outLR, int frames, const LayerParams lp[NLAYERS])
    {
        float* outL = outLR[0];
        float* outR = outLR[1];
        for (int i = 0; i < NLAYERS; i++)
            renderLayer(layers[i], lp[i], outL, outR, frames);
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
                     int frames)
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
        if (overlap > loopLen * 0.5) overlap = loopLen * 0.5;
        if (overlap < 0) overlap = 0;

        float attStep = 1.0f / fmaxf(1.0f, lp.attackMs  * 0.001f * sampleRate);
        float decStep = 1.0f / fmaxf(1.0f, lp.decayMs   * 0.001f * sampleRate);
        float relStep = 1.0f / fmaxf(1.0f, lp.releaseMs * 0.001f * sampleRate);

        float grainLenF = fmaxf(1.0f, lp.grainMs * 0.001f * s->srcRate);
        double spawnPerSample = (double)lp.density / sampleRate;

        for (int i = 0; i < frames; i++) {
            /* ---- amplitude ADSR ---- */
            advanceEnv(L, lp, attStep, decStep, relStep);
            if (L.stage == ENV_IDLE) break;
            float amp = L.env * lp.volume;

            float sl = 0, sr = 0;

            if (lp.mode == LOOP_GRANULAR) {
                renderGranular(L, lp, s, ls, le, pitchInc, grainLenF,
                               spawnPerSample, sl, sr);
            } else {
                renderSample(L, lp, s, ls, le, overlap, pitchInc, sl, sr);
            }

            outL[i] += sl * amp;
            outR[i] += sr * amp;
        }

        /* report a playhead position for the GUI */
        double ph = (lp.mode == LOOP_GRANULAR)
                    ? (L.inLoop ? (ls + 0.5 * loopLen) : L.pos)
                    : L.pos;
        L.playhead.store((float)(ph / s->frames));
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

    /* OneShot / Forward playback with crossfaded loop. */
    void renderSample(Layer& L, const LayerParams& lp, Sample* s,
                      double ls, double le, double overlap,
                      double inc, float& outL, float& outR)
    {
        if (!L.inLoop) {
            /* head pass: from 0 (playFromStart) or straight into loopStart */
            if (!lp.playFromStart && L.pos < ls) L.pos = ls;
            readFrame(s, L.pos, outL, outR);
            L.pos += inc;
            if (lp.mode == LOOP_ONESHOT) {
                if (L.pos >= s->frames - 1) { L.pos = s->frames - 1; L.stage = ENV_RELEASE; }
                return;
            }
            if (L.pos >= le) { L.pos = ls + overlap; L.inLoop = true; }
            return;
        }

        /* looping between ls..le with a crossfade near le */
        float aL, aR; readFrame(s, L.pos, aL, aR);
        double distToEnd = le - L.pos;
        if (overlap > 0 && distToEnd < overlap) {
            float t = (float)(1.0 - distToEnd / overlap);   /* 0..1 into fade */
            double posB = ls + (overlap - distToEnd);
            float bL, bR; readFrame(s, posB, bL, bR);
            outL = aL * (1.0f - t) + bL * t;
            outR = aR * (1.0f - t) + bR * t;
        } else {
            outL = aL; outR = aR;
        }
        L.pos += inc;
        if (L.pos >= le) L.pos = ls + overlap + (L.pos - le);
    }

    /* Granular cloud over [ls,le]. Head pass (playFromStart) plays normally
     * until it reaches loopStart, then hands over to the grain cloud. */
    void renderGranular(Layer& L, const LayerParams& lp, Sample* s,
                        double ls, double le, double inc, float grainLenF,
                        double spawnPerSample, float& outL, float& outR)
    {
        if (!L.inLoop) {
            double head = lp.playFromStart ? 0.0 : ls;
            if (L.pos < head) L.pos = head;
            if (lp.playFromStart && L.pos < ls) {
                readFrame(s, L.pos, outL, outR);
                L.pos += inc;
                if (L.pos >= ls) { L.pos = ls; L.inLoop = true; }
                return;
            }
            L.pos = ls; L.inLoop = true;
        }

        /* spawn grains at density */
        L.grainAccum += spawnPerSample;
        while (L.grainAccum >= 1.0) {
            L.grainAccum -= 1.0;
            spawnGrain(L, ls, le, inc, grainLenF);
        }

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
    }

    void spawnGrain(Layer& L, double ls, double le,
                    double inc, float grainLenF)
    {
        for (Grain& g : L.grains) {
            if (g.active) continue;
            double span = (le - ls) - grainLenF * inc;
            if (span < 1) span = 1;
            g.pos = ls + L.randf() * span;
            g.inc = inc;
            g.age = 0;
            g.len = (int)grainLenF;
            g.active = true;
            return;
        }
    }

    static inline float grainWindow(const Grain& g)
    {
        float x = (g.len > 1) ? (float)g.age / (float)(g.len - 1) : 0.0f;
        return 0.5f * (1.0f - cosf(ENG_TWO_PI * x));   /* Hann */
    }
};

/* Trigger this layer's envelope + playback from the top of the sample. */
inline void Layer::retrigger()
{
    resetVoice();
    stage = ENV_ATTACK;
    env = 0.0f;
}

#endif /* ENGINE_H */
