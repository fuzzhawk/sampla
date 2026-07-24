/*
 * Granular Sampler — milestone 2 (VST 2.4, GUI, 3-layer granular sampler)
 *
 * Three sample layers, stacked and driven monophonically by MIDI. Each layer
 * loads a WAV (drag-and-drop or double-click), shows its waveform + playhead,
 * and has a loop region with a granular tail:
 *   - Loop modes: OneShot, Forward (crossfaded), Granular (grain cloud)
 *   - "Play from start" plays the head once, then rolls into the loop
 *   - Amplitude ADSR (used as the granular envelope in Granular mode)
 *   - Per-layer Volume and fine-tune (cents)
 *
 * DSP lives in engine.h (headless, exercised by the CI smoke test); the Win32
 * editor lives in editor.h. This file is the VST glue: params, MIDI, chunk
 * state (sample paths), and the dispatcher.
 */

#include "vst2.h"
#include "engine.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#define PLUGIN_NAME    "Granular Sampler"
#define PLUGIN_VENDOR  "Morningcloak"
#define PLUGIN_VERSION 3000            /* 3.0.0.0 */
#define PLUGIN_UNIQUE_ID 0x4D436753    /* 'MCgS' */

/* ------------------------------------------------------------ param layout
 * index 0            : master volume
 * index 1 + l*PPL + o: layer l, per-layer offset o (see LayerOff)
 */
enum LayerOff {
    oVolume = 0, oTune, oMode, oPlay, oLoopStart, oLoopEnd, oOverlap,
    oAttack, oDecay, oSustain, oRelease, oGrainSize, oDensity,
    /* experimental granular (milestone 3) */
    oSpray, oPitchJit, oPanSpread, oRevProb, oScan, oShape,
    oBits, oDeci, oChaos, oTimeJit,
    PPL /* = 23 */
};
static const int NUM_LAYERS = Engine::NLAYERS;
static const int NUM_PARAMS = 1 + NUM_LAYERS * PPL;   /* 40 */

static inline void splitIndex(int idx, int& layer, int& off)
{
    if (idx == 0) { layer = -1; off = -1; return; }   /* master */
    int j = idx - 1;
    layer = j / PPL;
    off   = j % PPL;
}

/* normalized 0..1 -> real value for a per-layer offset */
static float layerReal(int off, float n)
{
    switch (off) {
    case oVolume:    return n;                          /* 0..1 gain      */
    case oTune:      return (n - 0.5f) * 200.0f;        /* -100..+100 ct  */
    case oMode:      return (float)(int)(n * 2.999f);   /* 0..2           */
    case oPlay:      return n >= 0.5f ? 1.0f : 0.0f;
    case oLoopStart: return n;                          /* 0..1           */
    case oLoopEnd:   return n;                          /* 0..1           */
    case oOverlap:   return n * n * 500.0f;             /* 0..500 ms      */
    case oAttack:    return 1.0f + n * n * 1999.0f;     /* 1..2000 ms     */
    case oDecay:     return 1.0f + n * n * 1999.0f;     /* 1..2000 ms     */
    case oSustain:   return n;                          /* 0..1           */
    case oRelease:   return 5.0f + n * n * 3995.0f;     /* 5..4000 ms     */
    case oGrainSize: return 5.0f + n * n * 495.0f;      /* 5..500 ms      */
    case oDensity:   return 1.0f + n * n * 99.0f;       /* 1..100 gr/s    */
    case oSpray:     return n * n * 500.0f;             /* 0..500 ms      */
    case oPitchJit:  return n * 12.0f;                  /* 0..12 st       */
    case oPanSpread: return n;                          /* 0..1           */
    case oRevProb:   return n;                          /* 0..1           */
    case oScan:      return (n - 0.5f) * 2.0f;          /* -1..+1         */
    case oShape:     return n;                          /* 0..1           */
    case oBits:      return 16.0f - n * 15.0f;          /* 16..1 bits     */
    case oDeci:      return 1.0f + n * n * 49.0f;       /* 1..50          */
    case oChaos:     return n;                          /* 0..1           */
    case oTimeJit:   return n;                          /* 0..1           */
    }
    return n;
}

struct ParamMeta { const char* name; const char* label; float def; };

/* per-layer defaults (normalized) and display metadata */
static const ParamMeta kLayerMeta[PPL] = {
    { "Vol",  "%",    0.80f },
    { "Tune", "ct",   0.50f },
    { "Mode", "",     0.50f },   /* -> Forward */
    { "Play", "",     1.00f },   /* -> on      */
    { "LpSt", "%",    0.00f },
    { "LpEn", "%",    1.00f },
    { "Ovlp", "ms",   0.20f },
    { "Atk",  "ms",   0.05f },
    { "Dec",  "ms",   0.30f },
    { "Sus",  "",     0.80f },
    { "Rel",  "ms",   0.30f },
    { "GrSz", "ms",   0.35f },
    { "GrDn", "gr/s", 0.45f },
    /* experimental granular — all default to neutral / off */
    { "Spray","ms",   0.00f },
    { "PJit", "st",   0.00f },
    { "Pan",  "%",    0.00f },
    { "Rev",  "%",    0.00f },
    { "Scan", "%",    0.50f },   /* -> 0 */
    { "Shape","%",    0.50f },   /* -> symmetric */
    { "Bits", "bit",  0.00f },   /* -> 16 (clean) */
    { "Deci", "x",    0.00f },   /* -> 1 (off) */
    { "Chaos","%",    0.00f },
    { "TJit", "%",    0.00f },
};

static const char* kModeNames[LOOP_MODE_COUNT] = { "OneShot", "Forward", "Granular" };

/* ---------------------------------------------------------------- state */

struct Plugin {
    AEffect* effect = nullptr;
    audioMasterCallback host = nullptr;
    float params[NUM_PARAMS];
    Engine engine;

    void*  editor = nullptr;         /* HWND, opaque here */
    std::string chunk;               /* backing store for effGetChunk */

    /* real value for any flat param index (for display) */
    float realOf(int idx) const
    {
        int layer, off; splitIndex(idx, layer, off);
        if (idx == 0) return params[0];
        return layerReal(off, params[idx]);
    }

    /* build the engine's per-block param snapshot for one layer */
    LayerParams layerParams(int l) const
    {
        int base = 1 + l * PPL;
        LayerParams lp;
        lp.volume        = layerReal(oVolume,    params[base + oVolume]);
        lp.cents         = layerReal(oTune,      params[base + oTune]);
        lp.mode          = (int)layerReal(oMode, params[base + oMode]);
        lp.playFromStart = layerReal(oPlay,      params[base + oPlay]) >= 0.5f;
        lp.loopStart     = layerReal(oLoopStart, params[base + oLoopStart]);
        lp.loopEnd       = layerReal(oLoopEnd,   params[base + oLoopEnd]);
        lp.overlapMs     = layerReal(oOverlap,   params[base + oOverlap]);
        lp.attackMs      = layerReal(oAttack,    params[base + oAttack]);
        lp.decayMs       = layerReal(oDecay,     params[base + oDecay]);
        lp.sustain       = layerReal(oSustain,   params[base + oSustain]);
        lp.releaseMs     = layerReal(oRelease,   params[base + oRelease]);
        lp.grainMs       = layerReal(oGrainSize, params[base + oGrainSize]);
        lp.density       = layerReal(oDensity,   params[base + oDensity]);
        lp.sprayMs       = layerReal(oSpray,     params[base + oSpray]);
        lp.pitchJit      = layerReal(oPitchJit,  params[base + oPitchJit]);
        lp.panSpread     = layerReal(oPanSpread, params[base + oPanSpread]);
        lp.revProb       = layerReal(oRevProb,   params[base + oRevProb]);
        lp.scan          = layerReal(oScan,      params[base + oScan]);
        lp.shape         = layerReal(oShape,     params[base + oShape]);
        lp.bits          = layerReal(oBits,      params[base + oBits]);
        lp.decimate      = layerReal(oDeci,      params[base + oDeci]);
        lp.chaos         = layerReal(oChaos,     params[base + oChaos]);
        lp.timeJit       = layerReal(oTimeJit,   params[base + oTimeJit]);
        if (lp.loopEnd < lp.loopStart + 0.001f) lp.loopEnd = lp.loopStart + 0.001f;
        return lp;
    }

    /* editor -> set a param and let the host record automation */
    void setParamFromUI(int idx, float v)
    {
        if (idx < 0 || idx >= NUM_PARAMS) return;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        params[idx] = v;
        if (host) host(effect, audioMasterAutomate, idx, 0, 0, v);
    }

    /* load a WAV file into a layer (GUI thread) */
    bool loadLayer(int l, const char* path)
    {
        if (l < 0 || l >= NUM_LAYERS) return false;
        WavData w = wav_load(path);
        if (!w.ok) return false;
        Sample* s = new Sample();
        s->data     = std::move(w.samples);
        s->frames   = w.frames;
        s->srcRate  = w.sampleRate;
        s->path     = path;
        s->computePeaks();
        engine.layers[l].publish(s);
        return true;
    }

    /* load a .txt chaos seed (GUI thread) */
    bool loadSeed(const char* path)
    {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (4L << 20)) { fclose(f); return false; }  /* cap 4MB */
        SeedData* sd = new SeedData();
        sd->bytes.resize((size_t)sz);
        size_t got = fread(sd->bytes.data(), 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) { delete sd; return false; }
        uint32_t h = 0x811C9DC5u;                     /* FNV-1a */
        for (uint8_t b : sd->bytes) { h ^= b; h *= 16777619u; }
        sd->hash = h;
        sd->path = path;
        engine.publishSeed(sd);
        return true;
    }

    std::string seedName() const
    {
        SeedData* s = engine.seedPending.load();
        if (!s) s = engine.seedLive.load();
        if (!s || s->path.empty()) return std::string();
        size_t sl = s->path.find_last_of("/\\");
        return sl == std::string::npos ? s->path : s->path.substr(sl + 1);
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

    LayerParams lp[NUM_LAYERS];
    for (int i = 0; i < NUM_LAYERS; i++) lp[i] = p->layerParams(i);

    p->engine.process(out, sampleFrames, lp);

    float master = p->params[0];
    if (master != 1.0f)
        for (int i = 0; i < sampleFrames; i++) { out[0][i] *= master; out[1][i] *= master; }
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
        if (e && e->type == kVstMidiType) {
            VstMidiEvent* m = (VstMidiEvent*)e;
            handleMidi(p, (const unsigned char*)m->midiData);
        }
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

/* ------------------------------------------------------------ chunk state
 * Text format, newline-separated: header line, then one path per layer.
 * Sample audio is re-loaded from these paths on preset recall.
 */
static std::string buildChunk(Plugin* p)
{
    std::string s = "SMPL3\n";
    for (int i = 0; i < NUM_LAYERS; i++) {
        Sample* smp = p->engine.layers[i].live.load();
        Sample* pnd = p->engine.layers[i].pending.load();
        const std::string& path = pnd ? pnd->path : (smp ? smp->path : std::string());
        s += path; s += "\n";
    }
    /* trailing line: chaos seed path (blank if none) */
    SeedData* sd = p->engine.seedPending.load();
    if (!sd) sd = p->engine.seedLive.load();
    s += (sd ? sd->path : std::string()); s += "\n";
    return s;
}

static void applyChunk(Plugin* p, const char* data, int len)
{
    if (!data || len < 5) return;
    std::string s(data, (size_t)len);
    size_t nl = s.find('\n');
    /* accept SMPL2 (no seed) and SMPL3 (with seed line) */
    if (nl == std::string::npos ||
        (s.compare(0, 5, "SMPL3") != 0 && s.compare(0, 5, "SMPL2") != 0))
        return;
    bool hasSeed = s.compare(0, 5, "SMPL3") == 0;
    size_t pos = nl + 1;
    for (int i = 0; i < NUM_LAYERS && pos <= s.size(); i++) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string path = s.substr(pos, e - pos);
        if (!path.empty()) p->loadLayer(i, path.c_str());
        pos = e + 1;
    }
    if (hasSeed && pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string seed = s.substr(pos, e - pos);
        if (!seed.empty()) p->loadSeed(seed.c_str());
    }
}

/* -------------------------------------------------------- string helpers */

static void copyStr(void* dst, const char* src, size_t max)
{
    if (!dst) return;
    strncpy((char*)dst, src, max - 1);
    ((char*)dst)[max - 1] = 0;
}

static void paramName(int idx, char* out)
{
    if (idx == 0) { strcpy(out, "Master"); return; }
    int layer, off; splitIndex(idx, layer, off);
    snprintf(out, kVstMaxParamStrLen + 1, "L%d %s", layer + 1, kLayerMeta[off].name);
}

static void paramDisplay(Plugin* p, int idx, char* out)
{
    if (idx == 0) { snprintf(out, 16, "%d", (int)(p->params[0] * 100 + 0.5f)); return; }
    int layer, off; splitIndex(idx, layer, off);
    float v = layerReal(off, p->params[idx]);
    switch (off) {
    case oVolume:
    case oSustain:
    case oLoopStart:
    case oLoopEnd:
    case oPanSpread:
    case oRevProb:
    case oShape:
    case oChaos:
    case oTimeJit: snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); break;
    case oScan:    snprintf(out, 16, "%+d", (int)(v * 100)); break;
    case oTune:    snprintf(out, 16, "%+d", (int)v); break;
    case oMode:    snprintf(out, 16, "%s", kModeNames[(int)v & 3]); break;
    case oPlay:    snprintf(out, 16, "%s", v >= 0.5f ? "Start" : "Loop"); break;
    case oBits:    snprintf(out, 16, "%d", (int)(v + 0.5f)); break;
    case oDeci:    snprintf(out, 16, "%dx", (int)(v + 0.5f)); break;
    default:       if (v >= 100.0f) snprintf(out, 16, "%d", (int)(v + 0.5f));
                   else snprintf(out, 16, "%.1f", v);
    }
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
        if (index >= 0 && index < NUM_PARAMS) { int l, o; splitIndex(index, l, o);
            copyStr(ptr, index == 0 ? "%" : kLayerMeta[o].label, kVstMaxParamStrLen + 1); }
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

    /* ---- editor ---- */
    case effEditGetRect: if (ptr) { *(ERect**)ptr = editorRect(); return 1; } return 0;
    case effEditOpen:    return editorOpen(p, ptr) ? 1 : 0;
    case effEditClose:   editorClose(p); return 0;
    case effEditIdle:    return 0;

    /* ---- chunk state (sample paths) ---- */
    case effGetChunk:
        p->chunk = buildChunk(p);
        if (ptr) *(void**)ptr = (void*)p->chunk.data();
        return (intptr_t)p->chunk.size();
    case effSetChunk:
        applyChunk(p, (const char*)ptr, (int)value);
        return 1;
    }
    (void)index; (void)value; (void)opt;
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
    p->params[0] = 0.80f;                         /* master */
    for (int l = 0; l < NUM_LAYERS; l++)
        for (int o = 0; o < PPL; o++)
            p->params[1 + l * PPL + o] = kLayerMeta[o].def;

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
