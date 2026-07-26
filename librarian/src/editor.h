/*
 * editor.h — Win32/GDI editor for the Sample Librarian.
 *
 * Included from plugin.cpp after the Plugin struct is defined. Layout:
 *   top bar    library path (click to browse), SCAN button
 *   settings   MinLen / MaxLen / MaxMB knobs, TuneKey toggle, status
 *   left       constellation: click a star to audition it; click-DRAG to
 *              lasso a region for style synthesis; the selected combo is
 *              drawn as a connected star shape
 *   right      RANDOMIZE + palette of 12 combos, message console below
 *   middle     SYNTHESIZE / EXPORT SYNTH row (lasso selection status)
 *   bottom     layer controls for the selected combo, voice knobs,
 *              EXPORT WAV
 *
 * All strings deliberately ASCII-only: this UI renders through DrawTextA,
 * and multi-byte glyphs turn to mojibake there.
 *
 * Pink "kawaii" theme (dark plum bg, hot-pink + lavender accents), roomy layout.
 */
#ifndef LIB_EDITOR_H
#define LIB_EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <math.h>

struct ERect { int16_t top, left, bottom, right; };

/* ---- kawaii palette ---- */
#define KW_BG        RGB(43, 27, 44)
#define KW_PANEL     RGB(32, 20, 34)
#define KW_CONST     RGB(26, 16, 30)
#define KW_BORDER    RGB(120, 70, 110)
#define KW_TEXT      RGB(255, 224, 240)
#define KW_TEXT_DIM  RGB(200, 150, 185)
#define KW_ACCENT    RGB(255, 150, 200)
#define KW_KNOB      RGB(92, 54, 84)
#define KW_KNOB_RIM  RGB(200, 110, 165)
#define KW_KNOB_IND  RGB(255, 214, 238)
#define KW_BTN       RGB(70, 44, 68)
#define KW_BTN_ON    RGB(210, 90, 160)
#define KW_CONSOLE   RGB(140, 235, 200)
#define KW_STAR      RGB(255, 214, 150)
#define KW_LASSO     RGB(255, 236, 150)
#define KW_SEL       RGB(160, 240, 190)
#define KW_PATH_BG   RGB(56, 36, 54)

static const int ED_W = 1000;
static const int ED_H = 920;
static const int KNOB_R = 14;

/* constellation view */
static const int CV_X = 12, CV_Y = 80, CV_W = 628, CV_H = 430;
/* palette + console (right column) */
static const int PAL_X = 656, PAL_Y = 100, PAL_BW = 104, PAL_BH = 76, PAL_GAP = 10;
static const int CON_Y = 440, CON_H = 88;
/* synth row, neural sculpt row, layer strip */
static const int SYN_Y = 534;
static const int NN2_Y = 592;    /* second neural row (shape/key + sculpt knobs) */
static const int LAY_Y = 680;
static const int MAX_LASSO = 256;

static ERect     g_rect = { 0, 0, (int16_t)ED_H, (int16_t)ED_W };
static HINSTANCE g_hInst = nullptr;

struct EditorState {
    Plugin* p = nullptr;
    HWND    hwnd = nullptr;
    int     dragParam = -1;
    int     dragStartY = 0;
    float   dragStartVal = 0;
    HANDLE  scanThread = nullptr;
    HBITMAP constBmp = nullptr;
    LibIndex* constFor = nullptr;

    /* lasso / click state */
    bool  mouseDownInMap = false;
    bool  lassoActive = false;
    int   downX = 0, downY = 0;
    POINT lasso[MAX_LASSO];
    int   lassoN = 0;

    /* scan logging state */
    bool  prevRunning = false;
    int   tick = 0;
};

/* ---- geometry ---- */

static inline void pathBoxRect(RECT* r)  { r->left = 150; r->right = 700; r->top = 8;  r->bottom = 28; }
static inline void scanBtnRect(RECT* r)  { r->left = 712; r->right = 780; r->top = 8;  r->bottom = 28; }
static inline void tuneBtnRect(RECT* r)  { r->left = 220; r->right = 344; r->top = 44; r->bottom = 68; }
static inline void randBtnRect(RECT* r)  { r->left = PAL_X; r->right = PAL_X + 3 * PAL_BW + 2 * PAL_GAP; r->top = 48; r->bottom = 92; }
static inline void synthBtnRect(RECT* r) { r->left = 12;  r->right = 132; r->top = SYN_Y; r->bottom = SYN_Y + 38; }
static inline void synExpBtnRect(RECT* r){ r->left = 140; r->right = 252; r->top = SYN_Y; r->bottom = SYN_Y + 38; }
static inline void nnGenBtnRect(RECT* r) { r->left = 262; r->right = 392; r->top = SYN_Y; r->bottom = SYN_Y + 38; }
static inline void nnExpBtnRect(RECT* r) { r->left = 400; r->right = 510; r->top = SYN_Y; r->bottom = SYN_Y + 38; }
static inline void exportBtnRect(RECT* r){ r->left = 800; r->right = 985; r->top = LAY_Y + 78; r->bottom = LAY_Y + 118; }
static inline void palBtnRect(int i, RECT* r)
{
    int col = i % 3, row = i / 3;
    r->left = PAL_X + col * (PAL_BW + PAL_GAP);
    r->top  = PAL_Y + row * (PAL_BH + PAL_GAP);
    r->right = r->left + PAL_BW;
    r->bottom = r->top + PAL_BH;
}
static inline bool inRect(const RECT& r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

struct KnobPos { int param, cx, cy; const char* label; };
static const int NSKNOBS = 3;
static inline void settingsKnob(int k, KnobPos* kp)
{
    static const int prm[NSKNOBS] = { pMinLen, pMaxLen, pMaxMB };
    static const char* lbl[NSKNOBS] = { "MinLen", "MaxLen", "MaxMB" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 44 + k * 70; kp->cy = 56;
}
static const int NVKNOBS = 3;
static inline void voiceKnob(int k, KnobPos* kp)
{
    static const int prm[NVKNOBS] = { pMaster, pAttack, pRelease };
    static const char* lbl[NVKNOBS] = { "Master", "Atk", "Rel" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 830 + k * 64; kp->cy = LAY_Y + 44;
}
static inline void layerKnob(int lay, int off, KnobPos* kp)
{
    static const char* lbl[PPLAY] = { "Vol", "Pan", "Tune" };
    kp->param = PARAM_LAYER0 + lay * PPLAY + off;
    kp->label = lbl[off];
    kp->cx = 590 + off * 68;
    kp->cy = LAY_Y + 44 + lay * 54;
}
static const int NNKNOBS = 4;
static inline void nnKnob(int k, KnobPos* kp)
{
    static const int prm[NNKNOBS] = { pNLen, pNChaos, pNMorph, pNSpread };
    static const char* lbl[NNKNOBS] = { "NLen", "Chaos", "Morph", "Sprd" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 552 + k * 68; kp->cy = SYN_Y + 20;
}
/* neural sculpt: Shape/Key buttons + Tone/Motion/Focus knobs (NN2 row) */
static const int NN2KNOBS = 3;
static inline void nn2Knob(int k, KnobPos* kp)
{
    static const int prm[NN2KNOBS] = { pNTone, pNMotion, pNFocus };
    static const char* lbl[NN2KNOBS] = { "Tone", "Motion", "Focus" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 568 + k * 68; kp->cy = NN2_Y + 34;
}
static inline void nnShapeBtnRect(RECT* r) { r->left = 94;  r->right = 214; r->top = NN2_Y + 20; r->bottom = NN2_Y + 44; }
static inline void nnKeyBtnRect(RECT* r)   { r->left = 224; r->right = 328; r->top = NN2_Y + 20; r->bottom = NN2_Y + 44; }

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
    RECT tl = { cx - 30, cy - KNOB_R - 16, cx + 30, cy - KNOB_R - 1 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, KW_TEXT);
    RECT vl = { cx - 30, cy + KNOB_R + 1, cx + 30, cy + KNOB_R + 16 };
    DrawTextA(dc, value, -1, &vl, DT_CENTER | DT_SINGLELINE);
}

static void drawTextBtn(HDC dc, const RECT& r, const char* text, COLORREF bg)
{
    HBRUSH bb = CreateSolidBrush(bg);
    FillRect(dc, &r, bb); DeleteObject(bb);
    HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ o = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(pen);
    SetTextColor(dc, KW_TEXT);
    DrawTextA(dc, text, -1, (RECT*)&r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void ensureConstBmp(EditorState* st, HDC refDc)
{
    LibIndex* ix = st->p->indexLive.load();
    if (st->constBmp && st->constFor == ix) return;
    if (st->constBmp) { DeleteObject(st->constBmp); st->constBmp = nullptr; }
    st->constFor = ix;

    HDC dc = CreateCompatibleDC(refDc);
    st->constBmp = CreateCompatibleBitmap(refDc, CV_W, CV_H);
    HGDIOBJ ob = SelectObject(dc, st->constBmp);
    RECT full = { 0, 0, CV_W, CV_H };
    HBRUSH bg = CreateSolidBrush(KW_CONST);
    FillRect(dc, &full, bg); DeleteObject(bg);

    if (ix) {
        for (size_t i = 0; i < ix->drawList.size(); i++) {
            const FileFeat& f = ix->files[ix->drawList[i]];
            int x = (int)(f.cx * CV_W);
            int y = (int)(f.cy * CV_H);
            int lum = 90 + (int)(f.pitchConf * 130.0f);
            if (lum > 235) lum = 235;
            SetPixel(dc, x, y, RGB(lum, lum / 2, lum * 3 / 4));       /* pink stars */
            SetPixel(dc, x + 1, y, RGB(lum * 3 / 4, lum / 3, lum / 2));
        }
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
}

static void drawConstellation(HDC dc, EditorState* st)
{
    Plugin* p = st->p;
    ensureConstBmp(st, dc);

    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ ob = SelectObject(mem, st->constBmp);
    BitBlt(dc, CV_X, CV_Y, CV_W, CV_H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteDC(mem);

    HPEN border = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ op = SelectObject(dc, border);
    HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, CV_X - 1, CV_Y - 1, CV_X + CV_W + 1, CV_Y + CV_H + 1);
    SelectObject(dc, op); SelectObject(dc, obr); DeleteObject(border);

    LibIndex* ix = p->indexLive.load();

    /* lasso selection highlight (cap the draw for paint speed) */
    if (ix && !p->lassoSel.empty()) {
        HPEN selPen = CreatePen(PS_SOLID, 1, KW_SEL);
        HGDIOBJ o3 = SelectObject(dc, selPen);
        int shown = 0;
        for (int fi : p->lassoSel) {
            if (fi >= (int)ix->files.size()) continue;
            int x = CV_X + (int)(ix->files[fi].cx * CV_W);
            int y = CV_Y + (int)(ix->files[fi].cy * CV_H);
            MoveToEx(dc, x - 3, y, nullptr); LineTo(dc, x + 4, y);
            MoveToEx(dc, x, y - 3, nullptr); LineTo(dc, x, y + 4);
            if (++shown >= 400) break;
        }
        SelectObject(dc, o3); DeleteObject(selPen);
    }

    /* live lasso stroke */
    if (st->lassoActive && st->lassoN > 1) {
        HPEN lp = CreatePen(PS_SOLID, 1, KW_LASSO);
        HGDIOBJ o4 = SelectObject(dc, lp);
        MoveToEx(dc, st->lasso[0].x, st->lasso[0].y, nullptr);
        for (int i = 1; i < st->lassoN; i++)
            LineTo(dc, st->lasso[i].x, st->lasso[i].y);
        SelectObject(dc, o4); DeleteObject(lp);
    }

    /* selected combo as a connected star shape */
    if (ix && p->selected >= 0) {
        const Combo& c = p->combos[p->selected];
        HPEN star = CreatePen(PS_SOLID, 2, KW_STAR);
        HGDIOBJ o2 = SelectObject(dc, star);
        int px[4], py[4], nPts = 0;
        for (int l = 0; l < c.nLayers; l++) {
            int fi = c.lay[l].fileIdx;
            if (fi < 0 || fi >= (int)ix->files.size()) continue;
            if (ix->files[fi].path != c.lay[l].path) continue;
            px[nPts] = CV_X + (int)(ix->files[fi].cx * CV_W);
            py[nPts] = CV_Y + (int)(ix->files[fi].cy * CV_H);
            nPts++;
        }
        for (int a = 0; a < nPts; a++)
            for (int b = a + 1; b < nPts; b++) {
                MoveToEx(dc, px[a], py[a], nullptr);
                LineTo(dc, px[b], py[b]);
            }
        for (int a = 0; a < nPts; a++)
            Ellipse(dc, px[a] - 4, py[a] - 4, px[a] + 4, py[a] + 4);
        SelectObject(dc, o2); DeleteObject(star);
    }
}

static void drawConsole(HDC dc, Plugin* p)
{
    RECT r = { PAL_X, CON_Y, PAL_X + 3 * PAL_BW + 2 * PAL_GAP, CON_Y + CON_H };
    HBRUSH bb = CreateSolidBrush(KW_PANEL);
    FillRect(dc, &r, bb); DeleteObject(bb);
    HPEN border = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ op = SelectObject(dc, border);
    HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op); SelectObject(dc, obr); DeleteObject(border);

    SetTextColor(dc, KW_CONSOLE);
    int lines = CON_H / 13 - 1;
    if (lines > p->logCount) lines = p->logCount;
    for (int i = 0; i < lines; i++) {
        const std::string& ln = p->logLine(lines - 1 - i);
        RECT tr = { r.left + 6, r.top + 4 + i * 13, r.right - 6, r.top + 17 + i * 13 };
        DrawTextA(dc, ln.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void paintEditor(HWND hwnd, EditorState* st)
{
    Plugin* p = st->p;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, ED_W, ED_H);
    HGDIOBJ ob = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    RECT full = { 0, 0, ED_W, ED_H };
    HBRUSH bg = CreateSolidBrush(KW_BG);
    FillRect(dc, &full, bg); DeleteObject(bg);

    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, 12, 10, "* SAMPLE LIBRARIAN *", 20);

    RECT pb; pathBoxRect(&pb);
    HBRUSH pbb = CreateSolidBrush(KW_PATH_BG);
    FillRect(dc, &pb, pbb); DeleteObject(pbb);
    { HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
      HGDIOBJ o = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, pb.left, pb.top, pb.right, pb.bottom);
      SelectObject(dc, o); SelectObject(dc, obb); DeleteObject(pen); }
    SetTextColor(dc, KW_TEXT);
    std::string shown = p->libPath.empty()
        ? std::string("  (click to choose your library folder)")
        : "  " + p->libPath;
    DrawTextA(dc, shown.c_str(), -1, &pb, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT sc; scanBtnRect(&sc);
    drawTextBtn(dc, sc, p->prog.running.load() ? "..." : "SCAN", KW_BTN_ON);

    char status[160];
    LibIndex* ix = p->indexLive.load();
    if (p->prog.running.load())
        snprintf(status, sizeof(status), "scanning: %d found, %d done, %d kept",
                 p->prog.found.load(), p->prog.done.load(), p->prog.kept.load());
    else if (ix)
        snprintf(status, sizeof(status), "index: %d files", (int)ix->files.size());
    else
        snprintf(status, sizeof(status), "no index - choose a folder and SCAN");
    SetTextColor(dc, KW_CONSOLE);
    TextOutA(dc, 360, 50, status, (int)strlen(status));

    for (int k = 0; k < NSKNOBS; k++) {
        KnobPos kp; settingsKnob(k, &kp);
        char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    RECT tb; tuneBtnRect(&tb);
    bool tk = paramReal(pTuneKey, p->params[pTuneKey]) >= 0.5f;
    drawTextBtn(dc, tb, tk ? "Tune to key: ON" : "Tune to key: OFF",
                tk ? KW_BTN_ON : KW_BTN);

    drawConstellation(dc, st);
    SetTextColor(dc, KW_TEXT_DIM);
    TextOutA(dc, CV_X + 6, CV_Y + CV_H - 16,
             "click a star = audition   |   click-drag = lasso a region",
             57);

    RECT rb; randBtnRect(&rb);
    drawTextBtn(dc, rb, "RANDOMIZE - 12 combos", KW_BTN_ON);
    for (int i = 0; i < 12; i++) {
        RECT r; palBtnRect(i, &r);
        const Combo& c = p->combos[i];
        COLORREF col = c.nLayers ? (i == p->selected ? RGB(200, 90, 150)
                                                     : RGB(78, 50, 74))
                                 : RGB(48, 32, 48);
        HBRUSH b = CreateSolidBrush(col);
        FillRect(dc, &r, b); DeleteObject(b);
        { HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
          HGDIOBJ o = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
          Rectangle(dc, r.left, r.top, r.right, r.bottom);
          SelectObject(dc, o); SelectObject(dc, obb); DeleteObject(pen); }
        char t1[32];
        if (c.nLayers)
            snprintf(t1, sizeof(t1), "#%d  %dx  %.0f%%", i + 1, c.nLayers,
                     c.score * 100.0f);
        else
            snprintf(t1, sizeof(t1), "#%d  -", i + 1);
        SetTextColor(dc, KW_TEXT);
        RECT tr = r; tr.bottom = tr.top + 22;
        DrawTextA(dc, t1, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (c.nLayers) {
            SetTextColor(dc, KW_TEXT_DIM);
            for (int l = 0; l < c.nLayers && l < 3; l++) {
                size_t sl = c.lay[l].path.find_last_of("/\\");
                std::string nm = sl == std::string::npos ? c.lay[l].path
                                                         : c.lay[l].path.substr(sl + 1);
                RECT lr = r; lr.top += 22 + l * 15; lr.bottom = lr.top + 15;
                lr.left += 4; lr.right -= 2;
                DrawTextA(dc, nm.c_str(), -1, &lr,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }

    drawConsole(dc, p);

    /* synth + neural row */
    RECT sb2; synthBtnRect(&sb2);
    drawTextBtn(dc, sb2, "SYNTHESIZE", RGB(170, 90, 130));
    RECT se; synExpBtnRect(&se);
    drawTextBtn(dc, se, "EXP SYNTH", p->synthBuf.empty() ? KW_BTN : KW_BTN_ON);
    RECT ng; nnGenBtnRect(&ng);
    drawTextBtn(dc, ng, "GENERATE NN", RGB(150, 90, 180));
    RECT ne; nnExpBtnRect(&ne);
    drawTextBtn(dc, ne, "EXP NN", p->neuralBuf.empty() ? KW_BTN : KW_BTN_ON);
    for (int k = 0; k < NNKNOBS; k++) {
        KnobPos kp; nnKnob(k, &kp);
        char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    char nnStat[64];
    bool nnReady = p->vae.ready || p->neural.ready;
    if (p->vae.ready)
        snprintf(nnStat, sizeof(nnStat), "NN: VAE %dHz", p->vae.sr);
    else if (p->neural.ready)
        snprintf(nnStat, sizeof(nnStat), "NN: RAVE %dHz", p->neural.modelRate);
    else
        snprintf(nnStat, sizeof(nnStat), "NN: %s",
                 (p->vaeTried || p->neuralTried) ? "absent" : "idle");
    SetTextColor(dc, nnReady ? KW_CONSOLE : KW_TEXT_DIM);
    TextOutA(dc, 828, SYN_Y + 12, nnStat, (int)strlen(nnStat));

    /* neural sculpt row: shape + key buttons, tone/motion/focus knobs */
    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, 12, NN2_Y + 26, "SCULPT", 6);
    RECT shr; nnShapeBtnRect(&shr);
    char shv[16]; paramDisplay(p, pNShape, shv);
    char shb[24]; snprintf(shb, sizeof(shb), "Shape: %s", shv);
    drawTextBtn(dc, shr, shb, RGB(120, 80, 150));
    RECT kyr; nnKeyBtnRect(&kyr);
    char kyv[16]; paramDisplay(p, pNKey, kyv);
    char kyb[24]; snprintf(kyb, sizeof(kyb), "Key: %s", kyv);
    drawTextBtn(dc, kyr, kyb, RGB(150, 80, 130));
    for (int k = 0; k < NN2KNOBS; k++) {
        KnobPos kp; nn2Knob(k, &kp);
        char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    SetTextColor(dc, KW_TEXT_DIM);
    TextOutA(dc, 760, NN2_Y + 30, "Shape + Key = melodic sculpt", 28);

    /* layer controls */
    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, 12, LAY_Y + 2, "COMBO LAYERS", 12);
    if (p->selected >= 0) {
        const Combo& c = p->combos[p->selected];
        for (int l = 0; l < 4; l++) {
            int y = LAY_Y + 36 + l * 54;
            SetTextColor(dc, KW_TEXT_DIM);
            if (l < c.nLayers) {
                size_t sl = c.lay[l].path.find_last_of("/\\");
                std::string nm = sl == std::string::npos ? c.lay[l].path
                                                         : c.lay[l].path.substr(sl + 1);
                char line[200];
                snprintf(line, sizeof(line), "L%d  %s  (%+.1f st)", l + 1,
                         nm.c_str(), c.lay[l].semis);
                RECT lr = { 14, y, 552, y + 16 };
                DrawTextA(dc, line, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                char line[16]; snprintf(line, sizeof(line), "L%d  (empty)", l + 1);
                TextOutA(dc, 14, y, line, (int)strlen(line));
            }
            if (l < c.nLayers)
                for (int o = 0; o < PPLAY; o++) {
                    KnobPos kp; layerKnob(l, o, &kp);
                    char val[16]; paramDisplay(p, kp.param, val);
                    drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
                }
        }
    } else {
        SetTextColor(dc, KW_TEXT_DIM);
        TextOutA(dc, 14, LAY_Y + 40,
                 "click a combo in the palette to open its layer controls", 55);
    }

    for (int k = 0; k < NVKNOBS; k++) {
        KnobPos kp; voiceKnob(k, &kp);
        char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    RECT eb; exportBtnRect(&eb);
    drawTextBtn(dc, eb, "EXPORT WAV", KW_BTN_ON);

    BitBlt(hdc, 0, 0, ED_W, ED_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- actions ---- */

static DWORD WINAPI scanThreadProc(LPVOID param)
{
    ((Plugin*)param)->runScan();
    return 0;
}

static void startScan(EditorState* st)
{
    Plugin* p = st->p;
    if (p->libPath.empty()) { p->logf("scan: choose a library folder first"); return; }
    if (p->prog.running.load()) return;
    if (st->scanThread) { CloseHandle(st->scanThread); st->scanThread = nullptr; }
    p->logf("scan started: %s", p->libPath.c_str());
    st->scanThread = CreateThread(nullptr, 0, scanThreadProc, p, 0, nullptr);
}

static void browseFolder(EditorState* st)
{
    char path[MAX_PATH] = { 0 };
    BROWSEINFOA bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = st->hwnd;
    bi.lpszTitle = "Choose your sample library folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl && SHGetPathFromIDListA(pidl, path) && path[0])
        st->p->libPath = path;
    if (pidl) CoTaskMemFree(pidl);
}

static void saveDialogWav(EditorState* st, const char* suggest, bool synth)
{
    Plugin* p = st->p;
    char file[MAX_PATH];
    snprintf(file, sizeof(file), "%s", suggest);
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "wav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) {
        if (synth) p->exportSynth(file);
        else       p->exportCombo(file);
    }
}

static void finishLasso(EditorState* st)
{
    Plugin* p = st->p;
    float px[MAX_LASSO], py[MAX_LASSO];
    for (int i = 0; i < st->lassoN; i++) {
        px[i] = (float)(st->lasso[i].x - CV_X) / CV_W;
        py[i] = (float)(st->lasso[i].y - CV_Y) / CV_H;
    }
    p->lassoSelect(px, py, st->lassoN);
}

static void onLDown(EditorState* st, int x, int y)
{
    Plugin* p = st->p;
    RECT r;

    pathBoxRect(&r);
    if (inRect(r, x, y)) { browseFolder(st); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    scanBtnRect(&r);
    if (inRect(r, x, y)) { startScan(st); return; }
    tuneBtnRect(&r);
    if (inRect(r, x, y)) {
        bool tk = paramReal(pTuneKey, p->params[pTuneKey]) >= 0.5f;
        p->setParamFromUI(pTuneKey, tk ? 0.0f : 1.0f);
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }
    randBtnRect(&r);
    if (inRect(r, x, y)) { p->randomize(); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    synthBtnRect(&r);
    if (inRect(r, x, y)) { p->synthesize(); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    synExpBtnRect(&r);
    if (inRect(r, x, y)) { saveDialogWav(st, "synth_oneshot.wav", true); return; }
    nnGenBtnRect(&r);
    if (inRect(r, x, y)) { p->neuralGenerate(); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    nnExpBtnRect(&r);
    if (inRect(r, x, y)) {
        if (!p->neuralBuf.empty()) {
            char file[MAX_PATH];
            snprintf(file, sizeof(file), "neural_oneshot.wav");
            OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = st->hwnd;
            ofn.lpstrFilter = "WAV files\0*.wav\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = "wav";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (GetSaveFileNameA(&ofn)) p->exportNeural(file);
        } else p->logf("neural export: nothing generated yet");
        return;
    }
    exportBtnRect(&r);
    if (inRect(r, x, y)) {
        char sug[32]; snprintf(sug, sizeof(sug), "combo_%02d.wav", p->selected + 1);
        if (p->selected >= 0) saveDialogWav(st, sug, false);
        return;
    }

    for (int i = 0; i < 12; i++) {
        palBtnRect(i, &r);
        if (inRect(r, x, y)) {
            if (p->combos[i].nLayers) p->selectCombo(i);
            InvalidateRect(st->hwnd, nullptr, FALSE); return;
        }
    }

    /* neural sculpt buttons: Shape cycles 4 states, Key cycles 13 (Off + notes) */
    nnShapeBtnRect(&r);
    if (inRect(r, x, y)) {
        int s = ((int)paramReal(pNShape, p->params[pNShape]) + 1) % NN_SHAPE_COUNT;
        p->setParamFromUI(pNShape, (s + 0.5f) / (float)NN_SHAPE_COUNT);
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }
    nnKeyBtnRect(&r);
    if (inRect(r, x, y)) {
        int key = ((int)paramReal(pNKey, p->params[pNKey]) + 1) % 13;
        p->setParamFromUI(pNKey, (key + 0.5f) / 13.0f);
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }

    /* constellation: press starts either a click-audition or a lasso */
    RECT cv = { CV_X, CV_Y, CV_X + CV_W, CV_Y + CV_H };
    if (inRect(cv, x, y)) {
        st->mouseDownInMap = true;
        st->lassoActive = false;
        st->downX = x; st->downY = y;
        st->lassoN = 0;
        return;
    }

    /* knobs */
    KnobPos kp;
    int nBase = NSKNOBS + NVKNOBS + NNKNOBS + NN2KNOBS;
    for (int k = 0; k < nBase + 4 * PPLAY; k++) {
        if (k < NSKNOBS) settingsKnob(k, &kp);
        else if (k < NSKNOBS + NVKNOBS) voiceKnob(k - NSKNOBS, &kp);
        else if (k < NSKNOBS + NVKNOBS + NNKNOBS) nnKnob(k - NSKNOBS - NVKNOBS, &kp);
        else if (k < nBase) nn2Knob(k - NSKNOBS - NVKNOBS - NNKNOBS, &kp);
        else {
            int j = k - nBase;
            layerKnob(j / PPLAY, j % PPLAY, &kp);
        }
        if ((x - kp.cx) * (x - kp.cx) + (y - kp.cy) * (y - kp.cy) <=
            (KNOB_R + 4) * (KNOB_R + 4)) {
            st->dragParam = kp.param;
            st->dragStartY = y;
            st->dragStartVal = p->params[kp.param];
            return;
        }
    }
}

static void onMove(EditorState* st, int x, int y)
{
    if (st->mouseDownInMap) {
        int dx = x - st->downX, dy = y - st->downY;
        if (!st->lassoActive && dx * dx + dy * dy > 36) {
            st->lassoActive = true;
            st->lasso[0].x = st->downX; st->lasso[0].y = st->downY;
            st->lassoN = 1;
        }
        if (st->lassoActive && st->lassoN < MAX_LASSO) {
            POINT& last = st->lasso[st->lassoN - 1];
            int mx = x - (int)last.x, my = y - (int)last.y;
            if (mx * mx + my * my >= 9) {
                st->lasso[st->lassoN].x = x;
                st->lasso[st->lassoN].y = y;
                st->lassoN++;
            }
        }
        InvalidateRect(st->hwnd, nullptr, FALSE);
        return;
    }
    if (st->dragParam < 0) return;
    float v = st->dragStartVal + (st->dragStartY - y) / 200.0f;
    st->p->setParamFromUI(st->dragParam, v);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static void onLUp(EditorState* st, int x, int y)
{
    Plugin* p = st->p;
    if (st->mouseDownInMap) {
        if (st->lassoActive && st->lassoN >= 3) {
            finishLasso(st);
        } else {
            float cx = (float)(x - CV_X) / CV_W;
            float cy = (float)(y - CV_Y) / CV_H;
            int fi = p->nearestFile(cx, cy, 0.035f);
            if (fi >= 0) p->auditionFile(fi);
        }
        st->mouseDownInMap = false;
        st->lassoActive = false;
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }
    st->dragParam = -1;
}

static LRESULT CALLBACK LibEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState* st = (EditorState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        SetTimer(hwnd, 1, 50, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (st) {
            Plugin* p = st->p;
            bool running = p->prog.running.load();
            if (running && ++st->tick % 40 == 0)          /* every ~2 s */
                p->logf("scan: %d done / %d found, %d kept",
                        p->prog.done.load(), p->prog.found.load(),
                        p->prog.kept.load());
            if (st->prevRunning && !running) {
                LibIndex* ix = p->indexLive.load();
                p->logf("scan complete: %d files indexed",
                        ix ? (int)ix->files.size() : 0);
            }
            st->prevRunning = running;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        if (st) paintEditor(hwnd, st);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (st) { SetCapture(hwnd); onLDown(st, (short)LOWORD(lp), (short)HIWORD(lp)); }
        return 0;
    case WM_MOUSEMOVE:
        if (st) onMove(st, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        if (st) onLUp(st, (short)LOWORD(lp), (short)HIWORD(lp));
        ReleaseCapture();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void ensureClass()
{
    static bool done = false;
    if (done) return;
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = LibEditorProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "SamplaLibrarianWnd";
    RegisterClassA(&wc);
    done = true;
}

static ERect* editorRect() { return &g_rect; }

static bool editorOpen(Plugin* p, void* parent)
{
    if (!parent) return false;
    ensureClass();
    EditorState* st = new EditorState();
    st->p = p;
    HWND h = CreateWindowExA(0, "SamplaLibrarianWnd", "",
                             WS_CHILD | WS_VISIBLE, 0, 0, ED_W, ED_H,
                             (HWND)parent, nullptr, g_hInst, st);
    if (!h) { delete st; return false; }
    st->hwnd = h;
    p->editor = h;
    p->logf("editor opened");
    return true;
}

static void editorClose(Plugin* p)
{
    if (!p || !p->editor) return;
    HWND h = (HWND)p->editor;
    EditorState* st = (EditorState*)GetWindowLongPtr(h, GWLP_USERDATA);
    KillTimer(h, 1);
    DestroyWindow(h);
    if (st) {
        if (st->scanThread) {
            p->prog.cancel.store(true);
            WaitForSingleObject(st->scanThread, 5000);
            CloseHandle(st->scanThread);
            p->prog.cancel.store(false);
        }
        if (st->constBmp) DeleteObject(st->constBmp);
        delete st;
    }
    p->editor = nullptr;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = inst;
        char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(inst, path, MAX_PATH)) {
            char* sl = strrchr(path, '\\');
            if (sl) { *sl = 0; g_moduleDir = path; }
        }
    }
    return TRUE;
}

#else  /* !_WIN32 — headless build for CI */

struct ERect { int16_t top, left, bottom, right; };
static ERect g_rect = { 0, 0, 920, 1000 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}

#endif /* _WIN32 */

#endif /* LIB_EDITOR_H */
