/*
 * librarian_test.cpp — headless test for the Sample Librarian engine.
 *
 * Generates a miniature library (pitched tones in different spectral bands,
 * noise, a too-short file, a too-long file), scans it, and verifies:
 * filters, pitch detection, constellation, combo generation (tonal match +
 * spectral complement), tune-to-key, playback voice, render + WAV export,
 * and the index cache. Exit 0 = pass.
 */
#include "../src/librarian.h"
#include <cstdio>
#include <cmath>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL: %s\n", msg); failures++; } else printf("ok:   %s\n", msg); } while (0)

/* write a stereo 16-bit wav of summed sines (freqs list) or noise */
static void writeTone(const char* path, int rate, float secs,
                      const float* freqs, int nf, float gain, bool noise)
{
    int frames = (int)(rate * secs);
    std::vector<float> lr((size_t)frames * 2);
    uint32_t rng = 12345;
    for (int i = 0; i < frames; i++) {
        float s = 0;
        if (noise) {
            rng = rng * 1664525u + 1013904223u;
            s = (((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f) * 0.5f;
        } else {
            for (int f = 0; f < nf; f++)
                s += sinf(2 * 3.14159265f * freqs[f] * i / rate) / nf;
        }
        s *= gain;
        lr[(size_t)i * 2] = s; lr[(size_t)i * 2 + 1] = s;
    }
    wav_write16(path, lr.data(), frames, rate);
}

int main()
{
    const char* root = "/tmp/sl_test_lib";
    mkdir(root, 0755);
    mkdir((std::string(root) + "/sub").c_str(), 0755);

    /* A-rooted tones in three different spectral bands (tonal match,
     * spectral complement), plus assorted others */
    float lowA[]  = { 110.0f, 220.0f };            /* A2+A3, low band   */
    float midA[]  = { 880.0f, 1760.0f };           /* A5+A6, mid band   */
    float hiA[]   = { 3520.0f, 7040.0f };          /* A7+A8, high band  */
    float lowC[]  = { 130.8f, 261.6f };            /* C, low band       */
    float a220[]  = { 220.0f };
    writeTone("/tmp/sl_test_lib/lowA.wav",  44100, 1.2f, lowA, 2, 0.8f, false);
    writeTone("/tmp/sl_test_lib/midA.wav",  44100, 1.0f, midA, 2, 0.8f, false);
    writeTone("/tmp/sl_test_lib/sub/hiA.wav", 44100, 1.4f, hiA, 2, 0.8f, false);
    writeTone("/tmp/sl_test_lib/lowC.wav",  44100, 1.1f, lowC, 2, 0.8f, false);
    writeTone("/tmp/sl_test_lib/a220.wav",  44100, 1.0f, a220, 1, 0.8f, false);
    writeTone("/tmp/sl_test_lib/noise.wav", 44100, 1.0f, nullptr, 0, 0.8f, true);
    writeTone("/tmp/sl_test_lib/tiny.wav",  44100, 0.05f, a220, 1, 0.8f, false);  /* < minLen */
    writeTone("/tmp/sl_test_lib/long.wav",  44100, 26.0f, a220, 1, 0.4f, false);  /* > maxLen */
    remove(lib_cachePath(root).c_str());

    LibSettings st;
    st.minLenSec = 0.2f; st.maxLenSec = 20.0f; st.maxMB = 64.0f; st.tuneKey = true;

    /* --- scan + filters --- */
    LibIndex ix;
    ScanProgress prog;
    bool ok = lib_scan(root, st, ix, &prog);
    CHECK(ok, "scan succeeds on the mini library");
    CHECK(prog.found.load() == 8, "walk finds all 8 wavs (incl. subdir)");
    CHECK(ix.files.size() == 6, "min/max length filters drop tiny + long");

    /* --- analysis --- */
    int iA220 = -1, iLow = -1, iMid = -1, iHi = -1, iNoise = -1;
    for (size_t i = 0; i < ix.files.size(); i++) {
        const std::string& p = ix.files[i].path;
        if (p.find("a220") != std::string::npos)  iA220 = (int)i;
        if (p.find("lowA") != std::string::npos)  iLow = (int)i;
        if (p.find("midA") != std::string::npos)  iMid = (int)i;
        if (p.find("hiA") != std::string::npos)   iHi = (int)i;
        if (p.find("noise") != std::string::npos) iNoise = (int)i;
    }
    CHECK(iA220 >= 0 && iLow >= 0 && iMid >= 0 && iHi >= 0 && iNoise >= 0,
          "all expected files are indexed");
    CHECK(fabsf(ix.files[iA220].pitchHz - 220.0f) < 6.0f,
          "pitch detection: 220 Hz tone found within 6 Hz");
    CHECK(ix.files[iA220].pitchConf > 0.5f, "pitch confidence high for a sine");
    CHECK(ix.files[iNoise].pitchConf < 0.5f ||
          ix.files[iNoise].pitchHz < 45.0f,
          "noise is low-confidence for pitch");

    /* tonal match across bands: lowA vs midA chroma should agree (both A) */
    int rot = 0;
    float simAA = lib_bestChroma(ix.files[iLow], ix.files[iMid], false, &rot);
    float bandAA = lib_bandCos(ix.files[iLow], ix.files[iMid]);
    CHECK(simAA > 0.8f, "chroma: A-tones in different bands still match");
    CHECK(bandAA < 0.5f, "bands: low-A and mid-A occupy different spectrum");

    /* --- constellation --- */
    { bool inRange = true; float spread = 0;
      for (const FileFeat& f : ix.files) {
          if (!(f.cx >= 0 && f.cx <= 1 && f.cy >= 0 && f.cy <= 1)) inRange = false;
      }
      for (const FileFeat& f : ix.files)
          for (const FileFeat& g : ix.files)
              spread = fmaxf(spread, fabsf(f.cx - g.cx) + fabsf(f.cy - g.cy));
      CHECK(inRange, "constellation coords are in [0,1]");
      CHECK(spread > 0.3f, "constellation actually spreads the library");
      CHECK(!ix.drawList.empty(), "draw list populated"); }

    /* --- combos --- */
    { Combo combos[12];
      lib_makePalette(ix, st, combos, 0xBEEF);
      int made = 0, layersOk = 1, scored = 0;
      for (int i = 0; i < 12; i++) {
          if (combos[i].nLayers == 0) continue;
          made++;
          if (combos[i].nLayers < 2 || combos[i].nLayers > 4) layersOk = 0;
          if (combos[i].score > 0.1f) scored++;
      }
      CHECK(made >= 6, "palette: most of the 12 slots filled");
      CHECK(layersOk, "combos always have 2-4 layers");
      CHECK(scored > 0, "combos carry a match score");

      /* --- playback voice --- */
      LibEngine eng;
      eng.setSampleRate(44100);
      int c0 = -1;
      for (int i = 0; i < 12; i++) if (combos[i].nLayers) { c0 = i; break; }
      CHECK(c0 >= 0, "a playable combo exists");
      for (int s = 0; s < 4 && c0 >= 0; s++) {
          LoadedBuf* b = nullptr;
          if (s < combos[c0].nLayers)
              b = lib_loadLayer(combos[c0].lay[s].path, combos[c0].lay[s].semis);
          eng.slots[s].publish(b);
      }
      LibVoiceParams vp;
      eng.noteOn(60);
      const int B = 512;
      std::vector<float> L(B), R(B);
      float* out[2] = { L.data(), R.data() };
      double sum = 0; bool fin = true; long cnt = 0;
      for (int blk = 0; blk < 60; blk++) {
          std::fill(L.begin(), L.end(), 0.0f);
          std::fill(R.begin(), R.end(), 0.0f);
          eng.process(out, B, vp);
          for (int i = 0; i < B; i++) {
              if (!std::isfinite(L[i])) fin = false;
              sum += (double)L[i] * L[i]; cnt++;
          }
      }
      CHECK(fin, "voice: combo playback is finite");
      CHECK(sqrt(sum / cnt) > 0.005, "voice: combo playback is audible");

      /* --- render + export --- */
      LoadedBuf* bufs[4] = { nullptr, nullptr, nullptr, nullptr };
      for (int s = 0; s < combos[c0].nLayers; s++)
          bufs[s] = lib_loadLayer(combos[c0].lay[s].path, combos[c0].lay[s].semis);
      std::vector<float> mix;
      lib_renderCombo(bufs, vp, 44100, mix);
      for (int s = 0; s < 4; s++) delete bufs[s];
      CHECK(mix.size() > 8192, "render: combo renders a real buffer");
      bool finR = true; double e = 0;
      for (float v : mix) { if (!std::isfinite(v)) finR = false; e += fabs(v); }
      CHECK(finR && e > 1.0, "render: output finite and non-silent");
      CHECK(wav_write16("/tmp/sl_export.wav", mix.data(),
                        (int)(mix.size() / 2), 44100),
            "export: wav written");
      WavData back = wav_load("/tmp/sl_export.wav");
      CHECK(back.ok && back.frames == (int)(mix.size() / 2),
            "export: written wav loads back with matching length"); }

    /* --- cache --- */
    { struct stat sb;
      CHECK(stat(lib_cachePath(root).c_str(), &sb) == 0, "index cache written");
      LibIndex ix2;
      ScanProgress prog2;
      bool ok2 = lib_scan(root, st, ix2, &prog2);
      CHECK(ok2 && ix2.files.size() == ix.files.size(),
            "rescan via cache keeps the same index"); }

    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL LIBRARIAN CHECKS PASSED\n",
           failures);
    return failures ? 1 : 0;
}
