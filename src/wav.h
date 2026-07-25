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

/* ------------------------------------------------------------------------
 * Additions for the Sample Librarian: header-only probe (cheap filters over
 * huge libraries), partial decode (analyze the head of a file without
 * slurping gigabytes), and a 16-bit writer for combo export.
 * ---------------------------------------------------------------------- */

struct WavInfo {
    int  frames = 0;
    int  sampleRate = 44100;
    int  channels = 0;
    int  bits = 0;
    int  fmt = 1;
    long dataOffset = 0;
    long dataLen = 0;
    long fileSize = 0;
    bool ok = false;
};

/* Parse only chunk headers — no sample data is read. */
static inline WavInfo wav_probe(const char* path)
{
    WavInfo out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    out.fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return out;
    }

    long pos = 12;
    while (pos + 8 <= out.fileSize) {
        unsigned char ck[8];
        fseek(f, pos, SEEK_SET);
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t ckLen = wav_rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && ckLen >= 16) {
            unsigned char body[40];
            size_t want = ckLen < 40 ? ckLen : 40;
            if (fread(body, 1, want, f) == want) {
                out.fmt        = wav_rd16(body + 0);
                out.channels   = wav_rd16(body + 2);
                out.sampleRate = (int)wav_rd32(body + 4);
                out.bits       = wav_rd16(body + 14);
                if (out.fmt == 0xFFFE && want >= 26)
                    out.fmt = wav_rd16(body + 24);
            }
        } else if (!memcmp(ck, "data", 4)) {
            out.dataOffset = pos + 8;
            out.dataLen = (long)ckLen;
            if (out.dataOffset + out.dataLen > out.fileSize)
                out.dataLen = out.fileSize - out.dataOffset;
        }
        pos += 8 + (long)ckLen + (ckLen & 1);
    }
    fclose(f);

    if (out.dataOffset > 0 && out.channels >= 1 && out.bits >= 8 &&
        !(out.bits & 7) && (out.fmt == 1 || out.fmt == 3) &&
        out.sampleRate > 0) {
        int frameBytes = (out.bits / 8) * out.channels;
        out.frames = (int)(out.dataLen / frameBytes);
        out.ok = out.frames > 0;
    }
    return out;
}

/* Decode only the first maxFrames (0 = everything). Interleaved stereo out. */
static inline WavData wav_load_partial(const char* path, int maxFrames)
{
    WavData out;
    WavInfo info = wav_probe(path);
    if (!info.ok) return out;

    int frames = info.frames;
    if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;
    int bytesPer = info.bits / 8;
    int frameBytes = bytesPer * info.channels;
    size_t need = (size_t)frames * frameBytes;
    if (need > (size_t)(400L << 20)) return out;     /* sanity cap */

    FILE* f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, info.dataOffset, SEEK_SET);
    std::vector<unsigned char> buf(need);
    size_t got = fread(buf.data(), 1, need, f);
    fclose(f);
    frames = (int)(got / frameBytes);
    if (frames <= 0) return out;

    out.samples.resize((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const unsigned char* fr = buf.data() + (size_t)i * frameBytes;
        float L = wav_sample(fr, info.fmt, info.bits);
        float R = (info.channels >= 2) ? wav_sample(fr + bytesPer, info.fmt, info.bits) : L;
        out.samples[(size_t)i * 2 + 0] = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames = frames;
    out.sampleRate = info.sampleRate;
    out.ok = true;
    return out;
}

/* Write interleaved stereo float as a 16-bit PCM WAV. */
static inline bool wav_write16(const char* path, const float* lr, int frames,
                               int rate)
{
    if (frames <= 0) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t dataBytes = (uint32_t)frames * 4;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + dataBytes;
    h[4] = (unsigned char)riff; h[5] = (unsigned char)(riff >> 8);
    h[6] = (unsigned char)(riff >> 16); h[7] = (unsigned char)(riff >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t f16 = 16; memcpy(h + 16, &f16, 4);
    uint16_t pcm = 1, ch = 2, blk = 4, bits = 16;
    memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    uint32_t sr = (uint32_t)rate, br = sr * 4;
    memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4);
    memcpy(h + 32, &blk, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &dataBytes, 4);
    if (fwrite(h, 1, 44, f) != 44) { fclose(f); return false; }

    for (int i = 0; i < frames * 2; i++) {
        float x = lr[i];
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
        int16_t s = (int16_t)(x * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return true;
}

#endif /* WAV_MIN_H */
