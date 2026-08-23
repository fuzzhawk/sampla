/*
 * split_test.cpp — headless checks for the Spectral Split engine.
 * Same DSP the DAW runs. Build/run via `make splittest`.
 */
#include <cstdio>
#include <cmath>
#include <vector>
#include "split.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } \
                         else printf("ok:   %s\n", m); } while (0)

static unsigned rng = 22222;
static float noise() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 8388608.0f - 1.0f; }

/* run a mono signal through the engine (both channels fed the same) */
static std::vector<float> run(std::vector<float>& sig, float split, float mix, float out)
{
    SplitEngine e; e.setSampleRate(44100);
    int n = (int)sig.size();
    std::vector<float> oL(n), oR(n);
    const float* ins[2] = { sig.data(), sig.data() };
    float* outs[2] = { oL.data(), oR.data() };
    /* process in 512-sample blocks like a DAW */
    int pos = 0;
    while (pos < n) {
        int b = n - pos; if (b > 512) b = 512;
        const float* bin[2] = { sig.data() + pos, sig.data() + pos };
        float* bout[2] = { oL.data() + pos, oR.data() + pos };
        e.process(bin, bout, b, split, mix, out);
        pos += b;
    }
    (void)ins; (void)outs;
    return oL;
}

/* energy of a signal over [a,b) */
static double energy(const std::vector<float>& d, int a, int b)
{ double e = 0; for (int i = a; i < b; i++) e += (double)d[i] * d[i]; return e; }

/* magnitude at frequency hz via Goertzel over [a,b) */
static double goertzel(const std::vector<float>& d, int a, int b, float hz, int rate)
{
    double w = 2.0 * M_PI * hz / rate, c = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = a; i < b; i++) { s0 = d[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return sqrt(re * re + im * im) / (b - a);
}

int main()
{
    int rate = 44100;
    int N = rate * 3;                 /* 3 seconds */
    int A = SP_FFT * 3, B = N - SP_FFT;  /* steady-state analysis window */

    CHECK(SP_FFT == 4096 && SP_HOP == 1024, "high-res 4096-pt FFT at 4x overlap");
    CHECK(SP_LAT > 0, "engine reports a processing latency");

    /* pure 440 Hz tone -> bypass at split=0.5 should reconstruct it */
    { std::vector<float> sig(N, 0.0f);
      for (int i = 0; i < N; i++) sig[i] = 0.4f * sinf(2 * M_PI * 440.0f * i / rate);
      std::vector<float> o = run(sig, 0.5f, 1.0f, 1.0f);
      double ein = energy(sig, A, B), eout = energy(o, A, B);
      bool fin = true; for (float v : o) if (!std::isfinite(v)) fin = false;
      CHECK(fin, "bypass output is finite");
      CHECK(eout > ein * 0.7 && eout < ein * 1.4, "split=0.5 is ~unity (bypass)"); }

    /* tone + noise: split=1 keeps the tone, kills the noise */
    { std::vector<float> sig(N);
      for (int i = 0; i < N; i++)
          sig[i] = 0.35f * sinf(2 * M_PI * 660.0f * i / rate) + 0.35f * noise();
      std::vector<float> tonal   = run(sig, 1.0f, 1.0f, 1.0f);
      std::vector<float> atonal  = run(sig, 0.0f, 1.0f, 1.0f);

      double toneIn  = goertzel(sig,    A, B, 660.0f, rate);
      double toneT   = goertzel(tonal,  A, B, 660.0f, rate);
      double toneA   = goertzel(atonal, A, B, 660.0f, rate);
      /* broadband proxy: total energy minus the tone's contribution */
      double eIn = energy(sig, A, B), eT = energy(tonal, A, B), eA = energy(atonal, A, B);
      /* tone energy ~ mag^2 * span; noise = everything else (same units) */
      double tEin = toneIn * toneIn * (B - A);
      double tET  = toneT  * toneT  * (B - A);
      double tEA  = toneA  * toneA  * (B - A);
      double noiseIn = eIn - tEin; if (noiseIn < 1e-9) noiseIn = 1e-9;
      double noiseT  = eT  - tET;  if (noiseT  < 1e-9) noiseT  = 1e-9;
      double noiseA  = eA  - tEA;  if (noiseA  < 1e-9) noiseA  = 1e-9;

      double snrIn = tEin / noiseIn;
      double snrT  = tET  / noiseT;
      double snrA  = tEA  / noiseA;
      printf("      tone/noise ratio: in=%.2f tonal=%.2f atonal=%.2f\n", snrIn, snrT, snrA);
      CHECK(snrT > snrIn * 1.4, "split=1 raises the tone-to-noise ratio (tonal)");
      CHECK(snrA < snrIn * 0.6, "split=0 lowers the tone-to-noise ratio (atonal)");
      CHECK(toneT > toneA * 2.0, "the tone is far stronger in the tonal split"); }

    /* complementary masks: tonal + atonal ~= balanced (bypass) */
    { std::vector<float> sig(N);
      for (int i = 0; i < N; i++)
          sig[i] = 0.3f * sinf(2 * M_PI * 300.0f * i / rate) + 0.3f * noise();
      std::vector<float> tonal  = run(sig, 1.0f, 1.0f, 1.0f);
      std::vector<float> atonal = run(sig, 0.0f, 1.0f, 1.0f);
      std::vector<float> bal    = run(sig, 0.5f, 1.0f, 1.0f);
      double err = 0, ref = 0;
      for (int i = A; i < B; i++) {
          double s = tonal[i] + atonal[i];
          err += (s - bal[i]) * (s - bal[i]);
          ref += bal[i] * bal[i];
      }
      printf("      tonal+atonal vs balanced: rel err = %.4f\n", err / (ref + 1e-12));
      CHECK(err < ref * 0.02, "tonal + atonal reconstructs the balanced output"); }

    /* dry/wet stays finite and Output gain scales */
    { std::vector<float> sig(N);
      for (int i = 0; i < N; i++) sig[i] = 0.3f * noise();
      std::vector<float> a = run(sig, 0.5f, 1.0f, 1.0f);
      std::vector<float> b = run(sig, 0.5f, 1.0f, 2.0f);
      double ea = energy(a, A, B), eb = energy(b, A, B);
      CHECK(eb > ea * 3.0, "Output gain 2x quadruples energy"); }

    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL SPLIT CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
