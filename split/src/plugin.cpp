/*
 * Spectral Split — VST 2.4 INSERT EFFECT. A single-purpose tonal/atonal
 * separator: one big knob crossfades between the sustained-tonal component of
 * the incoming audio and its broadband-atonal component. High-resolution
 * 4096-point STFT for fidelity; lightweight and real-time.
 *
 * DSP lives in split.h (headless, CI-tested); the dark Win32 editor in editor.h.
 */

#include "vst2.h"
#include "split.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

static std::string g_moduleDir;

#define PLUGIN_NAME    "Spectral Split"
#define PLUGIN_VENDOR  "Morningcloak"
#define PLUGIN_VERSION 1000
#define PLUGIN_UNIQUE_ID 0x53537031    /* 'SSp1' */

/* ------------------------------------------------------------ params */

enum { pSplit = 0, pMix, pOutput, NUM_PARAMS };

static float paramReal(int idx, float n)
{
    switch (idx) {
    case pSplit:  return n;              /* 0 atonal .. 0.5 both .. 1 tonal */
    case pMix:    return n;              /* dry .. wet                      */
    case pOutput: return n * 2.0f;       /* 0..2 gain                       */
    }
    return n;
}

struct PMeta { const char* name; float def; };
static const PMeta kMeta[NUM_PARAMS] = {
    { "Split",  0.50f },   /* balanced (bypass) */
    { "Mix",    1.00f },   /* full wet          */
    { "Output", 0.50f },   /* -> 1.0 gain       */
};

/* ---------------------------------------------------------------- state */

struct Plugin {
    AEffect* effect = nullptr;
    audioMasterCallback host = nullptr;
    float params[NUM_PARAMS];
    SplitEngine eng;
    void* editor = nullptr;

    Plugin() { for (int i = 0; i < NUM_PARAMS; i++) params[i] = kMeta[i].def; }

    void setParamFromUI(int idx, float v)
    {
        if (idx < 0 || idx >= NUM_PARAMS) return;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        params[idx] = v;
        if (host) host(effect, audioMasterAutomate, idx, 0, 0, v);
    }
};

static Plugin* self(AEffect* e) { return (Plugin*)e->object; }

/* ------------------------------------------------------------------ audio */

static void processReplacing(AEffect* e, float** in, float** out, int32_t n)
{
    Plugin* p = self(e);
    float split  = paramReal(pSplit, p->params[pSplit]);
    float mix    = paramReal(pMix, p->params[pMix]);
    float outg   = paramReal(pOutput, p->params[pOutput]);
    const float* ins[2] = { in ? in[0] : nullptr, in ? in[1] : nullptr };
    float* outs[2] = { out[0], out[1] };
    p->eng.process(ins, outs, n, split, mix, outg);
}

static void processAccumulate(AEffect* e, float** in, float** out, int32_t n)
{
    std::vector<float> tL(n), tR(n);
    float* tmp[2] = { tL.data(), tR.data() };
    processReplacing(e, in, tmp, n);
    for (int i = 0; i < n; i++) { out[0][i] += tL[i]; out[1][i] += tR[i]; }
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
    switch (idx) {
    case pSplit: {
        int pct = (int)((v - 0.5f) * 200 + 0.5f);   /* -100 atonal .. +100 tonal */
        if (pct > 0)      snprintf(out, 16, "T +%d", pct);
        else if (pct < 0) snprintf(out, 16, "A %d", -pct);
        else              snprintf(out, 16, "Both");
        break; }
    case pMix:    snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); break;
    case pOutput: snprintf(out, 16, "%.2f", v); break;
    default: snprintf(out, 16, "%.2f", v);
    }
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
    case effSetBlockSize:  return 0;
    case effMainsChanged:  p->eng.reset(); return 0;
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
    case effGetEffectName:    copyStr(ptr, PLUGIN_NAME, kVstMaxEffectNameLen); return 1;
    case effGetProductString: copyStr(ptr, PLUGIN_NAME, kVstMaxVendorStrLen);  return 1;
    case effGetVendorString:  copyStr(ptr, PLUGIN_VENDOR, kVstMaxVendorStrLen); return 1;
    case effGetVendorVersion: return PLUGIN_VERSION;
    case effGetVstVersion:    return 2400;
    case effCanDo: return 0;
    case effEditGetRect: if (ptr) { *(ERect**)ptr = editorRect(); return 1; } return 0;
    case effEditOpen:    return editorOpen(p, ptr) ? 1 : 0;
    case effEditClose:   editorClose(p); return 0;
    case effEditIdle:    return 0;
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

    e->magic = kEffectMagic;
    e->dispatcher = dispatcher;
    e->process = processAccumulate;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numPrograms = 1;
    e->numParams = NUM_PARAMS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsHasEditor;
    e->initialDelay = SplitEngine::latency();   /* report PDC latency */
    e->object = p;
    e->uniqueID = PLUGIN_UNIQUE_ID;
    e->version = PLUGIN_VERSION;
    e->processReplacing = processReplacing;
    return e;
}
