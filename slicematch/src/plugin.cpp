/*
 * Match-Slicer — VST 2.4 instrument. Load a GUIDE (an amen break, a synth
 * melody...) and a MAIN file to slice; the engine cuts the guide into slices,
 * searches the main file for the section that best matches each guide slice,
 * and rearranges those chunks into the guide's rhythm, locked to the host
 * transport. Original files are never modified; export writes a new WAV.
 *
 * DSP lives in slicer.h (headless, CI-tested); the Win32 editor in editor.h.
 */

#include "vst2.h"
#include "slicer.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>

static std::string g_moduleDir;

#define PLUGIN_NAME    "Match Slicer"
#define PLUGIN_VENDOR  "Morningcloak"
#define PLUGIN_VERSION 1000
#define PLUGIN_UNIQUE_ID 0x4D4D536C    /* 'MMSl' */

/* ------------------------------------------------------------ params */

enum {
    pMaster = 0, pMix, pDiv, pBars, pSliceMode, pXfade, pVariation,
    pSpectralW, pGainFollow, pPitchMatch, pStretchFit,
    pMainMode, pGuideThresh, pMainThresh, NUM_PARAMS
};

static float paramReal(int idx, float n)
{
    switch (idx) {
    case pMaster:     return n;
    case pMix:        return n;                            /* 0 guide .. 1 mosaic */
    case pDiv:        return (float)(int)(n * 3.999f);     /* 0..3  1/4..1/32     */
    case pBars:       return (float)(int)(n * 2.999f);     /* 0=1 1=2 2=4 bars    */
    case pSliceMode:  return n >= 0.5f ? 1.0f : 0.0f;      /* grid / transient    */
    case pXfade:      return n * 60.0f;                    /* 0..60 ms            */
    case pVariation:  return n;                            /* 0..1                */
    case pSpectralW:  return 0.2f + n * 0.8f;              /* 0.2..1 timbre weight*/
    case pGainFollow: return n >= 0.5f ? 1.0f : 0.0f;
    case pPitchMatch: return n >= 0.5f ? 1.0f : 0.0f;
    case pStretchFit: return n >= 0.5f ? 1.0f : 0.0f;
    case pMainMode:   return n >= 0.5f ? 1.0f : 0.0f;      /* grid / transient    */
    case pGuideThresh: return n;                           /* 0..1 sensitivity    */
    case pMainThresh:  return n;                           /* 0..1 sensitivity    */
    }
    return n;
}

struct PMeta { const char* name; float def; };
static const PMeta kMeta[NUM_PARAMS] = {
    { "Master",  0.85f },
    { "Mix",     1.00f },   /* full mosaic  */
    { "Div",     0.55f },   /* -> 1/16      */
    { "Bars",    0.00f },   /* -> 1 bar     */
    { "Slice",   0.00f },   /* -> grid      */
    { "Xfade",   0.15f },   /* ~9 ms        */
    { "Var",     0.30f },
    { "SpecW",   0.80f },
    { "GainF",   1.00f },   /* on           */
    { "Pitch",   0.00f },   /* off          */
    { "Fit",     1.00f },   /* stretch on   */
    { "MainSlc", 0.00f },   /* -> grid      */
    { "GThr",    0.40f },   /* guide onset  */
    { "MThr",    0.40f },   /* main onset   */
};
static const int kBarsVal[3] = { 1, 2, 4 };

/* ---------------------------------------------------------------- state */

struct Plugin {
    AEffect* effect = nullptr;
    audioMasterCallback host = nullptr;
    float params[NUM_PARAMS];
    Slicer eng;

    bool gate = false;              /* MIDI note held (for non-transport use) */
    void* editor = nullptr;
    std::string chunk;

    /* console log (GUI thread) */
    static const int LOG_LINES = 48;
    std::string logLines[LOG_LINES];
    int logHead = 0, logCount = 0;

    void logf(const char* fmt, ...)
    {
        char b[256]; va_list ap; va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
        logLines[logHead] = b; logHead = (logHead + 1) % LOG_LINES;
        if (logCount < LOG_LINES) logCount++;
    }
    const std::string& logLine(int back) const
    { return logLines[(logHead - 1 - back + 2 * LOG_LINES) % LOG_LINES]; }

    SlicerParams sp() const
    {
        SlicerParams s;
        s.sliceMode  = (int)paramReal(pSliceMode, params[pSliceMode]);
        s.div        = (int)paramReal(pDiv, params[pDiv]);
        s.bars       = kBarsVal[(int)paramReal(pBars, params[pBars]) % 3];
        s.xfadeMs    = paramReal(pXfade, params[pXfade]);
        s.variation  = paramReal(pVariation, params[pVariation]);
        s.spectralW  = paramReal(pSpectralW, params[pSpectralW]);
        s.gainFollow = paramReal(pGainFollow, params[pGainFollow]) >= 0.5f;
        s.pitchMatch = paramReal(pPitchMatch, params[pPitchMatch]) >= 0.5f;
        s.stretchFit = paramReal(pStretchFit, params[pStretchFit]) >= 0.5f;
        s.mainMode   = (int)paramReal(pMainMode, params[pMainMode]);
        s.guideThresh = paramReal(pGuideThresh, params[pGuideThresh]);
        s.mainThresh  = paramReal(pMainThresh, params[pMainThresh]);
        return s;
    }

    void setParamFromUI(int idx, float v)
    {
        if (idx < 0 || idx >= NUM_PARAMS) return;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        params[idx] = v;
        if (host) host(effect, audioMasterAutomate, idx, 0, 0, v);
    }

    bool loadGuide(const char* path)
    {
        if (!sl_load(path, eng.guide)) { logf("guide load failed"); return false; }
        logf("guide: %s (%.2fs)", base(path), eng.guide.frames / (float)eng.guide.rate);
        return true;
    }
    bool loadMain(const char* path)
    {
        if (!sl_load(path, eng.main)) { logf("main load failed"); return false; }
        logf("main: %s (%.2fs)", base(path), eng.main.frames / (float)eng.main.rate);
        return true;
    }

    void rematch()
    {
        if (!eng.haveBoth()) { logf("load a guide AND a main file first"); return; }
        eng.par = sp();
        if (eng.rematch())
            logf("matched %d slices -> %d main candidates (%s)",
                 (int)eng.slices.size(), (int)eng.cands.size(),
                 eng.par.sliceMode ? "transient" : "grid");
        else
            logf("match failed");
    }

    bool exportWav(const char* outPath)
    {
        AudioBuf* b = eng.live.load();
        if (!b || b->frames < 2) { logf("export: run MATCH first"); return false; }
        bool ok = wav_write16(outPath, b->data.data(), b->frames, b->rate);
        logf(ok ? "exported -> %s" : "export failed: %s", outPath);
        return ok;
    }

    static const char* base(const char* p)
    {
        const char* s = strrchr(p, '/'); const char* b = strrchr(p, '\\');
        if (b > s) s = b;
        return s ? s + 1 : p;
    }
};

static Plugin* self(AEffect* e) { return (Plugin*)e->object; }

/* ------------------------------------------------------------------ audio */

static void processReplacing(AEffect* e, float** in, float** out, int32_t n)
{
    (void)in;
    Plugin* p = self(e);
    memset(out[0], 0, sizeof(float) * n);
    memset(out[1], 0, sizeof(float) * n);

    bool playing = false;
    if (p->host) {
        VstTimeInfo* ti = (VstTimeInfo*)p->host(e, audioMasterGetTime, 0,
                              kVstPpqPosValid | kVstTempoValid, 0, 0);
        if (ti) {
            if (ti->flags & kVstTempoValid) p->eng.hostTempo = ti->tempo;
            p->eng.hostPpqValid = (ti->flags & kVstPpqPosValid) != 0;
            if (p->eng.hostPpqValid) p->eng.hostPpq = ti->ppqPos;
            playing = (ti->flags & kVstTransportPlaying) != 0;
        } else p->eng.hostPpqValid = false;
    }
    /* play when transport runs, or when a note is held (free-run) */
    if (!playing && !p->gate) { p->eng.adopt(); return; }

    float mix    = paramReal(pMix, p->params[pMix]);
    float master = paramReal(pMaster, p->params[pMaster]);
    p->eng.process(out, n, playing || p->gate, mix, master);
}

static void processAccumulate(AEffect* e, float** in, float** out, int32_t n)
{ processReplacing(e, in, out, n); }

/* --------------------------------------------------------------- MIDI */

static void handleMidi(Plugin* p, const unsigned char* d)
{
    unsigned char st = d[0] & 0xF0, velo = d[2] & 0x7F;
    if (st == 0x90 && velo > 0) p->gate = true;
    else if (st == 0x80 || (st == 0x90 && velo == 0)) p->gate = false;
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

static void setParameter(AEffect* e, int32_t i, float v)
{ if (i >= 0 && i < NUM_PARAMS) self(e)->params[i] = v; }
static float getParameter(AEffect* e, int32_t i)
{ return (i >= 0 && i < NUM_PARAMS) ? self(e)->params[i] : 0.0f; }

static void copyStr(void* dst, const char* src, size_t max)
{ if (!dst) return; strncpy((char*)dst, src, max - 1); ((char*)dst)[max - 1] = 0; }

static void paramDisplay(Plugin* p, int idx, char* out)
{
    float v = paramReal(idx, p->params[idx]);
    static const char* divN[4] = { "1/4", "1/8", "1/16", "1/32" };
    switch (idx) {
    case pMaster: case pMix: case pVariation:
        snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); break;
    case pDiv:       snprintf(out, 16, "%s", divN[(int)v & 3]); break;
    case pBars:      snprintf(out, 16, "%d", kBarsVal[(int)v % 3]); break;
    case pSliceMode: snprintf(out, 16, "%s", v >= 0.5f ? "Transnt" : "Grid"); break;
    case pMainMode:  snprintf(out, 16, "%s", v >= 0.5f ? "Transnt" : "Grid"); break;
    case pXfade:     snprintf(out, 16, "%.1f", v); break;
    case pSpectralW: case pGuideThresh: case pMainThresh:
        snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); break;
    case pGainFollow: case pPitchMatch: case pStretchFit:
        snprintf(out, 16, "%s", v >= 0.5f ? "On" : "Off"); break;
    default: snprintf(out, 16, "%.2f", v);
    }
}

/* ------------------------------------------------------------ chunk state */

static std::string buildChunk(Plugin* p)
{
    std::string s = "MSLC1\n";
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", NUM_PARAMS); s += buf;
    for (int i = 0; i < NUM_PARAMS; i++) { snprintf(buf, sizeof(buf), " %.6f", p->params[i]); s += buf; }
    s += "\n";
    s += p->eng.guide.path; s += "\n";
    s += p->eng.main.path;  s += "\n";
    return s;
}
static void applyChunk(Plugin* p, const char* data, int len)
{
    if (!data || len < 5) return;
    std::string s(data, (size_t)len);
    if (s.compare(0, 5, "MSLC1") != 0) return;
    size_t pos = s.find('\n'); if (pos == std::string::npos) return; pos++;
    auto line = [&](std::string& o) -> bool {
        if (pos > s.size()) return false;
        size_t e = s.find('\n', pos); if (e == std::string::npos) e = s.size();
        o = s.substr(pos, e - pos); pos = e + 1; return true;
    };
    std::string ln;
    if (line(ln)) { const char* c = ln.c_str(); char* end = nullptr;
        long cnt = strtol(c, &end, 10); if (cnt > NUM_PARAMS) cnt = NUM_PARAMS;
        for (long i = 0; i < cnt && end; i++) { float v = strtof(end, &end);
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            p->params[i] = v; } }
    std::string gp, mp;
    line(gp); line(mp);
    if (!gp.empty()) p->loadGuide(gp.c_str());
    if (!mp.empty()) p->loadMain(mp.c_str());
    if (p->eng.haveBoth()) p->rematch();
}

/* editor hooks */
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
    case effSetSampleRate: p->eng.setSampleRate(opt > 0 ? opt : 44100.0f); return 0;
    case effSetBlockSize:
    case effMainsChanged:  return 0;
    case effGetProgram: case effSetProgram: return 0;
    case effGetProgramName: copyStr(ptr, "Default", 24); return 0;
    case effSetProgramName: return 0;
    case effGetParamName:
        if (index >= 0 && index < NUM_PARAMS) copyStr(ptr, kMeta[index].name, kVstMaxParamStrLen + 1);
        return 0;
    case effGetParamLabel:  copyStr(ptr, "", 2); return 0;
    case effGetParamDisplay:
        if (index >= 0 && index < NUM_PARAMS) { char d[16]; paramDisplay(p, index, d);
            copyStr(ptr, d, kVstMaxParamStrLen + 1); }
        return 0;
    case effCanBeAutomated: return 1;
    case effProcessEvents:  processEvents(p, (VstEvents*)ptr); return 1;
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
    case effSetChunk: applyChunk(p, (const char*)ptr, (int)value); return 1;
    }
    (void)value; (void)opt;
    return 0;
}

/* ---------------------------------------------------------------- entry */

extern "C" __declspec(dllexport)
AEffect* VSTPluginMain(audioMasterCallback host)
{
    AEffect* e = (AEffect*)calloc(1, sizeof(AEffect));
    Plugin* p = new Plugin();
    if (!e || !p) { free(e); delete p; return 0; }
    p->effect = e; p->host = host;
    for (int i = 0; i < NUM_PARAMS; i++) p->params[i] = kMeta[i].def;

    e->magic = kEffectMagic;
    e->dispatcher = dispatcher;
    e->process = processAccumulate;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numPrograms = 1;
    e->numParams = NUM_PARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsHasEditor | effFlagsProgramChunks;
    e->object = p;
    e->uniqueID = PLUGIN_UNIQUE_ID;
    e->version = PLUGIN_VERSION;
    e->processReplacing = processReplacing;
    return e;
}
