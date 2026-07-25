/*
 * slicer.h — the Match-Slicer engine (audio mosaicing to a guide).
 *
 * Headless, so the CI unit test runs the same code the DAW does. Give it a
 * GUIDE file (an amen break, a synth melody...) and a MAIN file to slice. The
 * guide is cut into slices (a tempo grid or its own transients); for each
 * guide slice the engine searches the main file for the section whose spectral
 * fingerprint matches best, then rearranges those main-file chunks into the
 * guide's rhythm. Playback is locked to the host transport.
 *
 * Pipeline:
 *   sliceGuide   cut the guide into slices (grid or transient)
 *   analyze      16 log-band profile + centroid + rms per slice
 *   candidates   fingerprint main-file windows on a hop grid
 *   match        nearest candidate per guide slice (with anti-repeat variation)
 *   render       concatenate matched main chunks on the guide's timeline, with
 *                crossfades, optional gain-follow / pitch-match / stretch-to-fit
 *   read         transport-synced playback of the rendered pattern
 */
#ifndef SLICER_H
#define SLICER_H

#include <math.h>
#include <string.h>
#include <atomic>
#include <string>
#include <vector>
#include "wav.h"
#include "fft.h"

static const float SL_TWO_PI = 6.28318530717958647692f;
static const int   SL_NBANDS = 16;
static const int   SL_FFT = 2048;

enum { SLICE_GRID = 0, SLICE_TRANSIENT = 1 };

struct SliceFeat {
    float band[SL_NBANDS] = {0};   /* L2-normalized log-band energies */
    float centroid = 0;            /* 0..1 (fraction of nyquist)      */
    float rms = 0;
};

/* one loaded audio file: interleaved stereo + a mono downmix for analysis */
struct AudioBuf {
    std::vector<float> data;       /* interleaved stereo */
    std::vector<float> mono;
    int frames = 0;
    int rate = 44100;
    std::string path;

    void setStereo(std::vector<float>&& lr, int fr, int r)
    {
        data = std::move(lr); frames = fr; rate = r;
        mono.resize((size_t)fr);
        for (int i = 0; i < fr; i++)
            mono[i] = 0.5f * (data[(size_t)i * 2] + data[(size_t)i * 2 + 1]);
    }
};

struct GuideSlice { int start = 0; int len = 0; SliceFeat f; };
struct Candidate  { int start = 0; SliceFeat f; };

struct SlicerParams {
    int   sliceMode = SLICE_GRID;
    int   div = 2;          /* 0=1/4 1=1/8 2=1/16 3=1/32 grid steps per beat */
    int   bars = 1;         /* guide length in bars                          */
    float xfadeMs = 8.0f;
    float variation = 0.3f; /* 0..1 anti-repeat pressure                     */
    bool  gainFollow = true;/* match guide-slice loudness                    */
    bool  pitchMatch = false;
    bool  stretchFit = true;/* resample main chunk to fill the guide slot    */
    float spectralW = 0.8f; /* 0..1 weight of timbre vs loudness in matching */
};

/* --------------------------------------------------------------- features */

static inline SliceFeat sl_feat(const float* mono, int start, int len, int rate)
{
    SliceFeat f;
    if (len < 32) return f;
    static float win[SL_FFT];
    static bool init = false;
    if (!init) {
        for (int n = 0; n < SL_FFT; n++) win[n] = 0.5f - 0.5f * cosf(SL_TWO_PI * n / SL_FFT);
        init = true;
    }

    double band[SL_NBANDS] = {0};
    double cenNum = 0, cenDen = 0, e = 0;
    float re[SL_FFT], im[SL_FFT];
    int nfr = 0;
    for (int s = 0; s + SL_FFT <= len || (nfr == 0 && s == 0); s += SL_FFT / 2) {
        for (int n = 0; n < SL_FFT; n++) {
            int idx = start + s + n;
            float v = (n < len - s) ? mono[idx] : 0.0f;
            re[n] = v * win[n]; im[n] = 0;
        }
        fft_radix2(re, im, SL_FFT, 0);
        for (int k = 1; k < SL_FFT / 2; k++) {
            float fq = (float)k * rate / SL_FFT;
            if (fq < 30 || fq > 16000) continue;
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
            float bx = logf(fq / 40.0f) / logf(16000.0f / 40.0f);
            int b = (int)(bx * SL_NBANDS);
            if (b >= 0 && b < SL_NBANDS) band[b] += mag;
            cenNum += (double)fq * mag; cenDen += mag;
        }
        nfr++;
        if (s + SL_FFT > len) break;
    }
    double bl = 0;
    for (int b = 0; b < SL_NBANDS; b++) bl += band[b] * band[b];
    bl = sqrt(bl);
    for (int b = 0; b < SL_NBANDS; b++) f.band[b] = bl > 1e-12 ? (float)(band[b] / bl) : 0.0f;
    f.centroid = cenDen > 1e-9 ? (float)(cenNum / cenDen / (rate * 0.5)) : 0.0f;

    for (int i = 0; i < len; i++) e += (double)mono[start + i] * mono[start + i];
    f.rms = (float)sqrt(e / len);
    return f;
}

static inline float sl_dist(const SliceFeat& a, const SliceFeat& b, float specW)
{
    float bd = 0;
    for (int i = 0; i < SL_NBANDS; i++) { float d = a.band[i] - b.band[i]; bd += d * d; }
    float cd = (a.centroid - b.centroid) * (a.centroid - b.centroid);
    float ld = (a.rms - b.rms) * (a.rms - b.rms) * 4.0f;
    return specW * (bd + cd) + (1.0f - specW) * ld;
}

/* pitch via FFT-free autocorrelation over a short window */
static inline float sl_pitch(const float* mono, int start, int len, int rate)
{
    if (len < 1024) return 0;
    int N = len < 8192 ? len : 8192;
    double m = 0; for (int i = 0; i < N; i++) m += mono[start + i]; m /= N;
    int lo = rate / 1000, hi = rate / 50; if (hi >= N) hi = N - 1;
    double r0 = 0; for (int i = 0; i < N; i++) { double v = mono[start + i] - m; r0 += v * v; }
    if (r0 < 1e-9) return 0;
    float best = 0; int bestLag = 0;
    for (int lag = lo; lag <= hi; lag++) {
        double s = 0;
        for (int i = 0; i + lag < N; i++) s += (mono[start + i] - m) * (mono[start + i + lag] - m);
        float v = (float)(s / r0);
        if (v > best) { best = v; bestLag = lag; }
    }
    return (bestLag > 0 && best > 0.3f) ? (float)rate / bestLag : 0.0f;
}

/* ---------------------------------------------------------------- engine */

class Slicer {
public:
    AudioBuf guide, main;
    std::vector<GuideSlice> slices;
    std::vector<Candidate>  cands;
    std::vector<int>        match;     /* slice i -> candidate index      */
    SlicerParams par;

    /* rendered pattern (stereo), owned by GUI thread, handed to audio thread */
    std::atomic<AudioBuf*> live{nullptr};
    std::atomic<AudioBuf*> pending{nullptr};
    AudioBuf* retire = nullptr;

    float sampleRate = 44100;
    /* transport */
    double hostTempo = 120.0, hostPpq = 0.0;
    bool   hostPpqValid = false;
    double internalPpq = 0.0;
    std::atomic<float> playPos{0.0f};  /* 0..1 for the GUI                */

    ~Slicer()
    {
        delete live.exchange(nullptr);
        delete pending.exchange(nullptr);
        delete retire;
    }

    void setSampleRate(float sr) { sampleRate = sr > 0 ? sr : 44100.0f; }

    bool haveBoth() const { return guide.frames > SL_FFT && main.frames > SL_FFT; }

    /* ---- slice the guide ---- */
    void sliceGuide()
    {
        slices.clear();
        if (guide.frames < 64) return;
        if (par.sliceMode == SLICE_TRANSIENT) { sliceTransient(); if (slices.size() >= 2) return; }
        /* grid: bars * beats * steps-per-beat evenly across the guide */
        static const int stepsPerBeat[4] = { 1, 2, 4, 8 };
        int n = par.bars * 4 * stepsPerBeat[par.div & 3];
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++) {
            GuideSlice g;
            g.start = (int)((int64_t)i * guide.frames / n);
            int end = (int)((int64_t)(i + 1) * guide.frames / n);
            g.len = end - g.start;
            if (g.len > 8) g.f = sl_feat(guide.mono.data(), g.start, g.len, guide.rate);
            slices.push_back(g);
        }
    }

    /* transient slicing via spectral flux onsets */
    void sliceTransient()
    {
        slices.clear();
        int hop = 512, win = 1024;
        std::vector<float> flux;
        std::vector<float> prev(win / 2 + 1, 0.0f);
        float re[1024], im[1024], w[1024];
        for (int n = 0; n < win; n++) w[n] = 0.5f - 0.5f * cosf(SL_TWO_PI * n / win);
        for (int s = 0; s + win <= guide.frames; s += hop) {
            for (int n = 0; n < win; n++) { re[n] = guide.mono[s + n] * w[n]; im[n] = 0; }
            fft_radix2(re, im, win, 0);
            float fl = 0;
            for (int k = 0; k <= win / 2; k++) {
                float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
                float d = mag - prev[k]; if (d > 0) fl += d; prev[k] = mag;
            }
            flux.push_back(fl);
        }
        /* peak-pick flux above a moving threshold */
        std::vector<int> onsets; onsets.push_back(0);
        for (int i = 2; i < (int)flux.size() - 2; i++) {
            float loc = 0; int c = 0;
            for (int j = i - 8; j <= i + 8; j++) if (j >= 0 && j < (int)flux.size()) { loc += flux[j]; c++; }
            loc = c ? loc / c : 0;
            if (flux[i] > loc * 1.6f && flux[i] >= flux[i-1] && flux[i] > flux[i+1]) {
                int pos = i * hop;
                if (pos - onsets.back() > guide.rate / 16) onsets.push_back(pos);
            }
        }
        onsets.push_back(guide.frames);
        for (size_t i = 0; i + 1 < onsets.size(); i++) {
            GuideSlice g; g.start = onsets[i]; g.len = onsets[i + 1] - onsets[i];
            if (g.len > 8) { g.f = sl_feat(guide.mono.data(), g.start, g.len, guide.rate);
                             slices.push_back(g); }
        }
    }

    /* ---- fingerprint the main file on a hop grid ---- */
    void buildCandidates()
    {
        cands.clear();
        if (main.frames < SL_FFT || slices.empty()) return;
        int avg = 0; for (auto& s : slices) avg += s.len; avg /= (int)slices.size();
        if (avg < SL_FFT) avg = SL_FFT;
        int win = avg, hop = avg / 4; if (hop < 256) hop = 256;
        for (int s = 0; s + win <= main.frames; s += hop) {
            Candidate c; c.start = s;
            c.f = sl_feat(main.mono.data(), s, win, main.rate);
            cands.push_back(c);
        }
        if (cands.empty()) {           /* main shorter than a slice: one cand */
            Candidate c; c.start = 0;
            c.f = sl_feat(main.mono.data(), 0, main.frames, main.rate);
            cands.push_back(c);
        }
    }

    /* ---- match each guide slice to the best main candidate ---- */
    void doMatch()
    {
        match.assign(slices.size(), 0);
        if (cands.empty()) return;
        std::vector<int> recent;                     /* anti-repeat memory */
        for (size_t i = 0; i < slices.size(); i++) {
            float best = 1e30f; int bi = 0;
            for (size_t c = 0; c < cands.size(); c++) {
                float d = sl_dist(slices[i].f, cands[c].f, par.spectralW);
                /* penalize recently-used candidates for variety */
                for (size_t r = 0; r < recent.size(); r++)
                    if (recent[r] == (int)c) d += par.variation * 0.5f * (recent.size() - r) / recent.size();
                if (d < best) { best = d; bi = (int)c; }
            }
            match[i] = bi;
            recent.push_back(bi);
            if (recent.size() > 6) recent.erase(recent.begin());
        }
    }

    /* ---- render the mosaic to a stereo pattern buffer ---- */
    AudioBuf* render()
    {
        if (slices.empty() || match.empty() || cands.empty()) return nullptr;
        int outLen = 0; for (auto& s : slices) outLen += s.len;
        if (outLen < 16) return nullptr;
        AudioBuf* out = new AudioBuf();
        out->rate = main.rate;
        out->frames = outLen;
        out->data.assign((size_t)outLen * 2, 0.0f);

        int xf = (int)(par.xfadeMs * 0.001f * main.rate);
        int pos = 0;
        for (size_t i = 0; i < slices.size(); i++) {
            const GuideSlice& g = slices[i];
            int cs = cands[match[i]].start;
            int segLen = g.len;

            /* gather the source segment (stretch-to-fit or straight copy) */
            std::vector<float> segL(segLen), segR(segLen);
            for (int n = 0; n < segLen; n++) {
                double sp;
                if (par.stretchFit) sp = cs + (double)n * candWin() / segLen;
                else                sp = cs + n;
                int i0 = (int)sp;
                if (i0 >= main.frames - 1) i0 = main.frames - 2;
                if (i0 < 0) i0 = 0;
                float ft = (float)(sp - i0);
                const float* d = &main.data[(size_t)i0 * 2];
                segL[n] = d[0] + (d[2] - d[0]) * ft;
                segR[n] = d[1] + (d[3] - d[1]) * ft;
            }

            /* pitch match (melodic guides): repitch main seg toward guide pitch */
            if (par.pitchMatch) {
                float gp = sl_pitch(guide.mono.data(), g.start, g.len, guide.rate);
                float mp = sl_pitch(main.mono.data(), cs, candWin(), main.rate);
                if (gp > 25 && mp > 25) {
                    double ratio = mp / gp;           /* read faster/slower */
                    if (ratio > 0.5 && ratio < 2.0) repitchSeg(segL, segR, ratio, segLen);
                }
            }

            /* gain follow: scale to the guide slice's loudness */
            if (par.gainFollow) {
                double e = 0; for (int n = 0; n < segLen; n++) e += (double)segL[n] * segL[n];
                float srms = (float)sqrt(e / segLen) + 1e-6f;
                float scale = g.f.rms / srms;
                if (scale > 4) scale = 4;
                if (scale < 0.05f) scale = 0.05f;
                for (int n = 0; n < segLen; n++) { segL[n] *= scale; segR[n] *= scale; }
            }

            /* place with equal-power crossfade against what's already there */
            for (int n = 0; n < segLen; n++) {
                int op = pos + n; if (op >= outLen) break;
                float gain = 1.0f;
                if (xf > 0 && n < xf && pos > 0) {
                    float t = (float)n / xf;
                    float gin = sinf(t * 1.5707963f), gout = cosf(t * 1.5707963f);
                    out->data[(size_t)op * 2]     = out->data[(size_t)op * 2]     * gout + segL[n] * gin;
                    out->data[(size_t)op * 2 + 1] = out->data[(size_t)op * 2 + 1] * gout + segR[n] * gin;
                    continue;
                }
                out->data[(size_t)op * 2]     = segL[n] * gain;
                out->data[(size_t)op * 2 + 1] = segR[n] * gain;
            }
            pos += segLen;
        }
        /* normalize to a safe peak */
        float peak = 1e-6f;
        for (float v : out->data) { float a = fabsf(v); if (a > peak) peak = a; }
        if (peak > 1.0f) { float s = 0.98f / peak; for (float& v : out->data) v *= s; }
        out->mono.clear();
        return out;
    }

    /* GUI thread: rebuild everything and publish */
    bool rematch()
    {
        if (!haveBoth()) return false;
        sliceGuide();
        buildCandidates();
        doMatch();
        AudioBuf* r = render();
        if (!r) return false;
        publish(r);
        return true;
    }

    void publish(AudioBuf* b) { delete pending.exchange(b); }
    void adopt()
    {
        AudioBuf* p = pending.exchange(nullptr);
        if (p) { delete retire; retire = live.exchange(p); }
    }

    /* ---- transport-synced playback: read the pattern at the host's grid ---- */
    void process(float** out, int frames, bool playing, float mix, float master)
    {
        adopt();
        AudioBuf* buf = live.load();
        float* oL = out[0]; float* oR = out[1];
        if (!buf || buf->frames < 2) return;

        double tempo = hostTempo > 20 && hostTempo < 999 ? hostTempo : 120.0;
        double beatsInPattern = par.bars * 4.0;
        double ppqPerSample = tempo / 60.0 / sampleRate;

        for (int i = 0; i < frames; i++) {
            double ppq;
            if (hostPpqValid) ppq = hostPpq + i * ppqPerSample;
            else { ppq = internalPpq; internalPpq += ppqPerSample; }
            if (!playing && !hostPpqValid) { /* idle */ }

            double frac = fmod(ppq / beatsInPattern, 1.0);
            if (frac < 0) frac += 1.0;
            double sp = frac * buf->frames;
            int i0 = (int)sp;
            if (i0 >= buf->frames - 1) i0 = buf->frames - 2;
            if (i0 < 0) i0 = 0;
            float ft = (float)(sp - i0);
            const float* d = &buf->data[(size_t)i0 * 2];
            float L = d[0] + (d[2] - d[0]) * ft;
            float R = d[1] + (d[3] - d[1]) * ft;
            oL[i] += L * mix * master;
            oR[i] += R * mix * master;
            if (i == 0) playPos.store((float)frac);
        }
        if (hostPpqValid) hostPpq += frames * ppqPerSample;
    }

private:
    int candWin() const
    {
        if (slices.empty()) return SL_FFT;
        int avg = 0; for (auto& s : slices) avg += s.len; return avg / (int)slices.size();
    }

    void repitchSeg(std::vector<float>& L, std::vector<float>& R, double ratio, int outLen)
    {
        std::vector<float> nl(outLen), nr(outLen);
        for (int n = 0; n < outLen; n++) {
            double sp = n * ratio; int i0 = (int)sp;
            if (i0 >= (int)L.size() - 1) i0 = (int)L.size() - 2;
            if (i0 < 0) i0 = 0;
            float ft = (float)(sp - i0);
            nl[n] = L[i0] + (L[i0 + 1] - L[i0]) * ft;
            nr[n] = R[i0] + (R[i0 + 1] - R[i0]) * ft;
        }
        L.swap(nl); R.swap(nr);
    }
};

/* load a WAV into an AudioBuf (first maxSec seconds) */
static inline bool sl_load(const std::string& path, AudioBuf& out, float maxSec = 30.0f)
{
    WavInfo info = wav_probe(path.c_str());
    if (!info.ok) return false;
    WavData w = wav_load_partial(path.c_str(), (int)(maxSec * info.sampleRate));
    if (!w.ok || w.frames < 64) return false;
    out.setStereo(std::move(w.samples), w.frames, w.sampleRate);
    out.path = path;
    return true;
}

#endif /* SLICER_H */
