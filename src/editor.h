/*
 * editor.h — Win32/GDI editor for the Granular Sampler.
 *
 * Included from plugin.cpp *after* the Plugin struct and param helpers are
 * defined, so it can call straight into them. Everything here is Windows-only
 * (the DSP in engine.h stays platform-neutral for the CI smoke test).
 *
 * Layout: three stacked lanes. Each lane has a waveform view (with loop region,
 * draggable loop handles, and a live playhead), a clickable Mode and Play
 * button, and a panel of knobs (Vol, Tune, Overlap, ADSR, Grain size/density).
 * Drop a .wav on a lane — or double-click its waveform — to load a sample.
 */
#ifndef EDITOR_H
#define EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <math.h>

/* VST wants a rect with 16-bit fields. */
struct ERect { int16_t top, left, bottom, right; };

static const int ED_W = 760;
static const int ED_H = 540;
static const int LANE_TOP = 30;
static const int LANE_H   = 170;
static const int WAVE_X   = 14;
static const int WAVE_X2  = 474;
static const int PANEL_X  = 484;
static const int KNOB_R   = 18;

/* the 9 knobs shown per lane, in grid order (5 cols x 2 rows) */
static const int   kKnobOff[9] = {
    oVolume, oTune, oOverlap, oAttack, oDecay, oSustain, oRelease,
    oGrainSize, oDensity
};
static const char* kKnobLbl[9] = {
    "Vol", "Tune", "Ovlp", "Atk", "Dec", "Sus", "Rel", "Grain", "Dens"
};

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
    r->top = laneTopY(l) + 24; r->bottom = laneTopY(l) + 150;
}
static inline void knobCenter(int l, int k, int* cx, int* cy)
{
    int col = k % 5, row = k / 5;
    *cx = PANEL_X + 26 + col * 54;
    *cy = laneTopY(l) + 34 + row * 74;
}
static inline void modeBtnRect(int l, RECT* r)
{
    r->left = 300; r->right = 372;
    r->top = laneTopY(l) + 4; r->bottom = laneTopY(l) + 21;
}
static inline void playBtnRect(int l, RECT* r)
{
    r->left = 380; r->right = 452;
    r->top = laneTopY(l) + 4; r->bottom = laneTopY(l) + 21;
}
static inline bool inRect(const RECT& r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

/* ---- drawing ---- */

static void drawKnob(HDC dc, int cx, int cy, float val, const char* label,
                     const char* value)
{
    HBRUSH kb = CreateSolidBrush(RGB(64, 68, 84));
    HPEN   rim = CreatePen(PS_SOLID, 1, RGB(96, 102, 122));
    HGDIOBJ ob = SelectObject(dc, kb), op = SelectObject(dc, rim);
    Ellipse(dc, cx - KNOB_R, cy - KNOB_R, cx + KNOB_R, cy + KNOB_R);

    double a = (0.75 + (double)val * 1.5) * 3.14159265358979;
    int ex = cx + (int)(cos(a) * (KNOB_R - 3));
    int ey = cy + (int)(sin(a) * (KNOB_R - 3));
    HPEN ind = CreatePen(PS_SOLID, 2, RGB(232, 236, 246));
    SelectObject(dc, ind);
    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, ex, ey);

    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(kb); DeleteObject(rim); DeleteObject(ind);

    SetTextColor(dc, RGB(150, 156, 170));
    RECT tl = { cx - 27, cy - KNOB_R - 15, cx + 27, cy - KNOB_R - 1 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(205, 210, 222));
    RECT vl = { cx - 27, cy + KNOB_R + 1, cx + 27, cy + KNOB_R + 15 };
    DrawTextA(dc, value, -1, &vl, DT_CENTER | DT_SINGLELINE);
}

static void drawLane(HDC dc, Plugin* p, int l)
{
    int top = laneTopY(l);

    /* header: layer tag + filename */
    SetTextColor(dc, RGB(120, 200, 250));
    char tag[8]; snprintf(tag, sizeof(tag), "L%d", l + 1);
    TextOutA(dc, WAVE_X, top + 4, tag, (int)strlen(tag));

    Sample* s = p->engine.layers[l].live.load();
    SetTextColor(dc, RGB(190, 196, 208));
    const char* fname = "— drop a .wav here (or double-click) —";
    std::string base;
    if (s && !s->path.empty()) {
        size_t sl = s->path.find_last_of("/\\");
        base = (sl == std::string::npos) ? s->path : s->path.substr(sl + 1);
        char info[160];
        snprintf(info, sizeof(info), "%s   (%.1fs @ %dHz)",
                 base.c_str(), s->frames / (float)s->srcRate, s->srcRate);
        base = info; fname = base.c_str();
    }
    TextOutA(dc, WAVE_X + 34, top + 4, fname, (int)strlen(fname));

    /* mode + play buttons */
    int base0 = 1 + l * PPL;
    int mode = (int)layerReal(oMode, p->params[base0 + oMode]);
    bool play = layerReal(oPlay, p->params[base0 + oPlay]) >= 0.5f;
    RECT mr, pr; modeBtnRect(l, &mr); playBtnRect(l, &pr);
    HBRUSH bb = CreateSolidBrush(RGB(48, 52, 66));
    FillRect(dc, &mr, bb); FillRect(dc, &pr, bb); DeleteObject(bb);
    SetTextColor(dc, RGB(215, 220, 232));
    char mb[24]; snprintf(mb, sizeof(mb), "Mode: %s", kModeNames[mode & 3]);
    DrawTextA(dc, mb, -1, &mr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    char pb[24]; snprintf(pb, sizeof(pb), "%s", play ? "From start" : "From loop");
    DrawTextA(dc, pb, -1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* waveform background */
    RECT wr; waveRect(l, &wr);
    HBRUSH wb = CreateSolidBrush(RGB(16, 18, 24));
    FillRect(dc, &wr, wb); DeleteObject(wb);
    int waveW = wr.right - wr.left;
    int mid = (wr.top + wr.bottom) / 2;
    int halfH = (wr.bottom - wr.top) / 2 - 2;

    /* loop region shading + handles */
    float ls = p->params[base0 + oLoopStart];
    float le = p->params[base0 + oLoopEnd];
    int lx = wr.left + (int)(ls * waveW);
    int ex = wr.left + (int)(le * waveW);
    RECT loopR = { lx, wr.top, ex, wr.bottom };
    HBRUSH lb = CreateSolidBrush(RGB(30, 44, 70));
    FillRect(dc, &loopR, lb); DeleteObject(lb);

    /* waveform peaks */
    if (s && s->frames > 1) {
        HPEN wp = CreatePen(PS_SOLID, 1, RGB(96, 168, 232));
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

    /* loop handles */
    HPEN hp = CreatePen(PS_SOLID, 2, RGB(240, 200, 90));
    HGDIOBJ oh = SelectObject(dc, hp);
    MoveToEx(dc, lx, wr.top, nullptr); LineTo(dc, lx, wr.bottom);
    MoveToEx(dc, ex, wr.top, nullptr); LineTo(dc, ex, wr.bottom);
    SelectObject(dc, oh); DeleteObject(hp);

    /* playhead */
    float ph = p->engine.layers[l].playhead.load();
    if (ph > 0.0001f) {
        int px = wr.left + (int)(ph * waveW);
        HPEN pp = CreatePen(PS_SOLID, 1, RGB(250, 90, 90));
        HGDIOBJ opp = SelectObject(dc, pp);
        MoveToEx(dc, px, wr.top, nullptr); LineTo(dc, px, wr.bottom);
        SelectObject(dc, opp); DeleteObject(pp);
    }

    /* knobs */
    for (int k = 0; k < 9; k++) {
        int cx, cy; knobCenter(l, k, &cx, &cy);
        int idx = base0 + kKnobOff[k];
        char val[16]; paramDisplay(p, idx, val);
        drawKnob(dc, cx, cy, p->params[idx], kKnobLbl[k], val);
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
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(dc, &full, bg); DeleteObject(bg);

    SetTextColor(dc, RGB(230, 234, 244));
    TextOutA(dc, WAVE_X, 8, "GRANULAR SAMPLER", 16);
    SetTextColor(dc, RGB(140, 146, 160));
    const char* hint = "drag knobs vertically · drag loop edges · double-click wave to load";
    TextOutA(dc, 250, 10, hint, (int)strlen(hint));

    for (int l = 0; l < NUM_LAYERS; l++) drawLane(dc, p, l);

    BitBlt(hdc, 0, 0, ED_W, ED_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- interaction ---- */

static void loadDialog(EditorState* st, int lane)
{
    char file[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
        st->p->loadLayer(lane, file);
}

static int laneAt(int y)
{
    for (int l = 0; l < NUM_LAYERS; l++)
        if (y >= laneTopY(l) && y < laneTopY(l) + LANE_H) return l;
    return -1;
}

static void onLDown(EditorState* st, int x, int y)
{
    Plugin* p = st->p;
    int l = laneAt(y);
    if (l < 0) return;
    int base0 = 1 + l * PPL;

    /* mode / play buttons */
    RECT mr, pr; modeBtnRect(l, &mr); playBtnRect(l, &pr);
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

    /* loop handles (grab within 6px of either edge, inside the wave rect) */
    RECT wr; waveRect(l, &wr);
    if (y >= wr.top && y < wr.bottom) {
        int waveW = wr.right - wr.left;
        int lx = wr.left + (int)(p->params[base0 + oLoopStart] * waveW);
        int ex = wr.left + (int)(p->params[base0 + oLoopEnd] * waveW);
        if (abs(x - lx) <= 6) { st->dragKind = 2; st->dragParam = base0 + oLoopStart; return; }
        if (abs(x - ex) <= 6) { st->dragKind = 3; st->dragParam = base0 + oLoopEnd; return; }
    }

    /* knobs */
    for (int k = 0; k < 9; k++) {
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
        if (st) { SetCapture(hwnd); onLDown(st, LOWORD(lp), HIWORD(lp)); }
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
            int y = HIWORD(lp), l = laneAt(y);
            RECT wr; if (l >= 0) { waveRect(l, &wr);
                if (inRect(wr, (short)LOWORD(lp), y)) loadDialog(st, l); }
        }
        return 0;
    case WM_DROPFILES: {
        if (st) {
            HDROP h = (HDROP)wp;
            POINT pt; DragQueryPoint(h, &pt);
            int l = laneAt(pt.y);
            char file[MAX_PATH];
            if (l >= 0 && DragQueryFileA(h, 0, file, MAX_PATH))
                st->p->loadLayer(l, file);
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
static ERect g_rect = { 0, 0, 540, 760 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}

#endif /* _WIN32 */

#endif /* EDITOR_H */
