/*
 * Sample Librarian — VST 2.4 instrument that scans an audio library, finds
 * WAVs that match in tonal content but occupy different parts of the
 * spectrum, and layers 2-4 of them into playable, exportable combos.
 *
 * The analysis/matching/playback engine lives in librarian.h (headless,
 * exercised by CI); the Win32 editor in editor.h. This file is the VST glue:
 * params, MIDI, chunk state, the background scan thread, and the dispatcher.
 *
 * Original library files are never modified; export writes new WAVs.
 */

#include "vst2.h"
#include "librarian.h"
#include "neural.h"
#include "nnsynth.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>

/* directory the plugin DLL lives in; set by DllMain (editor.h) on Windows,
 * set explicitly by the headless tests */
static std::string g_moduleDir;

#define PLUGIN_NAME    "Sample Librarian"
#define PLUGIN_VENDOR  "Morningcloak"
#define PLUGIN_VERSION 1000
#define PLUGIN_UNIQUE_ID 0x4D43734C    /* 'MCsL' */

/* ------------------------------------------------------------ params */

enum {
    pMaster = 0, pAttack, pRelease, pTuneKey, pMinLen, pMaxLen, pMaxMB,
    PARAM_LAYER0                        /* 4 layers x (Vol, Pan, Tune) */
};
static const int NSLOTS = LibEngine::NSLOTS;
static const int PPLAY = 3;
static const int LAYER_PARAMS_END = PARAM_LAYER0 + NSLOTS * PPLAY;   /* 19 */
/* neural sampler controls */
static const int pNLen    = LAYER_PARAMS_END + 0;
static const int pNChaos  = LAYER_PARAMS_END + 1;
static const int pNMorph  = LAYER_PARAMS_END + 2;
static const int pNSpread = LAYER_PARAMS_END + 3;
static const int NUM_PARAMS = LAYER_PARAMS_END + 4;                  /* 23 */

static float paramReal(int idx, float n)
{
    switch (idx) {
    case pMaster:  return n;
    case pAttack:  return 1.0f + n * n * 1999.0f;       /* 1..2000 ms   */
    case pRelease: return 5.0f + n * n * 3995.0f;       /* 5..4000 ms   */
    case pTuneKey: return n >= 0.5f ? 1.0f : 0.0f;
    case pMinLen:  return 0.05f + n * n * 9.95f;        /* 0.05..10 s   */
    case pMaxLen:  return 1.0f + n * n * 119.0f;        /* 1..120 s     */
    case pMaxMB:   return 1.0f + n * n * 199.0f;        /* 1..200 MB    */
    }
    if (idx == pNLen)    return 0.3f + n * n * 5.7f;    /* 0.3..6 s     */
    if (idx == pNChaos)  return n;                      /* 0..1         */
    if (idx == pNMorph)  return n;                      /* 0..1         */
    if (idx == pNSpread) return n * 12.0f;              /* 0..12 st     */
    int off = (idx - PARAM_LAYER0) % PPLAY;
    if (off == 0) return n;                             /* vol          */
    if (off == 1) return n;                             /* pan          */
    return (n - 0.5f) * 24.0f;                          /* tune +-12 st */
}

struct PMeta { const char* name; const char* label; float def; };
static const PMeta kParamMeta[PARAM_LAYER0] = {
    { "Master", "%",  0.80f },
    { "Atk",    "ms", 0.05f },
    { "Rel",    "ms", 0.25f },
    { "TuneKey","",   1.00f },
    { "MinLen", "s",  0.14f },   /* ~0.25 s */
    { "MaxLen", "s",  0.40f },   /* ~20 s   */
    { "MaxMB",  "MB", 0.56f },   /* ~64 MB  */
};
static const char* kLayNames[PPLAY] = { "Vol", "Pan", "Tune" };

/* ---------------------------------------------------------------- state */

struct Plugin {
    AEffect* effect = nullptr;
    audioMasterCallback host = nullptr;
    float params[NUM_PARAMS];
    LibEngine engine;

    std::string libPath;
    std::atomic<LibIndex*> indexLive{nullptr};
    LibIndex* indexRetire = nullptr;
    ScanProgress prog;

    Combo combos[12];
    int   selected = -1;
    uint32_t rng = 0xA5A5A5u;

    /* lasso selection + synthesized one-shot (GUI thread only) */
    std::vector<int> lassoSel;             /* file indices               */
    std::vector<float> synthBuf;           /* last synthesized one-shot  */
    uint32_t synthSeed = 0x5EED0001u;

    /* neural sampler (GUI thread only). Two backends: the RTNeural-format
     * spectral VAE (nnvae.json, header-only, primary) and the ONNX/RAVE path
     * (dormant fallback). VAE wins when its model is present. */
    VaeSynth vae;
    bool vaeTried = false;
    NeuralEngine neural;
    bool neuralTried = false;
    std::string neuralDir;                 /* override for tests; else DLL dir */
    std::vector<float> neuralBuf;          /* last neural one-shot, stereo     */
    int neuralRate = 44100;
    uint32_t neuralSeed = 0x0DDB1A5Eu;

    /* scrolling message console (GUI thread only) */
    static const int LOG_LINES = 64;
    std::string logLines[LOG_LINES];
    int logHead = 0, logCount = 0;

    void*  editor = nullptr;
    std::string chunk;

    void logf(const char* fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        logLines[logHead] = buf;
        logHead = (logHead + 1) % LOG_LINES;
        if (logCount < LOG_LINES) logCount++;
    }
    const std::string& logLine(int back) const   /* 0 = newest */
    {
        int idx = (logHead - 1 - back + 2 * LOG_LINES) % LOG_LINES;
        return logLines[idx];
    }

    ~Plugin()
    {
        delete indexLive.exchange(nullptr);
        delete indexRetire;
    }

    LibSettings settings() const
    {
        LibSettings st;
        st.minLenSec = paramReal(pMinLen, params[pMinLen]);
        st.maxLenSec = paramReal(pMaxLen, params[pMaxLen]);
        st.maxMB     = paramReal(pMaxMB,  params[pMaxMB]);
        st.tuneKey   = paramReal(pTuneKey, params[pTuneKey]) >= 0.5f;
        return st;
    }

    LibVoiceParams voiceParams() const
    {
        LibVoiceParams vp;
        vp.master    = paramReal(pMaster,  params[pMaster]);
        vp.attackMs  = paramReal(pAttack,  params[pAttack]);
        vp.releaseMs = paramReal(pRelease, params[pRelease]);
        for (int i = 0; i < NSLOTS; i++) {
            int b = PARAM_LAYER0 + i * PPLAY;
            vp.lay[i].vol   = paramReal(b + 0, params[b + 0]);
            vp.lay[i].pan   = paramReal(b + 1, params[b + 1]);
            vp.lay[i].semis = paramReal(b + 2, params[b + 2]);
        }
        return vp;
    }

    void setParamFromUI(int idx, float v)
    {
        if (idx < 0 || idx >= NUM_PARAMS) return;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        params[idx] = v;
        if (host) host(effect, audioMasterAutomate, idx, 0, 0, v);
    }

    /* blocking scan (worker thread on Windows; direct in tests) */
    void runScan()
    {
        if (libPath.empty()) return;
        LibIndex* ix = new LibIndex();
        if (lib_scan(libPath, settings(), *ix, &prog)) {
            delete indexRetire;
            indexRetire = indexLive.exchange(ix);
        } else {
            delete ix;
        }
    }

    void randomize()
    {
        LibIndex* ix = indexLive.load();
        if (!ix || ix->files.empty()) { logf("randomize: scan a library first"); return; }
        rng = rng * 1664525u + 1013904223u;
        lib_makePalette(*ix, settings(), combos, rng);
        selected = -1;
        int made = 0;
        for (int i = 0; i < 12; i++) if (combos[i].nLayers) made++;
        logf("randomize: %d combos dealt from %d files", made,
             (int)ix->files.size());
    }

    /* load a combo's audio into the 4 slots and make it the live one.
     * Unused slots get an EMPTY buffer, not nullptr: publish(nullptr) is
     * indistinguishable from "no pending" in adopt(), so a previous combo's
     * layers 3/4 would keep sounding — an empty buffer actually clears. */
    void selectCombo(int i)
    {
        if (i < 0 || i >= 12 || combos[i].nLayers == 0) return;
        selected = i;
        const Combo& c = combos[i];
        for (int s = 0; s < NSLOTS; s++) {
            LoadedBuf* b = nullptr;
            if (s < c.nLayers)
                b = lib_loadLayer(c.lay[s].path, c.lay[s].semis);
            if (!b) b = new LoadedBuf();          /* empty = silent slot */
            engine.slots[s].publish(b);
        }
        logf("combo #%d: %d layers loaded", i + 1, c.nLayers);
        for (int l = 0; l < c.nLayers; l++) {
            size_t sl = c.lay[l].path.find_last_of("/\\");
            std::string nm = sl == std::string::npos ? c.lay[l].path
                                                     : c.lay[l].path.substr(sl + 1);
            logf("  L%d %s (%+.1f st)", l + 1, nm.c_str(), c.lay[l].semis);
        }
    }

    /* export the selected combo as a rendered WAV (originals untouched) */
    bool exportCombo(const char* outPath)
    {
        if (selected < 0 || combos[selected].nLayers == 0) return false;
        const Combo& c = combos[selected];
        LoadedBuf* bufs[4] = { nullptr, nullptr, nullptr, nullptr };
        for (int s = 0; s < c.nLayers; s++)
            bufs[s] = lib_loadLayer(c.lay[s].path, c.lay[s].semis, 30.0f);
        std::vector<float> mix;
        lib_renderCombo(bufs, voiceParams(), engine.sampleRate, mix);
        for (int s = 0; s < 4; s++) delete bufs[s];
        if (mix.empty()) return false;
        bool ok = wav_write16(outPath, mix.data(), (int)(mix.size() / 2),
                              (int)engine.sampleRate);
        if (ok) logf("exported combo #%d -> %s", selected + 1, outPath);
        else    logf("export FAILED: %s", outPath);
        return ok;
    }

    /* nearest indexed file to a constellation position (normalized coords) */
    int nearestFile(float cx, float cy, float maxDist)
    {
        LibIndex* ix = indexLive.load();
        if (!ix) return -1;
        float best = maxDist * maxDist;
        int bestI = -1;
        for (int di : ix->drawList) {
            const FileFeat& f = ix->files[di];
            float dx = f.cx - cx, dy = f.cy - cy;
            float d = dx * dx + dy * dy;
            if (d < best) { best = d; bestI = di; }
        }
        return bestI;
    }

    /* click-to-audition: play a snippet + log its analysis */
    void auditionFile(int fi)
    {
        LibIndex* ix = indexLive.load();
        if (!ix || fi < 0 || fi >= (int)ix->files.size()) return;
        const FileFeat& f = ix->files[fi];
        LoadedBuf* b = lib_loadLayer(f.path, 0.0f, 2.5f);
        if (!b) { logf("audition failed: %s", f.path.c_str()); return; }
        engine.aud.publish(b);
        engine.auditionStart();

        size_t sl = f.path.find_last_of("/\\");
        std::string nm = sl == std::string::npos ? f.path : f.path.substr(sl + 1);
        char note[24]; lib_noteName(f.pitchHz, note, sizeof(note));
        int peakBand = 0;
        for (int k = 1; k < NBANDS; k++)
            if (f.band[k] > f.band[peakBand]) peakBand = k;
        const char* reg = peakBand < 5 ? "low" : peakBand < 11 ? "mid" : "high";
        logf("> %s  %.1fs %dHz", nm.c_str(), f.durationSec, f.sampleRate);
        logf("  pitch %s (%.0fHz, %d%%)  spectrum %s-heavy",
             note, f.pitchHz, (int)(f.pitchConf * 100), reg);
    }

    /* lasso: normalized polygon -> file selection */
    void lassoSelect(const float* px, const float* py, int n)
    {
        lassoSel.clear();
        LibIndex* ix = indexLive.load();
        if (!ix || n < 3) return;
        for (int di : ix->drawList) {
            const FileFeat& f = ix->files[di];
            if (lib_pointInPoly(px, py, n, f.cx, f.cy))
                lassoSel.push_back(di);
        }
        logf("lasso: %d sounds selected", (int)lassoSel.size());
    }

    /* style synthesis from the lasso selection */
    bool synthesize()
    {
        LibIndex* ix = indexLive.load();
        if (!ix || lassoSel.empty()) { logf("synth: lasso a region first"); return false; }
        std::vector<std::string> paths;
        uint32_t r = synthSeed;
        std::vector<int> pool = lassoSel;
        for (int i = 0; i < 8 && !pool.empty(); i++) {
            r = r * 1664525u + 1013904223u;
            size_t pick = (r >> 8) % pool.size();
            paths.push_back(ix->files[pool[pick]].path);
            pool.erase(pool.begin() + pick);
        }
        logf("synth: style from %d of %d sounds, seed %08X",
             (int)paths.size(), (int)lassoSel.size(), synthSeed);
        std::vector<float> out;
        if (!lib_synthStyle(paths, synthSeed, engine.sampleRate, out)) {
            logf("synth: failed (unreadable style set)");
            return false;
        }
        synthBuf = out;
        synthSeed = synthSeed * 1664525u + 1013904223u;   /* next roll */

        LoadedBuf* b = new LoadedBuf();
        b->data = out;
        b->frames = (int)(out.size() / 2);
        b->rate = (int)engine.sampleRate;
        engine.aud.publish(b);
        engine.auditionStart();
        logf("synth: %.2fs one-shot rendered - playing",
             b->frames / engine.sampleRate);
        return true;
    }

    bool exportSynth(const char* outPath)
    {
        if (synthBuf.empty()) { logf("synth export: nothing synthesized yet"); return false; }
        bool ok = wav_write16(outPath, synthBuf.data(),
                              (int)(synthBuf.size() / 2), (int)engine.sampleRate);
        logf(ok ? "exported synth -> %s" : "synth export FAILED: %s", outPath);
        return ok;
    }

    /* ---- neural sampler (RAVE via ONNX Runtime) ---- */

    bool ensureNeural()
    {
        if (neural.ready) return true;
        if (neuralTried) return false;
        neuralTried = true;
        std::string dir = neuralDir.empty() ? g_moduleDir : neuralDir;
        if (neural.init(dir)) {
            logf("neural: model loaded (%d Hz, dir %s)", neural.modelRate,
                 dir.c_str());
            return true;
        }
        logf("neural: unavailable - %s", neural.err.c_str());
        logf("neural: put onnxruntime + rave_*.onnx next to the plugin");
        return false;
    }

    bool ensureVae()
    {
        if (vae.ready) return true;
        if (vaeTried) return false;
        vaeTried = true;
        std::string dir = neuralDir.empty() ? g_moduleDir : neuralDir;
        if (vae.load(dir)) {
            logf("neural: VAE model loaded (%d Hz, dir %s)", vae.sr, dir.c_str());
            return true;
        }
        logf("neural: no VAE - %s", vae.err.c_str());
        return false;
    }

    /* load a file as mono at the model's sample rate */
    static std::vector<float> loadMonoAtRate(const std::string& path, int rate,
                                             float maxSec)
    {
        std::vector<float> out;
        WavInfo info = wav_probe(path.c_str());
        if (!info.ok) return out;
        WavData w = wav_load_partial(path.c_str(),
                                     (int)(maxSec * info.sampleRate));
        if (!w.ok || w.frames < 64) return out;
        double ratio = (double)w.sampleRate / rate;
        int n = (int)(w.frames / ratio);
        out.resize((size_t)n);
        for (int i = 0; i < n; i++) {
            double sp = i * ratio;
            int i0 = (int)sp;
            if (i0 >= w.frames - 1) i0 = w.frames - 2;
            float t = (float)(sp - i0);
            const float* d = &w.samples[(size_t)i0 * 2];
            float a = 0.5f * (d[0] + d[1]);
            float b = 0.5f * (d[2] + d[3]);
            out[(size_t)i] = a + (b - a) * t;
        }
        return out;
    }

    /* publish a mono one-shot to the audition voice as stereo */
    void publishNeural(const std::vector<float>& mono, int rate)
    {
        neuralBuf.resize(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); i++) {
            neuralBuf[i * 2] = mono[i]; neuralBuf[i * 2 + 1] = mono[i];
        }
        neuralRate = rate;
        LoadedBuf* b = new LoadedBuf();
        b->data = neuralBuf;
        b->frames = (int)mono.size();
        b->rate = rate;
        engine.aud.publish(b);
        engine.auditionStart();
        logf("neural: %.2fs one-shot - playing", mono.size() / (float)rate);
    }

    /* primary path: RTNeural-format spectral VAE. Generates a one-shot in the
     * trained style; a lassoed sound (with Morph > 0) seeds the latent. */
    bool vaeGenerate()
    {
        float lenSec = paramReal(pNLen, params[pNLen]);
        float chaos  = paramReal(pNChaos, params[pNChaos]);
        float morph  = paramReal(pNMorph, params[pNMorph]);
        float spread = paramReal(pNSpread, params[pNSpread]);

        std::vector<float> style;
        const std::vector<float>* stylePtr = nullptr;
        LibIndex* ix = indexLive.load();
        if (morph > 0.001f && ix && !lassoSel.empty()) {
            uint32_t r = neuralSeed;
            r = r * 1664525u + 1013904223u;
            int fa = lassoSel[(r >> 8) % lassoSel.size()];
            style = loadMonoAtRate(ix->files[fa].path, vae.sr, 4.0f);
            if (style.size() >= 1024) stylePtr = &style;
        }

        logf("neural(VAE): %.1fs, chaos %d%%, morph %d%%, spread %.0fst, seed %08X",
             lenSec, (int)(chaos * 100), (int)(morph * 100), spread, neuralSeed);
        std::vector<float> mono;
        if (!vae.generate(lenSec, chaos, morph, spread, neuralSeed, stylePtr, mono)) {
            logf("neural(VAE): generate failed"); return false;
        }
        neuralSeed = neuralSeed * 1664525u + 1013904223u;
        publishNeural(mono, vae.sr);
        return true;
    }

    /* top-level GENERATE NN: prefer the VAE, fall back to ONNX/RAVE */
    bool neuralGenerate()
    {
        if (ensureVae())     return vaeGenerate();
        if (ensureNeural())  return onnxGenerate();
        logf("neural: install nnvae.json (train workflow) or a RAVE model");
        return false;
    }

    bool onnxGenerate()
    {
        LibIndex* ix = indexLive.load();
        if (!ix || lassoSel.empty()) { logf("neural: lasso a region first"); return false; }

        uint32_t r = neuralSeed;
        auto pick = [&]() {
            r = r * 1664525u + 1013904223u;
            return lassoSel[(r >> 8) % lassoSel.size()];
        };
        int fa = pick();
        int fb = pick();
        float morph = paramReal(pNMorph, params[pNMorph]);

        std::vector<std::vector<float>> styles;
        styles.push_back(loadMonoAtRate(ix->files[fa].path, neural.modelRate, 4.0f));
        if (styles[0].size() < 1024) { logf("neural: style file unreadable"); return false; }
        if (morph > 0.001f && fb != fa) {
            std::vector<float> b = loadMonoAtRate(ix->files[fb].path,
                                                  neural.modelRate, 4.0f);
            if (b.size() >= 1024) styles.push_back(b);
        }

        float lenSec = paramReal(pNLen, params[pNLen]);
        float chaos  = paramReal(pNChaos, params[pNChaos]);
        logf("neural: generating %.1fs, chaos %d%%, morph %d%%, seed %08X",
             lenSec, (int)(chaos * 100), (int)(morph * 100), neuralSeed);

        std::vector<float> mono;
        if (!neural.generate(styles, lenSec, chaos, morph, neuralSeed, mono)) {
            logf("neural: generate failed - %s", neural.err.c_str());
            return false;
        }

        /* pitch spread: random transpose within +-spread, by resampling */
        float spread = paramReal(pNSpread, params[pNSpread]);
        if (spread > 0.01f) {
            r = r * 1664525u + 1013904223u;
            float semis = (((r >> 8) & 0xFFFF) / 32768.0f - 1.0f) * spread;
            double f = pow(2.0, semis / 12.0);
            int n2 = (int)(mono.size() / f);
            if (n2 > 64) {
                std::vector<float> shifted((size_t)n2);
                for (int i = 0; i < n2; i++) {
                    double sp = i * f;
                    int i0 = (int)sp;
                    if (i0 >= (int)mono.size() - 1) i0 = (int)mono.size() - 2;
                    float t = (float)(sp - i0);
                    shifted[(size_t)i] = mono[(size_t)i0] +
                        (mono[(size_t)i0 + 1] - mono[(size_t)i0]) * t;
                }
                mono.swap(shifted);
                logf("neural: pitch spread applied %+.1f st", semis);
            }
        }
        neuralSeed = neuralSeed * 1664525u + 1013904223u;

        neuralBuf.resize(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); i++) {
            neuralBuf[i * 2] = mono[i];
            neuralBuf[i * 2 + 1] = mono[i];
        }
        neuralRate = neural.modelRate;

        LoadedBuf* b = new LoadedBuf();
        b->data = neuralBuf;
        b->frames = (int)mono.size();
        b->rate = neuralRate;
        engine.aud.publish(b);
        engine.auditionStart();
        logf("neural: %.2fs one-shot - playing", mono.size() / (float)neuralRate);
        return true;
    }

    bool exportNeural(const char* outPath)
    {
        if (neuralBuf.empty()) { logf("neural export: nothing generated yet"); return false; }
        bool ok = wav_write16(outPath, neuralBuf.data(),
                              (int)(neuralBuf.size() / 2), neuralRate);
        logf(ok ? "exported neural -> %s" : "neural export FAILED: %s", outPath);
        return ok;
    }
};

static Plugin* self(AEffect* e) { return (Plugin*)e->object; }

/* ------------------------------------------------------------------ audio */

static void processReplacing(AEffect* e, float** in, float** out,
                             int32_t sampleFrames)
{
    (void)in;
    Plugin* p = self(e);
    memset(out[0], 0, sizeof(float) * sampleFrames);
    memset(out[1], 0, sizeof(float) * sampleFrames);
    LibVoiceParams vp = p->voiceParams();
    p->engine.process(out, sampleFrames, vp);
}

static void processAccumulate(AEffect* e, float** in, float** out, int32_t n)
{
    processReplacing(e, in, out, n);
}

/* --------------------------------------------------------------- MIDI */

static void handleMidi(Plugin* p, const unsigned char* d)
{
    unsigned char status = d[0] & 0xF0;
    unsigned char note   = d[1] & 0x7F;
    unsigned char velo   = d[2] & 0x7F;
    if (status == 0x90 && velo > 0)                 p->engine.noteOn(note);
    else if (status == 0x80 || (status == 0x90 && velo == 0))
                                                    p->engine.noteOff(note);
}

static void processEvents(Plugin* p, VstEvents* ev)
{
    if (!ev) return;
    for (int i = 0; i < ev->numEvents; i++) {
        VstEvent* e = ev->events[i];
        if (e && e->type == kVstMidiType)
            handleMidi(p, (const unsigned char*)((VstMidiEvent*)e)->midiData);
    }
}

/* --------------------------------------------------------------- params */

static void setParameter(AEffect* e, int32_t index, float value)
{
    if (index >= 0 && index < NUM_PARAMS) self(e)->params[index] = value;
}
static float getParameter(AEffect* e, int32_t index)
{
    if (index >= 0 && index < NUM_PARAMS) return self(e)->params[index];
    return 0.0f;
}

static void copyStr(void* dst, const char* src, size_t max)
{
    if (!dst) return;
    strncpy((char*)dst, src, max - 1);
    ((char*)dst)[max - 1] = 0;
}

static void paramName(int idx, char* out)
{
    if (idx < PARAM_LAYER0) { snprintf(out, 16, "%s", kParamMeta[idx].name); return; }
    if (idx == pNLen)    { strcpy(out, "NLen");   return; }
    if (idx == pNChaos)  { strcpy(out, "NChaos"); return; }
    if (idx == pNMorph)  { strcpy(out, "NMorph"); return; }
    if (idx == pNSpread) { strcpy(out, "NSprd");  return; }
    int lay = (idx - PARAM_LAYER0) / PPLAY, off = (idx - PARAM_LAYER0) % PPLAY;
    snprintf(out, kVstMaxParamStrLen + 1, "L%d %s", lay + 1, kLayNames[off]);
}

static void paramDisplay(Plugin* p, int idx, char* out)
{
    float v = paramReal(idx, p->params[idx]);
    if (idx == pMaster) { snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); return; }
    if (idx == pTuneKey) { snprintf(out, 16, "%s", v >= 0.5f ? "On" : "Off"); return; }
    if (idx == pMinLen || idx == pMaxLen) { snprintf(out, 16, "%.2f", v); return; }
    if (idx == pMaxMB) { snprintf(out, 16, "%d", (int)(v + 0.5f)); return; }
    if (idx == pNLen) { snprintf(out, 16, "%.1f", v); return; }
    if (idx == pNChaos || idx == pNMorph) { snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); return; }
    if (idx == pNSpread) { snprintf(out, 16, "%.1f", v); return; }
    if (idx >= PARAM_LAYER0 && idx < LAYER_PARAMS_END) {
        int off = (idx - PARAM_LAYER0) % PPLAY;
        if (off == 2) { snprintf(out, 16, "%+.1f", v); return; }
        snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); return;
    }
    if (v >= 100.0f) snprintf(out, 16, "%d", (int)(v + 0.5f));
    else snprintf(out, 16, "%.1f", v);
}

/* ------------------------------------------------------------ chunk state
 * SLBC1
 * <params line: count then values>
 * <library path>
 * <selected index> <combo count>
 * per combo: "n score" then n lines of "semis<TAB>path"
 */
static std::string buildChunk(Plugin* p)
{
    std::string s = "SLBC1\n";
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", NUM_PARAMS);
    s += buf;
    for (int i = 0; i < NUM_PARAMS; i++) {
        snprintf(buf, sizeof(buf), " %.6f", p->params[i]);
        s += buf;
    }
    s += "\n";
    s += p->libPath; s += "\n";
    snprintf(buf, sizeof(buf), "%d 12\n", p->selected);
    s += buf;
    for (int c = 0; c < 12; c++) {
        snprintf(buf, sizeof(buf), "%d %.4f\n", p->combos[c].nLayers,
                 p->combos[c].score);
        s += buf;
        for (int l = 0; l < p->combos[c].nLayers; l++) {
            snprintf(buf, sizeof(buf), "%.4f\t", p->combos[c].lay[l].semis);
            s += buf;
            s += p->combos[c].lay[l].path;
            s += "\n";
        }
    }
    return s;
}

static void applyChunk(Plugin* p, const char* data, int len)
{
    if (!data || len < 5) return;
    std::string s(data, (size_t)len);
    if (s.compare(0, 5, "SLBC1") != 0) return;
    size_t pos = s.find('\n');
    if (pos == std::string::npos) return;
    pos++;

    auto readLine = [&](std::string& line) -> bool {
        if (pos > s.size()) return false;
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        line = s.substr(pos, e - pos);
        pos = e + 1;
        return true;
    };

    std::string line;
    if (!readLine(line)) return;                  /* params */
    {
        const char* c = line.c_str();
        char* end = nullptr;
        long count = strtol(c, &end, 10);
        if (count > NUM_PARAMS) count = NUM_PARAMS;
        for (long i = 0; i < count && end; i++) {
            float v = strtof(end, &end);
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            p->params[i] = v;
        }
    }
    if (!readLine(line)) return;                  /* library path */
    p->libPath = line;
    if (!readLine(line)) return;                  /* selected + count */
    int sel = -1, cnt = 0;
    if (sscanf(line.c_str(), "%d %d", &sel, &cnt) != 2) return;
    if (cnt > 12) cnt = 12;
    for (int c = 0; c < cnt; c++) {
        if (!readLine(line)) return;
        int n = 0; float score = 0;
        if (sscanf(line.c_str(), "%d %f", &n, &score) < 1) return;
        if (n > 4) n = 4;
        Combo& cb = p->combos[c];
        cb = Combo();
        cb.score = score;
        for (int l = 0; l < n; l++) {
            if (!readLine(line)) return;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            ComboLayer& L = cb.lay[cb.nLayers++];
            L.semis = strtof(line.substr(0, tab).c_str(), nullptr);
            L.path = line.substr(tab + 1);
        }
    }
    if (sel >= 0 && sel < 12 && p->combos[sel].nLayers > 0)
        p->selectCombo(sel);
}

/* The editor needs these; declared here, defined in editor.h. */
struct ERect;
static bool editorOpen(Plugin* p, void* parent);
static void editorClose(Plugin* p);
static ERect* editorRect();

#include "editor.h"

/* ------------------------------------------------------------ dispatcher */

static intptr_t dispatcher(AEffect* e, int32_t opcode, int32_t index,
                           intptr_t value, void* ptr, float opt)
{
    Plugin* p = self(e);

    switch (opcode) {
    case effOpen:  return 0;
    case effClose: editorClose(p); delete p; free(e); return 0;

    case effSetSampleRate: p->engine.setSampleRate(opt > 0 ? opt : 44100.0f); return 0;
    case effSetBlockSize:
    case effMainsChanged:  return 0;

    case effGetProgram:     return 0;
    case effSetProgram:     return 0;
    case effGetProgramName: copyStr(ptr, "Default", 24); return 0;
    case effSetProgramName: return 0;

    case effGetParamName:
        if (index >= 0 && index < NUM_PARAMS) { char n[32]; paramName(index, n);
            copyStr(ptr, n, kVstMaxParamStrLen + 1); }
        return 0;
    case effGetParamLabel:
        if (index >= 0 && index < NUM_PARAMS) {
            const char* lbl = "";
            if (index < PARAM_LAYER0)      lbl = kParamMeta[index].label;
            else if (index == pNLen)       lbl = "s";
            else if (index == pNSpread)    lbl = "st";
            else if (index >= LAYER_PARAMS_END) lbl = "%";   /* NChaos/NMorph */
            else if ((index - PARAM_LAYER0) % PPLAY == 2) lbl = "st";
            else lbl = "%";
            copyStr(ptr, lbl, kVstMaxParamStrLen + 1);
        }
        return 0;
    case effGetParamDisplay:
        if (index >= 0 && index < NUM_PARAMS) { char d[16]; paramDisplay(p, index, d);
            copyStr(ptr, d, kVstMaxParamStrLen + 1); }
        return 0;
    case effCanBeAutomated: return 1;

    case effProcessEvents: processEvents(p, (VstEvents*)ptr); return 1;

    case effGetEffectName:    copyStr(ptr, PLUGIN_NAME, kVstMaxEffectNameLen); return 1;
    case effGetProductString: copyStr(ptr, PLUGIN_NAME, kVstMaxVendorStrLen);  return 1;
    case effGetVendorString:  copyStr(ptr, PLUGIN_VENDOR, kVstMaxVendorStrLen); return 1;
    case effGetVendorVersion: return PLUGIN_VERSION;
    case effGetVstVersion:    return 2400;

    case effCanDo:
        if (ptr) { const char* s = (const char*)ptr;
            if (!strcmp(s, "receiveVstEvents") || !strcmp(s, "receiveVstMidiEvent")) return 1; }
        return 0;

    case effEditGetRect: if (ptr) { *(ERect**)ptr = editorRect(); return 1; } return 0;
    case effEditOpen:    return editorOpen(p, ptr) ? 1 : 0;
    case effEditClose:   editorClose(p); return 0;
    case effEditIdle:    return 0;

    case effGetChunk:
        p->chunk = buildChunk(p);
        if (ptr) *(void**)ptr = (void*)p->chunk.data();
        return (intptr_t)p->chunk.size();
    case effSetChunk:
        applyChunk(p, (const char*)ptr, (int)value);
        return 1;
    }
    (void)value; (void)opt;
    return 0;
}

/* ---------------------------------------------------------------- entry */

extern "C" __declspec(dllexport)
AEffect* VSTPluginMain(audioMasterCallback host)
{
    AEffect* e = (AEffect*)calloc(1, sizeof(AEffect));
    Plugin*  p = new Plugin();
    if (!e || !p) { free(e); delete p; return 0; }

    p->effect = e;
    p->host = host;
    for (int i = 0; i < PARAM_LAYER0; i++) p->params[i] = kParamMeta[i].def;
    for (int l = 0; l < NSLOTS; l++) {
        int b = PARAM_LAYER0 + l * PPLAY;
        p->params[b + 0] = 0.80f;   /* vol  */
        p->params[b + 1] = 0.50f;   /* pan  */
        p->params[b + 2] = 0.50f;   /* tune */
    }
    p->params[pNLen]    = 0.35f;    /* ~1 s   */
    p->params[pNChaos]  = 0.35f;
    p->params[pNMorph]  = 0.00f;
    p->params[pNSpread] = 0.00f;

    e->magic            = kEffectMagic;
    e->dispatcher       = dispatcher;
    e->process          = processAccumulate;
    e->setParameter     = setParameter;
    e->getParameter     = getParameter;
    e->numPrograms      = 1;
    e->numParams        = NUM_PARAMS;
    e->numInputs        = 0;
    e->numOutputs       = 2;
    e->flags            = effFlagsCanReplacing | effFlagsIsSynth |
                          effFlagsHasEditor | effFlagsProgramChunks;
    e->object           = p;
    e->uniqueID         = PLUGIN_UNIQUE_ID;
    e->version          = PLUGIN_VERSION;
    e->processReplacing = processReplacing;

    return e;
}
