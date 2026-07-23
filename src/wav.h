/*
 * wav.h — minimal clean-room WAV reader. No external deps.
 *
 * Supports the formats a sampler actually meets in the wild:
 *   - PCM integer: 8-bit (unsigned), 16 / 24 / 32-bit (signed)
 *   - IEEE float:  32 / 64-bit
 *   - mono or multi-channel (folded to stereo: mono duplicated, >2 takes L/R)
 *
 * Returns audio as interleaved stereo float in [-1, 1] at the file's own
 * sample rate; the engine reconciles that against the host rate.
 *
 * Public-domain / CC0.
 */
#ifndef WAV_MIN_H
#define WAV_MIN_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct WavData {
    std::vector<float> samples; /* interleaved stereo: L,R,L,R,...   */
    int   frames = 0;
    int   sampleRate = 44100;
    bool  ok = false;
};

static inline uint32_t wav_rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t wav_rd16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Read one sample from raw frame data into a normalized float. */
static inline float wav_sample(const unsigned char* p, int fmt, int bits)
{
    if (fmt == 3) {                               /* IEEE float */
        if (bits == 32) { float f; memcpy(&f, p, 4); return f; }
        if (bits == 64) { double d; memcpy(&d, p, 8); return (float)d; }
        return 0.0f;
    }
    /* PCM integer */
    switch (bits) {
    case 8:  return ((int)p[0] - 128) / 128.0f;   /* 8-bit is unsigned */
    case 16: return (int16_t)wav_rd16(p) / 32768.0f;
    case 24: {
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                              ((uint32_t)p[2] << 16));
        if (v & 0x800000) v |= ~0xFFFFFF;         /* sign-extend */
        return v / 8388608.0f;
    }
    case 32: return (int32_t)wav_rd32(p) / 2147483648.0f;
    }
    return 0.0f;
}

static inline WavData wav_load(const char* path)
{
    WavData out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > (200L << 20)) { fclose(f); return out; }  /* cap 200MB */

    std::vector<unsigned char> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) return out;

    const unsigned char* b = buf.data();
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return out;

    int fmt = 1, channels = 0, rate = 44100, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataLen = 0;

    size_t pos = 12;
    while (pos + 8 <= (size_t)sz) {
        const unsigned char* ck = b + pos;
        uint32_t ckLen = wav_rd32(ck + 4);
        const unsigned char* body = ck + 8;
        if (pos + 8 + ckLen > (size_t)sz) ckLen = (uint32_t)(sz - pos - 8);

        if (!memcmp(ck, "fmt ", 4) && ckLen >= 16) {
            fmt      = wav_rd16(body + 0);
            channels = wav_rd16(body + 2);
            rate     = (int)wav_rd32(body + 4);
            bits     = wav_rd16(body + 14);
            if (fmt == 0xFFFE && ckLen >= 26)     /* WAVE_FORMAT_EXTENSIBLE */
                fmt = wav_rd16(body + 24);        /* real format in subFormat */
        } else if (!memcmp(ck, "data", 4)) {
            data = body;
            dataLen = ckLen;
        }
        pos += 8 + ckLen + (ckLen & 1);           /* chunks are word-aligned */
    }

    if (!data || channels < 1 || bits < 8 || (bits & 7) ||
        (fmt != 1 && fmt != 3))
        return out;

    int bytesPer = bits / 8;
    int frameBytes = bytesPer * channels;
    if (frameBytes <= 0) return out;
    int frames = (int)(dataLen / (size_t)frameBytes);
    if (frames <= 0) return out;

    out.samples.resize((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const unsigned char* fr = data + (size_t)i * frameBytes;
        float L = wav_sample(fr, fmt, bits);
        float R = (channels >= 2) ? wav_sample(fr + bytesPer, fmt, bits) : L;
        out.samples[(size_t)i * 2 + 0] = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames = frames;
    out.sampleRate = rate > 0 ? rate : 44100;
    out.ok = true;
    return out;
}

#endif /* WAV_MIN_H */
