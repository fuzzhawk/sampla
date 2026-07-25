/*
 * slicer_test.cpp — headless test for the Match-Slicer engine.
 *
 * Builds a synthetic guide (alternating low/high timbre slices) and a main file
 * with a distinct low region followed by a high region, then verifies the
 * engine slices the guide, fingerprints the main file, and matches each guide
 * slice to the correct region of the main file — the core "search main for the
 * best match to each guide slice" behavior. Also checks render + transport read
 * + a WAV load round-trip. Exit 0 = pass.
 */
#include "../src/slicer.h"
#include <cstdio>
#include <cmath>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } \
    else printf("ok:   %s\n", m); } while (0)

/* write a stereo tone region [start,end) at frequency hz into an interleaved buf */
static void tone(std::vector<float>& d, int start, int end, float hz, int rate)
{
    for (int i = start; i < end; i++) {
        float s = 0;
        for (int h = 1; h <= 4; h++) s += sinf(2 * 3.14159265f * hz * h * i / rate) / h;
        s *= 0.5f;
        d[(size_t)i * 2] = s; d[(size_t)i * 2 + 1] = s;
    }
}

int main()
{
    int rate = 44100;

    /* main: 1 s low (220 Hz) then 1 s high (3000 Hz) */
    Slicer sl;
    sl.setSampleRate(rate);
    sl.main.frames = rate * 2;
    sl.main.rate = rate;
    sl.main.data.assign((size_t)sl.main.frames * 2, 0.0f);
    tone(sl.main.data, 0, rate, 220.0f, rate);
    tone(sl.main.data, rate, rate * 2, 3000.0f, rate);
    sl.main.mono.resize((size_t)sl.main.frames);
    for (int i = 0; i < sl.main.frames; i++)
        sl.main.mono[i] = 0.5f * (sl.main.data[(size_t)i*2] + sl.main.data[(size_t)i*2+1]);

    /* guide: 1 s = low, high, low, high (4 quarter slices) */
    sl.guide.frames = rate;
    sl.guide.rate = rate;
    sl.guide.data.assign((size_t)sl.guide.frames * 2, 0.0f);
    int q = rate / 4;
    tone(sl.guide.data, 0*q, 1*q, 220.0f, rate);
    tone(sl.guide.data, 1*q, 2*q, 3000.0f, rate);
    tone(sl.guide.data, 2*q, 3*q, 220.0f, rate);
    tone(sl.guide.data, 3*q, 4*q, 3000.0f, rate);
    sl.guide.mono.resize((size_t)sl.guide.frames);
    for (int i = 0; i < sl.guide.frames; i++)
        sl.guide.mono[i] = 0.5f * (sl.guide.data[(size_t)i*2] + sl.guide.data[(size_t)i*2+1]);

    CHECK(sl.haveBoth(), "both files present");

    /* grid slicing: 1 bar, div 1/4 -> 4 slices */
    sl.par.sliceMode = SLICE_GRID; sl.par.bars = 1; sl.par.div = 0;
    sl.sliceGuide();
    CHECK(sl.slices.size() == 4, "guide sliced into 4 quarter-note slices");

    sl.buildCandidates();
    CHECK(!sl.cands.empty(), "main-file candidates built");

    sl.doMatch();
    CHECK(sl.match.size() == 4, "one match per guide slice");

    /* slices 0,2 are low -> should match the low half of main;
       slices 1,3 are high -> the high half */
    int win = 0; { int a = 0; for (auto& s : sl.slices) a += s.len; win = a / (int)sl.slices.size(); }
    auto center = [&](int slot){ return sl.cands[sl.match[slot]].start + win / 2; };
    bool lowOK  = center(0) < sl.main.frames / 2 && center(2) < sl.main.frames / 2;
    bool highOK = center(1) >= sl.main.frames / 2 && center(3) >= sl.main.frames / 2;
    printf("      matches: s0@%d s1@%d s2@%d s3@%d (mid=%d)\n",
           center(0), center(1), center(2), center(3), sl.main.frames / 2);
    CHECK(lowOK, "low guide slices matched the LOW region of main");
    CHECK(highOK, "high guide slices matched the HIGH region of main");

    /* render */
    AudioBuf* r = sl.render();
    CHECK(r != nullptr, "render produced a pattern");
    if (r) {
        bool fin = true; double e = 0;
        for (float v : r->data) { if (!std::isfinite(v)) fin = false; e += fabs(v); }
        CHECK(fin && e > 1.0, "rendered pattern is finite and non-silent");
        CHECK(abs(r->frames - sl.guide.frames) < win, "pattern length ~ guide length");
        delete r;
    }

    /* full rematch + transport-synced playback */
    CHECK(sl.rematch(), "rematch (slice+match+render+publish) succeeds");
    { std::vector<float> L(512), R(512); float* out[2] = { L.data(), R.data() };
      sl.hostTempo = 120; sl.hostPpqValid = true; sl.hostPpq = 0;
      double sum = 0; bool fin = true;
      for (int b = 0; b < 40; b++) {
          std::fill(L.begin(), L.end(), 0.0f); std::fill(R.begin(), R.end(), 0.0f);
          sl.process(out, 512, true, 1.0f, 0.9f);
          for (int i = 0; i < 512; i++) { if (!std::isfinite(L[i])) fin = false; sum += fabs(L[i]); }
      }
      CHECK(fin && sum > 1.0, "transport-synced playback produces audio"); }

    /* transient mode still yields slices */
    sl.par.sliceMode = SLICE_TRANSIENT;
    sl.sliceGuide();
    CHECK(sl.slices.size() >= 2, "transient slicing yields onsets");

    /* WAV round-trip through the loader */
    { std::vector<float> lr((size_t)rate * 2);
      tone(lr, 0, rate, 440.0f, rate);
      wav_write16("/tmp/sl_guide.wav", lr.data(), rate, rate);
      AudioBuf b;
      CHECK(sl_load("/tmp/sl_guide.wav", b) && b.frames == rate && !b.mono.empty(),
            "sl_load reads a WAV into an AudioBuf"); }

    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL SLICER CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
