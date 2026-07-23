/*
 * Granular Sampler — milestone 1 skeleton (VST 2.4, GUI-less)
 *
 * What this build does:
 *   - Loads in Ableton Live 9 (and any VST2 host) as an Instrument
 *   - Exposes 8 named, automatable parameters with human-readable displays
 *   - Receives MIDI and plays a simple sine test voice with attack/release,
 *     so the whole MIDI -> parameter -> audio path is verifiable by ear
 *
 * The sine voice is a placeholder: the granular engine replaces the body of
 * renderVoice() in the next milestone. Everything else (ABI, params, MIDI)
 * stays as-is.
 *
 * Single translation unit, no dependencies beyond src/vst2.h.
 */

#include "vst2.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PLUGIN_NAME    "Granular Sampler"
#define PLUGIN_VENDOR  "Morningcloak"
#define PLUGIN_VERSION 1000            /* 1.0.0.0 */
#define PLUGIN_UNIQUE_ID 0x4D436753    /* 'MCgS' — change if it ever collides */

static const float TWO_PI = 6.28318530717958647692f;

/* ---------------------------------------------------------------- params */

enum ParamId {
    pGrainSize = 0,   /* 5 .. 500 ms   (inert until granular engine lands) */
    pDensity,         /* 1 .. 100 grains/s (inert)                          */
    pPosition,        /* 0 .. 100 %        (inert)                          */
    pSpray,           /* 0 .. 250 ms       (inert)                          */
    pPitch,           /* -24 .. +24 semitones (applied to test voice)       */
    pAttack,          /* 1 .. 2000 ms      (applied)                        */
    pRelease,         /* 5 .. 4000 ms      (applied)                        */
    pMix,             /* 0 .. 100 %        (applied as output level)        */
    NUM_PARAMS
};

struct ParamInfo {
    const char* name;   /* what Live shows on the slider     */
    const char* label;  /* unit suffix                       */
    float def;          /* default, normalized 0..1          */
};

static const ParamInfo kParams[NUM_PARAMS] = {
    { "GrainSize", "ms",   0.30f },
    { "Density",   "gr/s", 0.25f },
    { "Position",  "%",    0.00f },
    { "Spray",     "ms",   0.10f },
    { "Pitch",     "st",   0.50f },  /* 0.5 => 0 semitones */
    { "Attack",    "ms",   0.10f },
    { "Release",   "ms",   0.30f },
    { "Mix",       "%",    0.80f },
};

/* normalized 0..1 -> real value, per parameter */
static float paramValue(int id, float n)
{
    switch (id) {
    case pGrainSize: return 5.0f + n * n * 495.0f;      /* 5..500 ms, curved */
    case pDensity:   return 1.0f + n * n * 99.0f;       /* 1..100 gr/s       */
    case pPosition:  return n * 100.0f;                 /* 0..100 %          */
    case pSpray:     return n * n * 250.0f;             /* 0..250 ms         */
    case pPitch:     return floorf(n * 48.0f + 0.5f) - 24.0f; /* -24..+24 st */
    case pAttack:    return 1.0f + n * n * 1999.0f;     /* 1..2000 ms        */
    case pRelease:   return 5.0f + n * n * 3995.0f;     /* 5..4000 ms        */
    case pMix:       return n * 100.0f;                 /* 0..100 %          */
    }
    return n;
}

/* ---------------------------------------------------------------- state */

struct Plugin {
    audioMasterCallback host;
    float params[NUM_PARAMS];
    float sampleRate;

    /* placeholder mono test voice */
    int   note;        /* -1 = off */
    float phase;
    float env;         /* current envelope level 0..1 */
    int   gate;        /* 1 while key held */
};

static Plugin* self(AEffect* e) { return (Plugin*)e->object; }

/* ------------------------------------------------------------- test voice
 * Replace this function with the granular engine in milestone 2.
 * Contract: add sampleFrames of audio into outL/outR at current params.
 */
static void renderVoice(Plugin* p, float* outL, float* outR, int sampleFrames)
{
    if (p->note < 0 && p->env <= 0.0001f)
        return;

    float semis = paramValue(pPitch, p->params[pPitch]);
    float freq  = 440.0f * powf(2.0f, ((float)p->note - 69.0f + semis) / 12.0f);
    float inc   = TWO_PI * freq / p->sampleRate;

    float attMs = paramValue(pAttack,  p->params[pAttack]);
    float relMs = paramValue(pRelease, p->params[pRelease]);
    float attStep = 1.0f / (attMs * 0.001f * p->sampleRate);
    float relStep = 1.0f / (relMs * 0.001f * p->sampleRate);

    float gain = paramValue(pMix, p->params[pMix]) * 0.01f * 0.5f;

    for (int i = 0; i < sampleFrames; i++) {
        if (p->gate) {
            p->env += attStep;
            if (p->env > 1.0f) p->env = 1.0f;
        } else {
            p->env -= relStep;
            if (p->env < 0.0f) { p->env = 0.0f; p->note = -1; break; }
        }
        float s = sinf(p->phase) * p->env * gain;
        p->phase += inc;
        if (p->phase > TWO_PI) p->phase -= TWO_PI;
        outL[i] += s;
        outR[i] += s;
    }
}

/* ---------------------------------------------------------------- MIDI */

static void handleMidi(Plugin* p, const unsigned char* d)
{
    unsigned char status = d[0] & 0xF0;
    unsigned char note   = d[1] & 0x7F;
    unsigned char velo   = d[2] & 0x7F;

    if (status == 0x90 && velo > 0) {           /* note on  */
        p->note = note;
        p->gate = 1;
        if (p->env <= 0.0f) p->phase = 0.0f;
    } else if (status == 0x80 ||                /* note off */
              (status == 0x90 && velo == 0)) {
        if (p->note == (int)note)
            p->gate = 0;
    }
}

static void processEvents(Plugin* p, VstEvents* ev)
{
    for (int i = 0; i < ev->numEvents; i++) {
        VstEvent* e = ev->events[i];
        if (e && e->type == kVstMidiType) {
            VstMidiEvent* m = (VstMidiEvent*)e;
            handleMidi(p, (const unsigned char*)m->midiData);
        }
    }
}

/* ---------------------------------------------------------------- audio */

static void processReplacing(AEffect* e, float** in, float** out,
                             int32_t sampleFrames)
{
    (void)in;
    Plugin* p = self(e);
    float* L = out[0];
    float* R = out[1];
    memset(L, 0, sizeof(float) * sampleFrames);
    memset(R, 0, sizeof(float) * sampleFrames);
    renderVoice(p, L, R, sampleFrames);
}

/* deprecated accumulate-mode process; some old hosts still probe it */
static void processAccumulate(AEffect* e, float** in, float** out,
                              int32_t sampleFrames)
{
    processReplacing(e, in, out, sampleFrames);
}

/* --------------------------------------------------------------- params */

static void setParameter(AEffect* e, int32_t index, float value)
{
    if (index >= 0 && index < NUM_PARAMS)
        self(e)->params[index] = value;
}

static float getParameter(AEffect* e, int32_t index)
{
    if (index >= 0 && index < NUM_PARAMS)
        return self(e)->params[index];
    return 0.0f;
}

/* ------------------------------------------------------------ dispatcher */

static void copyStr(void* dst, const char* src, size_t max)
{
    if (!dst) return;
    strncpy((char*)dst, src, max - 1);
    ((char*)dst)[max - 1] = 0;
}

static intptr_t dispatcher(AEffect* e, int32_t opcode, int32_t index,
                           intptr_t value, void* ptr, float opt)
{
    Plugin* p = self(e);
    (void)value;

    switch (opcode) {
    case effOpen:
        return 0;

    case effClose:
        free(p);
        free(e);
        return 0;

    case effSetSampleRate:
        p->sampleRate = opt > 0 ? opt : 44100.0f;
        return 0;

    case effSetBlockSize:
    case effMainsChanged:
        return 0;

    case effGetProgram:      return 0;
    case effSetProgram:      return 0;
    case effGetProgramName:  copyStr(ptr, "Default", 24); return 0;
    case effSetProgramName:  return 0;

    case effGetParamName:
        if (index >= 0 && index < NUM_PARAMS)
            copyStr(ptr, kParams[index].name, kVstMaxParamStrLen + 1);
        return 0;

    case effGetParamLabel:
        if (index >= 0 && index < NUM_PARAMS)
            copyStr(ptr, kParams[index].label, kVstMaxParamStrLen + 1);
        return 0;

    case effGetParamDisplay:
        if (index >= 0 && index < NUM_PARAMS) {
            char buf[16];
            float v = paramValue(index, p->params[index]);
            if (index == pPitch)
                snprintf(buf, sizeof(buf), "%+d", (int)v);
            else if (v >= 100.0f)
                snprintf(buf, sizeof(buf), "%d", (int)(v + 0.5f));
            else
                snprintf(buf, sizeof(buf), "%.1f", v);
            copyStr(ptr, buf, kVstMaxParamStrLen + 1);
        }
        return 0;

    case effCanBeAutomated:
        return 1;

    case effProcessEvents:
        processEvents(p, (VstEvents*)ptr);
        return 1;

    case effGetEffectName:
        copyStr(ptr, PLUGIN_NAME, kVstMaxEffectNameLen);
        return 1;

    case effGetProductString:
        copyStr(ptr, PLUGIN_NAME, kVstMaxVendorStrLen);
        return 1;

    case effGetVendorString:
        copyStr(ptr, PLUGIN_VENDOR, kVstMaxVendorStrLen);
        return 1;

    case effGetVendorVersion:
        return PLUGIN_VERSION;

    case effGetVstVersion:
        return 2400;

    case effCanDo:
        if (ptr) {
            const char* s = (const char*)ptr;
            if (!strcmp(s, "receiveVstEvents") ||
                !strcmp(s, "receiveVstMidiEvent"))
                return 1;
        }
        return 0;

    /* no editor */
    case effEditGetRect:
    case effEditOpen:
    case effEditClose:
        return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- entry */

extern "C" __declspec(dllexport)
AEffect* VSTPluginMain(audioMasterCallback host)
{
    AEffect* e = (AEffect*)calloc(1, sizeof(AEffect));
    Plugin*  p = (Plugin*)calloc(1, sizeof(Plugin));
    if (!e || !p) { free(e); free(p); return 0; }

    p->host = host;
    p->sampleRate = 44100.0f;
    p->note = -1;
    for (int i = 0; i < NUM_PARAMS; i++)
        p->params[i] = kParams[i].def;

    e->magic            = kEffectMagic;
    e->dispatcher       = dispatcher;
    e->process          = processAccumulate;
    e->setParameter     = setParameter;
    e->getParameter     = getParameter;
    e->numPrograms      = 1;
    e->numParams        = NUM_PARAMS;
    e->numInputs        = 0;
    e->numOutputs       = 2;
    e->flags            = effFlagsCanReplacing | effFlagsIsSynth;
    e->object           = p;
    e->uniqueID         = PLUGIN_UNIQUE_ID;
    e->version          = PLUGIN_VERSION;
    e->processReplacing = processReplacing;

    return e;
}
