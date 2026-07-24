/*
 * fft.h — minimal iterative radix-2 complex FFT. Clean-room, no deps.
 * Sizes must be powers of two; the engine only uses 1024.
 * Public-domain / CC0.
 */
#ifndef FFT_MIN_H
#define FFT_MIN_H

#include <math.h>

static inline void fft_radix2(float* re, float* im, int n, int inverse)
{
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * 3.14159265358979323846 / len * (inverse ? 1 : -1);
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci;
                float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }

    if (inverse) {
        float s = 1.0f / n;
        for (int i = 0; i < n; i++) { re[i] *= s; im[i] *= s; }
    }
}

#endif /* FFT_MIN_H */
