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
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

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
static const int NUM_PARAMS = PARAM_LAYER0 + NSLOTS * PPLAY;   /* 19 */

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

    void*  editor = nullptr;
    std::string chunk;

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
        if (!ix || ix->files.empty()) return;
        rng = rng * 1664525u + 1013904223u;
        lib_makePalette(*ix, settings(), combos, rng);
        selected = -1;
    }

    /* load a combo's audio into the 4 slots and make it the live one */
    void selectCombo(int i)
    {
        if (i < 0 || i >= 12 || combos[i].nLayers == 0) return;
        selected = i;
        const Combo& c = combos[i];
        for (int s = 0; s < NSLOTS; s++) {
            LoadedBuf* b = nullptr;
            if (s < c.nLayers)
                b = lib_loadLayer(c.lay[s].path, c.lay[s].semis);
            engine.slots[s].publish(b);
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
        return wav_write16(outPath, mix.data(), (int)(mix.size() / 2),
                           (int)engine.sampleRate);
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
    if (idx >= PARAM_LAYER0) {
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
            if (index < PARAM_LAYER0) lbl = kParamMeta[index].label;
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
