/*
 * editor.h — Win32/GDI editor for the Granular Sampler.
 *
 * Included from plugin.cpp *after* the Plugin struct and param helpers are
 * defined, so it can call straight into them. Everything here is Windows-only
 * (the DSP in engine.h stays platform-neutral for the CI smoke test).
 *
 * Layout: three stacked lanes (waveform with loop region, overlap bands,
 * draggable handles and live playhead; Mode / Play / Spec buttons; 25 knobs in
 * three rows — core sampler, experimental granular, wav operators). Below the
 * lanes sits the tempo-synced GLITCH bar: 16 step cells (click cycles the
 * algorithm, right-click clears; the playing step is highlighted) plus Div,
 * Mix and Master knobs. A global chaos SEED (.txt) box sits in the top bar.
 *
 * Pink "kawaii" theme (dark plum bg, hot-pink + lavender accents), roomy layout.
 */
#ifndef EDITOR_H
#define EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <math.h>

/* VST wants a rect with 16-bit fields. */
struct ERect { int16_t top, left, bottom, right; };

/* ---- kawaii palette ---- */
#define KW_BG        RGB(43, 27, 44)
#define KW_PANEL     RGB(32, 20, 34)
#define KW_BORDER    RGB(120, 70, 110)
#define KW_TEXT      RGB(255, 224, 240)
#define KW_TEXT_DIM  RGB(200, 150, 185)
#define KW_ACCENT    RGB(255, 150, 200)
#define KW_KNOB      RGB(92, 54, 84)
#define KW_KNOB_RIM  RGB(200, 110, 165)
#define KW_KNOB_IND  RGB(255, 214, 238)
#define KW_BTN       RGB(70, 44, 68)
#define KW_WAVE      RGB(240, 150, 205)
#define KW_LOOP      RGB(74, 48, 92)
#define KW_OVERLAP   RGB(158, 90, 122)
#define KW_HANDLE    RGB(255, 208, 130)
#define KW_PLAY      RGB(255, 90, 140)
#define KW_SEED_BG   RGB(60, 40, 58)
#define KW_SEED_TX   RGB(255, 196, 224)

static const int ED_W = 1024;
static const int ED_H = 772;
static const int LANE_TOP = 40;
static const int LANE_H   = 210;
static const int WAVE_X   = 16;
static const int WAVE_X2  = 560;
static const int KNOB_R   = 14;
static const int KCOL0    = 596;   /* first knob column center */
static const int KCOLW    = 48;    /* column spacing            */
static const int GBAR_Y   = LANE_TOP + 3 * LANE_H;   /* 670 */

/* the 25 knobs shown per lane, rows of 9 */
static const int NKNOBS = 25;
static const int kKnobOff[NKNOBS] = {
    oVolume, oTune, oOverlap, oAttack, oDecay, oSustain, oRelease,
    oGrainSize, oDensity,
    oSpray, oPitchJit, oPanSpread, oRevProb, oScan, oShape, oTimeJit,
    oBits, oDeci,
    oChaos, oStrch, oTonal, oTilt, oShift, oFrz, oSAmt
};
static const char* kKnobLbl[NKNOBS] = {
    "Vol", "Tune", "Ovlp", "Atk", "Dec", "Sus", "Rel", "Grain", "Dens",
    "Spray", "PJit", "Pan", "Rev", "Scan", "Shape", "TJit", "Bits", "Deci",
    "Chaos", "Strch", "Tonal", "Tilt", "Shft", "Frz", "SAmt"
};

/* glitch-bar knobs: {param index, x center} resolved at draw/hit time */
static const int NGKNOBS = 3;

static ERect     g_rect = { 0, 0, (int16_t)ED_H, (int16_t)ED_W };
static HINSTANCE g_hInst = nullptr;

struct EditorState {
    Plugin* p = nullptr;
    HWND    hwnd = nullptr;
    int     dragKind = 0;   /* 0 none, 1 knob, 2 loopStart, 3 loopEnd */
    int     dragParam = -1;
    int     dragStartY = 0;
    float   dragStartVal = 0;
};

/* ---- geometry (shared by paint and hit-test) ---- */

static inline int laneTopY(int l)              { return LANE_TOP + l * LANE_H; }
static inline void waveRect(int l, RECT* r)
{
    r->left = WAVE_X; r->right = WAVE_X2;
    r->top = laneTopY(l) + 26; r->bottom = laneTopY(l) + 158;
}
static inline void knobCenter(int l, int k, int* cx, int* cy)
{
    int col = k % 9, row = k / 9;
    *cx = KCOL0 + col * KCOLW;
    *cy = laneTopY(l) + 48 + row * 64;
}
static inline void modeBtnRect(int l, RECT* r)
{
    r->left = 248; r->right = 328;
    r->top = laneTopY(l) + 4; r->bottom = laneTopY(l) + 22;
}
static inline void playBtnRect(int l, RECT* r)
{
    r->left = 336; r->right = 424;
    r->top = laneTopY(l) + 4; r->bottom = laneTopY(l) + 22;
}
static inline void specBtnRect(int l, RECT* r)
{
    r->left = 432; r->right = 524;
    r->top = laneTopY(l) + 4; r->bottom = laneTopY(l) + 22;
}
static inline void seedBtnRect(RECT* r)
{
    r->left = 672; r->right = 1012; r->top = 8; r->bottom = 28;
}
static inline void glitchCellRect(int i, RECT* r)
{
    r->left = 16 + i * 48; r->right = r->left + 44;
    r->top = GBAR_Y + 32; r->bottom = GBAR_Y + 76;
}
static inline void glitchKnob(int k, int* cx, int* cy, int* param)
{
    static const int px[NGKNOBS] = { 828, 900, 976 };
    static const int pp[NGKNOBS] = { gDiv, gMix, 0 };   /* 0 = Master */
    *cx = px[k]; *cy = GBAR_Y + 54; *param = pp[k];
}
static inline bool inRect(const RECT& r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

/* ---- drawing ---- */

static void drawKnob(HDC dc, int cx, int cy, float val, const char* label,
                     const char* value)
{
    HBRUSH kb = CreateSolidBrush(KW_KNOB);
    HPEN   rim = CreatePen(PS_SOLID, 2, KW_KNOB_RIM);
    HGDIOBJ ob = SelectObject(dc, kb), op = SelectObject(dc, rim);
    Ellipse(dc, cx - KNOB_R, cy - KNOB_R, cx + KNOB_R, cy + KNOB_R);

    double a = (0.75 + (double)val * 1.5) * 3.14159265358979;
    int ex = cx + (int)(cos(a) * (KNOB_R - 3));
    int ey = cy + (int)(sin(a) * (KNOB_R - 3));
    HPEN ind = CreatePen(PS_SOLID, 2, KW_KNOB_IND);
    SelectObject(dc, ind);
    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, ex, ey);

    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(kb); DeleteObject(rim); DeleteObject(ind);

    SetTextColor(dc, KW_TEXT_DIM);
    RECT tl = { cx - 24, cy - KNOB_R - 15, cx + 24, cy - KNOB_R - 1 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, KW_TEXT);
    RECT vl = { cx - 24, cy + KNOB_R + 1, cx + 24, cy + KNOB_R + 15 };
    DrawTextA(dc, value, -1, &vl, DT_CENTER | DT_SINGLELINE);
}

static void drawTextBtn(HDC dc, const RECT& r, const char* text)
{
    HBRUSH bb = CreateSolidBrush(KW_BTN);
    FillRect(dc, &r, bb); DeleteObject(bb);
    HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ o = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(pen);
    SetTextColor(dc, KW_TEXT);
    DrawTextA(dc, text, -1, (RECT*)&r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void drawLane(HDC dc, Plugin* p, int l)
{
    int top = laneTopY(l);
    int base0 = 1 + l * PPL;

    /* header: layer tag + filename */
    SetTextColor(dc, KW_ACCENT);
    char tag[8]; snprintf(tag, sizeof(tag), "L%d", l + 1);
    TextOutA(dc, WAVE_X, top + 4, tag, (int)strlen(tag));

    Sample* s = p->engine.layers[l].live.load();
    SetTextColor(dc, KW_TEXT_DIM);
    const char* fname = "- drop a .wav (or double-click) -";
    std::string base;
    if (s && !s->path.empty()) {
        size_t sl = s->path.find_last_of("/\\");
        base = (sl == std::string::npos) ? s->path : s->path.substr(sl + 1);
        char info[160];
        snprintf(info, sizeof(info), "%s (%.1fs @ %dHz)",
                 base.c_str(), s->frames / (float)s->srcRate, s->srcRate);
        base = info; fname = base.c_str();
    }
    TextOutA(dc, WAVE_X + 32, top + 4, fname, (int)strlen(fname));

    /* mode / play / spec buttons */
    int mode = (int)layerReal(oMode, p->params[base0 + oMode]);
    bool play = layerReal(oPlay, p->params[base0 + oPlay]) >= 0.5f;
    int smode = (int)layerReal(oSMode, p->params[base0 + oSMode]);
    RECT mr, pr, sr2; modeBtnRect(l, &mr); playBtnRect(l, &pr); specBtnRect(l, &sr2);
    char mb[24]; snprintf(mb, sizeof(mb), "Mode: %s", kModeNames[mode & 3]);
    drawTextBtn(dc, mr, mb);
    drawTextBtn(dc, pr, play ? "From start" : "From loop");
    char sb[24]; snprintf(sb, sizeof(sb), "Spec: %s", kSpecNames[smode % SPEC_MODE_COUNT]);
    drawTextBtn(dc, sr2, sb);

    /* waveform background */
    RECT wr; waveRect(l, &wr);
    HBRUSH wb = CreateSolidBrush(KW_PANEL);
    FillRect(dc, &wr, wb); DeleteObject(wb);
    int waveW = wr.right - wr.left;
    int mid = (wr.top + wr.bottom) / 2;
    int halfH = (wr.bottom - wr.top) / 2 - 2;

    /* loop region shading */
    float ls = p->params[base0 + oLoopStart];
    float le = p->params[base0 + oLoopEnd];
    int lx = wr.left + (int)(ls * waveW);
    int ex = wr.left + (int)(le * waveW);
    RECT loopR = { lx, wr.top, ex, wr.bottom };
    HBRUSH lb = CreateSolidBrush(KW_LOOP);
    FillRect(dc, &loopR, lb); DeleteObject(lb);

    /* crossfade / overlap bands, drawn under the waveform */
    if (s && s->frames > 1 && mode == LOOP_FORWARD) {
        float ovMs = layerReal(oOverlap, p->params[base0 + oOverlap]);
        double xf = ovMs * 0.001 * s->srcRate / s->frames;   /* as fraction   */
        double loopLen = (double)le - ls;
        if (xf > loopLen * 0.5) xf = loopLen * 0.5;
        if (xf > 0) {
            int w = (int)(xf * waveW);
            HBRUSH ab = CreateSolidBrush(KW_OVERLAP);
            RECT b1 = { ex - w, wr.top, ex, wr.bottom };       /* fade out  */
            RECT b2 = { lx, wr.top, lx + w, wr.bottom };       /* fade in   */
            FillRect(dc, &b1, ab); FillRect(dc, &b2, ab);
            DeleteObject(ab);
        }
    }

    /* waveform peaks */
    if (s && s->frames > 1) {
        HPEN wp = CreatePen(PS_SOLID, 1, KW_WAVE);
        HGDIOBJ o = SelectObject(dc, wp);
        for (int x = 0; x < waveW; x++) {
            int pk = x * Sample::PEAKS / waveW;
            if (pk >= Sample::PEAKS) pk = Sample::PEAKS - 1;
            int y0 = mid - (int)(s->peakMax[pk] * halfH);
            int y1 = mid - (int)(s->peakMin[pk] * halfH);
            MoveToEx(dc, wr.left + x, y0, nullptr);
            LineTo(dc, wr.left + x, y1 + 1);
        }
        SelectObject(dc, o); DeleteObject(wp);
    }

    /* wave border */
    { HPEN bp = CreatePen(PS_SOLID, 1, KW_BORDER);
      HGDIOBJ o = SelectObject(dc, bp), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, wr.left, wr.top, wr.right, wr.bottom);
      SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(bp); }

    /* loop handles */
    HPEN hp = CreatePen(PS_SOLID, 2, KW_HANDLE);
    HGDIOBJ oh = SelectObject(dc, hp);
    MoveToEx(dc, lx, wr.top, nullptr); LineTo(dc, lx, wr.bottom);
    MoveToEx(dc, ex, wr.top, nullptr); LineTo(dc, ex, wr.bottom);
    SelectObject(dc, oh); DeleteObject(hp);

    /* playhead */
    float ph = p->engine.layers[l].playhead.load();
    if (ph > 0.0001f) {
        int px = wr.left + (int)(ph * waveW);
        HPEN pp = CreatePen(PS_SOLID, 2, KW_PLAY);
        HGDIOBJ opp = SelectObject(dc, pp);
        MoveToEx(dc, px, wr.top, nullptr); LineTo(dc, px, wr.bottom);
        SelectObject(dc, opp); DeleteObject(pp);
    }

    /* knobs */
    for (int k = 0; k < NKNOBS; k++) {
        int cx, cy; knobCenter(l, k, &cx, &cy);
        int idx = base0 + kKnobOff[k];
        char val[16]; paramDisplay(p, idx, val);
        drawKnob(dc, cx, cy, p->params[idx], kKnobLbl[k], val);
    }
}

static void drawGlitchBar(HDC dc, Plugin* p)
{
    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, 16, GBAR_Y + 10, "GLITCH SEQ", 10);
    SetTextColor(dc, KW_TEXT_DIM);
    const char* legend =
        "click: cycle algo | right-click: clear | - Stut St16 Rev Tape Half Gate Scrm";
    TextOutA(dc, 116, GBAR_Y + 12, legend, (int)strlen(legend));

    int cur = p->engine.glitchStep.load();
    static const COLORREF algoCol[GL_ALGO_COUNT] = {
        RGB(48, 32, 48),  RGB(150, 90, 150), RGB(180, 110, 175),
        RGB(200, 120, 170), RGB(190, 130, 110), RGB(150, 110, 170),
        RGB(210, 150, 120), RGB(220, 100, 140)
    };

    for (int i = 0; i < 16; i++) {
        RECT r; glitchCellRect(i, &r);
        int algo = (int)glitchReal(GLITCH_BASE + i, p->params[GLITCH_BASE + i]);
        HBRUSH b = CreateSolidBrush(algoCol[algo % GL_ALGO_COUNT]);
        FillRect(dc, &r, b); DeleteObject(b);
        if (i == cur) {                          /* playing-step highlight */
            HPEN hp = CreatePen(PS_SOLID, 2, RGB(255, 236, 246));
            HGDIOBJ o = SelectObject(dc, hp);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(hp);
        }
        SetTextColor(dc, KW_TEXT);
        DrawTextA(dc, kGlitchNames[algo % GL_ALGO_COUNT], -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if ((i & 3) == 0) {                      /* beat marker */
            SetTextColor(dc, KW_TEXT_DIM);
            char num[4]; snprintf(num, sizeof(num), "%d", i / 4 + 1);
            TextOutA(dc, r.left + 2, r.top - 15, num, (int)strlen(num));
        }
    }

    static const char* gl2[NGKNOBS] = { "Div", "Mix", "Master" };
    for (int k = 0; k < NGKNOBS; k++) {
        int cx, cy, param; glitchKnob(k, &cx, &cy, &param);
        char val[16]; paramDisplay(p, param, val);
        drawKnob(dc, cx, cy, p->params[param], gl2[k], val);
    }
}

static void paintEditor(HWND hwnd, Plugin* p)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    /* double-buffer to kill flicker on the timer-driven playhead */
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, ED_W, ED_H);
    HGDIOBJ ob = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    RECT full = { 0, 0, ED_W, ED_H };
    HBRUSH bg = CreateSolidBrush(KW_BG);
    FillRect(dc, &full, bg); DeleteObject(bg);

    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, WAVE_X, 9, "* GRANULAR SAMPLER *", 20);
    SetTextColor(dc, KW_TEXT_DIM);
    const char* hint = "drag knobs vertically | drag loop edges | dbl-click wave = load";
    TextOutA(dc, 210, 11, hint, (int)strlen(hint));

    /* chaos seed box */
    RECT sr; seedBtnRect(&sr);
    HBRUSH sb = CreateSolidBrush(KW_SEED_BG);
    FillRect(dc, &sr, sb); DeleteObject(sb);
    { HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
      HGDIOBJ o = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, sr.left, sr.top, sr.right, sr.bottom);
      SelectObject(dc, o); SelectObject(dc, obb); DeleteObject(pen); }
    std::string sn = p->seedName();
    char seedLine[200];
    if (sn.empty())
        snprintf(seedLine, sizeof(seedLine), "CHAOS SEED: (drop a .txt or double-click)");
    else
        snprintf(seedLine, sizeof(seedLine), "CHAOS SEED: %s", sn.c_str());
    SetTextColor(dc, KW_SEED_TX);
    DrawTextA(dc, seedLine, -1, &sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    for (int l = 0; l < NUM_LAYERS; l++) drawLane(dc, p, l);
    drawGlitchBar(dc, p);

    BitBlt(hdc, 0, 0, ED_W, ED_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- interaction ---- */

static bool endsWithTxt(const char* path)
{
    size_t n = strlen(path);
    return n >= 4 &&
        (path[n-4] == '.') &&
        (path[n-3] == 't' || path[n-3] == 'T') &&
        (path[n-2] == 'x' || path[n-2] == 'X') &&
        (path[n-1] == 't' || path[n-1] == 'T');
}

static void loadSampleDialog(EditorState* st, int lane)
{
    char file[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) st->p->loadLayer(lane, file);
}

static void loadSeedDialog(EditorState* st)
{
    char file[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "Text seed\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) st->p->loadSeed(file);
}

static int laneAt(int y)
{
    for (int l = 0; l < NUM_LAYERS; l++)
        if (y >= laneTopY(l) && y < laneTopY(l) + LANE_H) return l;
    return -1;
}

/* click in the glitch bar: cells cycle algorithms, knobs start a drag */
static bool glitchBarClick(EditorState* st, int x, int y, bool rightBtn)
{
    Plugin* p = st->p;
    if (y < GBAR_Y) return false;
    for (int i = 0; i < 16; i++) {
        RECT r; glitchCellRect(i, &r);
        if (inRect(r, x, y)) {
            int idx = GLITCH_BASE + i;
            int algo = (int)glitchReal(idx, p->params[idx]);
            int next = rightBtn ? 0 : (algo + 1) % GL_ALGO_COUNT;
            p->setParamFromUI(idx, (next + 0.5f) / (float)GL_ALGO_COUNT);
            InvalidateRect(st->hwnd, nullptr, FALSE);
            return true;
        }
    }
    if (!rightBtn) {
        for (int k = 0; k < NGKNOBS; k++) {
            int cx, cy, param; glitchKnob(k, &cx, &cy, &param);
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <=
                (KNOB_R + 4) * (KNOB_R + 4)) {
                st->dragKind = 1;
                st->dragParam = param;
                st->dragStartY = y;
                st->dragStartVal = p->params[param];
                return true;
            }
        }
    }
    return true;   /* clicks in the bar never fall through to lanes */
}

static void onLDown(EditorState* st, int x, int y)
{
    Plugin* p = st->p;
    if (glitchBarClick(st, x, y, false)) return;
    int l = laneAt(y);
    if (l < 0) return;
    int base0 = 1 + l * PPL;

    /* mode / play / spec buttons */
    RECT mr, pr, sr2; modeBtnRect(l, &mr); playBtnRect(l, &pr); specBtnRect(l, &sr2);
    if (inRect(mr, x, y)) {
        int m = ((int)layerReal(oMode, p->params[base0 + oMode]) + 1) % LOOP_MODE_COUNT;
        p->setParamFromUI(base0 + oMode, (m + 0.5f) / (float)LOOP_MODE_COUNT);
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }
    if (inRect(pr, x, y)) {
        bool on = layerReal(oPlay, p->params[base0 + oPlay]) >= 0.5f;
        p->setParamFromUI(base0 + oPlay, on ? 0.0f : 1.0f);
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }
    if (inRect(sr2, x, y)) {
        int m = ((int)layerReal(oSMode, p->params[base0 + oSMode]) + 1) % SPEC_MODE_COUNT;
        p->setParamFromUI(base0 + oSMode, (m + 0.5f) / (float)SPEC_MODE_COUNT);
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }

    /* loop handles (grab within 6px of either edge, inside the wave rect) */
    RECT wr; waveRect(l, &wr);
    if (y >= wr.top && y < wr.bottom && x >= wr.left - 6 && x <= wr.right + 6) {
        int waveW = wr.right - wr.left;
        int lx = wr.left + (int)(p->params[base0 + oLoopStart] * waveW);
        int ex = wr.left + (int)(p->params[base0 + oLoopEnd] * waveW);
        if (abs(x - lx) <= 6) { st->dragKind = 2; st->dragParam = base0 + oLoopStart; return; }
        if (abs(x - ex) <= 6) { st->dragKind = 3; st->dragParam = base0 + oLoopEnd; return; }
    }

    /* knobs */
    for (int k = 0; k < NKNOBS; k++) {
        int cx, cy; knobCenter(l, k, &cx, &cy);
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= (KNOB_R + 4) * (KNOB_R + 4)) {
            st->dragKind = 1;
            st->dragParam = base0 + kKnobOff[k];
            st->dragStartY = y;
            st->dragStartVal = p->params[st->dragParam];
            return;
        }
    }
}

static void onMove(EditorState* st, int x, int y)
{
    if (!st->dragKind) return;
    Plugin* p = st->p;
    if (st->dragKind == 1) {
        float v = st->dragStartVal + (st->dragStartY - y) / 200.0f;
        p->setParamFromUI(st->dragParam, v);
    } else {
        /* dragging a loop handle: map x within that lane's wave rect */
        int lane = (st->dragParam - 1) / PPL;
        RECT wr; waveRect(lane, &wr);
        int waveW = wr.right - wr.left;
        float frac = (x - wr.left) / (float)waveW;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        p->setParamFromUI(st->dragParam, frac);
    }
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState* st = (EditorState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 33, nullptr);
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        if (st) paintEditor(hwnd, st->p);
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* handled in WM_PAINT */
    case WM_LBUTTONDOWN:
        if (st) { SetCapture(hwnd); onLDown(st, (short)LOWORD(lp), (short)HIWORD(lp)); }
        return 0;
    case WM_RBUTTONDOWN:
        if (st) glitchBarClick(st, (short)LOWORD(lp), (short)HIWORD(lp), true);
        return 0;
    case WM_MOUSEMOVE:
        if (st) onMove(st, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        if (st) st->dragKind = 0;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
        if (st) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            RECT sr; seedBtnRect(&sr);
            if (inRect(sr, x, y)) { loadSeedDialog(st); return 0; }
            int l = laneAt(y);
            RECT wr;
            if (l >= 0) { waveRect(l, &wr); if (inRect(wr, x, y)) loadSampleDialog(st, l); }
        }
        return 0;
    case WM_DROPFILES: {
        if (st) {
            HDROP h = (HDROP)wp;
            POINT pt; DragQueryPoint(h, &pt);
            char file[MAX_PATH];
            if (DragQueryFileA(h, 0, file, MAX_PATH)) {
                if (endsWithTxt(file)) {
                    st->p->loadSeed(file);              /* seed is global */
                } else {
                    int l = laneAt(pt.y);
                    if (l >= 0) st->p->loadLayer(l, file);
                }
            }
            DragFinish(h);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void ensureClass()
{
    static bool done = false;
    if (done) return;
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = EditorProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "SamplaEditorWnd";
    RegisterClassA(&wc);
    done = true;
}

/* ---- entry points called by the dispatcher (match forward decls) ---- */

static ERect* editorRect() { return &g_rect; }

static bool editorOpen(Plugin* p, void* parent)
{
    if (!parent) return false;
    ensureClass();
    EditorState* st = new EditorState();
    st->p = p;
    HWND h = CreateWindowExA(0, "SamplaEditorWnd", "",
                             WS_CHILD | WS_VISIBLE, 0, 0, ED_W, ED_H,
                             (HWND)parent, nullptr, g_hInst, st);
    if (!h) { delete st; return false; }
    st->hwnd = h;
    p->editor = h;
    return true;
}

static void editorClose(Plugin* p)
{
    if (!p || !p->editor) return;
    HWND h = (HWND)p->editor;
    EditorState* st = (EditorState*)GetWindowLongPtr(h, GWLP_USERDATA);
    KillTimer(h, 1);
    DestroyWindow(h);
    delete st;
    p->editor = nullptr;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) g_hInst = inst;
    return TRUE;
}

#else  /* !_WIN32 — headless build: editor is a no-op so DSP still compiles */

struct ERect { int16_t top, left, bottom, right; };
static ERect g_rect = { 0, 0, 772, 1024 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}

#endif /* _WIN32 */

#endif /* EDITOR_H */
