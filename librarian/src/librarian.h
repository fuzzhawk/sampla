/*
 * librarian.h — the Sample Librarian engine. No Win32, no VST: scanning,
 * spectral analysis, matching, combo generation, playback and export are all
 * headless so the CI unit test exercises the same code the DAW runs.
 *
 * Pipeline:
 *   scan     walk the library, filter by length/size, analyze each WAV
 *   analyze  16 log-band energy profile + 12-bin chroma + pitch (FFT
 *            autocorrelation) from the first few seconds of audio
 *   cache    binary index next to the library root so a 100k-file rescan
 *            only touches new/changed files
 *   map      2-component PCA over band+chroma -> constellation coords,
 *            so similar sounds cluster together
 *   combo    layer 2-4 files that MATCH in tonal content (chroma cosine,
 *            optionally best-rotated for tune-to-key) but OCCUPY DIFFERENT
 *            spectral regions (low band-profile overlap)
 *   play     monophonic MIDI voice stacking the combo's layers
 *   export   render a combo to a 16-bit WAV (originals never touched)
 */
#ifndef LIBRARIAN_H
#define LIBRARIAN_H

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <atomic>
#include <string>
#include <vector>
#include "wav.h"
#include "fft.h"

static const float LIB_TWO_PI = 6.28318530717958647692f;

/* ------------------------------------------------------------- features */

static const int NBANDS = 16;
static const int NCHROMA = 12;
static const int FEAT_DIM = NBANDS + NCHROMA;

struct FileFeat {
    std::string path;
    long  fileSize = 0;
    long  mtime = 0;
    float durationSec = 0;
    int   sampleRate = 44100;
    float band[NBANDS] = {0};      /* L2-normalized log-band energies  */
    float chroma[NCHROMA] = {0};   /* L2-normalized pitch-class energy */
    float pitchHz = 0;             /* 0 = unpitched                    */
    float pitchConf = 0;
    float rms = 0;
    float cx = 0.5f, cy = 0.5f;    /* constellation coords 0..1        */
};

struct LibIndex {
    std::vector<FileFeat> files;
    std::vector<int> drawList;     /* decimated subset for the GUI     */
};

struct LibSettings {
    float minLenSec = 0.25f;
    float maxLenSec = 20.0f;
    float maxMB     = 64.0f;
    bool  tuneKey   = true;
};

struct ScanProgress {
    std::atomic<int>  found{0};    /* .wav files discovered            */
    std::atomic<int>  done{0};     /* files processed                  */
    std::atomic<int>  kept{0};     /* passed filters, in the index     */
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
};

/* ------------------------------------------------------------- analysis */

static const int   AN_FFT = 4096;
static const float AN_SECONDS = 3.0f;

/* Analyze the head of one WAV into feat. Returns false if unreadable. */
static inline bool lib_analyze(const char* path, FileFeat& feat)
{
    WavInfo info = wav_probe(path);
    if (!info.ok) return false;
    feat.durationSec = (float)info.frames / info.sampleRate;
    feat.sampleRate = info.sampleRate;

    int want = (int)(AN_SECONDS * info.sampleRate);
    WavData w = wav_load_partial(path, want);
    if (!w.ok || w.frames < 512) return false;

    /* mono downmix */
    std::vector<float> mono((size_t)w.frames);
    for (int i = 0; i < w.frames; i++)
        mono[i] = 0.5f * (w.samples[(size_t)i * 2] + w.samples[(size_t)i * 2 + 1]);

    double rmsAcc = 0;
    for (int i = 0; i < w.frames; i++) rmsAcc += (double)mono[i] * mono[i];
    feat.rms = (float)sqrt(rmsAcc / w.frames);

    /* band + chroma from non-overlapping 4096 frames */
    double band[NBANDS] = {0};
    double chroma[NCHROMA] = {0};
    static float hann[AN_FFT];
    static bool hannInit = false;
    if (!hannInit) {
        for (int n = 0; n < AN_FFT; n++)
            hann[n] = 0.5f - 0.5f * cosf(LIB_TWO_PI * n / AN_FFT);
        hannInit = true;
    }

    float re[AN_FFT], im[AN_FFT];
    int nFrames = 0;
    for (int start = 0; start + AN_FFT <= w.frames; start += AN_FFT) {
        for (int n = 0; n < AN_FFT; n++) {
            re[n] = mono[(size_t)start + n] * hann[n];
            im[n] = 0;
        }
        fft_radix2(re, im, AN_FFT, 0);
        for (int k = 1; k < AN_FFT / 2; k++) {
            float f = (float)k * info.sampleRate / AN_FFT;
            if (f < 30.0f || f > 16000.0f) continue;
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
            /* log band 40..16000 Hz */
            float bx = logf(f / 40.0f) / logf(16000.0f / 40.0f);
            int b = (int)(bx * NBANDS);
            if (b >= 0 && b < NBANDS) band[b] += mag;
            /* chroma */
            float midi = 69.0f + 12.0f * log2f(f / 440.0f);
            int pc = ((int)floorf(midi + 0.5f)) % 12;
            if (pc < 0) pc += 12;
            chroma[pc] += mag;
        }
        if (++nFrames >= 32) break;
    }
    if (nFrames == 0) return false;

    double bl = 0, cl = 0;
    for (int b = 0; b < NBANDS; b++) bl += band[b] * band[b];
    for (int c = 0; c < NCHROMA; c++) cl += chroma[c] * chroma[c];
    bl = sqrt(bl); cl = sqrt(cl);
    for (int b = 0; b < NBANDS; b++)
        feat.band[b] = bl > 1e-12 ? (float)(band[b] / bl) : 0.0f;
    for (int c = 0; c < NCHROMA; c++)
        feat.chroma[c] = cl > 1e-12 ? (float)(chroma[c] / cl) : 0.0f;

    /* pitch: FFT autocorrelation on an 8192 window from 1/4 into the clip */
    {
        const int N = 8192;
        std::vector<float> ar(N, 0.0f), ai(N, 0.0f);
        int off = w.frames / 4;
        if (off + N / 2 > w.frames) off = 0;
        int avail = w.frames - off; if (avail > N / 2) avail = N / 2;
        for (int i = 0; i < avail; i++) ar[i] = mono[(size_t)off + i];
        fft_radix2(ar.data(), ai.data(), N, 0);
        for (int k = 0; k < N; k++) {
            float p = ar[k] * ar[k] + ai[k] * ai[k];
            ar[k] = p; ai[k] = 0;
        }
        fft_radix2(ar.data(), ai.data(), N, 1);
        float r0 = ar[0] > 1e-9f ? ar[0] : 1e-9f;
        int lo = info.sampleRate / 1000;  if (lo < 2) lo = 2;
        int hi = info.sampleRate / 40;    if (hi > N / 2 - 1) hi = N / 2 - 1;
        float best = 0; int bestLag = 0;
        for (int lag = lo; lag <= hi; lag++) {
            float v = ar[lag] / r0;
            if (v > best) { best = v; bestLag = lag; }
        }
        if (bestLag > 0 && best > 0.2f) {
            feat.pitchHz = (float)info.sampleRate / bestLag;
            feat.pitchConf = best;
        }
    }
    return true;
}

/* ------------------------------------------------------------ dir walk */

static inline bool lib_isWav(const char* name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    const char* e = name + n - 4;
    return e[0] == '.' &&
           (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') &&
           (e[3] == 'v' || e[3] == 'V');
}

static inline void lib_walk(const std::string& root,
                            std::vector<std::string>& out, int depth,
                            ScanProgress* prog)
{
    if (depth > 16) return;
    if (prog && prog->cancel.load()) return;
    DIR* d = opendir(root.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string p = root + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            lib_walk(p, out, depth + 1, prog);
        } else if (lib_isWav(e->d_name)) {
            out.push_back(p);
            if (prog) prog->found.fetch_add(1);
        }
        if (prog && prog->cancel.load()) break;
    }
    closedir(d);
}

/* --------------------------------------------------------- index cache */

struct CacheRec {
    long fileSize, mtime;
    float durationSec; int sampleRate;
    float band[NBANDS], chroma[NCHROMA];
    float pitchHz, pitchConf, rms;
};

static inline std::string lib_cachePath(const std::string& root)
{
    return root + "/.sample_librarian.idx";
}

static inline void lib_cacheLoad(const std::string& root,
                                 std::vector<std::string>& paths,
                                 std::vector<CacheRec>& recs)
{
    FILE* f = fopen(lib_cachePath(root).c_str(), "rb");
    if (!f) return;
    char magic[5] = {0};
    if (fread(magic, 1, 5, f) != 5 || memcmp(magic, "SLIB1", 5)) { fclose(f); return; }
    uint32_t count = 0;
    if (fread(&count, 4, 1, f) != 1 || count > 2000000u) { fclose(f); return; }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t plen = 0;
        if (fread(&plen, 2, 1, f) != 1) break;
        std::string p(plen, '\0');
        CacheRec r;
        if (plen > 0 && fread(&p[0], 1, plen, f) != plen) break;
        if (fread(&r, sizeof(CacheRec), 1, f) != 1) break;
        paths.push_back(p);
        recs.push_back(r);
    }
    fclose(f);
}

static inline void lib_cacheSave(const std::string& root, const LibIndex& ix)
{
    FILE* f = fopen(lib_cachePath(root).c_str(), "wb");
    if (!f) return;                    /* read-only library: just skip */
    fwrite("SLIB1", 1, 5, f);
    uint32_t count = (uint32_t)ix.files.size();
    fwrite(&count, 4, 1, f);
    for (const FileFeat& ft : ix.files) {
        uint16_t plen = (uint16_t)(ft.path.size() < 65535 ? ft.path.size() : 65535);
        fwrite(&plen, 2, 1, f);
        fwrite(ft.path.data(), 1, plen, f);
        CacheRec r;
        r.fileSize = ft.fileSize; r.mtime = ft.mtime;
        r.durationSec = ft.durationSec; r.sampleRate = ft.sampleRate;
        memcpy(r.band, ft.band, sizeof(r.band));
        memcpy(r.chroma, ft.chroma, sizeof(r.chroma));
        r.pitchHz = ft.pitchHz; r.pitchConf = ft.pitchConf; r.rms = ft.rms;
        fwrite(&r, sizeof(CacheRec), 1, f);
    }
    fclose(f);
}

/* ------------------------------------------------------- constellation */

/* 2-component PCA over [band|chroma]: similar sounds land close together. */
static inline void lib_constellation(LibIndex& ix, uint32_t rngSeed)
{
    int n = (int)ix.files.size();
    ix.drawList.clear();
    if (n == 0) return;

    auto featVec = [](const FileFeat& ft, double* v) {
        for (int b = 0; b < NBANDS; b++)  v[b] = ft.band[b];
        for (int c = 0; c < NCHROMA; c++) v[NBANDS + c] = ft.chroma[c];
    };

    const int D = FEAT_DIM;
    double mean[FEAT_DIM] = {0};
    int step = n > 20000 ? n / 20000 : 1;
    int used = 0;
    double v[FEAT_DIM];
    for (int i = 0; i < n; i += step) {
        featVec(ix.files[i], v);
        for (int d = 0; d < D; d++) mean[d] += v[d];
        used++;
    }
    for (int d = 0; d < D; d++) mean[d] /= used;

    static double cov[FEAT_DIM][FEAT_DIM];
    memset(cov, 0, sizeof(cov));
    for (int i = 0; i < n; i += step) {
        featVec(ix.files[i], v);
        for (int d = 0; d < D; d++) v[d] -= mean[d];
        for (int a = 0; a < D; a++)
            for (int b = a; b < D; b++) cov[a][b] += v[a] * v[b];
    }
    for (int a = 0; a < D; a++)
        for (int b = 0; b < a; b++) cov[a][b] = cov[b][a];

    /* top-2 eigenvectors via power iteration + deflation */
    double e1[FEAT_DIM], e2[FEAT_DIM];
    uint32_t r = rngSeed | 1;
    for (int c = 0; c < 2; c++) {
        double* ev = c == 0 ? e1 : e2;
        for (int d = 0; d < D; d++) {
            r = r * 1664525u + 1013904223u;
            ev[d] = ((r >> 8) & 0xFFFF) / 65536.0 - 0.5;
        }
        for (int it = 0; it < 40; it++) {
            double t[FEAT_DIM] = {0};
            for (int a = 0; a < D; a++)
                for (int b = 0; b < D; b++) t[a] += cov[a][b] * ev[b];
            double norm = 0;
            for (int d = 0; d < D; d++) norm += t[d] * t[d];
            norm = sqrt(norm);
            if (norm < 1e-12) break;
            for (int d = 0; d < D; d++) ev[d] = t[d] / norm;
        }
        if (c == 0) {                            /* deflate */
            double lam = 0;
            double t[FEAT_DIM] = {0};
            for (int a = 0; a < D; a++)
                for (int b = 0; b < D; b++) t[a] += cov[a][b] * e1[b];
            for (int d = 0; d < D; d++) lam += e1[d] * t[d];
            for (int a = 0; a < D; a++)
                for (int b = 0; b < D; b++) cov[a][b] -= lam * e1[a] * e1[b];
        }
    }

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < n; i++) {
        featVec(ix.files[i], v);
        double x = 0, y = 0;
        for (int d = 0; d < D; d++) {
            x += (v[d] - mean[d]) * e1[d];
            y += (v[d] - mean[d]) * e2[d];
        }
        ix.files[i].cx = (float)x; ix.files[i].cy = (float)y;
        if (ix.files[i].cx < minX) minX = ix.files[i].cx;
        if (ix.files[i].cx > maxX) maxX = ix.files[i].cx;
        if (ix.files[i].cy < minY) minY = ix.files[i].cy;
        if (ix.files[i].cy > maxY) maxY = ix.files[i].cy;
    }
    float sx = maxX - minX > 1e-9f ? 1.0f / (maxX - minX) : 1.0f;
    float sy = maxY - minY > 1e-9f ? 1.0f / (maxY - minY) : 1.0f;
    for (int i = 0; i < n; i++) {
        ix.files[i].cx = 0.04f + 0.92f * (ix.files[i].cx - minX) * sx;
        ix.files[i].cy = 0.04f + 0.92f * (ix.files[i].cy - minY) * sy;
    }

    int keep = n < 4000 ? n : 4000;
    int dstep = n / keep > 0 ? n / keep : 1;
    for (int i = 0; i < n && (int)ix.drawList.size() < keep; i += dstep)
        ix.drawList.push_back(i);
}

/* ------------------------------------------------------------- scanning */

/* Blocking scan. The plugin runs this on a worker thread; tests call it
 * directly. Results land in `out` only on success. */
static inline bool lib_scan(const std::string& root, const LibSettings& st,
                            LibIndex& out, ScanProgress* prog)
{
    if (prog) { prog->running.store(true); prog->found.store(0);
                prog->done.store(0); prog->kept.store(0); }

    std::vector<std::string> paths;
    lib_walk(root, paths, 0, prog);

    /* cache lookup table (linear scan into a sorted copy would be nicer;
     * a simple two-vector search keeps it dependency-free) */
    std::vector<std::string> cPaths;
    std::vector<CacheRec> cRecs;
    lib_cacheLoad(root, cPaths, cRecs);

    LibIndex ix;
    long maxBytes = (long)(st.maxMB * 1024.0f * 1024.0f);

    for (size_t i = 0; i < paths.size(); i++) {
        if (prog && prog->cancel.load()) break;
        const std::string& p = paths[i];
        if (prog) prog->done.fetch_add(1);

        struct stat sb;
        if (stat(p.c_str(), &sb) != 0) continue;
        if ((long)sb.st_size > maxBytes) continue;       /* ignore huge files */

        FileFeat ft;
        bool fromCache = false;
        for (size_t c = 0; c < cPaths.size(); c++) {
            if (cPaths[c] == p && cRecs[c].fileSize == (long)sb.st_size &&
                cRecs[c].mtime == (long)sb.st_mtime) {
                const CacheRec& r = cRecs[c];
                ft.durationSec = r.durationSec; ft.sampleRate = r.sampleRate;
                memcpy(ft.band, r.band, sizeof(r.band));
                memcpy(ft.chroma, r.chroma, sizeof(r.chroma));
                ft.pitchHz = r.pitchHz; ft.pitchConf = r.pitchConf;
                ft.rms = r.rms;
                fromCache = true;
                break;
            }
        }
        if (!fromCache) {
            if (!lib_analyze(p.c_str(), ft)) continue;
        }
        if (ft.durationSec < st.minLenSec || ft.durationSec > st.maxLenSec)
            continue;
        if (ft.rms < 1e-5f) continue;                    /* silence */

        ft.path = p;
        ft.fileSize = (long)sb.st_size;
        ft.mtime = (long)sb.st_mtime;
        ix.files.push_back(ft);
        if (prog) prog->kept.fetch_add(1);
    }

    if (prog && prog->cancel.load()) { if (prog) prog->running.store(false); return false; }

    lib_constellation(ix, 0xC0FFEEu);
    lib_cacheSave(root, ix);
    out = ix;
    if (prog) prog->running.store(false);
    return !out.files.empty();
}

/* --------------------------------------------------------------- combos */

struct ComboLayer {
    std::string path;
    int   fileIdx = -1;    /* into LibIndex.files, for the constellation */
    float semis = 0;       /* transpose applied for tune-to-key          */
};

struct Combo {
    int nLayers = 0;
    ComboLayer lay[4];
    float score = 0;
};

static inline float lib_bandCos(const FileFeat& a, const FileFeat& b)
{
    float s = 0;
    for (int i = 0; i < NBANDS; i++) s += a.band[i] * b.band[i];
    return s;
}

static inline float lib_chromaCos(const FileFeat& a, const FileFeat& b, int rot)
{
    float s = 0;
    for (int i = 0; i < NCHROMA; i++)
        s += a.chroma[i] * b.chroma[(i + rot) % NCHROMA];
    return s;
}

/* best chroma rotation of b against a; returns similarity, rot out-param */
static inline float lib_bestChroma(const FileFeat& a, const FileFeat& b,
                                   bool allowRot, int* rotOut)
{
    if (!allowRot) { *rotOut = 0; return lib_chromaCos(a, b, 0); }
    float best = -1; int bestR = 0;
    for (int rTry = 0; rTry < 12; rTry++) {
        float s = lib_chromaCos(a, b, rTry);
        if (s > best) { best = s; bestR = rTry; }
    }
    *rotOut = bestR;
    return best;
}

/* semitone transpose that aligns b to a given the winning chroma rotation,
 * refined to the exact Hz ratio when both files have a confident pitch */
static inline float lib_tuneSemis(const FileFeat& a, const FileFeat& b, int rot)
{
    float semis = (float)(rot <= 6 ? rot : rot - 12);
    if (a.pitchConf > 0.3f && b.pitchConf > 0.3f &&
        a.pitchHz > 20 && b.pitchHz > 20) {
        float t = 12.0f * log2f(a.pitchHz / b.pitchHz) - semis;
        t -= floorf(t / 12.0f + 0.5f) * 12.0f;     /* fold to +-6 octups */
        if (t > 0.5f || t < -0.5f) t -= floorf(t + 0.5f);  /* fine part  */
        semis += t;
    }
    if (semis > 12) semis -= 12;
    if (semis < -12) semis += 12;
    return semis;
}

/* Build one combo around a random seed file: partners must be tonally
 * similar (chroma) but spectrally complementary (low band overlap). */
static inline Combo lib_makeCombo(const LibIndex& ix, const LibSettings& st,
                                  uint32_t* rng)
{
    Combo out;
    int n = (int)ix.files.size();
    if (n < 2) return out;

    auto rnd = [&]() {
        *rng = *rng * 1664525u + 1013904223u;
        return (*rng >> 8);
    };

    int seed = (int)(rnd() % (uint32_t)n);
    out.lay[0].path = ix.files[seed].path;
    out.lay[0].fileIdx = seed;
    out.lay[0].semis = 0;
    out.nLayers = 1;

    int wantLayers = 2 + (int)(rnd() % 3);          /* 2..4 */
    int tries = n < 400 ? n : 400;
    float bestScore = 0;

    for (int t = 0; t < tries && out.nLayers < wantLayers; t++) {
        int c = (int)(rnd() % (uint32_t)n);
        if (c == seed) continue;
        bool dup = false;
        for (int l = 0; l < out.nLayers; l++)
            if (out.lay[l].fileIdx == c) dup = true;
        if (dup) continue;

        const FileFeat& fa = ix.files[seed];
        const FileFeat& fc = ix.files[c];
        int rot = 0;
        float tonal = lib_bestChroma(fa, fc, st.tuneKey, &rot);
        float overlap = lib_bandCos(fa, fc);
        /* also must differ from already-picked partners */
        bool distinct = true;
        for (int l = 1; l < out.nLayers; l++) {
            const FileFeat& fl = ix.files[out.lay[l].fileIdx];
            if (lib_bandCos(fl, fc) > 0.80f) distinct = false;
        }
        float score = tonal * (1.0f - overlap);
        if (tonal > 0.55f && overlap < 0.75f && distinct) {
            ComboLayer& L = out.lay[out.nLayers++];
            L.path = fc.path;
            L.fileIdx = c;
            L.semis = st.tuneKey ? lib_tuneSemis(fa, fc, rot) : 0.0f;
            if (score > bestScore) bestScore = score;
        }
    }
    out.score = bestScore;
    if (out.nLayers < 2) out.nLayers = 0;           /* no partner found */
    return out;
}

static inline void lib_makePalette(const LibIndex& ix, const LibSettings& st,
                                   Combo combos[12], uint32_t rngSeed)
{
    uint32_t rng = rngSeed | 1;
    for (int i = 0; i < 12; i++) {
        combos[i] = Combo();
        for (int attempt = 0; attempt < 24 && combos[i].nLayers == 0; attempt++)
            combos[i] = lib_makeCombo(ix, st, &rng);
    }
}

/* ------------------------------------------------------------- playback */

/* One loaded audio layer, GUI -> audio handoff with a retire generation. */
struct LoadedBuf {
    std::vector<float> data;       /* interleaved stereo */
    int frames = 0;
    int rate = 44100;
    float semis = 0;
    std::string path;
};

struct LibSlot {
    std::atomic<LoadedBuf*> live{nullptr};
    std::atomic<LoadedBuf*> pending{nullptr};
    LoadedBuf* retire = nullptr;

    void publish(LoadedBuf* b) { delete pending.exchange(b); }
    void adopt()
    {
        LoadedBuf* p = pending.exchange(nullptr);
        if (p) { delete retire; retire = live.exchange(p); }
    }
};

struct LibVoiceParams {
    float master = 0.8f;
    float attackMs = 5.0f, releaseMs = 200.0f;
    struct { float vol = 0.8f, pan = 0.5f, semis = 0.0f; } lay[4];
};

class LibEngine {
public:
    static const int NSLOTS = 4;
    LibSlot slots[NSLOTS];
    float sampleRate = 44100;

    int  note = -1;
    bool gate = false;
    float env = 0;
    int  stage = 0;                 /* 0 idle, 1 attack, 2 hold, 3 release */
    double pos[NSLOTS] = {0, 0, 0, 0};

    ~LibEngine()
    {
        for (LibSlot& s : slots) {
            delete s.live.exchange(nullptr);
            delete s.pending.exchange(nullptr);
            delete s.retire;
        }
    }

    void setSampleRate(float sr) { sampleRate = sr > 0 ? sr : 44100; }

    void noteOn(int n)
    {
        note = n; gate = true; stage = 1; env = 0;
        for (int i = 0; i < NSLOTS; i++) pos[i] = 0;
    }
    void noteOff(int n) { if (n == note) gate = false; }

    void process(float** out, int frames, const LibVoiceParams& vp)
    {
        for (LibSlot& s : slots) s.adopt();
        if (stage == 0) return;

        float attStep = 1.0f / fmaxf(1.0f, vp.attackMs * 0.001f * sampleRate);
        float relStep = 1.0f / fmaxf(1.0f, vp.releaseMs * 0.001f * sampleRate);

        for (int i = 0; i < frames; i++) {
            if (!gate && stage != 3 && stage != 0) stage = 3;
            if (stage == 1) {
                env += attStep;
                if (env >= 1) { env = 1; stage = 2; }
            } else if (stage == 3) {
                env -= relStep;
                if (env <= 0) { env = 0; stage = 0; break; }
            }

            float mixL = 0, mixR = 0;
            bool anyLive = false;
            for (int sIdx = 0; sIdx < NSLOTS; sIdx++) {
                LoadedBuf* b = slots[sIdx].live.load();
                if (!b || b->frames < 2) continue;
                double p = pos[sIdx];
                if (p >= b->frames - 1) continue;
                anyLive = true;
                int i0 = (int)p;
                float t = (float)(p - i0);
                const float* d = &b->data[(size_t)i0 * 2];
                float L = d[0] + (d[2] - d[0]) * t;
                float R = d[1] + (d[3] - d[1]) * t;
                float pan = vp.lay[sIdx].pan;
                float gl = fminf(1.0f, 2.0f * (1.0f - pan));
                float gr = fminf(1.0f, 2.0f * pan);
                float g = vp.lay[sIdx].vol;
                mixL += L * g * gl; mixR += R * g * gr;

                double semis = (note >= 0 ? note - 60 : 0) + b->semis +
                               vp.lay[sIdx].semis;
                pos[sIdx] = p + pow(2.0, semis / 12.0) *
                                ((double)b->rate / sampleRate);
            }
            if (!anyLive && stage != 3) stage = 3;   /* all layers finished */

            out[0][i] += mixL * env * vp.master;
            out[1][i] += mixR * env * vp.master;
        }
    }
};

/* ------------------------------------------------------------- export */

/* Render up to 4 loaded layers into an interleaved stereo buffer at note 60.
 * Length = longest layer after transpose, capped at 30 s, plus a short fade.
 * Never touches the source files. */
static inline void lib_renderCombo(LoadedBuf* bufs[4], const LibVoiceParams& vp,
                                   float sampleRate, std::vector<float>& out)
{
    double longest = 0;
    for (int i = 0; i < 4; i++) {
        if (!bufs[i] || bufs[i]->frames < 2) continue;
        double semis = bufs[i]->semis + vp.lay[i].semis;
        double rate = pow(2.0, semis / 12.0) * ((double)bufs[i]->rate / sampleRate);
        double dur = bufs[i]->frames / rate;
        if (dur > longest) longest = dur;
    }
    int frames = (int)longest;
    int cap = (int)(30.0f * sampleRate);
    if (frames > cap) frames = cap;
    if (frames < 16) { out.clear(); return; }

    out.assign((size_t)frames * 2, 0.0f);
    for (int i = 0; i < 4; i++) {
        if (!bufs[i] || bufs[i]->frames < 2) continue;
        double semis = bufs[i]->semis + vp.lay[i].semis;
        double inc = pow(2.0, semis / 12.0) * ((double)bufs[i]->rate / sampleRate);
        float pan = vp.lay[i].pan;
        float gl = fminf(1.0f, 2.0f * (1.0f - pan)) * vp.lay[i].vol;
        float gr = fminf(1.0f, 2.0f * pan) * vp.lay[i].vol;
        double p = 0;
        for (int fIdx = 0; fIdx < frames && p < bufs[i]->frames - 1; fIdx++) {
            int i0 = (int)p;
            float t = (float)(p - i0);
            const float* d = &bufs[i]->data[(size_t)i0 * 2];
            out[(size_t)fIdx * 2 + 0] += (d[0] + (d[2] - d[0]) * t) * gl;
            out[(size_t)fIdx * 2 + 1] += (d[1] + (d[3] - d[1]) * t) * gr;
            p += inc;
        }
    }
    float m = vp.master;
    int fadeN = (int)(0.05f * sampleRate);
    for (int i = 0; i < frames; i++) {
        float g = m;
        if (i > frames - fadeN)
            g *= (float)(frames - i) / fadeN;
        out[(size_t)i * 2] *= g; out[(size_t)i * 2 + 1] *= g;
    }
}

/* Load a combo layer for playback/export (first 15 s max). */
static inline LoadedBuf* lib_loadLayer(const std::string& path, float semis,
                                       float maxSec = 15.0f)
{
    WavInfo info = wav_probe(path.c_str());
    if (!info.ok) return nullptr;
    int want = (int)(maxSec * info.sampleRate);
    WavData w = wav_load_partial(path.c_str(), want);
    if (!w.ok) return nullptr;
    LoadedBuf* b = new LoadedBuf();
    b->data = std::move(w.samples);
    b->frames = w.frames;
    b->rate = w.sampleRate;
    b->semis = semis;
    b->path = path;
    return b;
}

#endif /* LIBRARIAN_H */
