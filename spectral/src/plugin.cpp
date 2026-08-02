/*
 * Spectral Canvas — VST 2.4 INSERT EFFECT. Captures an N-bar loop of the
 * incoming audio, turns it into an editable spectrogram, applies a stack of
 * paint operations (move / pitch / smear / cellular-automata / gain), and
 * resynthesises the result once per loop on a background thread. Play it and
 * you hear the painted version, re-rendered every loop (streaming) or against
 * a frozen capture. Because the render is offline-within-the-loop, it uses
 * Griffin-Lim phase reconstruction a real-time effect could never afford.
 *
 * DSP lives in canvas.h (headless, CI-tested); the Win32 editor in editor.h.
 */

#include "vst2.h"
#include "canvas.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <atomic>
#include <string>
#include <vector>

static std::string g_moduleDir;

#define PLUGIN_NAME    "Spectral Canvas"
#define PLUGIN_VENDOR  "Morningcloak"
#define PLUGIN_VERSION 1000
#define PLUGIN_UNIQUE_ID 0x53437631    /* 'SCv1' */

/* ------------------------------------------------------------ params */

enum { pMix = 0, pMaster, pBars, pFreeze, pQuality, NUM_PARAMS };

static const int kBarsVal[4] = { 1, 2, 4, 8 };

static float paramReal(int idx, float n)
{
    switch (idx) {
    case pMix:     return n;                                  /* 0 dry .. 1 wet */
    case pMaster:  return n * 2.0f;                           /* 0..2 gain      */
    case pBars:    return (float)(int)(n * 3.999f);           /* 0..3 -> 1/2/4/8*/
    case pFreeze:  return n >= 0.5f ? 1.0f : 0.0f;
    case pQuality: return (float)(int)(n * 2.999f);           /* 0 lo 1 md 2 hi */
    }
    return n;
}

struct PMeta { const char* name; float def; };
static const PMeta kMeta[NUM_PARAMS] = {
    { "Mix",     1.00f },
    { "Master",  0.50f },   /* -> 1.0 gain */
    { "Bars",    1.00f },   /* -> 8 bars   */
    { "Freeze",  0.00f },
    { "Qual",    0.50f },   /* -> medium   */
};
static const int kGLiters[3] = { 4, 10, 20 };

/* ------------------------------------------------------------ render job */

/* a self-contained unit of work handed to the render thread: a copy of the
 * captured audio + a snapshot of the op stack. The worker owns it. */
struct RenderJob {
    std::vector<float> cap;   /* interleaved stereo */
    int len = 0, channels = 2, rate = 44100;
    int glIters = 10;
    std::vector<Op> ops;
};

/* ---------------------------------------------------------------- state */

struct Plugin {
    AEffect* effect = nullptr;
    audioMasterCallback host = nullptr;
    float params[NUM_PARAMS];
    float sampleRate = 44100.0f;

    Canvas eng;                 /* used for loop-length math + sample rate */
    void* editor = nullptr;
    std::string chunk;

    /* transport */
    double hostPpq = 0.0, internalPpq = 0.0, hostTempo = 120.0;
    int    tsNum = 4, tsDen = 4;
    bool   hostPpqValid = false;

    /* capture (ping-pong), audio thread writes */
    static const int MAX_SECONDS = 30;
    int    capMax = 0;                     /* max samples per loop */
    std::vector<float> capA, capB;         /* interleaved */
    std::vector<float>* capWrite = nullptr;
    int    capLen = 0;                      /* current loop length (samples) */
    double prevLoopPos = 0.0;

    /* frozen source (freeze mode keeps painting against this) */
    std::vector<float> frozenCap; int frozenLen = 0; bool haveFrozen = false;
    bool prevFreeze = false;

    /* op stack (edited by GUI, snapshotted into jobs) — guarded by opsCS */
    CRITICAL_SECTION opsCS;
    std::vector<Op> ops;

    /* render worker */
    HANDLE renderThread = nullptr;
    HANDLE renderEvent = nullptr;
    std::atomic<bool> renderQuit{false};
    std::atomic<RenderJob*> jobPending{nullptr};
    std::atomic<RenderBuf*> liveRender{nullptr};
    std::atomic<RenderBuf*> pendingRender{nullptr};
    RenderBuf* retireRender = nullptr;
    std::atomic<int> renderGen{0};          /* bumped when a render lands */

    std::atomic<float> playPos{0.0f};       /* 0..1 for the GUI playhead */

    /* console */
    static const int LOG_LINES = 48;
    std::string logLines[LOG_LINES];
    int logHead = 0, logCount = 0;

    Plugin()
    {
        for (int i = 0; i < NUM_PARAMS; i++) params[i] = kMeta[i].def;
        InitializeCriticalSection(&opsCS);
        renderEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        renderThread = CreateThread(nullptr, 0, renderProc, this, 0, nullptr);
    }
    ~Plugin()
    {
        renderQuit.store(true);
        if (renderEvent) SetEvent(renderEvent);
        if (renderThread) { WaitForSingleObject(renderThread, 4000); CloseHandle(renderThread); }
        if (renderEvent) CloseHandle(renderEvent);
        delete liveRender.exchange(nullptr);
        delete pendingRender.exchange(nullptr);
        delete jobPending.exchange(nullptr);
        delete retireRender;
        DeleteCriticalSection(&opsCS);
    }

    void logf(const char* fmt, ...)
    {
        char b[256]; va_list ap; va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
        logLines[logHead] = b; logHead = (logHead + 1) % LOG_LINES;
        if (logCount < LOG_LINES) logCount++;
    }
    const std::string& logLine(int back) const
    { return logLines[(logHead - 1 - back + 2 * LOG_LINES) % LOG_LINES]; }

    void setSampleRate(float sr)
    {
        sampleRate = sr > 0 ? sr : 44100.0f;
        eng.setSampleRate(sampleRate);
        capMax = ((int)(MAX_SECONDS * sampleRate) / SC_HOP) * SC_HOP;
        capA.assign((size_t)capMax * 2, 0.0f);
        capB.assign((size_t)capMax * 2, 0.0f);
        capWrite = &capA;
    }

    int glIters() const { return kGLiters[(int)paramReal(pQuality, params[pQuality]) % 3]; }
    int bars() const { return kBarsVal[(int)paramReal(pBars, params[pBars]) % 4]; }

    /* ---- worker ---- */
    static DWORD WINAPI renderProc(LPVOID param) { ((Plugin*)param)->renderLoop(); return 0; }
    void renderLoop()
    {
        while (!renderQuit.load()) {
            WaitForSingleObject(renderEvent, 200);
            if (renderQuit.load()) break;
            RenderJob* job = jobPending.exchange(nullptr);
            if (!job) continue;
            Canvas cv; cv.setSampleRate(sampleRate); cv.glIters = job->glIters;
            cv.ops = job->ops;
            cv.analyze(job->cap.data(), job->len, job->channels, job->rate);
            RenderBuf* rb = new RenderBuf();
            cv.render(*rb);
            delete pendingRender.exchange(rb);
            renderGen.fetch_add(1);
            delete job;
        }
    }

    /* build a job from a buffer + current ops, hand it to the worker */
    void postJob(const float* cap, int len)
    {
        if (len < SC_HOP * 4) return;
        RenderJob* job = new RenderJob();
        job->cap.assign(cap, cap + (size_t)len * 2);
        job->len = len; job->channels = 2; job->rate = (int)sampleRate;
        job->glIters = glIters();
        EnterCriticalSection(&opsCS); job->ops = ops; LeaveCriticalSection(&opsCS);
        delete jobPending.exchange(job);
        SetEvent(renderEvent);
    }

    void adopt()
    {
        RenderBuf* p = pendingRender.exchange(nullptr);
        if (p) { delete retireRender; retireRender = liveRender.exchange(p); }
    }

    /* GUI hooks */
    void addOp(const Op& o) { EnterCriticalSection(&opsCS); ops.push_back(o); LeaveCriticalSection(&opsCS);
        logf("+ op %d (%d total)", o.type, (int)ops.size()); reRenderIfFrozen(); }
    void undoOp() { EnterCriticalSection(&opsCS); if (!ops.empty()) ops.pop_back(); int n=(int)ops.size(); LeaveCriticalSection(&opsCS);
        logf("undo (%d ops)", n); reRenderIfFrozen(); }
    void clearOps() { EnterCriticalSection(&opsCS); ops.clear(); LeaveCriticalSection(&opsCS);
        logf("cleared all ops"); reRenderIfFrozen(); }
    int opCount() { EnterCriticalSection(&opsCS); int n = (int)ops.size(); LeaveCriticalSection(&opsCS); return n; }

    void reRenderIfFrozen()
    {
        if (paramReal(pFreeze, params[pFreeze]) >= 0.5f && haveFrozen)
            postJob(frozenCap.data(), frozenLen);
    }

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
    float* iL = in ? in[0] : nullptr; float* iR = in ? in[1] : nullptr;
    float* oL = out[0]; float* oR = out[1];

    /* transport */
    bool playing = false;
    if (p->host) {
        VstTimeInfo* ti = (VstTimeInfo*)p->host(e, audioMasterGetTime, 0,
            kVstPpqPosValid | kVstTempoValid | kVstTimeSigValid, 0, 0);
        if (ti) {
            if (ti->flags & kVstTempoValid) p->hostTempo = ti->tempo;
            if (ti->flags & kVstTimeSigValid) { p->tsNum = ti->timeSigNumerator; p->tsDen = ti->timeSigDenominator; }
            p->hostPpqValid = (ti->flags & kVstPpqPosValid) != 0;
            if (p->hostPpqValid) p->hostPpq = ti->ppqPos;
            playing = (ti->flags & kVstTransportPlaying) != 0;
        } else p->hostPpqValid = false;
    }

    if (p->capWrite == nullptr) p->setSampleRate(p->sampleRate);

    double tempo = (p->hostTempo > 20 && p->hostTempo < 400) ? p->hostTempo : 120.0;
    int bars = p->bars();
    int loopLen = p->eng.samplesPerLoop(bars, tempo, p->tsNum, p->tsDen);
    if (loopLen > p->capMax) loopLen = p->capMax;
    p->capLen = loopLen;
    double loopQ = Canvas::loopQuarters(bars, p->tsNum, p->tsDen);
    double ppqPerSample = tempo / 60.0 / p->sampleRate;

    bool freeze = paramReal(pFreeze, p->params[pFreeze]) >= 0.5f;
    /* rising edge of Freeze: arm a one-shot capture of the next full loop */
    static const float* dummy = nullptr; (void)dummy;
    if (freeze && !p->prevFreeze) p->logf("freeze armed");
    if (!freeze && p->prevFreeze) { p->haveFrozen = false; p->logf("streaming"); }

    float mix    = paramReal(pMix, p->params[pMix]);
    float master = paramReal(pMaster, p->params[pMaster]);
    RenderBuf* live = p->liveRender.load();

    for (int i = 0; i < n; i++) {
        double ppq = p->hostPpqValid ? (p->hostPpq + i * ppqPerSample)
                                     : (p->internalPpq + i * ppqPerSample);
        double loopPos = fmod(ppq / loopQ, 1.0);
        if (loopPos < 0) loopPos += 1.0;

        /* capture incoming audio into the loop buffer (streaming: always;
         * freeze: only until we have a frozen snapshot) */
        if (playing && (!freeze || !p->haveFrozen)) {
            int idx = (int)(loopPos * loopLen);
            if (idx >= loopLen) idx = loopLen - 1;
            std::vector<float>& w = *p->capWrite;
            w[(size_t)idx * 2]     = iL ? iL[i] : 0.0f;
            w[(size_t)idx * 2 + 1] = iR ? iR[i] : 0.0f;
        }

        /* loop boundary: wrap detected */
        if (loopPos < p->prevLoopPos - 0.5) {
            std::vector<float>* done = p->capWrite;
            if (freeze && !p->haveFrozen) {
                /* snapshot the just-captured loop as the frozen source */
                p->frozenCap.assign(done->begin(), done->begin() + (size_t)loopLen * 2);
                p->frozenLen = loopLen; p->haveFrozen = true;
                p->postJob(p->frozenCap.data(), loopLen);
                p->logf("frozen %d-bar canvas", bars);
            } else if (!freeze) {
                /* streaming: render the loop we just captured, ping-pong */
                p->postJob(done->data(), loopLen);
                p->capWrite = (done == &p->capA) ? &p->capB : &p->capA;
            }
            p->adopt();
            live = p->liveRender.load();
        }
        p->prevLoopPos = loopPos;

        /* output: wet (rendered) mixed with dry input */
        float wetL = 0, wetR = 0;
        if (live && live->frames > 1) {
            double sp = loopPos * live->frames;
            int i0 = (int)sp; if (i0 >= live->frames - 1) i0 = live->frames - 2;
            if (i0 < 0) i0 = 0;
            float fr = (float)(sp - i0);
            const float* d = &live->data[(size_t)i0 * 2];
            wetL = d[0] + (d[2] - d[0]) * fr;
            wetR = d[1] + (d[3] - d[1]) * fr;
        }
        float dryL = iL ? iL[i] : 0.0f, dryR = iR ? iR[i] : 0.0f;
        float useMix = (live && live->frames > 1) ? mix : 0.0f;   /* dry until first render */
        oL[i] = (dryL * (1.0f - useMix) + wetL * useMix) * master;
        oR[i] = (dryR * (1.0f - useMix) + wetR * useMix) * master;

        if (i == 0) p->playPos.store((float)loopPos);
    }

    if (p->hostPpqValid) p->internalPpq = p->hostPpq + n * ppqPerSample;
    else p->internalPpq += n * ppqPerSample;
    p->prevFreeze = freeze;
}

static void processAccumulate(AEffect* e, float** in, float** out, int32_t n)
{
    /* deprecated accumulate path: render to temp then add */
    std::vector<float> tL(n), tR(n);
    float* tmpOut[2] = { tL.data(), tR.data() };
    processReplacing(e, in, tmpOut, n);
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
    case pMix:     snprintf(out, 16, "%d", (int)(v * 100 + 0.5f)); break;
    case pMaster:  snprintf(out, 16, "%.2f", v); break;
    case pBars:    snprintf(out, 16, "%d", kBarsVal[(int)v % 4]); break;
    case pFreeze:  snprintf(out, 16, "%s", v >= 0.5f ? "On" : "Off"); break;
    case pQuality: { static const char* q[3] = { "Lo", "Md", "Hi" };
                     snprintf(out, 16, "%s", q[(int)v % 3]); break; }
    default: snprintf(out, 16, "%.2f", v);
    }
}

/* ------------------------------------------------------------ chunk state */

static std::string buildChunk(Plugin* p)
{
    std::string s = "SCNV1\n";
    char buf[128];
    snprintf(buf, sizeof(buf), "%d", NUM_PARAMS); s += buf;
    for (int i = 0; i < NUM_PARAMS; i++) { snprintf(buf, sizeof(buf), " %.6f", p->params[i]); s += buf; }
    s += "\n";
    EnterCriticalSection(&p->opsCS);
    snprintf(buf, sizeof(buf), "%d\n", (int)p->ops.size()); s += buf;
    for (const Op& o : p->ops) {
        snprintf(buf, sizeof(buf), "%d %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %d %d %d\n",
                 o.type, o.r.t0, o.r.t1, o.r.f0, o.r.f1,
                 o.dt, o.df, o.pitch, o.amount, o.caRule, o.smearDir, o.cut);
        s += buf;
    }
    LeaveCriticalSection(&p->opsCS);
    return s;
}
static void applyChunk(Plugin* p, const char* data, int len)
{
    if (!data || len < 5) return;
    std::string s(data, (size_t)len);
    if (s.compare(0, 5, "SCNV1") != 0) return;
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
    std::vector<Op> newOps;
    if (line(ln)) {
        int nOps = atoi(ln.c_str());
        for (int i = 0; i < nOps && line(ln); i++) {
            Op o; float t0,t1,f0,f1;
            if (sscanf(ln.c_str(), "%d %f %f %f %f %f %f %f %f %d %d %d",
                       &o.type, &t0, &t1, &f0, &f1,
                       &o.dt, &o.df, &o.pitch, &o.amount,
                       &o.caRule, &o.smearDir, &o.cut) >= 5) {
                o.r.t0 = t0; o.r.t1 = t1; o.r.f0 = f0; o.r.f1 = f1;
                newOps.push_back(o);
            }
        }
    }
    EnterCriticalSection(&p->opsCS); p->ops = newOps; LeaveCriticalSection(&p->opsCS);
    p->logf("preset: %d ops", (int)newOps.size());
    p->reRenderIfFrozen();
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
    case effSetSampleRate: p->setSampleRate(opt > 0 ? opt : 44100.0f); return 0;
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
    p->setSampleRate(44100.0f);

    e->magic = kEffectMagic;
    e->dispatcher = dispatcher;
    e->process = processAccumulate;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numPrograms = 1;
    e->numParams = NUM_PARAMS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsHasEditor | effFlagsProgramChunks;
    e->object = p;
    e->uniqueID = PLUGIN_UNIQUE_ID;
    e->version = PLUGIN_VERSION;
    e->processReplacing = processReplacing;
    return e;
}
