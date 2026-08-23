/*
 * editor.h — Win32/GDI editor for Spectral Split.
 *
 * A single big SPLIT knob (atonal <-> tonal) is the hero, its arc shifting from
 * a warm amber on the atonal side to a cool cyan on the tonal side. Two small
 * knobs (Mix, Output) sit below. Dark, minimal, single-purpose. ASCII strings.
 */
#ifndef SS_EDITOR_H
#define SS_EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <math.h>

struct ERect { int16_t top, left, bottom, right; };

/* ---- dark palette ---- */
#define DK_BG      RGB(18, 18, 22)
#define DK_PANEL   RGB(28, 29, 36)
#define DK_BORDER  RGB(54, 56, 68)
#define DK_TEXT    RGB(214, 218, 228)
#define DK_DIM     RGB(128, 132, 146)
#define DK_KNOB    RGB(38, 40, 50)
#define DK_KNOBHI  RGB(48, 51, 63)
#define DK_RIM     RGB(70, 74, 90)
#define DK_TONAL   RGB(80, 200, 210)    /* cool  */
#define DK_ATONAL  RGB(232, 150, 70)    /* warm  */
#define DK_IND     RGB(236, 240, 248)

static const int ED_W = 460;
static const int ED_H = 460;

/* big knob */
static const int BK_CX = 230, BK_CY = 210, BK_R = 104;
/* small knobs */
static const int SK_R = 26;

static ERect     g_rect = { 0, 0, (int16_t)ED_H, (int16_t)ED_W };
static HINSTANCE g_hInst = nullptr;

struct EditorState {
    Plugin* p = nullptr;
    HWND hwnd = nullptr;
    int  dragParam = -1, dragStartY = 0; float dragStartVal = 0;
};

static inline void smallKnobPos(int k, int* cx, int* cy)
{ *cx = (k == 0) ? 150 : 310; *cy = 386; }
static inline bool inRect(const RECT& r, int x, int y)
{ return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

/* blend two colors by t (0..1) */
static COLORREF blend(COLORREF a, COLORREF b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    return RGB((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t));
}

/* ---- the hero split knob: thick arc, warm->cool, value in the middle ---- */
static void drawBigKnob(HDC dc, Plugin* p)
{
    float val = p->params[pSplit];                 /* 0..1 */
    /* backing plate */
    HBRUSH plate = CreateSolidBrush(DK_PANEL);
    HPEN plp = CreatePen(PS_SOLID, 1, DK_BORDER);
    HGDIOBJ ob = SelectObject(dc, plate), op = SelectObject(dc, plp);
    Ellipse(dc, BK_CX - BK_R - 12, BK_CY - BK_R - 12, BK_CX + BK_R + 12, BK_CY + BK_R + 12);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(plate); DeleteObject(plp);

    /* value arc from 225deg sweeping 270deg, colored per position */
    const double a0 = 0.75 * 3.14159265358979;     /* start (lower-left)  */
    const double sweep = 1.5 * 3.14159265358979;   /* 270 deg             */
    int steps = 96;
    int arcR = BK_R + 2;
    for (int i = 0; i < steps; i++) {
        double f0 = (double)i / steps;
        double ang = a0 + sweep * f0;
        bool lit = f0 <= val;
        COLORREF c = lit ? blend(DK_ATONAL, DK_TONAL, (float)f0) : RGB(44, 46, 56);
        HPEN pen = CreatePen(PS_SOLID, 6, c);
        HGDIOBJ o = SelectObject(dc, pen);
        int x0 = BK_CX + (int)(cos(ang) * (arcR - 6));
        int y0 = BK_CY + (int)(sin(ang) * (arcR - 6));
        int x1 = BK_CX + (int)(cos(ang) * arcR);
        int y1 = BK_CY + (int)(sin(ang) * arcR);
        MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1);
        SelectObject(dc, o); DeleteObject(pen);
    }

    /* knob body */
    HBRUSH kb = CreateSolidBrush(DK_KNOB); HPEN rim = CreatePen(PS_SOLID, 2, DK_RIM);
    HGDIOBJ o2 = SelectObject(dc, kb), o3 = SelectObject(dc, rim);
    Ellipse(dc, BK_CX - BK_R, BK_CY - BK_R, BK_CX + BK_R, BK_CY + BK_R);
    SelectObject(dc, o3); SelectObject(dc, o2); DeleteObject(kb); DeleteObject(rim);

    /* pointer */
    double ang = a0 + sweep * val;
    COLORREF pc = blend(DK_ATONAL, DK_TONAL, val);
    HPEN ind = CreatePen(PS_SOLID, 4, pc); HGDIOBJ oi = SelectObject(dc, ind);
    MoveToEx(dc, BK_CX + (int)(cos(ang) * (BK_R * 0.30)),
             BK_CY + (int)(sin(ang) * (BK_R * 0.30)), nullptr);
    LineTo(dc, BK_CX + (int)(cos(ang) * (BK_R - 14)),
           BK_CY + (int)(sin(ang) * (BK_R - 14)));
    SelectObject(dc, oi); DeleteObject(ind);

    /* centre readout */
    char v[16]; paramDisplay(p, pSplit, v);
    HFONT big = CreateFontA(34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ of = SelectObject(dc, big);
    SetTextColor(dc, pc);
    RECT vr = { BK_CX - BK_R, BK_CY - 20, BK_CX + BK_R, BK_CY + 20 };
    DrawTextA(dc, v, -1, &vr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(big);

    /* end labels */
    SetTextColor(dc, DK_ATONAL); TextOutA(dc, BK_CX - BK_R - 6, BK_CY + BK_R - 6, "ATONAL", 6);
    SetTextColor(dc, DK_TONAL);  TextOutA(dc, BK_CX + BK_R - 40, BK_CY + BK_R - 6, "TONAL", 5);
}

static void drawSmallKnob(HDC dc, int cx, int cy, float val, const char* label, const char* value)
{
    HBRUSH kb = CreateSolidBrush(DK_KNOBHI); HPEN rim = CreatePen(PS_SOLID, 2, DK_RIM);
    HGDIOBJ ob = SelectObject(dc, kb), op = SelectObject(dc, rim);
    Ellipse(dc, cx - SK_R, cy - SK_R, cx + SK_R, cy + SK_R);
    double a = (0.75 + (double)val * 1.5) * 3.14159265358979;
    HPEN ind = CreatePen(PS_SOLID, 2, DK_IND); SelectObject(dc, ind);
    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, cx + (int)(cos(a)*(SK_R-4)), cy + (int)(sin(a)*(SK_R-4)));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(kb); DeleteObject(rim); DeleteObject(ind);
    SetTextColor(dc, DK_DIM); RECT tl = { cx-40, cy-SK_R-16, cx+40, cy-SK_R-2 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, DK_TEXT); RECT vl = { cx-40, cy+SK_R+2, cx+40, cy+SK_R+16 };
    DrawTextA(dc, value, -1, &vl, DT_CENTER | DT_SINGLELINE);
}

static void paintEditor(HWND hwnd, EditorState* st)
{
    Plugin* p = st->p;
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    HDC dc = CreateCompatibleDC(hdc); HBITMAP bmp = CreateCompatibleBitmap(hdc, ED_W, ED_H);
    HGDIOBJ ob = SelectObject(dc, bmp); SetBkMode(dc, TRANSPARENT);
    RECT full = { 0, 0, ED_W, ED_H }; HBRUSH bgb = CreateSolidBrush(DK_BG);
    FillRect(dc, &full, bgb); DeleteObject(bgb);

    SetTextColor(dc, DK_TEXT); TextOutA(dc, 20, 22, "SPECTRAL SPLIT", 14);
    SetTextColor(dc, DK_DIM);  TextOutA(dc, 20, 42, "tonal / atonal separator", 24);
    SetTextColor(dc, DK_DIM);  TextOutA(dc, 320, 42, "4096-pt", 7);

    drawBigKnob(dc, p);

    static const int sp[2] = { pMix, pOutput };
    static const char* nm[2] = { "Mix", "Output" };
    for (int k = 0; k < 2; k++) {
        int cx, cy; smallKnobPos(k, &cx, &cy);
        char v[16]; paramDisplay(p, sp[k], v);
        drawSmallKnob(dc, cx, cy, p->params[sp[k]], nm[k], v);
    }

    SetTextColor(dc, DK_DIM);
    TextOutA(dc, 20, ED_H - 24, "drag knobs vertically  |  centre = full-range bypass", 52);

    BitBlt(hdc, 0, 0, ED_W, ED_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static void onLDown(EditorState* st, int x, int y)
{
    Plugin* p = st->p;
    int dx = x - BK_CX, dy = y - BK_CY;
    if (dx * dx + dy * dy <= (BK_R + 12) * (BK_R + 12)) {
        st->dragParam = pSplit; st->dragStartY = y; st->dragStartVal = p->params[pSplit]; return;
    }
    static const int sp[2] = { pMix, pOutput };
    for (int k = 0; k < 2; k++) { int cx, cy; smallKnobPos(k, &cx, &cy);
        if ((x-cx)*(x-cx) + (y-cy)*(y-cy) <= (SK_R+6)*(SK_R+6)) {
            st->dragParam = sp[k]; st->dragStartY = y; st->dragStartVal = p->params[sp[k]]; return;
        } }
}
static void onMove(EditorState* st, int y)
{
    if (st->dragParam < 0) return;
    float range = (st->dragParam == pSplit) ? 260.0f : 200.0f;
    float v = st->dragStartVal + (st->dragStartY - y) / range;
    st->p->setParamFromUI(st->dragParam, v);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK SsEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState* st = (EditorState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams); return 0; }
    case WM_PAINT: if (st) paintEditor(hwnd, st); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: if (st) { SetCapture(hwnd); onLDown(st, (short)LOWORD(lp), (short)HIWORD(lp)); } return 0;
    case WM_MOUSEMOVE: if (st) onMove(st, (short)HIWORD(lp)); return 0;
    case WM_LBUTTONUP: if (st) st->dragParam = -1; ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK:   /* double-click a knob to reset to default */
        if (st) { int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            int dx = x - BK_CX, dy = y - BK_CY;
            if (dx*dx + dy*dy <= (BK_R+12)*(BK_R+12)) st->p->setParamFromUI(pSplit, 0.5f);
            InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void ensureClass()
{
    static bool done = false; if (done) return;
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS | CS_OWNDC; wc.lpfnWndProc = SsEditorProc;
    wc.hInstance = g_hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "SpectralSplitWnd"; RegisterClassA(&wc); done = true;
}
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin* p, void* parent)
{
    if (!parent) return false;
    ensureClass();
    EditorState* st = new EditorState(); st->p = p;
    HWND h = CreateWindowExA(0, "SpectralSplitWnd", "", WS_CHILD | WS_VISIBLE, 0, 0, ED_W, ED_H,
                             (HWND)parent, nullptr, g_hInst, st);
    if (!h) { delete st; return false; }
    st->hwnd = h; p->editor = h; return true;
}
static void editorClose(Plugin* p)
{
    if (!p || !p->editor) return;
    HWND h = (HWND)p->editor;
    EditorState* st = (EditorState*)GetWindowLongPtr(h, GWLP_USERDATA);
    DestroyWindow(h); delete st; p->editor = nullptr;
}
extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) g_hInst = inst;
    return TRUE;
}

#else  /* !_WIN32 headless */
struct ERect { int16_t top, left, bottom, right; };
static ERect g_rect = { 0, 0, 460, 460 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}
#endif

#endif /* SS_EDITOR_H */
