/*
 * nnsynth.h — neural one-shot synthesis for the Sample Librarian.
 *
 * Runs the small spectral VAE trained by tools/train_vae.py. Weights load from
 * an RTNeural-format JSON (nnvae.json) — a plain Dense stack, so the model is
 * portable to the RTNeural library; here we run the forward pass directly
 * (a few matmuls) so the plugin has NO runtime library to ship: header-only,
 * MinGW-clean, compiles into the DLL. Only nlohmann/json is vendored, the same
 * JSON parser RTNeural itself uses.
 *
 * generate(): draw a latent trajectory (optionally seeded by an encoded style
 * sound), decode each frame to a log-magnitude spectrum, and reconstruct audio
 * with Griffin-Lim (via fft.h). Knobs: Length = number of frames, Chaos =
 * latent temperature / drift, PitchSpread = post-resample detune. Deterministic
 * for a given seed. Falls back gracefully (ready=false) when no model is present.
 */
#ifndef NNSYNTH_H
#define NNSYNTH_H

#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "fft.h"
#include "../../third_party/json.hpp"

/* one-shot shapes (envelope + latent motion presets) */
enum { NN_PLUCK = 0, NN_PAD, NN_DRONE, NN_FREE, NN_SHAPE_COUNT };

struct GenParams {
    float lengthSec  = 1.5f;
    float chaos      = 0.35f;  /* latent drift / temperature   */
    float morph      = 0.0f;   /* blend an encoded style latent*/
    float pitchSpread= 0.0f;   /* random detune, semitones     */
    int   shape      = NN_PLUCK;
    int   keySemi    = -1;     /* -1 = off, else pitch-class 0..11 to snap to */
    float tone       = 0.0f;   /* spectral tilt dark(-1)..bright(+1) */
    float motion     = 0.0f;   /* sweeping band emphasis 0..1  */
    float focus      = 0.25f;  /* harmonic sharpening 0..1     */
    uint32_t seed    = 1;
};

class VaeSynth {
public:
    bool ready = false;
    std::string err = "no model loaded";
    int sr = 44100;

    bool load(const std::string& dir)
    {
        ready = false;
        std::string path = dir +
#ifdef _WIN32
            "\\nnvae.json";
#else
            "/nnvae.json";
#endif
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { err = "nnvae.json not found in " + dir; return false; }
        std::string txt;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (128L << 20)) { fclose(f); err = "bad model size"; return false; }
        txt.resize((size_t)sz);
        size_t got = fread(&txt[0], 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) { err = "read failed"; return false; }

        try {
            auto j = nlohmann::json::parse(txt);
            auto& m = j.at("meta");
            sr    = m.at("sr").get<int>();
            fftN  = m.at("fft").get<int>();
            hop   = m.at("hop").get<int>();
            nbins = m.at("nbins").get<int>();
            latent= m.at("latent").get<int>();
            logeps= m.at("logeps").get<float>();
            mean  = m.at("mean").get<std::vector<float>>();
            stdv  = m.at("std").get<std::vector<float>>();
            latStd= m.at("lat_std").get<std::vector<float>>();
            loadStack(j.at("encoder").at("layers"), enc);
            loadStack(j.at("decoder").at("layers"), dec);
            if ((int)mean.size() != nbins || (int)stdv.size() != nbins ||
                fftN < 64 || hop < 1 || latent < 1) {
                err = "meta/shape mismatch"; return false;
            }
        } catch (const std::exception& e) {
            err = std::string("json: ") + e.what();
            return false;
        }
        ready = true;
        return true;
    }

    /* Generate a mono one-shot. `style` (optional, at sr) seeds the latent. */
    bool generate(const GenParams& gp, const std::vector<float>* style,
                  std::vector<float>& outMono)
    {
        outMono.clear();
        if (!ready) return false;

        int nFrames = (int)(gp.lengthSec * sr / hop);
        if (nFrames < 4) nFrames = 4;
        if (nFrames > 3072) nFrames = 3072;

        uint32_t rng = gp.seed * 2654435761u + 0x9E3779B9u;
        if (rng == 0) rng = 1;
        auto rf = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                          return (rng >> 8) * (1.0f / 16777216.0f); };
        auto rn = [&]() { return (rf() + rf() + rf()) * 2.0f - 3.0f; };

        /* per-shape latent motion + LFO character */
        float walkScale = 1.0f, lfoAmt = 0.0f;
        switch (gp.shape) {
        case NN_PLUCK: walkScale = 0.12f; lfoAmt = 0.0f;  break;
        case NN_PAD:   walkScale = 0.55f; lfoAmt = 0.45f; break;
        case NN_DRONE: walkScale = 0.22f; lfoAmt = 0.15f; break;
        default:       walkScale = 1.0f;  lfoAmt = 0.0f;  break;  /* Free */
        }

        std::vector<float> base(latent), styleLat(latent, 0.0f), lfoPh(latent);
        for (int d = 0; d < latent; d++) { base[d] = rn() * latStd[d];
                                           lfoPh[d] = rf() * 6.2831853f; }
        if (style && gp.morph > 0.001f && encodeStyle(*style, styleLat))
            for (int d = 0; d < latent; d++)
                base[d] = base[d] * (1.0f - gp.morph) + styleLat[d] * gp.morph;

        int half = fftN / 2;
        std::vector<float> spec((size_t)nFrames * (half + 1), 0.0f);
        std::vector<float> z(latent), walk(latent, 0.0f), mag(nbins), frame(nbins);

        float sig = nbins * 0.12f;             /* motion band width */
        for (int t = 0; t < nFrames; t++) {
            float tn = nFrames > 1 ? (float)t / (nFrames - 1) : 0.0f;
            for (int d = 0; d < latent; d++) {
                walk[d] = 0.88f * walk[d] + 0.12f * rn();
                float lfo = lfoAmt * latStd[d] * sinf(1.5f * 6.2831853f * tn + lfoPh[d]);
                z[d] = base[d] + walk[d] * gp.chaos * 2.0f * latStd[d] * walkScale + lfo;
            }
            decodeFrame(z, frame);
            for (int k = 0; k < nbins; k++) {
                float m = expf(frame[k] * stdv[k] + mean[k]) - logeps;
                mag[k] = m > 0 ? m : 0;
            }

            /* focus: sharpen spectral peaks, energy-preserving -> more tonal */
            if (gp.focus > 0.01f) {
                double s0 = 0, s1 = 0;
                float p = 1.0f + gp.focus * 1.6f;
                for (int k = 0; k < nbins; k++) { s0 += mag[k]; mag[k] = powf(mag[k], p); s1 += mag[k]; }
                float sc = (float)(s0 / (s1 + 1e-9));
                for (int k = 0; k < nbins; k++) mag[k] *= sc;
            }

            /* tone tilt + sweeping band emphasis (motion) */
            float centerBin = (0.42f + 0.4f * sinf(6.2831853f * tn * 1.0f)) * nbins;
            for (int k = 0; k < nbins; k++) {
                if (gp.tone > 0.01f || gp.tone < -0.01f)
                    mag[k] *= powf(10.0f, gp.tone * ((float)k / nbins - 0.45f) * 1.2f);
                if (gp.motion > 0.01f) {
                    float d = (k - centerBin) / sig;
                    mag[k] *= 1.0f + gp.motion * 1.6f * expf(-0.5f * d * d) - gp.motion * 0.35f;
                    if (mag[k] < 0) mag[k] = 0;
                }
            }

            float amp = shapeEnv(gp.shape, t, nFrames);
            float* row = &spec[(size_t)t * (half + 1)];
            for (int k = 0; k < nbins && k <= half; k++) row[k] = mag[k] * amp;
        }

        griffinLim(spec, nFrames, 40, outMono);

        /* spectral pitch correction: snap the fundamental to the chosen key */
        if (gp.keySemi >= 0) {
            float f0 = detectF0(outMono);
            if (f0 > 25.0f) {
                float midf = 69.0f + 12.0f * log2f(f0 / 440.0f);
                float best = 1e9f; int bestM = (int)lroundf(midf);
                for (int m = (int)midf - 6; m <= (int)midf + 6; m++) {
                    int pc = ((m % 12) + 12) % 12;
                    if (pc == gp.keySemi && fabsf(m - midf) < best) { best = fabsf(m - midf); bestM = m; }
                }
                if (best <= 3.0f) {                       /* only "fine" corrections */
                    float target = 440.0f * powf(2.0f, (bestM - 69) / 12.0f);
                    resampleRatio(outMono, target / f0);   /* raise/lower f0 -> target */
                }
            }
        }

        /* random detune spread */
        if (gp.pitchSpread > 0.01f)
            resampleRatio(outMono, powf(2.0f, ((rf() * 2.0f - 1.0f) * gp.pitchSpread) / 12.0f));

        /* normalize + short fades */
        float peak = 1e-9f;
        for (float v : outMono) { float a = fabsf(v); if (a > peak) peak = a; }
        float g = 0.9f / peak;
        int n = (int)outMono.size(), fN = sr / 200;
        for (int i = 0; i < n; i++) {
            float e = g;
            if (i < fN) e *= (float)i / fN;
            if (i > n - fN) e *= (float)(n - i) / fN;
            outMono[i] *= e;
        }
        return n > 0;
    }

private:
    struct Layer { int in, out, act; std::vector<float> W, b; };  /* act:0 lin 1 tanh */
    std::vector<Layer> enc, dec;
    int fftN = 1024, hop = 256, nbins = 192, latent = 16;
    float logeps = 1e-4f;
    std::vector<float> mean, stdv, latStd;

    /* per-shape amplitude envelope over the one-shot */
    static float shapeEnv(int shape, int t, int nFrames)
    {
        float tn = nFrames > 1 ? (float)t / (nFrames - 1) : 0.0f;
        switch (shape) {
        case NN_PLUCK: {
            float att = 1.0f - expf(-(float)(t + 1) * 0.9f);
            return att * expf(-4.5f * tn);
        }
        case NN_PAD: {
            float aF = 0.30f, rF = 0.32f;
            float a = tn < aF ? tn / aF : 1.0f;
            a = 0.5f * (1.0f - cosf(3.14159265f * a));
            float r = tn > (1.0f - rF) ? (1.0f - tn) / rF : 1.0f;
            r = 0.5f * (1.0f - cosf(3.14159265f * r));
            return a * r;
        }
        case NN_DRONE: {
            float f = 0.04f;
            float a = tn < f ? tn / f : 1.0f;
            float r = tn > (1.0f - f) ? (1.0f - tn) / f : 1.0f;
            return a * r;
        }
        default:       /* Free: original quick-attack exp-decay */
            return (1.0f - expf(-(float)(t + 1) * 0.5f)) * expf(-3.0f * tn);
        }
    }

    /* resample the buffer so pitch multiplies by `ratio` (in-place) */
    void resampleRatio(std::vector<float>& x, double ratio)
    {
        if (ratio < 0.25 || ratio > 4.0 || fabs(ratio - 1.0) < 1e-4) return;
        int n2 = (int)(x.size() / ratio);
        if (n2 < 64) return;
        std::vector<float> s((size_t)n2);
        for (int i = 0; i < n2; i++) {
            double sp = i * ratio; int i0 = (int)sp;
            if (i0 >= (int)x.size() - 1) i0 = (int)x.size() - 2;
            float ft = (float)(sp - i0);
            s[i] = x[i0] + (x[i0 + 1] - x[i0]) * ft;
        }
        x.swap(s);
    }

    /* fundamental frequency via autocorrelation, from the sound's body */
    float detectF0(const std::vector<float>& a)
    {
        int N = 8192;
        int off = (int)a.size() / 4;
        if (off + N > (int)a.size()) { off = 0; if (N > (int)a.size()) N = (int)a.size(); }
        if (N < 512) return 0;
        std::vector<float> x(a.begin() + off, a.begin() + off + N);
        double m = 0; for (float v : x) m += v; m /= N;
        for (float& v : x) v -= (float)m;
        int lo = sr / 1000, hi = sr / 50;
        if (hi >= N) hi = N - 1;
        double r0 = 0; for (float v : x) r0 += (double)v * v;
        if (r0 < 1e-9) return 0;
        float best = 0; int bestLag = 0;
        for (int lag = lo; lag <= hi; lag++) {
            double s = 0;
            for (int i = 0; i + lag < N; i++) s += (double)x[i] * x[i + lag];
            float v = (float)(s / r0);
            if (v > best) { best = v; bestLag = lag; }
        }
        return (bestLag > 0 && best > 0.25f) ? (float)sr / bestLag : 0.0f;
    }

    void loadStack(const nlohmann::json& layers, std::vector<Layer>& out)
    {
        out.clear();
        for (const auto& L : layers) {
            Layer ly;
            ly.act = (L.at("activation").get<std::string>() == "tanh") ? 1 : 0;
            ly.out = L.at("out").get<int>();
            ly.b = L.at("bias").get<std::vector<float>>();
            const auto& W = L.at("weights");
            ly.out = (int)W.size();
            ly.in = ly.out ? (int)W[0].size() : 0;
            ly.W.resize((size_t)ly.out * ly.in);
            for (int o = 0; o < ly.out; o++) {
                const auto& row = W[o];
                for (int i = 0; i < ly.in; i++)
                    ly.W[(size_t)o * ly.in + i] = row[i].get<float>();
            }
            out.push_back(std::move(ly));
        }
    }

    static void runStack(const std::vector<Layer>& st, std::vector<float>& x)
    {
        for (const Layer& L : st) {
            std::vector<float> y((size_t)L.out);
            for (int o = 0; o < L.out; o++) {
                float s = L.b[o];
                const float* w = &L.W[(size_t)o * L.in];
                for (int i = 0; i < L.in; i++) s += w[i] * x[i];
                y[o] = L.act == 1 ? tanhf(s) : s;
            }
            x.swap(y);
        }
    }

    void decodeFrame(const std::vector<float>& z, std::vector<float>& out)
    {
        std::vector<float> x = z;
        runStack(dec, x);
        out = x;                          /* length nbins, normalized log-mag */
    }

    /* encode a style sound to an average latent */
    bool encodeStyle(const std::vector<float>& mono, std::vector<float>& lat)
    {
        if ((int)mono.size() < fftN) return false;
        std::vector<float> win(fftN);
        for (int n = 0; n < fftN; n++)
            win[n] = 0.5f - 0.5f * cosf(6.2831853f * n / fftN);
        std::vector<float> re(fftN), im(fftN), acc(latent, 0.0f);
        int frames = 0;
        for (int s = 0; s + fftN <= (int)mono.size() && frames < 64; s += hop) {
            for (int n = 0; n < fftN; n++) { re[n] = mono[s + n] * win[n]; im[n] = 0; }
            fft_radix2(re.data(), im.data(), fftN, 0);
            std::vector<float> x((size_t)nbins);
            for (int k = 0; k < nbins; k++) {
                float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
                x[k] = (logf(mag + logeps) - mean[k]) / stdv[k];
            }
            runStack(enc, x);             /* -> latent */
            for (int d = 0; d < latent; d++) acc[d] += x[d];
            frames++;
        }
        if (!frames) return false;
        lat.assign(latent, 0.0f);
        for (int d = 0; d < latent; d++) lat[d] = acc[d] / frames;
        return true;
    }

    /* Griffin-Lim over a magnitude spectrogram [nFrames][half+1]. */
    void griffinLim(const std::vector<float>& mag, int nFrames, int iters,
                    std::vector<float>& out)
    {
        int half = fftN / 2;
        int outLen = (nFrames - 1) * hop + fftN;
        std::vector<float> win(fftN);
        for (int n = 0; n < fftN; n++)
            win[n] = 0.5f - 0.5f * cosf(6.2831853f * n / fftN);

        std::vector<float> phase((size_t)nFrames * (half + 1), 0.0f);
        uint32_t r = 0x2468ACEu;
        for (float& p : phase) { r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            p = (r >> 8) * (6.2831853f / 16777216.0f); }

        std::vector<float> time(outLen);
        for (int it = 0; it <= iters; it++) {
            istft(mag, phase, nFrames, win, time);
            if (it == iters) break;
            reStft(time, nFrames, win, phase);           /* update phases only */
        }
        out.swap(time);
    }

    void istft(const std::vector<float>& mag, const std::vector<float>& phase,
               int nFrames, const std::vector<float>& win, std::vector<float>& out)
    {
        int half = fftN / 2;
        int outLen = (nFrames - 1) * hop + fftN;
        out.assign(outLen, 0.0f);
        std::vector<float> denom(outLen, 1e-8f);
        std::vector<float> re(fftN), im(fftN);
        for (int t = 0; t < nFrames; t++) {
            const float* mrow = &mag[(size_t)t * (half + 1)];
            const float* prow = &phase[(size_t)t * (half + 1)];
            for (int k = 0; k <= half; k++) { re[k] = mrow[k] * cosf(prow[k]);
                                              im[k] = mrow[k] * sinf(prow[k]); }
            for (int k = 1; k < half; k++) { re[fftN - k] = re[k]; im[fftN - k] = -im[k]; }
            im[0] = 0; im[half] = 0;
            fft_radix2(re.data(), im.data(), fftN, 1);
            int base = t * hop;
            for (int n = 0; n < fftN; n++) {
                out[base + n] += re[n] * win[n];
                denom[base + n] += win[n] * win[n];
            }
        }
        for (int i = 0; i < outLen; i++) out[i] /= denom[i];
    }

    void reStft(const std::vector<float>& time, int nFrames,
                const std::vector<float>& win, std::vector<float>& phase)
    {
        int half = fftN / 2;
        std::vector<float> re(fftN), im(fftN);
        for (int t = 0; t < nFrames; t++) {
            int base = t * hop;
            for (int n = 0; n < fftN; n++) { re[n] = time[base + n] * win[n]; im[n] = 0; }
            fft_radix2(re.data(), im.data(), fftN, 0);
            float* prow = &phase[(size_t)t * (half + 1)];
            for (int k = 0; k <= half; k++) prow[k] = atan2f(im[k], re[k]);
        }
    }
};

#endif /* NNSYNTH_H */
