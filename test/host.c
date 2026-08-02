/*
 * host.c — minimal VST2 smoke-test host for CI.
 * Loads the plugin DLL, calls VSTPluginMain, verifies the AEffect handshake,
 * queries name/params, runs one silent audio block. Exit 0 = pass.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct AEffectStub AEffectStub;
typedef intptr_t (*HostCb)(AEffectStub*, int32_t, int32_t, intptr_t, void*, float);
typedef AEffectStub* (*EntryProc)(HostCb);
typedef intptr_t (*DispProc)(AEffectStub*, int32_t, int32_t, intptr_t, void*, float);
typedef void (*ProcRepl)(AEffectStub*, float**, float**, int32_t);

struct AEffectStub {
    int32_t magic;
    DispProc dispatcher;
    void* process;
    void* setParameter;
    void* getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object; void* user;
    int32_t uniqueID; int32_t version;
    ProcRepl processReplacing;
};

static intptr_t hostCb(AEffectStub* e, int32_t op, int32_t idx,
                       intptr_t val, void* ptr, float opt)
{
    (void)e; (void)idx; (void)val; (void)ptr; (void)opt;
    if (op == 1)  return 2400;    /* audioMasterVersion */
    if (op == 16) return 44100;   /* sample rate */
    if (op == 17) return 512;     /* block size */
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: host <plugin.dll>\n"); return 2; }

    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) { printf("FAIL: LoadLibrary (%lu)\n", GetLastError()); return 1; }

    EntryProc entry = (EntryProc)GetProcAddress(lib, "VSTPluginMain");
    if (!entry) { printf("FAIL: VSTPluginMain not exported\n"); return 1; }

    AEffectStub* fx = entry(hostCb);
    if (!fx) { printf("FAIL: entry returned NULL\n"); return 1; }
    if (fx->magic != 0x56737450) { printf("FAIL: bad magic 0x%08X\n", fx->magic); return 1; }

    char name[64] = {0};
    fx->dispatcher(fx, 45, 0, 0, name, 0);          /* effGetEffectName */
    fx->dispatcher(fx, 10, 0, 0, 0, 44100.0f);      /* effSetSampleRate */
    fx->dispatcher(fx, 11, 0, 512, 0, 0);           /* effSetBlockSize  */
    fx->dispatcher(fx, 0, 0, 0, 0, 0);              /* effOpen          */

    printf("name:    %s\n", name);
    printf("params:  %d\n", fx->numParams);
    printf("io:      %d in / %d out\n", fx->numInputs, fx->numOutputs);
    printf("synth:   %s\n", (fx->flags & (1 << 8)) ? "yes" : "no");
    printf("vstver:  %d\n", (int)fx->dispatcher(fx, 58, 0, 0, 0, 0));

    for (int i = 0; i < fx->numParams; i++) {
        char pn[32] = {0}, pd[32] = {0}, pl[32] = {0};
        fx->dispatcher(fx, 8, i, 0, pn, 0);
        fx->dispatcher(fx, 7, i, 0, pd, 0);
        fx->dispatcher(fx, 6, i, 0, pl, 0);
        printf("  p%d %-10s = %s %s\n", i, pn, pd, pl);
    }

    /* one silent block through processReplacing. Effects read inputs, so hand
     * over real (silent) input buffers, not NULL. */
    static float IL[512], IR[512], L[512], R[512];
    float* ins[2]  = { IL, IR };
    float* outs[2] = { L, R };
    if (fx->processReplacing) fx->processReplacing(fx, ins, outs, 512);

    /* accept both synth instruments and stereo effects */
    if (fx->numOutputs != 2) {
        printf("FAIL: expected stereo output\n"); return 1;
    }

    printf("PASS\n");
    return 0;
}
