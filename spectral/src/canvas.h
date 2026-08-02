/*
 * canvas.h — the Spectral Canvas engine (paint-tool spectral re-arranger).
 *
 * Headless, so the CI unit test runs the same DSP the DAW does. The plugin is
 * an INSERT EFFECT: it captures an N-bar loop of incoming audio, transforms it
 * into an editable spectrogram "canvas" (STFT magnitudes + phases), applies a
 * non-destructive stack of paint operations (move, pitch, smear, cellular
 * automata, gain/erase), and resynthesises the result with Griffin-Lim phase
 * reconstruction. Because the render happens once per loop on a background
 * thread, it can afford quality real-time DSP never could.
 *
 * The STFT is CIRCULAR over the loop length, so the rendered loop seams
 * perfectly and time-moves wrap musically around the bar grid.
 *
 * Pipeline (all in this header, threading lives in plugin.cpp):
 *   analyze   capture buffer -> src spectrogram (mag + phase, per channel)
 *   applyOps  run the edit stack over a working copy of the magnitudes
 *   render    ISTFT (+ Griffin-Lim where an op broke phase) -> loop audio
 *   display   downsampled magnitude image for the canvas GUI
 */
#ifndef SPECTRAL_CANVAS_H
#define SPECTRAL_CANVAS_H

#include <math.h>
#include <string.h>
#include <vector>
#include <string>
#include "fft.h"

static const float SC_TWO_PI = 6.28318530717958647692f;
static const int   SC_FFT    = 1024;
static const int   SC_HOP    = 256;              /* 4x overlap            */
static const int   SC_BINS   = SC_FFT / 2 + 1;   /* 513                   */
static const int   SC_DISP_T = 512;              /* display image width   */
static const int   SC_DISP_F = 256;              /* display image height  */

/* paint operations */
enum {
    OP_GAIN = 0,   /* scale magnitudes in a region                        */
    OP_ERASE,      /* silence a region (gain 0)                           */
    OP_MOVE,       /* copy/cut a region and paste it at a time/freq offset*/
    OP_PITCH,      /* resample the freq axis inside a region (semitones)  */
    OP_SMEAR,      /* temporal freeze-tail or spectral blur               */
    OP_CA,         /* cellular-automata gate pattern                      */
    OP_TYPE_COUNT
};

/* a normalized rectangle on the canvas: time 0..1 (left->right across the
 * loop), freq 0..1 (bottom->top, linear in FFT bins) */
struct Region {
    float t0 = 0, t1 = 1, f0 = 0, f1 = 1;
    void norm() {
        if (t1 < t0) { float t = t0; t0 = t1; t1 = t; }
        if (f1 < f0) { float f = f0; f0 = f1; f1 = f; }
    }
};

struct Op {
    int    type = OP_GAIN;
    Region r;
    float  dt = 0, df = 0;   /* OP_MOVE: normalized time / freq offset     */
    float  pitch = 0;        /* OP_PITCH: semitones (+up / -down)          */
    float  amount = 1.0f;    /* gain factor / smear length / CA duck       */
    int    caRule = 90;      /* OP_CA: Wolfram elementary rule (0..255)    */
    int    smearDir = 0;     /* OP_SMEAR: 0 time-freeze, 1 freq-blur       */
    int    cut = 0;          /* OP_MOVE: 1 = cut (clear source), 0 = copy  */
};

/* per-channel STFT of the captured loop */
struct Spectro {
    int frames = 0;
    std::vector<float> mag[2];   /* frames * SC_BINS, per channel */
    std::vector<float> ph[2];
    int lenSamples = 0;          /* loop length in samples (== frames*HOP) */
    int rate = 44100;
    int channels = 2;

    void alloc(int fr, int ch) {
        frames = fr; channels = ch;
        for (int c = 0; c < 2; c++) {
            mag[c].assign((size_t)fr * SC_BINS, 0.0f);
            ph[c].assign((size_t)fr * SC_BINS, 0.0f);
        }
    }
};

/* a finished render, published to the audio thread */
struct RenderBuf {
    std::vector<float> data;   /* interleaved stereo */
    int frames = 0;
    int rate = 44100;
    /* downsampled magnitude image (dB, 0..1) for the canvas view */
    std::vector<float> disp;   /* SC_DISP_T * SC_DISP_F, row-major [t][f] */
};

/* ------------------------------------------------------------- windowing */

static inline const float* sc_window()
{
    static float w[SC_FFT];
    static bool init = false;
    if (!init) {
        for (int n = 0; n < SC_FFT; n++)
            w[n] = 0.5f - 0.5f * cosf(SC_TWO_PI * n / SC_FFT);
        init = true;
    }
    return w;
}

/* ------------------------------------------------------------- STFT core */

/* forward circular STFT of one channel: mono[len] -> mag/ph[frames*BINS] */
static inline void sc_stft(const float* mono, int len, int frames,
                           float* mag, float* ph)
{
    const float* w = sc_window();
    float re[SC_FFT], im[SC_FFT];
    for (int f = 0; f < frames; f++) {
        int base = f * SC_HOP;
        for (int n = 0; n < SC_FFT; n++) {
            int idx = base + n; if (idx >= len) idx -= len;   /* circular */
            re[n] = mono[idx] * w[n]; im[n] = 0.0f;
        }
        fft_radix2(re, im, SC_FFT, 0);
        float* m = mag + (size_t)f * SC_BINS;
        float* p = ph  + (size_t)f * SC_BINS;
        for (int k = 0; k < SC_BINS; k++) {
            m[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
            p[k] = atan2f(im[k], re[k]);
        }
    }
}

/* circular ISTFT with overlap-add + window normalization -> out[len] */
static inline void sc_istft(const float* mag, const float* ph, int len,
                            int frames, float* out)
{
    const float* w = sc_window();
    std::vector<float> denom((size_t)len, 0.0f);
    memset(out, 0, sizeof(float) * len);
    float re[SC_FFT], im[SC_FFT];
    for (int f = 0; f < frames; f++) {
        const float* m = mag + (size_t)f * SC_BINS;
        const float* p = ph  + (size_t)f * SC_BINS;
        for (int k = 0; k < SC_BINS; k++) {
            re[k] = m[k] * cosf(p[k]);
            im[k] = m[k] * sinf(p[k]);
        }
        /* hermitian mirror for the upper half */
        for (int k = 1; k < SC_FFT - SC_BINS + 1; k++) {
            re[SC_FFT - k] = re[k];
            im[SC_FFT - k] = -im[k];
        }
        fft_radix2(re, im, SC_FFT, 1);
        int base = f * SC_HOP;
        for (int n = 0; n < SC_FFT; n++) {
            int idx = base + n; if (idx >= len) idx -= len;
            out[idx]   += re[n] * w[n];
            denom[idx] += w[n] * w[n];
        }
    }
    for (int i = 0; i < len; i++)
        if (denom[i] > 1e-8f) out[i] /= denom[i];
}

/* ------------------------------------------------------------- the engine */

class Canvas {
public:
    float sampleRate = 44100.0f;
    int   glIters = 12;          /* Griffin-Lim iterations when phase breaks */
    std::vector<Op> ops;

    Spectro src;                 /* the captured source spectrogram */

    void setSampleRate(float sr) { sampleRate = sr > 0 ? sr : 44100.0f; }

    /* loop length in quarter-notes for `bars` bars of the given time sig */
    static double loopQuarters(int bars, int tsNum, int tsDen)
    {
        if (tsNum < 1) tsNum = 4;
        if (tsDen < 1) tsDen = 4;
        double quartersPerBar = tsNum * 4.0 / tsDen;
        return bars * quartersPerBar;
    }
    /* samples in that loop, rounded to a whole number of hops (>= 1 frame) */
    int samplesPerLoop(int bars, double tempo, int tsNum, int tsDen) const
    {
        if (tempo < 20 || tempo > 400) tempo = 120.0;
        double q = loopQuarters(bars, tsNum, tsDen);
        double s = q * (60.0 / tempo) * sampleRate;
        int frames = (int)(s / SC_HOP + 0.5);
        if (frames < 4) frames = 4;
        return frames * SC_HOP;
    }

    /* capture an interleaved-stereo buffer into the source spectrogram.
     * `len` must be a multiple of SC_HOP (use samplesPerLoop). */
    void analyze(const float* interleaved, int len, int channels, int rate)
    {
        int frames = len / SC_HOP;
        if (frames < 1) { src.frames = 0; return; }
        src.alloc(frames, channels < 2 ? 1 : 2);
        src.lenSamples = frames * SC_HOP;
        src.rate = rate;

        std::vector<float> mono((size_t)len);
        for (int c = 0; c < src.channels; c++) {
            for (int i = 0; i < len; i++)
                mono[i] = interleaved[(size_t)i * channels + (channels > 1 ? c : 0)];
            sc_stft(mono.data(), len, frames,
                    src.mag[c].data(), src.ph[c].data());
        }
        if (src.channels == 1) { src.mag[1] = src.mag[0]; src.ph[1] = src.ph[0]; }
    }

    /* does the current op stack require phase reconstruction? */
    bool needsGriffinLim() const
    {
        for (const Op& o : ops)
            if (o.type == OP_MOVE || o.type == OP_PITCH ||
                o.type == OP_SMEAR || o.type == OP_CA) return true;
        return false;
    }

    /* full render of the edited canvas into a RenderBuf */
    void render(RenderBuf& rb)
    {
        int frames = src.frames;
        rb.frames = 0;
        if (frames < 1) return;
        int len = src.lenSamples;
        rb.rate = src.rate;
        rb.data.assign((size_t)len * 2, 0.0f);

        bool gl = needsGriffinLim();
        std::vector<float> emag((size_t)frames * SC_BINS);
        std::vector<float> ephase((size_t)frames * SC_BINS);
        std::vector<float> chan((size_t)len);

        for (int c = 0; c < 2; c++) {
            int sc = (src.channels == 1) ? 0 : c;
            emag = src.mag[sc];
            ephase = src.ph[sc];
            applyOps(emag, ephase, frames);
            if (gl) griffinLim(emag, ephase, len, frames);
            sc_istft(emag.data(), ephase.data(), len, frames, chan.data());
            for (int i = 0; i < len; i++) rb.data[(size_t)i * 2 + c] = chan[i];
        }
        rb.frames = len;
        buildDisplay(rb);
    }

    /* build a downsampled magnitude image from the EDITED canvas (ch0),
     * for the GUI. Uses the same op stack, magnitude-only (no GL/ISTFT). */
    void buildDisplay(RenderBuf& rb)
    {
        int frames = src.frames;
        rb.disp.assign((size_t)SC_DISP_T * SC_DISP_F, 0.0f);
        if (frames < 1) return;
        std::vector<float> emag = src.mag[0];
        std::vector<float> ephase = src.ph[0];
        applyOps(emag, ephase, frames);
        for (int tx = 0; tx < SC_DISP_T; tx++) {
            int f0 = (int)((int64_t)tx * frames / SC_DISP_T);
            int f1 = (int)((int64_t)(tx + 1) * frames / SC_DISP_T);
            if (f1 <= f0) f1 = f0 + 1;
            if (f1 > frames) f1 = frames;
            for (int fy = 0; fy < SC_DISP_F; fy++) {
                int b0 = (int)((int64_t)fy * SC_BINS / SC_DISP_F);
                int b1 = (int)((int64_t)(fy + 1) * SC_BINS / SC_DISP_F);
                if (b1 <= b0) b1 = b0 + 1;
                if (b1 > SC_BINS) b1 = SC_BINS;
                float mx = 0;
                for (int f = f0; f < f1; f++) {
                    const float* m = &emag[(size_t)f * SC_BINS];
                    for (int b = b0; b < b1; b++) if (m[b] > mx) mx = m[b];
                }
                float db = 20.0f * log10f(mx + 1e-6f);      /* ~ -120..+something */
                float v = (db + 90.0f) / 90.0f;             /* map -90dB..0dB -> 0..1 */
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                rb.disp[(size_t)tx * SC_DISP_F + fy] = v;
            }
        }
    }

    /* build a display image straight from the SOURCE (no edits) */
    void buildSourceDisplay(std::vector<float>& disp)
    {
        int frames = src.frames;
        disp.assign((size_t)SC_DISP_T * SC_DISP_F, 0.0f);
        if (frames < 1) return;
        for (int tx = 0; tx < SC_DISP_T; tx++) {
            int f0 = (int)((int64_t)tx * frames / SC_DISP_T);
            int f1 = (int)((int64_t)(tx + 1) * frames / SC_DISP_T);
            if (f1 <= f0) f1 = f0 + 1;
            if (f1 > frames) f1 = frames;
            for (int fy = 0; fy < SC_DISP_F; fy++) {
                int b0 = (int)((int64_t)fy * SC_BINS / SC_DISP_F);
                int b1 = (int)((int64_t)(fy + 1) * SC_BINS / SC_DISP_F);
                if (b1 <= b0) b1 = b0 + 1;
                float mx = 0;
                for (int f = f0; f < f1; f++) {
                    const float* m = &src.mag[0][(size_t)f * SC_BINS];
                    for (int b = b0; b < b1 && b < SC_BINS; b++) if (m[b] > mx) mx = m[b];
                }
                float db = 20.0f * log10f(mx + 1e-6f);
                float v = (db + 90.0f) / 90.0f;
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                disp[(size_t)tx * SC_DISP_F + fy] = v;
            }
        }
    }

private:
    /* clamp a normalized region to concrete frame/bin ranges */
    void regionBounds(const Region& r, int frames,
                      int& fa, int& fb, int& ba, int& bb) const
    {
        fa = (int)(r.t0 * frames); fb = (int)(r.t1 * frames);
        ba = (int)(r.f0 * SC_BINS); bb = (int)(r.f1 * SC_BINS);
        if (fa < 0) fa = 0;
        if (fb > frames) fb = frames;
        if (ba < 0) ba = 0;
        if (bb > SC_BINS) bb = SC_BINS;
    }

    void applyOps(std::vector<float>& mag, std::vector<float>& ph, int frames)
    {
        for (const Op& o : ops) {
            switch (o.type) {
            case OP_GAIN:  opGain(mag, frames, o, o.amount); break;
            case OP_ERASE: opGain(mag, frames, o, 0.0f);     break;
            case OP_MOVE:  opMove(mag, ph, frames, o);       break;
            case OP_PITCH: opPitch(mag, ph, frames, o);      break;
            case OP_SMEAR: opSmear(mag, frames, o);          break;
            case OP_CA:    opCA(mag, frames, o);             break;
            }
        }
    }

    void opGain(std::vector<float>& mag, int frames, const Op& o, float g)
    {
        int fa, fb, ba, bb; regionBounds(o.r, frames, fa, fb, ba, bb);
        for (int f = fa; f < fb; f++) {
            float* m = &mag[(size_t)f * SC_BINS];
            for (int b = ba; b < bb; b++) m[b] *= g;
        }
    }

    void opMove(std::vector<float>& mag, std::vector<float>& ph, int frames, const Op& o)
    {
        int fa, fb, ba, bb; regionBounds(o.r, frames, fa, fb, ba, bb);
        int dtF = (int)(o.dt * frames);
        int dfB = (int)(o.df * SC_BINS);
        std::vector<float> sm = mag;      /* snapshot before writing */
        std::vector<float> sp = ph;
        if (o.cut) {
            for (int f = fa; f < fb; f++) {
                float* m = &mag[(size_t)f * SC_BINS];
                for (int b = ba; b < bb; b++) m[b] = 0.0f;
            }
        }
        for (int f = fa; f < fb; f++) {
            int df = f + dtF; df %= frames; if (df < 0) df += frames;   /* wrap */
            float* dm = &mag[(size_t)df * SC_BINS];
            float* dph = &ph[(size_t)df * SC_BINS];
            const float* sMag = &sm[(size_t)f * SC_BINS];
            const float* sPh  = &sp[(size_t)f * SC_BINS];
            for (int b = ba; b < bb; b++) {
                int db = b + dfB;
                if (db < 0 || db >= SC_BINS) continue;
                dm[db] = sMag[b];
                dph[db] = sPh[b];
            }
        }
    }

    void opPitch(std::vector<float>& mag, std::vector<float>& ph, int frames, const Op& o)
    {
        int fa, fb, ba, bb; regionBounds(o.r, frames, fa, fb, ba, bb);
        double ratio = pow(2.0, o.pitch / 12.0);
        if (ratio < 1e-3) ratio = 1e-3;
        for (int f = fa; f < fb; f++) {
            float* m = &mag[(size_t)f * SC_BINS];
            float* p = &ph[(size_t)f * SC_BINS];
            std::vector<float> tmpM(bb - ba), tmpP(bb - ba);
            for (int b = ba; b < bb; b++) {
                double srcB = ba + (b - ba) / ratio;   /* up-pitch pulls from lower */
                int i0 = (int)srcB;
                float fr = (float)(srcB - i0);
                float v0 = (i0 >= ba && i0 < bb) ? m[i0] : 0.0f;
                float v1 = (i0 + 1 >= ba && i0 + 1 < bb) ? m[i0 + 1] : 0.0f;
                tmpM[b - ba] = v0 + (v1 - v0) * fr;
                int pi = (i0 >= ba && i0 < bb) ? i0 : b;
                tmpP[b - ba] = p[pi];
            }
            for (int b = ba; b < bb; b++) { m[b] = tmpM[b - ba]; p[b] = tmpP[b - ba]; }
        }
    }

    void opSmear(std::vector<float>& mag, int frames, const Op& o)
    {
        int fa, fb, ba, bb; regionBounds(o.r, frames, fa, fb, ba, bb);
        if (o.smearDir == 0) {
            /* temporal freeze-tail: leaky max forward in time */
            float decay = 0.5f + 0.499f * (o.amount > 1 ? 1 : o.amount);
            for (int f = fa + 1; f < fb; f++) {
                float* m = &mag[(size_t)f * SC_BINS];
                float* pm = &mag[(size_t)(f - 1) * SC_BINS];
                for (int b = ba; b < bb; b++) {
                    float tail = pm[b] * decay;
                    if (tail > m[b]) m[b] = tail;
                }
            }
        } else {
            /* spectral blur: box average across bins per frame */
            int rad = 1 + (int)(o.amount * 8.0f);
            for (int f = fa; f < fb; f++) {
                float* m = &mag[(size_t)f * SC_BINS];
                std::vector<float> tmp(bb - ba);
                for (int b = ba; b < bb; b++) {
                    float s = 0; int n = 0;
                    for (int d = -rad; d <= rad; d++) {
                        int bb2 = b + d;
                        if (bb2 >= ba && bb2 < bb) { s += m[bb2]; n++; }
                    }
                    tmp[b - ba] = n ? s / n : m[b];
                }
                for (int b = ba; b < bb; b++) m[b] = tmp[b - ba];
            }
        }
    }

    /* cellular-automata gate: a coarse grid of freq-bands x time-steps, seeded
     * from the magnitude, evolves with a 1D Wolfram rule across the freq axis,
     * one generation per time-step. Dead cells duck their bins. */
    void opCA(std::vector<float>& mag, int frames, const Op& o)
    {
        int fa, fb, ba, bb; regionBounds(o.r, frames, fa, fb, ba, bb);
        if (fb - fa < 2 || bb - ba < 2) return;
        const int NB = 16;                        /* freq bands */
        int steps = 8 + (int)(o.amount * 24.0f);  /* time steps across region */
        if (steps < 2) steps = 2;
        int rule = o.caRule & 0xFF;
        float duck = 0.0f;                         /* dead-cell level */

        /* seed generation 0 from average energy per band */
        std::vector<unsigned char> cell(NB, 0), next(NB, 0);
        {
            int f0 = fa;
            for (int nb = 0; nb < NB; nb++) {
                int kb0 = ba + (int)((int64_t)nb * (bb - ba) / NB);
                int kb1 = ba + (int)((int64_t)(nb + 1) * (bb - ba) / NB);
                float s = 0; int n = 0;
                for (int b = kb0; b < kb1; b++) { s += mag[(size_t)f0 * SC_BINS + b]; n++; }
                float avg = n ? s / n : 0;
                cell[nb] = avg > 1e-3f ? 1 : 0;
            }
            bool any = false; for (int i = 0; i < NB; i++) any |= cell[i];
            if (!any) cell[NB / 2] = 1;            /* ensure a seed */
        }

        for (int st = 0; st < steps; st++) {
            int sf = fa + (int)((int64_t)st * (fb - fa) / steps);
            int ef = fa + (int)((int64_t)(st + 1) * (fb - fa) / steps);
            if (ef <= sf) ef = sf + 1;
            /* apply current generation as a gate over this time slab */
            for (int nb = 0; nb < NB; nb++) {
                int kb0 = ba + (int)((int64_t)nb * (bb - ba) / NB);
                int kb1 = ba + (int)((int64_t)(nb + 1) * (bb - ba) / NB);
                float g = cell[nb] ? 1.0f : duck;
                for (int f = sf; f < ef && f < frames; f++) {
                    float* m = &mag[(size_t)f * SC_BINS];
                    for (int b = kb0; b < kb1; b++) m[b] *= g;
                }
            }
            /* evolve: elementary rule over (left,center,right) with wrap */
            for (int nb = 0; nb < NB; nb++) {
                int l = cell[(nb - 1 + NB) % NB];
                int c = cell[nb];
                int r = cell[(nb + 1) % NB];
                int idx = (l << 2) | (c << 1) | r;
                next[nb] = (rule >> idx) & 1;
            }
            cell.swap(next);
        }
    }

    /* Griffin-Lim: refine phase so the edited magnitudes resynthesise
     * coherently. Seeded with the (mostly original) phase in `ph`. */
    void griffinLim(std::vector<float>& mag, std::vector<float>& ph,
                    int len, int frames)
    {
        std::vector<float> sig((size_t)len);
        std::vector<float> m2((size_t)frames * SC_BINS);
        for (int it = 0; it < glIters; it++) {
            sc_istft(mag.data(), ph.data(), len, frames, sig.data());
            /* re-analyze to pull consistent phases; keep target magnitudes */
            sc_stft(sig.data(), len, frames, m2.data(), ph.data());
        }
    }
};

#endif /* SPECTRAL_CANVAS_H */
