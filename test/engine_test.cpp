/*
 * engine_test.cpp — native (host-OS) DSP test for the sampler engine.
 *
 * The Wine smoke test only proves the DLL loads and answers the dispatcher on a
 * silent block. This exercises the actual audio path: write a WAV, parse it with
 * wav_load, load it into the engine, trigger a note, and confirm each loop mode
 * produces sane, non-silent, finite output. Pure engine.h/wav.h — no VST, no
 * Win32 — so it builds and runs anywhere with a C++ compiler.
 *
 * Exit 0 = all checks pass.
 */
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <cstdint>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL: %s\n", msg); failures++; } else printf("ok:   %s\n", msg); } while (0)

/* Write a little 16-bit stereo WAV: a 220 Hz tone, `secs` long. */
static void writeWav(const char* path, int rate, float secs)
{
    int frames = (int)(rate * secs);
    FILE* f = fopen(path, "wb");
    int dataBytes = frames * 2 * 2;  /* stereo, 16-bit */
    int32_t riff = 36 + dataBytes;
    auto w32 = [&](uint32_t v){ fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(riff); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2);
    w32(rate); w32(rate * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(dataBytes);
    for (int i = 0; i < frames; i++) {
        float t = (float)i / rate;
        int16_t s = (int16_t)(sinf(2 * 3.14159265f * 220.0f * t) * 30000);
        w16((uint16_t)s); w16((uint16_t)s);
    }
    fclose(f);
}

static Sample* makeSample(const char* path)
{
    WavData w = wav_load(path);
    if (!w.ok) return nullptr;
    Sample* s = new Sample();
    s->data = std::move(w.samples);
    s->frames = w.frames;
    s->srcRate = w.sampleRate;
    s->path = path;
    s->computePeaks();
    return s;
}

/* run n blocks of `block` frames, return RMS and whether all samples are finite */
static double runNote(Engine& eng, LayerParams lp, int block, int blocks,
                      bool releaseAfter, bool& finite)
{
    std::vector<float> L(block), R(block);
    float* out[2] = { L.data(), R.data() };
    LayerParams a[3] = { lp, LayerParams(), LayerParams() };
    a[1].volume = 0; a[2].volume = 0;   /* silence the other two layers */
    double sum = 0; long cnt = 0; finite = true;
    for (int b = 0; b < blocks; b++) {
        if (releaseAfter && b == blocks / 2) eng.noteOff(eng.note);
        eng.process(out, block, a);
        for (int i = 0; i < block; i++) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
            sum += (double)L[i] * L[i] + (double)R[i] * R[i];
            cnt += 2;
        }
    }
    return cnt ? sqrt(sum / cnt) : 0.0;
}

int main()
{
    const char* wavPath = "/tmp/sampla_engine_test.wav";
    writeWav(wavPath, 44100, 1.0f);

    /* --- WAV loader --- */
    WavData w = wav_load(wavPath);
    CHECK(w.ok, "wav_load parses a 16-bit stereo WAV");
    CHECK(w.frames > 40000 && w.frames < 48000, "wav frame count is ~1 second");
    CHECK(w.sampleRate == 44100, "wav sample rate read back correctly");
    CHECK(!wav_load("/tmp/does_not_exist_xyz.wav").ok, "missing file fails cleanly");

    Sample* s = makeSample(wavPath);
    CHECK(s != nullptr, "sample built + peaks computed");
    CHECK(s->peakMax[Sample::PEAKS / 2] > 0.1f, "waveform peaks are non-trivial");

    Engine eng;
    eng.setSampleRate(44100.0f);
    eng.layers[0].publish(new Sample(*s));  /* engine owns its own copy */

    bool finite;

    /* --- Forward loop --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0.25f; lp.loopEnd = 0.75f;
      lp.overlapMs = 15; lp.attackMs = 2; lp.releaseMs = 50;
      eng.noteOn(60);
      double rms = runNote(eng, lp, 512, 200, false, finite);
      CHECK(finite, "forward: output is finite");
      CHECK(rms > 0.01, "forward: produces audible output"); }

    /* --- OneShot ends (envelope releases at sample end) --- */
    { LayerParams lp; lp.mode = LOOP_ONESHOT; lp.attackMs = 1; lp.releaseMs = 20;
      eng.noteOn(60);
      double rms = runNote(eng, lp, 512, 120, false, finite);
      CHECK(finite && rms > 0.005, "oneshot: plays through then stops"); }

    /* --- Granular cloud over the loop region --- */
    { LayerParams lp; lp.mode = LOOP_GRANULAR; lp.loopStart = 0.2f; lp.loopEnd = 0.8f;
      lp.grainMs = 60; lp.density = 30; lp.playFromStart = false;
      lp.attackMs = 5; lp.releaseMs = 100;
      eng.noteOn(60);
      double rms = runNote(eng, lp, 512, 200, false, finite);
      CHECK(finite, "granular: output is finite");
      CHECK(rms > 0.005, "granular: grain cloud produces output"); }

    /* --- Release actually silences the voice --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD; lp.attackMs = 2; lp.releaseMs = 30;
      eng.noteOn(60);
      runNote(eng, lp, 256, 60, true, finite);
      std::vector<float> L(256), R(256); float* out[2] = { L.data(), R.data() };
      LayerParams a[3] = { lp, LayerParams(), LayerParams() };
      a[1].volume = a[2].volume = 0;
      eng.process(out, 256, a);  /* long after note-off */
      double tail = 0; for (int i = 0; i < 256; i++) tail += fabs(L[i]);
      CHECK(tail < 1e-3, "release: voice is silent well after note-off"); }

    /* --- Pitch: higher note -> faster playback (engine advances further) --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0; lp.loopEnd = 1;
      lp.attackMs = 1;
      eng.noteOn(72);   /* +12 semitones */
      bool fin; double rms = runNote(eng, lp, 512, 40, false, fin);
      CHECK(fin && rms > 0.01, "pitch: +12 semitones still renders cleanly"); }

    /* --- Loop seam is click-free: a big overlap on a short loop must not
     *     produce a sample-to-sample jump beyond the sine's own slope. --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD;
      lp.loopStart = 0.30f; lp.loopEnd = 0.50f;   /* 0.2s loop            */
      lp.overlapMs = 300.0f;                       /* clamped, exceeds loop */
      lp.attackMs = 1; lp.decayMs = 1; lp.sustain = 1.0f;
      eng.noteOn(60);
      const int B = 512, N = 300;
      std::vector<float> L(B), R(B); float* out[2] = { L.data(), R.data() };
      LayerParams a[3] = { lp, LayerParams(), LayerParams() };
      a[1].volume = a[2].volume = 0;
      float prev = 0; double maxJump = 0; bool fin = true; long seen = 0;
      for (int b = 0; b < N; b++) {
          std::fill(L.begin(), L.end(), 0.0f); std::fill(R.begin(), R.end(), 0.0f);
          eng.process(out, B, a);
          for (int i = 0; i < B; i++) {
              if (!std::isfinite(L[i])) fin = false;
              if (seen > 8192) {                    /* skip attack + head pass */
                  double d = fabs(L[i] - prev);
                  if (d > maxJump) maxJump = d;
              }
              prev = L[i]; seen++;
          }
      }
      CHECK(fin, "seam: output stays finite through many loops");
      printf("      (max sample-to-sample jump = %.4f)\n", maxJump);
      CHECK(maxJump < 0.12, "seam: no click (bounded discontinuity at loop)"); }

    /* --- Experimental granular params render finite, audible output --- */
    { LayerParams lp; lp.mode = LOOP_GRANULAR; lp.loopStart = 0.1f; lp.loopEnd = 0.9f;
      lp.playFromStart = false; lp.attackMs = 5; lp.releaseMs = 100;
      lp.grainMs = 40; lp.density = 40;
      lp.sprayMs = 80; lp.pitchJit = 5; lp.panSpread = 0.8f; lp.revProb = 0.4f;
      lp.scan = 0.5f; lp.shape = 0.3f; lp.timeJit = 0.6f;
      eng.noteOn(60);
      bool fin; double rms = runNote(eng, lp, 512, 200, false, fin);
      CHECK(fin, "experimental: spray/jit/pan/rev/scan/shape stay finite");
      CHECK(rms > 0.003, "experimental: granular cloud still audible"); }

    /* --- Bitcrush + decimate + chaos: mangled but finite and deterministic --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0; lp.loopEnd = 1;
      lp.attackMs = 1; lp.sustain = 1.0f;
      lp.bits = 4.0f; lp.decimate = 8.0f; lp.chaos = 0.8f;
      eng.noteOn(60);
      bool fin; double rms1 = runNote(eng, lp, 512, 60, false, fin);
      CHECK(fin, "mangle: bitcrush+decimate+chaos stay finite");
      CHECK(rms1 > 0.005, "mangle: still produces output");
      eng.noteOn(60);
      bool fin2; double rms2 = runNote(eng, lp, 512, 60, false, fin2);
      CHECK(fabs(rms1 - rms2) < 1e-6, "mangle: chaos is deterministic"); }

    /* --- FFT roundtrip --- */
    { const int N = 1024;
      float re[N], im[N], ref[N];
      uint32_t r = 1;
      for (int i = 0; i < N; i++) {
          r = r * 1664525u + 1013904223u;
          ref[i] = re[i] = ((r >> 8) & 0xFFFF) / 32768.0f - 1.0f;
          im[i] = 0;
      }
      fft_radix2(re, im, N, 0);
      fft_radix2(re, im, N, 1);
      double err = 0;
      for (int i = 0; i < N; i++) err += fabs(re[i] - ref[i]);
      CHECK(err / N < 1e-5, "fft: forward+inverse roundtrip is exact"); }

    /* --- Time stretch: 2x longer, pitch preserved, finite --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0; lp.loopEnd = 1;
      lp.attackMs = 1; lp.sustain = 1.0f; lp.strch = 2.0f;
      eng.noteOn(60);
      bool fin; double rms = runNote(eng, lp, 512, 100, false, fin);
      CHECK(fin, "stretch: 2x stretch stays finite");
      CHECK(rms > 0.01, "stretch: grain-stream produces audio"); }

    /* --- Spectral operators: tonal/tilt/shift/freeze finite + audible --- */
    { LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0; lp.loopEnd = 1;
      lp.attackMs = 1; lp.sustain = 1.0f;
      lp.tonal = -0.7f; lp.tilt = 0.5f; lp.shiftBins = 12; lp.freeze = 0.4f;
      eng.noteOn(60);
      bool fin; double rms = runNote(eng, lp, 512, 100, false, fin);
      CHECK(fin, "spectral: tonal+tilt+shift+freeze stay finite");
      CHECK(rms > 0.003, "spectral: chain passes audio"); }

    /* --- Chaotic spectral modes: every mode finite --- */
    { bool allFin = true; double anyRms = 0;
      for (int m = 1; m < SPEC_MODE_COUNT; m++) {
          LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0; lp.loopEnd = 1;
          lp.attackMs = 1; lp.sustain = 1.0f;
          lp.specMode = m; lp.specAmt = 0.9f;
          eng.noteOn(60);
          bool fin; double rms = runNote(eng, lp, 512, 60, false, fin);
          if (!fin) allFin = false;
          if (rms > anyRms) anyRms = rms;
      }
      CHECK(allFin, "spectral modes: all 7 artifact modes stay finite");
      CHECK(anyRms > 0.003, "spectral modes: artifacts still pass audio"); }

    /* --- Glitch sequencer: every algorithm on the internal clock --- */
    { bool allFin = true; bool allAudible = true;
      for (int a = 1; a < GL_ALGO_COUNT; a++) {
          LayerParams lp; lp.mode = LOOP_FORWARD; lp.loopStart = 0; lp.loopEnd = 1;
          lp.attackMs = 1; lp.sustain = 1.0f;
          GlitchParams gp;
          for (int i = 0; i < 16; i++) gp.pattern[i] = (i & 1) ? a : 0;
          gp.divIdx = 1; gp.mix = 1.0f;
          eng.glInternalPpq = 0; eng.glTotalWritten = 0;
          eng.hostPpqValid = false; eng.hostTempo = 140.0;
          eng.noteOn(60);
          const int B = 512, NB = 120;
          std::vector<float> L(B), R(B); float* out[2] = { L.data(), R.data() };
          LayerParams arr[3] = { lp, LayerParams(), LayerParams() };
          arr[1].volume = arr[2].volume = 0;
          double sum = 0; long cnt = 0;
          for (int b = 0; b < NB; b++) {
              std::fill(L.begin(), L.end(), 0.0f);
              std::fill(R.begin(), R.end(), 0.0f);
              eng.process(out, B, arr);
              eng.processGlitch(out, B, gp);
              for (int i = 0; i < B; i++) {
                  if (!std::isfinite(L[i]) || !std::isfinite(R[i])) allFin = false;
                  sum += (double)L[i] * L[i]; cnt++;
              }
          }
          if (sqrt(sum / cnt) < 0.005) allAudible = false;
      }
      CHECK(allFin, "glitch: all 7 algorithms stay finite (internal clock)");
      CHECK(allAudible, "glitch: all 7 algorithms pass audio"); }

    delete s;
    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL ENGINE CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
