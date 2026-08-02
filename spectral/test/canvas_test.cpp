/*
 * canvas_test.cpp — headless checks for the Spectral Canvas engine.
 * Same DSP the DAW runs. Build/run via `make spectraltest`.
 */
#include <cstdio>
#include <cmath>
#include <vector>
#include "canvas.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } \
                         else printf("ok:   %s\n", m); } while (0)

/* interleaved stereo sine at hz for [start,end) */
static void tone(std::vector<float>& d, int start, int end, float hz, int rate, float amp = 0.4f)
{
    for (int i = start; i < end; i++) {
        float s = amp * sinf(2 * 3.14159265f * hz * i / rate);
        d[(size_t)i * 2] = s; d[(size_t)i * 2 + 1] = s;
    }
}

static double energy(const std::vector<float>& d) { double e = 0; for (float v : d) e += (double)v * v; return e; }

int main()
{
    int rate = 44100;
    Canvas cv;
    cv.setSampleRate((float)rate);
    cv.glIters = 12;

    /* loop-sync math: 8 bars of 4/4 at 120 BPM = 16 s, multiple of HOP */
    int len = cv.samplesPerLoop(8, 120.0, 4, 4);
    CHECK(len % SC_HOP == 0, "loop length is a whole number of hops");
    CHECK(fabs(len - 16.0 * rate) < SC_HOP * 2, "8 bars @120 = ~16 s");
    int half = cv.samplesPerLoop(8, 240.0, 4, 4);
    CHECK(abs(half * 2 - len) < SC_HOP * 4, "doubling tempo halves the loop");
    int threefour = cv.samplesPerLoop(8, 120.0, 3, 4);
    CHECK(threefour < len, "3/4 loop is shorter than 4/4");

    /* build a source: low tone in first half, high tone in second half */
    std::vector<float> buf((size_t)len * 2, 0.0f);
    tone(buf, 0, len / 2, 220.0f, rate);
    tone(buf, len / 2, len, 1760.0f, rate);
    cv.analyze(buf.data(), len, 2, rate);
    CHECK(cv.src.frames == len / SC_HOP, "analyze produced the right frame count");

    /* round-trip with no ops should closely reconstruct the input */
    { RenderBuf rb; cv.render(rb);
      CHECK(rb.frames == len, "render length matches the loop");
      double ein = energy(buf), eout = energy(rb.data);
      bool fin = true; for (float v : rb.data) if (!std::isfinite(v)) fin = false;
      CHECK(fin, "no-op render is finite");
      CHECK(eout > ein * 0.5 && eout < ein * 2.0, "no-op render preserves energy"); }

    /* helper: RMS of a time window of one channel */
    auto winRMS = [&](const std::vector<float>& d, int a, int b) {
        double s = 0; int n = 0;
        for (int i = a; i < b; i++) { s += (double)d[(size_t)i * 2] * d[(size_t)i * 2]; n++; }
        return n ? sqrt(s / n) : 0.0;
    };
    /* spectral energy in a bin band of a time window (from a fresh STFT) */
    auto bandEnergy = [&](const std::vector<float>& interleaved, int a, int b,
                          float loHz, float hiHz) {
        int n = b - a; std::vector<float> mono(n);
        for (int i = 0; i < n; i++) mono[i] = interleaved[(size_t)(a + i) * 2];
        int fr = n / SC_HOP; if (fr < 1) return 0.0;
        std::vector<float> mg((size_t)fr * SC_BINS), pp((size_t)fr * SC_BINS);
        sc_stft(mono.data(), n, fr, mg.data(), pp.data());
        int k0 = (int)(loHz * SC_FFT / rate), k1 = (int)(hiHz * SC_FFT / rate);
        if (k0 < 0) k0 = 0;
        if (k1 > SC_BINS) k1 = SC_BINS;
        double e = 0; for (int f = 0; f < fr; f++)
            for (int k = k0; k < k1; k++) e += mg[(size_t)f * SC_BINS + k];
        return e;
    };

    /* ---- OP_ERASE: silence the low band in the first half ---- */
    { Canvas c2; c2.setSampleRate((float)rate);
      c2.analyze(buf.data(), len, 2, rate);
      Op o; o.type = OP_ERASE; o.r = { 0.0f, 0.5f, 0.0f, 0.10f };  /* low bins, first half */
      c2.ops.push_back(o);
      RenderBuf rb; c2.render(rb);
      double before = bandEnergy(buf, 0, len / 2, 150, 320);
      double after  = bandEnergy(rb.data, 0, len / 2, 150, 320);
      CHECK(after < before * 0.35, "erase removes the low band in the first half"); }

    /* ---- OP_GAIN: boosting a region raises its energy ---- */
    { Canvas c2; c2.setSampleRate((float)rate);
      c2.analyze(buf.data(), len, 2, rate);
      Op o; o.type = OP_GAIN; o.amount = 3.0f; o.r = { 0.5f, 1.0f, 0.0f, 1.0f };
      c2.ops.push_back(o);
      RenderBuf rb; c2.render(rb);
      double a = winRMS(rb.data, len / 2 + rate / 4, len - rate / 4);
      double b = winRMS(buf,    len / 2 + rate / 4, len - rate / 4);
      CHECK(a > b * 1.5, "gain 3x boosts the second-half loudness"); }

    /* ---- OP_MOVE: copy the high tone from the 2nd half into the 1st half ---- */
    { Canvas c2; c2.setSampleRate((float)rate);
      c2.analyze(buf.data(), len, 2, rate);
      Op o; o.type = OP_MOVE; o.cut = 0;
      o.r = { 0.5f, 1.0f, 0.0f, 1.0f };   /* whole 2nd half */
      o.dt = -0.5f;                        /* move it back to the 1st half */
      c2.ops.push_back(o);
      RenderBuf rb; c2.render(rb);
      /* the first half should now contain high-frequency energy it didn't have */
      double hiBefore = bandEnergy(buf,     0, len / 2, 1500, 2000);
      double hiAfter  = bandEnergy(rb.data, 0, len / 2, 1500, 2000);
      CHECK(hiAfter > hiBefore * 4.0, "move copies the high tone into the first half"); }

    /* ---- OP_PITCH: pitch the whole loop up an octave shifts energy higher ---- */
    { Canvas c2; c2.setSampleRate((float)rate);
      std::vector<float> b2((size_t)len * 2, 0.0f);
      tone(b2, 0, len, 440.0f, rate);           /* pure 440 everywhere */
      c2.analyze(b2.data(), len, 2, rate);
      Op o; o.type = OP_PITCH; o.pitch = 12.0f; o.r = { 0.0f, 1.0f, 0.0f, 1.0f };
      c2.ops.push_back(o);
      RenderBuf rb; c2.render(rb);
      double e440 = bandEnergy(rb.data, 0, len, 380, 500);
      double e880 = bandEnergy(rb.data, 0, len, 760, 1000);
      CHECK(e880 > e440, "pitch +12 st moves 440 Hz energy up toward 880 Hz"); }

    /* ---- OP_SMEAR (freeze-tail): fills quiet gaps with sustained energy ---- */
    { Canvas c2; c2.setSampleRate((float)rate);
      std::vector<float> b2((size_t)len * 2, 0.0f);
      tone(b2, 0, len / 8, 660.0f, rate);       /* short blip, then silence */
      c2.analyze(b2.data(), len, 2, rate);
      Op o; o.type = OP_SMEAR; o.smearDir = 0; o.amount = 1.0f;
      o.r = { 0.0f, 0.9f, 0.0f, 1.0f };
      c2.ops.push_back(o);
      RenderBuf rb; c2.render(rb);
      double gapBefore = winRMS(b2,     len / 2, len / 2 + rate / 4);
      double gapAfter  = winRMS(rb.data, len / 2, len / 2 + rate / 4);
      CHECK(gapAfter > gapBefore * 3.0, "smear freeze-tail sustains into the gap"); }

    /* ---- OP_CA: cellular-automata gate changes the loop but stays finite ---- */
    { Canvas c2; c2.setSampleRate((float)rate);
      c2.analyze(buf.data(), len, 2, rate);
      Op o; o.type = OP_CA; o.caRule = 90; o.amount = 0.5f;
      o.r = { 0.0f, 1.0f, 0.0f, 1.0f };
      c2.ops.push_back(o);
      RenderBuf rb; c2.render(rb);
      bool fin = true; for (float v : rb.data) if (!std::isfinite(v)) fin = false;
      double e0 = energy(buf), e1 = energy(rb.data);
      CHECK(fin, "CA render is finite");
      CHECK(e1 < e0 && e1 > 0, "CA gate reduces (but keeps some) energy"); }

    /* ---- display image is populated and normalized ---- */
    { RenderBuf rb; cv.render(rb);
      CHECK((int)rb.disp.size() == SC_DISP_T * SC_DISP_F, "display image sized");
      float mx = 0; for (float v : rb.disp) { if (v < 0 || v > 1) mx = -1; if (v > mx && mx >= 0) mx = v; }
      CHECK(mx > 0.05f, "display image has visible content in 0..1"); }

    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL CANVAS CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
