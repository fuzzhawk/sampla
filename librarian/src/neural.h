/*
 * neural.h — RAVE inference for the Sample Librarian via ONNX Runtime.
 *
 * ONNX Runtime is loaded DYNAMICALLY at runtime (LoadLibrary / dlopen), so
 * the plugin has no link-time dependency: the MinGW cross-build needs only
 * the vendored MIT-licensed C API header, and when onnxruntime.dll or the
 * models are absent the plugin still runs — the neural panel just reports
 * itself unavailable.
 *
 * Expected files in the model directory (next to the plugin DLL):
 *   onnxruntime.dll / libonnxruntime.so   the runtime
 *   rave_encoder.onnx                     audio [1,1,N] -> latent [1,D,T]
 *   rave_decoder.onnx                     latent [1,D,T] -> audio [1,1,N']
 *   rave_sr.txt                           model sample rate (e.g. "48000")
 *
 * tools/export_rave_onnx.py produces all of these from a pretrained RAVE
 * checkpoint; tools/make_test_model.py builds a tiny stand-in pair for CI.
 *
 * generate(): encode one or two style sources from the lasso, blend the
 * latent trajectories (Morph), stretch/tile to the requested Length, add
 * seeded gaussian exploration scaled by per-dim latent deviation (Chaos),
 * then decode. Deterministic for a given seed.
 */
#ifndef NEURAL_H
#define NEURAL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <math.h>

/* ---- MinGW compatibility shims for the ONNX Runtime C API header ----
 * The vendored header assumes MSVC on Windows: it spells stdcall the MSVC
 * way and expects <specstrings.h> to supply the full SAL2 annotation set.
 * MinGW-w64's sal.h covers most SAL but misses a few SAL2 macros, so we
 * include it first and fill only the gaps (guarded, so nothing conflicts).
 * On non-Windows the header defines these itself and this block is skipped.
 */
#if defined(_WIN32) && defined(__GNUC__) && !defined(_MSC_VER)
#ifndef _stdcall
#define _stdcall __stdcall
#endif
#include <sal.h>
#ifndef _In_
#define _In_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _In_reads_
#define _In_reads_(X)
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(X)
#endif
#ifndef _Out_writes_
#define _Out_writes_(X)
#endif
#ifndef _Inout_updates_all_
#define _Inout_updates_all_(X)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(X)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(X)
#endif
#ifndef _Success_
#define _Success_(X)
#endif
#ifndef _Outptr_result_buffer_maybenull_
#define _Outptr_result_buffer_maybenull_(X)
#endif
#endif  /* MinGW SAL shim */

#include "../../third_party/onnxruntime_c_api.h"

#ifdef _WIN32
#include <windows.h>
typedef HMODULE LibHandle;
static inline LibHandle nn_dlopen(const char* p) { return LoadLibraryA(p); }
static inline void* nn_dlsym(LibHandle h, const char* s)
{ return (void*)GetProcAddress(h, s); }
#else
#include <dlfcn.h>
typedef void* LibHandle;
static inline LibHandle nn_dlopen(const char* p)
{ return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static inline void* nn_dlsym(LibHandle h, const char* s) { return dlsym(h, s); }
#endif

class NeuralEngine {
public:
    bool ready = false;
    std::string err = "not initialized";
    int modelRate = 44100;
    int hopRatio = 2048;          /* audio samples per latent frame (measured) */
    int latentDim = 0;

    ~NeuralEngine() { shutdown(); }

    /* Load the runtime + both models from dir. Safe to call again. */
    bool init(const std::string& dir)
    {
        shutdown();
        err.clear();

#ifdef _WIN32
        std::string rt = dir + "\\onnxruntime.dll";
#else
        std::string rt = dir + "/libonnxruntime.so";
#endif
        lib = nn_dlopen(rt.c_str());
        if (!lib) { err = "runtime not found: " + rt; return false; }

        typedef const OrtApiBase* (ORT_API_CALL *GetBaseFn)(void);
        GetBaseFn getBase = (GetBaseFn)nn_dlsym(lib, "OrtGetApiBase");
        if (!getBase) { err = "OrtGetApiBase missing"; return false; }
        api = getBase()->GetApi(ORT_API_VERSION);
        if (!api) { err = "ONNX Runtime too old for API 17"; return false; }

        if (!check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "sampla", &env)))
            return false;
        OrtSessionOptions* opts = nullptr;
        if (!check(api->CreateSessionOptions(&opts))) return false;
        check(api->SetIntraOpNumThreads(opts, 2));   /* best-effort */

        bool ok = openSession(dir, "rave_encoder.onnx", opts, &enc) &&
                  openSession(dir, "rave_decoder.onnx", opts, &dec);
        api->ReleaseSessionOptions(opts);
        if (!ok) return false;

        if (!check(api->GetAllocatorWithDefaultOptions(&alloc))) return false;
        if (!check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                            &memInfo)))
            return false;

        if (!ioName(enc, true, encIn) || !ioName(enc, false, encOut) ||
            !ioName(dec, true, decIn) || !ioName(dec, false, decOut))
            return false;

        /* model sample rate sidecar */
        {
            std::string p = dir +
#ifdef _WIN32
                "\\rave_sr.txt";
#else
                "/rave_sr.txt";
#endif
            FILE* f = fopen(p.c_str(), "rb");
            if (f) {
                char buf[32] = {0};
                if (fread(buf, 1, 31, f) > 0) {
                    int sr = atoi(buf);
                    if (sr >= 8000 && sr <= 192000) modelRate = sr;
                }
                fclose(f);
            }
        }

        ready = true;
        return true;
    }

    void shutdown()
    {
        if (api) {
            if (enc) api->ReleaseSession(enc);
            if (dec) api->ReleaseSession(dec);
            if (env) api->ReleaseEnv(env);
            if (memInfo) api->ReleaseMemoryInfo(memInfo);
        }
        enc = dec = nullptr; env = nullptr; memInfo = nullptr;
        api = nullptr; ready = false;
        /* keep `lib` loaded once opened: unloading ORT mid-host is risky */
    }

    /* audio [n] mono -> latent [D*T] row-major (D rows, T cols) */
    bool encode(const float* mono, int n, std::vector<float>& lat,
                int64_t& D, int64_t& T)
    {
        if (!ready) return false;
        int64_t shape[3] = { 1, 1, n };
        return run(enc, encIn, encOut, mono, (size_t)n, shape, 3, lat, &D, &T);
    }

    bool decode(const float* lat, int64_t D, int64_t T, std::vector<float>& audio)
    {
        if (!ready) return false;
        int64_t shape[3] = { 1, D, T };
        int64_t d1 = 0, d2 = 0;
        return run(dec, decIn, decOut, lat, (size_t)(D * T), shape, 3,
                   audio, &d1, &d2);
    }

    /* Style generation. styles = 1..2 mono buffers at modelRate. */
    bool generate(const std::vector<std::vector<float>>& styles,
                  float lengthSec, float chaos, float morph, uint32_t seed,
                  std::vector<float>& outMono)
    {
        outMono.clear();
        if (!ready || styles.empty()) return false;

        int64_t D = 0, Ta = 0;
        std::vector<float> latA;
        if (!encode(styles[0].data(), (int)styles[0].size(), latA, D, Ta) ||
            D <= 0 || Ta <= 0)
            return false;
        latentDim = (int)D;
        hopRatio = (int)(styles[0].size() / (size_t)Ta);
        if (hopRatio < 1) hopRatio = 1;

        std::vector<float> latB;
        int64_t Db = 0, Tb = 0;
        if (styles.size() > 1 && morph > 0.001f)
            if (!encode(styles[1].data(), (int)styles[1].size(), latB, Db, Tb) ||
                Db != D)
                latB.clear();

        /* per-dim mean + deviation of the primary trajectory */
        std::vector<float> mean((size_t)D, 0), dev((size_t)D, 0);
        for (int64_t d = 0; d < D; d++) {
            double m = 0;
            for (int64_t t = 0; t < Ta; t++) m += latA[(size_t)(d * Ta + t)];
            m /= Ta;
            double v = 0;
            for (int64_t t = 0; t < Ta; t++) {
                double x = latA[(size_t)(d * Ta + t)] - m;
                v += x * x;
            }
            mean[(size_t)d] = (float)m;
            dev[(size_t)d] = (float)sqrt(v / Ta) + 0.05f;
        }

        int64_t Tout = (int64_t)(lengthSec * modelRate / hopRatio);
        if (Tout < 2) Tout = 2;
        if (Tout > 2048) Tout = 2048;

        /* hash the seed so adjacent seeds diverge (seed|1 collided 42/43) */
        uint32_t rng = seed * 2654435761u + 0x9E3779B9u;
        if (rng == 0) rng = 1;
        auto rf = [&]() {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return (rng >> 8) * (1.0f / 16777216.0f);
        };
        auto rn = [&]() { return (rf() + rf() + rf()) * 2.0f - 3.0f; };

        /* target latent: time-stretched read of A (blended with B), plus
         * temporally-smoothed seeded exploration scaled by dev * chaos */
        std::vector<float> lat((size_t)(D * Tout));
        std::vector<float> walk((size_t)D, 0);
        float phase = rf();                      /* random start offset */
        for (int64_t t = 0; t < Tout; t++) {
            double srcT = (phase + (double)t / Tout) * (Ta - 1);
            while (srcT > Ta - 1) srcT -= (Ta - 1);
            int64_t s0 = (int64_t)srcT;
            float ft = (float)(srcT - s0);
            for (int64_t d = 0; d < D; d++) {
                float a = latA[(size_t)(d * Ta + s0)];
                float a1 = latA[(size_t)(d * Ta + (s0 + 1 < Ta ? s0 + 1 : s0))];
                float v = a + (a1 - a) * ft;
                if (!latB.empty()) {
                    double bT = (double)t / Tout * (Tb - 1);
                    int64_t b0 = (int64_t)bT;
                    float bf = (float)(bT - b0);
                    float b = latB[(size_t)(d * Tb + b0)];
                    float b1 = latB[(size_t)(d * Tb + (b0 + 1 < Tb ? b0 + 1 : b0))];
                    v = v * (1 - morph) + (b + (b1 - b) * bf) * morph;
                }
                walk[(size_t)d] = 0.82f * walk[(size_t)d] + 0.18f * rn();
                v += walk[(size_t)d] * dev[(size_t)d] * chaos * 2.0f;
                lat[(size_t)(d * Tout + t)] = v;
            }
        }

        if (!decode(lat.data(), D, Tout, outMono)) return false;

        /* short fades so the one-shot never clicks */
        int n = (int)outMono.size();
        int f = modelRate / 200;
        for (int i = 0; i < f && i < n; i++) {
            outMono[i] *= (float)i / f;
            outMono[n - 1 - i] *= (float)i / f;
        }
        return !outMono.empty();
    }

private:
    LibHandle lib = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* enc = nullptr;
    OrtSession* dec = nullptr;
    OrtAllocator* alloc = nullptr;
    OrtMemoryInfo* memInfo = nullptr;
    std::string encIn, encOut, decIn, decOut;

    bool check(OrtStatus* st)
    {
        if (!st) return true;
        err = api ? api->GetErrorMessage(st) : "ort error";
        if (api) api->ReleaseStatus(st);
        return false;
    }

    /* read the model file ourselves + CreateSessionFromArray: avoids the
     * ORTCHAR_T wide-path dance on Windows entirely */
    bool openSession(const std::string& dir, const char* name,
                     OrtSessionOptions* opts, OrtSession** out)
    {
        std::string p = dir +
#ifdef _WIN32
            "\\" + name;
#else
            "/" + name;
#endif
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) { err = "model not found: " + p; return false; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (512L << 20)) { fclose(f); err = "bad model size"; return false; }
        std::vector<unsigned char> buf((size_t)sz);
        size_t got = fread(buf.data(), 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) { err = "model read failed"; return false; }
        return check(api->CreateSessionFromArray(env, buf.data(), (size_t)sz,
                                                 opts, out));
    }

    bool ioName(OrtSession* s, bool input, std::string& out)
    {
        char* name = nullptr;
        OrtStatus* st = input
            ? api->SessionGetInputName(s, 0, alloc ? alloc : defaultAlloc(), &name)
            : api->SessionGetOutputName(s, 0, alloc ? alloc : defaultAlloc(), &name);
        if (!check(st) || !name) return false;
        out = name;
        check(api->AllocatorFree(alloc ? alloc : defaultAlloc(), name));
        return true;
    }

    OrtAllocator* defaultAlloc()
    {
        OrtAllocator* a = nullptr;
        check(api->GetAllocatorWithDefaultOptions(&a));
        return a;
    }

    /* run a single-input single-output float session; returns dims 1 and 2
     * of the output (i.e. D,T for [1,D,T] or channel,length for audio) */
    bool run(OrtSession* s, const std::string& inName, const std::string& outName,
             const float* data, size_t count, const int64_t* shape, int ndim,
             std::vector<float>& out, int64_t* d1, int64_t* d2)
    {
        OrtValue* inVal = nullptr;
        if (!check(api->CreateTensorWithDataAsOrtValue(
                memInfo, (void*)data, count * sizeof(float), shape, ndim,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal)))
            return false;

        const char* inNames[1] = { inName.c_str() };
        const char* outNames[1] = { outName.c_str() };
        OrtValue* outVal = nullptr;
        bool ok = check(api->Run(s, nullptr, inNames,
                                 (const OrtValue* const*)&inVal, 1,
                                 outNames, 1, &outVal));
        api->ReleaseValue(inVal);
        if (!ok || !outVal) return false;

        OrtTensorTypeAndShapeInfo* info = nullptr;
        ok = check(api->GetTensorTypeAndShape(outVal, &info));
        size_t nd = 0, total = 0;
        int64_t dims[8] = {0};
        if (ok) ok = check(api->GetDimensionsCount(info, &nd));
        if (ok && nd > 8) ok = false;
        if (ok) ok = check(api->GetDimensions(info, dims, nd));
        if (ok) ok = check(api->GetTensorShapeElementCount(info, &total));
        if (info) api->ReleaseTensorTypeAndShapeInfo(info);

        float* src = nullptr;
        if (ok) ok = check(api->GetTensorMutableData(outVal, (void**)&src));
        if (ok && src && total > 0 && total < (1u << 27)) {
            out.assign(src, src + total);
            *d1 = nd > 1 ? dims[1] : 1;
            *d2 = nd > 2 ? dims[2] : (int64_t)total;
        } else {
            ok = false;
        }
        api->ReleaseValue(outVal);
        return ok;
    }
};

#endif /* NEURAL_H */
