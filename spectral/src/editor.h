/*
 * editor.h — Win32/GDI editor for the Spectral Canvas.
 *
 * Included from plugin.cpp after the Plugin struct is defined. Layout:
 *   top       title + tool palette (Move/Pitch/Smear/CA/Gain/Erase) + Freeze
 *   canvas    the editable spectrogram (log-freq y, time x) as a heatmap,
 *             with op overlays, a marquee, and a sweeping playhead
 *   controls  Mix / Master / Amt / Pitch / Rule / Qual knobs + Bars / Dir /
 *             Cut toggles + Undo / Clear
 *   console   scrolling status log
 *
 * Paint model: drag on the canvas to marquee a region. For Gain/Erase/Pitch/
 * Smear/CA the op is applied on release. For Move, the marquee selects; then
 * drag the selection to a new spot to place it. Edits are non-destructive and
 * re-render every loop (streaming) or immediately (Freeze). ASCII strings only.
 *
 * Pink "kawaii" theme to match the rest of the suite.
 */
#ifndef SC_EDITOR_H
#define SC_EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <math.h>

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
#define KW_BTN_ON    RGB(210, 90, 160)
#define KW_CONSOLE   RGB(140, 235, 200)
#define KW_PLAY      RGB(255, 236, 120)
#define KW_MARQUEE   RGB(255, 255, 180)

static const int ED_W = 1040;
static const int ED_H = 772;
static const int KNOB_R = 14;

/* canvas rect */
static const int CANX = 16, CANY = 92, CANW = 1008, CANH = 408;
static const float LOGK = 8.0f;   /* log-freq compression for the y axis */

static ERect     g_rect = { 0, 0, (int16_t)ED_H, (int16_t)ED_W };
static HINSTANCE g_hInst = nullptr;

/* tool ids match op types where sensible */
enum { T_MOVE = 0, T_PITCH, T_SMEAR, T_CA, T_GAIN, T_ERASE, T_COUNT };
static const char* kToolName[T_COUNT] = { "Move", "Pitch", "Smear", "CA", "Gain", "Erase" };

struct EditorState {
    Plugin* p = nullptr;
    HWND hwnd = nullptr;

    int  tool = T_MOVE;
    /* editor-local tool params (normalized 0..1 where noted) */
    float amtN = 0.5f;     /* gain/smear/ca amount */
    float pitchN = 0.5f;   /* -24..+24 semis */
    float ruleN = 90.0f/255.0f;
    int   smearDir = 0;
    int   cut = 0;

    /* selection + drag */
    bool  hasSel = false;
    Region sel;
    int   dragMode = 0;    /* 0 none, 1 marquee, 2 move-drag, 3 knob */
    float downT = 0, downF = 0, curT = 0, curF = 0;
    int   dragParam = -1;  /* VST param being knob-dragged (>=0) */
    float* dragLocal = nullptr; /* editor-local knob being dragged */
    int   dragStartY = 0; float dragStartVal = 0;

    /* cached spectrogram bitmap */
    HBITMAP specBmp = nullptr;
    uint32_t* specBits = nullptr;
    int   lastGen = -1;
};

/* ---- coordinate maps ---- */
static inline float x2t(int x) { float t = (float)(x - CANX) / CANW; return t < 0 ? 0 : (t > 1 ? 1 : t); }
static inline int   t2x(float t) { return CANX + (int)(t * CANW); }
static inline float y2f(int y)   /* screen y -> normalized bin fraction (log) */
{
    float yf = 1.0f - (float)(y - CANY) / CANH;
    if (yf < 0) yf = 0;
    if (yf > 1) yf = 1;
    return (powf(2.0f, yf * LOGK) - 1.0f) / (powf(2.0f, LOGK) - 1.0f);
}
static inline int f2y(float bf)  /* normalized bin fraction -> screen y */
{
    if (bf < 0) bf = 0;
    if (bf > 1) bf = 1;
    float yf = log2f(bf * (powf(2.0f, LOGK) - 1.0f) + 1.0f) / LOGK;
    return CANY + (int)((1.0f - yf) * CANH);
}

/* ---- knob geometry ---- */
struct KnobPos { int cx, cy; const char* label; int vstParam; float* local; };
static const int NKNOBS = 6;
static void knob(EditorState* st, int k, KnobPos* kp)
{
    static const char* lbl[NKNOBS] = { "Mix", "Master", "Amt", "Pitch", "Rule", "Qual" };
    kp->label = lbl[k]; kp->vstParam = -1; kp->local = nullptr;
    kp->cx = 60 + k * 84; kp->cy = 560;
    switch (k) {
    case 0: kp->vstParam = pMix; break;
    case 1: kp->vstParam = pMaster; break;
    case 2: kp->local = &st->amtN; break;
    case 3: kp->local = &st->pitchN; break;
    case 4: kp->local = &st->ruleN; break;
    case 5: kp->vstParam = pQuality; break;
    }
}
static inline void toolRect(int i, RECT* r) { r->left = 16 + i * 92; r->right = r->left + 84; r->top = 40; r->bottom = 66; }
static inline void freezeRect(RECT* r) { r->left = 592; r->right = 700; r->top = 40; r->bottom = 66; }
static inline void undoRect(RECT* r)   { r->left = 708; r->right = 792; r->top = 40; r->bottom = 66; }
static inline void clearRect(RECT* r)  { r->left = 800; r->right = 884; r->top = 40; r->bottom = 66; }
static inline void barsRect(RECT* r)   { r->left = 892; r->right = 1024; r->top = 40; r->bottom = 66; }
static inline void dirRect(RECT* r)    { r->left = 560; r->right = 700; r->top = 548; r->bottom = 574; }
static inline void cutRect(RECT* r)    { r->left = 710; r->right = 820; r->top = 548; r->bottom = 574; }
static inline bool inRect(const RECT& r, int x, int y)
{ return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

/* ---- drawing helpers ---- */
static COLORREF specColor(float v)
{
    int r, g, b;
    if (v < 0.5f) { float t = v / 0.5f;
        r = (int)(30 + t * (200 - 30)); g = (int)(16 + t * (40 - 16)); b = (int)(34 + t * (140 - 34)); }
    else { float t = (v - 0.5f) / 0.5f;
        r = (int)(200 + t * (255 - 200)); g = (int)(40 + t * (220 - 40)); b = (int)(140 + t * (240 - 140)); }
    return (uint32_t)((r << 16) | (g << 8) | b);
}
static void drawKnob(HDC dc, int cx, int cy, float val, const char* label, const char* value)
{
    HBRUSH kb = CreateSolidBrush(KW_KNOB); HPEN rim = CreatePen(PS_SOLID, 2, KW_KNOB_RIM);
    HGDIOBJ ob = SelectObject(dc, kb), op = SelectObject(dc, rim);
    Ellipse(dc, cx - KNOB_R, cy - KNOB_R, cx + KNOB_R, cy + KNOB_R);
    double a = (0.75 + (double)val * 1.5) * 3.14159265358979;
    HPEN ind = CreatePen(PS_SOLID, 2, KW_KNOB_IND); SelectObject(dc, ind);
    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, cx + (int)(cos(a)*(KNOB_R-3)), cy + (int)(sin(a)*(KNOB_R-3)));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(kb); DeleteObject(rim); DeleteObject(ind);
    SetTextColor(dc, KW_TEXT_DIM); RECT tl = { cx-36, cy-KNOB_R-16, cx+36, cy-KNOB_R-2 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, KW_TEXT); RECT vl = { cx-36, cy+KNOB_R+2, cx+36, cy+KNOB_R+16 };
    DrawTextA(dc, value, -1, &vl, DT_CENTER | DT_SINGLELINE);
}
static void drawBtn(HDC dc, const RECT& r, const char* t, COLORREF bg)
{
    HBRUSH b = CreateSolidBrush(bg); FillRect(dc, &r, b); DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ o = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(pen);
    SetTextColor(dc, KW_TEXT);
    DrawTextA(dc, t, -1, (RECT*)&r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* rebuild the cached spectrogram bitmap from the live render's display image */
static void rebuildSpec(EditorState* st)
{
    Plugin* p = st->p;
    if (!st->specBmp) {
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = CANW; bi.bmiHeader.biHeight = -CANH;   /* top-down */
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        st->specBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS,
                                       (void**)&st->specBits, nullptr, 0);
    }
    if (!st->specBits) return;
    RenderBuf* live = p->liveRender.load();
    if (!live || live->disp.empty()) {
        for (int i = 0; i < CANW * CANH; i++) st->specBits[i] = specColor(0.0f);
        return;
    }
    const float* d = live->disp.data();
    for (int sy = 0; sy < CANH; sy++) {
        float bf = y2f(CANY + sy);
        int fy = (int)(bf * SC_DISP_F); if (fy >= SC_DISP_F) fy = SC_DISP_F - 1;
        for (int sx = 0; sx < CANW; sx++) {
            int tx = (int)((float)sx / CANW * SC_DISP_T); if (tx >= SC_DISP_T) tx = SC_DISP_T - 1;
            st->specBits[sy * CANW + sx] = specColor(d[(size_t)tx * SC_DISP_F + fy]);
        }
    }
}

static COLORREF opTint(int type)
{
    switch (type) {
    case OP_MOVE:  return RGB(120, 200, 255);
    case OP_PITCH: return RGB(255, 200, 120);
    case OP_SMEAR: return RGB(160, 255, 200);
    case OP_CA:    return RGB(255, 150, 240);
    case OP_GAIN:  return RGB(180, 255, 140);
    case OP_ERASE: return RGB(255, 110, 110);
    }
    return RGB(220, 220, 220);
}

static void paintEditor(HWND hwnd, EditorState* st)
{
    Plugin* p = st->p;
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    HDC dc = CreateCompatibleDC(hdc); HBITMAP bmp = CreateCompatibleBitmap(hdc, ED_W, ED_H);
    HGDIOBJ ob = SelectObject(dc, bmp); SetBkMode(dc, TRANSPARENT);
    RECT full = { 0, 0, ED_W, ED_H }; HBRUSH bgb = CreateSolidBrush(KW_BG);
    FillRect(dc, &full, bgb); DeleteObject(bgb);

    SetTextColor(dc, KW_ACCENT); TextOutA(dc, 16, 12, "* SPECTRAL CANVAS *", 19);
    SetTextColor(dc, KW_TEXT_DIM);
    TextOutA(dc, 210, 14, "paint the spectrogram - re-renders every loop  <3", 49);

    /* tool palette */
    for (int i = 0; i < T_COUNT; i++) {
        RECT r; toolRect(i, &r);
        drawBtn(dc, r, kToolName[i], st->tool == i ? KW_BTN_ON : KW_BTN);
    }
    bool freeze = paramReal(pFreeze, p->params[pFreeze]) >= 0.5f;
    RECT fr; freezeRect(&fr);
    char fb[24]; snprintf(fb, sizeof(fb), "Freeze: %s", freeze ? "ON" : "off");
    drawBtn(dc, fr, fb, freeze ? KW_BTN_ON : KW_BTN);
    RECT ur; undoRect(&ur); drawBtn(dc, ur, "Undo", KW_BTN);
    RECT clr; clearRect(&clr); drawBtn(dc, clr, "Clear", KW_BTN);
    RECT br; barsRect(&br);
    char bb2[24]; snprintf(bb2, sizeof(bb2), "Bars: %d", p->bars());
    drawBtn(dc, br, bb2, KW_BTN);

    /* spectrogram */
    if (st->lastGen != p->renderGen.load() || !st->specBmp) {
        rebuildSpec(st); st->lastGen = p->renderGen.load();
    }
    if (st->specBmp) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ om = SelectObject(mem, st->specBmp);
        BitBlt(dc, CANX, CANY, CANW, CANH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, om); DeleteDC(mem);
    }
    { HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
      HGDIOBJ o = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, CANX - 1, CANY - 1, CANX + CANW + 1, CANY + CANH + 1);
      SelectObject(dc, o); SelectObject(dc, obb); DeleteObject(pen); }

    /* op overlays */
    std::vector<Op> local;
    EnterCriticalSection(&p->opsCS); local = p->ops; LeaveCriticalSection(&p->opsCS);
    for (const Op& o : local) {
        int x0 = t2x(o.r.t0), x1 = t2x(o.r.t1);
        int y0 = f2y(o.r.f1), y1 = f2y(o.r.f0);   /* f1 top, f0 bottom */
        HPEN pen = CreatePen(PS_SOLID, 1, opTint(o.type));
        HGDIOBJ oo = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x0, y0, x1, y1);
        if (o.type == OP_MOVE) {                    /* arrow to destination */
            int dx0 = t2x(o.r.t0 + o.dt), dy0 = f2y(o.r.f0 + o.df);
            MoveToEx(dc, (x0 + x1) / 2, (y0 + y1) / 2, nullptr);
            LineTo(dc, dx0 + (x1 - x0) / 2, dy0 - (y1 - y0) / 2);
        }
        SelectObject(dc, oo); SelectObject(dc, obb); DeleteObject(pen);
        SetTextColor(dc, opTint(o.type));
        static const char opLetter[OP_TYPE_COUNT] = { 'G', 'E', 'M', 'P', 'S', 'C' };
        char t[2] = { (o.type >= 0 && o.type < OP_TYPE_COUNT) ? opLetter[o.type] : '?', 0 };
        TextOutA(dc, x0 + 2, y0 + 1, t, 1);
    }

    /* current selection / marquee */
    if (st->hasSel || st->dragMode == 1) {
        Region r = (st->dragMode == 1) ?
            Region{ st->downT < st->curT ? st->downT : st->curT,
                    st->downT < st->curT ? st->curT : st->downT,
                    st->downF < st->curF ? st->downF : st->curF,
                    st->downF < st->curF ? st->curF : st->downF } : st->sel;
        int x0 = t2x(r.t0), x1 = t2x(r.t1), y0 = f2y(r.f1), y1 = f2y(r.f0);
        HPEN pen = CreatePen(PS_DOT, 1, KW_MARQUEE);
        HGDIOBJ oo = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x0, y0, x1, y1);
        SelectObject(dc, oo); SelectObject(dc, obb); DeleteObject(pen);
    }
    /* live move-drag ghost */
    if (st->dragMode == 2 && st->hasSel) {
        float dt = st->curT - st->downT, df = st->curF - st->downF;
        int x0 = t2x(st->sel.t0 + dt), x1 = t2x(st->sel.t1 + dt);
        int y0 = f2y(st->sel.f1 + df), y1 = f2y(st->sel.f0 + df);
        HPEN pen = CreatePen(PS_DASH, 1, opTint(OP_MOVE));
        HGDIOBJ oo = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x0, y0, x1, y1);
        SelectObject(dc, oo); SelectObject(dc, obb); DeleteObject(pen);
    }

    /* playhead */
    float pp = p->playPos.load();
    int px = t2x(pp);
    { HPEN pen = CreatePen(PS_SOLID, 2, KW_PLAY);
      HGDIOBJ o = SelectObject(dc, pen);
      MoveToEx(dc, px, CANY, nullptr); LineTo(dc, px, CANY + CANH);
      SelectObject(dc, o); DeleteObject(pen); }

    /* knobs */
    for (int k = 0; k < NKNOBS; k++) {
        KnobPos kp; knob(st, k, &kp);
        float val; char valStr[16];
        if (kp.vstParam >= 0) { val = p->params[kp.vstParam]; paramDisplay(p, kp.vstParam, valStr); }
        else {
            val = *kp.local;
            if (kp.local == &st->amtN)      snprintf(valStr, sizeof(valStr), "%d", (int)(val * 100 + 0.5f));
            else if (kp.local == &st->pitchN) snprintf(valStr, sizeof(valStr), "%+d", (int)((val - 0.5f) * 48));
            else                              snprintf(valStr, sizeof(valStr), "%d", (int)(val * 255));
        }
        drawKnob(dc, kp.cx, kp.cy, val, kp.label, valStr);
    }
    /* Dir / Cut toggles */
    RECT dr; dirRect(&dr);
    char db[24]; snprintf(db, sizeof(db), "Smear: %s", st->smearDir ? "Blur" : "Freeze");
    drawBtn(dc, dr, db, KW_BTN);
    RECT cr; cutRect(&cr);
    char cb[16]; snprintf(cb, sizeof(cb), "Move: %s", st->cut ? "Cut" : "Copy");
    drawBtn(dc, cr, cb, KW_BTN);
    SetTextColor(dc, KW_TEXT_DIM);
    TextOutA(dc, 840, 554, "tool tip: drag = region; Move = drag it again", 44);

    /* console */
    RECT con = { 16, 600, 1024, 758 };
    HBRUSH cbg = CreateSolidBrush(KW_PANEL); FillRect(dc, &con, cbg); DeleteObject(cbg);
    { HPEN pen = CreatePen(PS_SOLID, 1, KW_BORDER);
      HGDIOBJ o = SelectObject(dc, pen), obb = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, con.left, con.top, con.right, con.bottom);
      SelectObject(dc, o); SelectObject(dc, obb); DeleteObject(pen); }
    SetTextColor(dc, KW_CONSOLE);
    int lines = (con.bottom - con.top) / 14 - 1; if (lines > p->logCount) lines = p->logCount;
    for (int i = 0; i < lines; i++) {
        const std::string& ln = p->logLine(lines - 1 - i);
        RECT tr = { con.left + 8, con.top + 6 + i * 14, con.right - 8, con.top + 20 + i * 14 };
        DrawTextA(dc, ln.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    BitBlt(hdc, 0, 0, ED_W, ED_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- build an op from the current tool + selection ---- */
static void applyTool(EditorState* st, const Region& r)
{
    Plugin* p = st->p;
    Op o; o.r = r; o.r.norm();
    switch (st->tool) {
    case T_GAIN:  o.type = OP_GAIN;  o.amount = st->amtN * 4.0f; break;
    case T_ERASE: o.type = OP_ERASE; break;
    case T_PITCH: o.type = OP_PITCH; o.pitch = (st->pitchN - 0.5f) * 48.0f; break;
    case T_SMEAR: o.type = OP_SMEAR; o.amount = st->amtN; o.smearDir = st->smearDir; break;
    case T_CA:    o.type = OP_CA;    o.amount = st->amtN; o.caRule = (int)(st->ruleN * 255); break;
    default: return;   /* Move handled separately */
    }
    p->addOp(o);
}

static bool inSel(EditorState* st, int x, int y)
{
    if (!st->hasSel) return false;
    float t = x2t(x), f = y2f(y);
    return t >= st->sel.t0 && t <= st->sel.t1 && f >= st->sel.f0 && f <= st->sel.f1;
}

static void onLDown(EditorState* st, int x, int y)
{
    Plugin* p = st->p; RECT r;
    for (int i = 0; i < T_COUNT; i++) { toolRect(i, &r);
        if (inRect(r, x, y)) { st->tool = i; InvalidateRect(st->hwnd, nullptr, FALSE); return; } }
    freezeRect(&r); if (inRect(r, x, y)) {
        bool on = paramReal(pFreeze, p->params[pFreeze]) >= 0.5f;
        p->setParamFromUI(pFreeze, on ? 0.0f : 1.0f); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    undoRect(&r);  if (inRect(r, x, y)) { p->undoOp(); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    clearRect(&r); if (inRect(r, x, y)) { p->clearOps(); st->hasSel = false; InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    barsRect(&r);  if (inRect(r, x, y)) {
        int b = ((int)paramReal(pBars, p->params[pBars]) + 1) % 4;
        p->setParamFromUI(pBars, (b + 0.5f) / 4.0f); InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    dirRect(&r);   if (inRect(r, x, y)) { st->smearDir ^= 1; InvalidateRect(st->hwnd, nullptr, FALSE); return; }
    cutRect(&r);   if (inRect(r, x, y)) { st->cut ^= 1; InvalidateRect(st->hwnd, nullptr, FALSE); return; }

    /* knobs */
    for (int k = 0; k < NKNOBS; k++) { KnobPos kp; knob(st, k, &kp);
        if ((x-kp.cx)*(x-kp.cx) + (y-kp.cy)*(y-kp.cy) <= (KNOB_R+4)*(KNOB_R+4)) {
            st->dragMode = 3; st->dragParam = kp.vstParam; st->dragLocal = kp.local;
            st->dragStartY = y; st->dragStartVal = kp.vstParam >= 0 ? p->params[kp.vstParam] : *kp.local;
            return; } }

    /* canvas */
    if (x >= CANX && x < CANX + CANW && y >= CANY && y < CANY + CANH) {
        st->downT = x2t(x); st->downF = y2f(y);
        st->curT = st->downT; st->curF = st->downF;
        if (st->tool == T_MOVE && inSel(st, x, y)) st->dragMode = 2;   /* move the selection */
        else st->dragMode = 1;                                          /* marquee */
        return;
    }
}

static void onMove(EditorState* st, int x, int y)
{
    if (st->dragMode == 3) {
        float v = st->dragStartVal + (st->dragStartY - y) / 200.0f;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        if (st->dragParam >= 0) st->p->setParamFromUI(st->dragParam, v);
        else if (st->dragLocal) *st->dragLocal = v;
        InvalidateRect(st->hwnd, nullptr, FALSE); return;
    }
    if (st->dragMode == 1 || st->dragMode == 2) {
        st->curT = x2t(x); st->curF = y2f(y);
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }
}

static void onLUp(EditorState* st, int x, int y)
{
    (void)x; (void)y;
    if (st->dragMode == 1) {
        Region r{ st->downT, st->curT, st->downF, st->curF }; r.norm();
        if (r.t1 - r.t0 > 0.003f && r.f1 - r.f0 > 0.003f) {
            st->sel = r; st->hasSel = true;
            if (st->tool != T_MOVE) { applyTool(st, r); }   /* paint tools apply now */
        }
    } else if (st->dragMode == 2 && st->hasSel) {
        float dt = st->curT - st->downT, df = st->curF - st->downF;
        if (fabsf(dt) > 0.002f || fabsf(df) > 0.002f) {
            Op o; o.type = OP_MOVE; o.r = st->sel; o.dt = dt; o.df = df; o.cut = st->cut;
            st->p->addOp(o);
        }
    }
    st->dragMode = 0; st->dragParam = -1; st->dragLocal = nullptr;
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK ScEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState* st = (EditorState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        SetTimer(hwnd, 1, 50, nullptr); return 0; }
    case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_PAINT: if (st) paintEditor(hwnd, st); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: if (st) { SetCapture(hwnd); onLDown(st, (short)LOWORD(lp), (short)HIWORD(lp)); } return 0;
    case WM_MOUSEMOVE: if (st) onMove(st, (short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONUP: if (st) onLUp(st, (short)LOWORD(lp), (short)HIWORD(lp)); ReleaseCapture(); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void ensureClass()
{
    static bool done = false; if (done) return;
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC; wc.lpfnWndProc = ScEditorProc;
    wc.hInstance = g_hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "SpectralCanvasWnd"; RegisterClassA(&wc); done = true;
}
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin* p, void* parent)
{
    if (!parent) return false;
    ensureClass();
    EditorState* st = new EditorState(); st->p = p;
    HWND h = CreateWindowExA(0, "SpectralCanvasWnd", "", WS_CHILD | WS_VISIBLE, 0, 0, ED_W, ED_H,
                             (HWND)parent, nullptr, g_hInst, st);
    if (!h) { delete st; return false; }
    st->hwnd = h; p->editor = h; p->logf("editor opened"); return true;
}
static void editorClose(Plugin* p)
{
    if (!p || !p->editor) return;
    HWND h = (HWND)p->editor;
    EditorState* st = (EditorState*)GetWindowLongPtr(h, GWLP_USERDATA);
    KillTimer(h, 1); DestroyWindow(h);
    if (st) { if (st->specBmp) DeleteObject(st->specBmp); delete st; }
    p->editor = nullptr;
}
extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = inst; char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(inst, path, MAX_PATH)) { char* s = strrchr(path, '\\');
            if (s) { *s = 0; g_moduleDir = path; } }
    }
    return TRUE;
}

#else  /* !_WIN32 headless */
struct ERect { int16_t top, left, bottom, right; };
static ERect g_rect = { 0, 0, 772, 1040 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}
#endif

#endif /* SC_EDITOR_H */
