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

    /* Generate a mono one-shot. `style` (optional, at sr) seeds the latent;
     * morph blends prior<->style. */
    bool generate(float lengthSec, float chaos, float morph, float pitchSpread,
                  uint32_t seed, const std::vector<float>* style,
                  std::vector<float>& outMono)
    {
        outMono.clear();
        if (!ready) return false;

        int nFrames = (int)(lengthSec * sr / hop);
        if (nFrames < 4) nFrames = 4;
        if (nFrames > 2048) nFrames = 2048;

        uint32_t rng = seed * 2654435761u + 0x9E3779B9u;
        if (rng == 0) rng = 1;
        auto rf = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                          return (rng >> 8) * (1.0f / 16777216.0f); };
        auto rn = [&]() { return (rf() + rf() + rf()) * 2.0f - 3.0f; };

        /* base latent: prior N(0,1) scaled by learned spread, optionally
         * blended toward an encoded style sound */
        std::vector<float> base(latent), styleLat(latent, 0.0f);
        for (int d = 0; d < latent; d++) base[d] = rn() * latStd[d];
        if (style && morph > 0.001f && encodeStyle(*style, styleLat))
            for (int d = 0; d < latent; d++)
                base[d] = base[d] * (1.0f - morph) + styleLat[d] * morph;

        /* magnitude spectrogram: walk the latent, decode each frame */
        std::vector<float> z(latent), walk(latent, 0.0f);
        int half = fftN / 2;
        std::vector<float> spec((size_t)nFrames * (half + 1), 0.0f);
        std::vector<float> frame(nbins);
        for (int t = 0; t < nFrames; t++) {
            for (int d = 0; d < latent; d++) {
                walk[d] = 0.85f * walk[d] + 0.15f * rn();
                z[d] = base[d] + walk[d] * chaos * 2.0f * latStd[d];
            }
            decodeFrame(z, frame);                       /* normalized log-mag */
            /* one-shot amplitude envelope: quick attack, exp decay */
            float tn = (float)t / nFrames;
            float amp = (1.0f - expf(-(float)(t + 1) * 0.5f)) * expf(-3.0f * tn);
            float* row = &spec[(size_t)t * (half + 1)];
            for (int k = 0; k < nbins && k <= half; k++) {
                float lm = frame[k] * stdv[k] + mean[k];
                float mag = expf(lm) - logeps;
                if (mag < 0) mag = 0;
                row[k] = mag * amp;
            }
        }

        griffinLim(spec, nFrames, 40, outMono);

        /* pitch spread: random detune via resample */
        if (pitchSpread > 0.01f) {
            float semis = (rf() * 2.0f - 1.0f) * pitchSpread;
            double r = pow(2.0, semis / 12.0);
            int n2 = (int)(outMono.size() / r);
            if (n2 > 64) {
                std::vector<float> s((size_t)n2);
                for (int i = 0; i < n2; i++) {
                    double sp = i * r; int i0 = (int)sp;
                    if (i0 >= (int)outMono.size() - 1) i0 = (int)outMono.size() - 2;
                    float ft = (float)(sp - i0);
                    s[i] = outMono[i0] + (outMono[i0 + 1] - outMono[i0]) * ft;
                }
                outMono.swap(s);
            }
        }

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
