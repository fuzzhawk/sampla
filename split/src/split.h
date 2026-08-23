/*
 * split.h — the Spectral Split engine (tonal / atonal separator).
 *
 * Headless, so the CI unit test runs the same DSP the DAW does. A real-time
 * INSERT EFFECT: a streaming STFT decomposes the incoming audio, every frame,
 * into a TONAL component (sustained, narrow spectral peaks) and an ATONAL
 * component (broadband / transient energy), using median-based harmonic-
 * percussive separation (Fitzgerald 2010). One knob crossfades between them.
 *
 * High resolution for fidelity: a 4096-point FFT at 4x overlap (Hann analysis
 * + synthesis, constant-overlap-add). Latency is one hop shy of the window and
 * reported to the host for delay compensation; the dry path is delay-matched
 * so the Mix control stays phase-coherent.
 *
 *   tonal   estimate: a short CAUSAL median across time per bin (sustained
 *           energy survives, one-frame transients don't) -> no added latency
 *   atonal  estimate: a median across FREQUENCY within the frame (broadband
 *           energy survives, narrow tonal peaks are suppressed)
 *   masks   complementary Wiener masks Mh + Mp = 1 (so Split at centre is a
 *           bit-exact bypass); Split scales how much of each is kept
 */
#ifndef SPECTRAL_SPLIT_H
#define SPECTRAL_SPLIT_H

#include <math.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include "fft.h"

static const float SP_TWO_PI = 6.28318530717958647692f;
static const int   SP_FFT   = 4096;               /* high-res window        */
static const int   SP_HOP   = 1024;               /* 4x overlap             */
static const int   SP_BINS  = SP_FFT / 2 + 1;     /* 2049                   */
static const int   SP_LAT   = SP_FFT - SP_HOP;    /* processing latency     */
static const int   SP_TMED  = 7;                  /* time-median frames     */
static const int   SP_FMED  = 8;                  /* freq-median radius      */

static inline const float* sp_window()
{
    static float w[SP_FFT];
    static bool init = false;
    if (!init) {
        for (int n = 0; n < SP_FFT; n++) w[n] = 0.5f - 0.5f * cosf(SP_TWO_PI * n / SP_FFT);
        init = true;
    }
    return w;
}

/* one audio channel's streaming STFT state */
struct SplitChan {
    std::vector<float> inFIFO, outFIFO, outAccum;
    std::vector<float> re, im, mag;
    std::vector<float> hist;      /* SP_TMED * SP_BINS ring of past magnitudes */
    int   histPos = 0, histFill = 0;
    int   rover = SP_LAT;
    std::vector<float> dry;       /* delay line to align the dry path */
    int   dryPos = 0;

    void reset()
    {
        inFIFO.assign(SP_FFT, 0.0f);
        outFIFO.assign(SP_FFT, 0.0f);
        outAccum.assign(SP_FFT, 0.0f);
        re.assign(SP_FFT, 0.0f); im.assign(SP_FFT, 0.0f);
        mag.assign(SP_BINS, 0.0f);
        hist.assign((size_t)SP_TMED * SP_BINS, 0.0f);
        histPos = histFill = 0;
        rover = SP_LAT;
        dry.assign(SP_LAT, 0.0f);
        dryPos = 0;
    }

    /* process n samples. wT/wA are the tonal/atonal weights; mix is dry..wet;
     * outGain a post gain. */
    void process(const float* in, float* out, int n, float wT, float wA,
                 float mix, float outGain)
    {
        const float* w = sp_window();
        for (int i = 0; i < n; i++) {
            float x = in ? in[i] : 0.0f;
            inFIFO[rover] = x;
            float wet = outFIFO[rover - SP_LAT];

            /* delay-matched dry sample */
            float dsamp = dry[dryPos];
            dry[dryPos] = x;
            dryPos = (dryPos + 1) % SP_LAT;

            out[i] = (dsamp * (1.0f - mix) + wet * mix) * outGain;

            if (++rover >= SP_FFT) {
                rover = SP_LAT;
                frame(w, wT, wA);
                memmove(inFIFO.data(), inFIFO.data() + SP_HOP,
                        (SP_FFT - SP_HOP) * sizeof(float));
            }
        }
    }

private:
    float timeMedian(int k)
    {
        float tmp[SP_TMED];
        int m = histFill;
        for (int j = 0; j < m; j++) tmp[j] = hist[(size_t)j * SP_BINS + k];
        if (m == 0) return 0.0f;
        std::nth_element(tmp, tmp + m / 2, tmp + m);
        return tmp[m / 2];
    }
    float freqMedian(int k)
    {
        float tmp[2 * SP_FMED + 1];
        int c = 0;
        for (int d = -SP_FMED; d <= SP_FMED; d++) {
            int b = k + d;
            if (b >= 0 && b < SP_BINS) tmp[c++] = mag[b];
        }
        std::nth_element(tmp, tmp + c / 2, tmp + c);
        return tmp[c / 2];
    }

    void frame(const float* w, float wT, float wA)
    {
        for (int k = 0; k < SP_FFT; k++) { re[k] = inFIFO[k] * w[k]; im[k] = 0.0f; }
        fft_radix2(re.data(), im.data(), SP_FFT, 0);
        for (int k = 0; k < SP_BINS; k++)
            mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);

        /* push current magnitude into the time-history ring */
        memcpy(&hist[(size_t)histPos * SP_BINS], mag.data(), SP_BINS * sizeof(float));
        histPos = (histPos + 1) % SP_TMED;
        if (histFill < SP_TMED) histFill++;

        for (int k = 0; k < SP_BINS; k++) {
            float Ht = timeMedian(k);           /* tonal (sustained)   */
            float Pf = freqMedian(k);           /* atonal (broadband)  */
            float ht = Ht * Ht, pf = Pf * Pf;
            float den = ht + pf + 1e-12f;
            float Mh = ht / den;                /* Mh + Mp == 1        */
            float Mp = pf / den;
            float g = Mh * wT + Mp * wA;
            re[k] *= g; im[k] *= g;
        }
        /* rebuild the conjugate-symmetric upper half for a real IFFT */
        for (int k = 1; k < SP_FFT / 2; k++) {
            re[SP_FFT - k] = re[k];
            im[SP_FFT - k] = -im[k];
        }
        im[0] = 0.0f; im[SP_FFT / 2] = 0.0f;
        fft_radix2(re.data(), im.data(), SP_FFT, 1);   /* inverse divides by N */

        /* Hann synthesis window + overlap-add; 4x Hann COLA sum-of-squares=1.5 */
        const float scale = 2.0f / 3.0f;
        for (int k = 0; k < SP_FFT; k++) outAccum[k] += re[k] * w[k] * scale;
        memcpy(outFIFO.data(), outAccum.data(), SP_HOP * sizeof(float));
        memmove(outAccum.data(), outAccum.data() + SP_HOP,
                (SP_FFT - SP_HOP) * sizeof(float));
        memset(outAccum.data() + (SP_FFT - SP_HOP), 0, SP_HOP * sizeof(float));
    }
};

class SplitEngine {
public:
    float sampleRate = 44100.0f;
    SplitChan ch[2];

    SplitEngine() { ch[0].reset(); ch[1].reset(); }
    void setSampleRate(float sr) { sampleRate = sr > 0 ? sr : 44100.0f; }
    void reset() { ch[0].reset(); ch[1].reset(); }
    static int latency() { return SP_LAT; }

    /* split 0..1: 0 = atonal only, 0.5 = balanced (bypass), 1 = tonal only */
    static void weights(float split, float& wT, float& wA)
    {
        if (split < 0) split = 0;
        if (split > 1) split = 1;
        wT = split * 2.0f;   if (wT > 1.0f) wT = 1.0f;
        wA = (1.0f - split) * 2.0f; if (wA > 1.0f) wA = 1.0f;
    }

    void process(const float* const* in, float* const* out, int n,
                 float split, float mix, float outGain)
    {
        float wT, wA; weights(split, wT, wA);
        ch[0].process(in ? in[0] : nullptr, out[0], n, wT, wA, mix, outGain);
        ch[1].process(in ? in[1] : nullptr, out[1], n, wT, wA, mix, outGain);
    }
};

#endif /* SPECTRAL_SPLIT_H */
