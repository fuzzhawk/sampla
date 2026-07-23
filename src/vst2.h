/*
 * vst2.h — minimal VST 2.4 ABI declarations, clean-room, just what this
 * plugin needs. No Steinberg code. Public-domain / CC0.
 */
#ifndef VST2_MIN_H
#define VST2_MIN_H

#include <stdint.h>

#define kEffectMagic 0x56737450 /* 'VstP' */

struct AEffect;

typedef intptr_t (*audioMasterCallback)(AEffect* effect, int32_t opcode,
                                        int32_t index, intptr_t value,
                                        void* ptr, float opt);

typedef intptr_t (*AEffectDispatcherProc)(AEffect* effect, int32_t opcode,
                                          int32_t index, intptr_t value,
                                          void* ptr, float opt);
typedef void (*AEffectProcessProc)(AEffect* effect, float** inputs,
                                   float** outputs, int32_t sampleFrames);
typedef void (*AEffectProcessDoubleProc)(AEffect* effect, double** inputs,
                                         double** outputs, int32_t sampleFrames);
typedef void (*AEffectSetParameterProc)(AEffect* effect, int32_t index,
                                        float parameter);
typedef float (*AEffectGetParameterProc)(AEffect* effect, int32_t index);

struct AEffect {
    int32_t magic;                        /* must be kEffectMagic */
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;           /* deprecated accumulate process */
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;                /* deprecated */
    int32_t offQualities;                 /* deprecated */
    float ioRatio;                        /* deprecated */
    void* object;                         /* plugin instance pointer */
    void* user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};

/* AEffect flags */
enum {
    effFlagsHasEditor          = 1 << 0,
    effFlagsCanReplacing       = 1 << 4,
    effFlagsProgramChunks      = 1 << 5,
    effFlagsIsSynth            = 1 << 8,
    effFlagsCanDoubleReplacing = 1 << 12
};

/* Dispatcher opcodes (host -> plugin) */
enum {
    effOpen            = 0,
    effClose           = 1,
    effSetProgram      = 2,
    effGetProgram      = 3,
    effSetProgramName  = 4,
    effGetProgramName  = 5,
    effGetParamLabel   = 6,
    effGetParamDisplay = 7,
    effGetParamName    = 8,
    effSetSampleRate   = 10,
    effSetBlockSize    = 11,
    effMainsChanged    = 12,
    effEditGetRect     = 13,
    effEditOpen        = 14,
    effEditClose       = 15,
    effGetChunk        = 23,
    effSetChunk        = 24,
    effProcessEvents   = 25,
    effCanBeAutomated  = 26,
    effGetEffectName   = 45,
    effGetVendorString = 47,
    effGetProductString = 48,
    effGetVendorVersion = 49,
    effCanDo           = 51,
    effGetVstVersion   = 58
};

/* Host opcodes (plugin -> host) */
enum {
    audioMasterAutomate      = 0,
    audioMasterVersion       = 1,
    audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize  = 17
};

/* MIDI events */
#define kVstMidiType 1

struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
};

struct VstMidiEvent {
    int32_t type;        /* kVstMidiType */
    int32_t byteSize;    /* sizeof(VstMidiEvent) */
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2]; /* variable length in practice */
};

/* String buffer length conventions */
#define kVstMaxParamStrLen  8
#define kVstMaxEffectNameLen 32
#define kVstMaxVendorStrLen 64

#endif /* VST2_MIN_H */
