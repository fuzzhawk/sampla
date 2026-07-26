/*
 * editor.h — Win32/GDI editor for the Match-Slicer.
 *
 * Included from plugin.cpp after the Plugin struct is defined. Layout:
 *   top      title + EXPORT WAV
 *   guide    waveform of the guide file, with slice boundaries + playhead
 *   main     waveform of the main file, with source regions + onset markers
 *   map      one cell per guide slice, colored by which main region it pulled
 *   controls MATCH button, knobs, guide/main slice + follow toggle buttons
 *   console  scrolling status log
 *
 * Pink "kawaii" theme (dark plum bg, hot-pink + lavender accents). Drop a .wav
 * on the guide or main lane (or double-click to browse). All strings are
 * ASCII (DrawTextA).
 */
#ifndef MS_EDITOR_H
#define MS_EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <math.h>

struct ERect { int16_t top, left, bottom, right; };

/* ---- kawaii palette ---- */
#define KW_BG        RGB(43, 27, 44)    /* deep plum background        */
#define KW_PANEL     RGB(32, 20, 34)    /* wave / console panel        */
#define KW_BORDER    RGB(120, 70, 110)  /* panel borders               */
#define KW_TEXT      RGB(255, 224, 240) /* bright text                 */
#define KW_TEXT_DIM  RGB(200, 150, 185) /* secondary text              */
#define KW_ACCENT    RGB(255, 150, 200) /* hot pink                    */
#define KW_KNOB      RGB(92, 54, 84)    /* knob body                   */
#define KW_KNOB_RIM  RGB(200, 110, 165) /* knob rim                    */
#define KW_KNOB_IND  RGB(255, 214, 238) /* knob indicator              */
#define KW_BTN_ON    RGB(210, 90, 160)  /* active toggle / action      */
#define KW_BTN_OFF   RGB(70, 44, 68)    /* inactive toggle             */
#define KW_WAVE_G    RGB(255, 150, 200) /* guide waveform (pink)       */
#define KW_WAVE_M    RGB(180, 150, 245) /* main waveform (lavender)    */
#define KW_PLAY      RGB(255, 90, 140)  /* playhead                    */
#define KW_SLICE     RGB(255, 208, 130) /* guide slice boundaries      */
#define KW_ONSET     RGB(140, 235, 200) /* main transient markers      */

static const int ED_W = 1000;
static const int ED_H = 700;
static const int KNOB_R = 15;

static ERect     g_rect = { 0, 0, (int16_t)ED_H, (int16_t)ED_W };
static HINSTANCE g_hInst = nullptr;

struct EditorState {
    Plugin* p = nullptr;
    HWND hwnd = nullptr;
    int dragParam = -1, dragStartY = 0; float dragStartVal = 0;
};

/* ---- geometry ---- */
static inline void guideRect(RECT* r) { r->left = 16; r->right = 984; r->top = 56;  r->bottom = 150; }
static inline void mainRect(RECT* r)  { r->left = 16; r->right = 984; r->top = 176; r->bottom = 270; }
static inline void mapRect(RECT* r)   { r->left = 16; r->right = 984; r->top = 286; r->bottom = 316; }
static inline void exportRect(RECT* r){ r->left = 856; r->right = 984; r->top = 12; r->bottom = 40; }
static inline void matchRect(RECT* r) { r->left = 16; r->right = 176; r->top = 334; r->bottom = 374; }
static inline bool inRect(const RECT& r, int x, int y)
{ return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

struct KnobPos { int param, cx, cy; const char* label; };
static const int NKNOBS = 9;
static inline void knob(int k, KnobPos* kp)
{
    static const int prm[NKNOBS] = { pMaster, pMix, pDiv, pBars, pXfade,
                                     pVariation, pSpectralW, pGuideThresh, pMainThresh };
    static const char* lbl[NKNOBS] = { "Master", "Mix", "Div", "Bars", "Xfade",
                                       "Var", "SpecW", "GuideThr", "MainThr" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 128 + k * 96; kp->cy = 424;
}
/* toggle buttons: 0 GuideSlc 1 MainSlc 2 Gain 3 Pitch 4 Fit */
static const int NTOGS = 5;
static inline void togRect(int t, RECT* r)
{ r->left = 16 + t * 192; r->right = r->left + 180; r->top = 470; r->bottom = 494; }

/* ---- drawing helpers ---- */
static void drawKnob(HDC dc, int cx, int cy, float val, const char* label, const char* value)
{
    HBRUSH kb = CreateSolidBrush(KW_KNOB); HPEN rim = CreatePen(PS_SOLID, 2, KW_KNOB_RIM);
    HGDIOBJ ob = SelectObject(dc, kb), op = SelectObject(dc, rim);
    Ellipse(dc, cx - KNOB_R, cy - KNOB_R, cx + KNOB_R, cy + KNOB_R);
    double a = (0.75 + (double)val * 1.5) * 3.14159265358979;
    HPEN ind = CreatePen(PS_SOLID, 2, KW_KNOB_IND); SelectObject(dc, ind);
    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, cx + (int)(cos(a)*(KNOB_R-3)), cy + (int)(sin(a)*(KNOB_R-3)));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(kb); DeleteObject(rim); DeleteObject(ind);
    SetTextColor(dc, KW_TEXT_DIM); RECT tl = { cx-40, cy-KNOB_R-16, cx+40, cy-KNOB_R-2 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, KW_TEXT); RECT vl = { cx-40, cy+KNOB_R+2, cx+40, cy+KNOB_R+16 };
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
static void drawWave(HDC dc, const RECT& r, const std::vector<float>& mono, int frames, COLORREF col)
{
    HBRUSH bg = CreateSolidBrush(KW_PANEL); FillRect(dc, &r, bg); DeleteObject(bg);
    HPEN border = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ o = SelectObject(dc, border); HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom); SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(border);
    if (frames < 2 || mono.empty()) return;
    int w = r.right - r.left, mid = (r.top + r.bottom) / 2, hh = (r.bottom - r.top) / 2 - 2;
    HPEN wp = CreatePen(PS_SOLID, 1, col); HGDIOBJ ow = SelectObject(dc, wp);
    for (int x = 0; x < w; x++) {
        int a = (int)((int64_t)x * frames / w), b = (int)((int64_t)(x + 1) * frames / w);
        if (b <= a) b = a + 1;
        if (b > frames) b = frames;
        float mn = 1e9f, mx = -1e9f;
        for (int i = a; i < b; i += (b - a > 64 ? (b - a) / 64 : 1)) { float v = mono[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
        MoveToEx(dc, r.left + x, mid - (int)(mx * hh), nullptr);
        LineTo(dc, r.left + x, mid - (int)(mn * hh) + 1);
    }
    SelectObject(dc, ow); DeleteObject(wp);
}
/* stable-ish color from a candidate start position */
static COLORREF candColor(int start, int total)
{
    float h = total > 0 ? (float)start / total : 0.0f;
    int r = (int)(170 + 80 * sinf(h * 6.2831853f));
    int g = (int)(110 + 90 * sinf(h * 6.2831853f + 2.09f));
    int b = (int)(170 + 80 * sinf(h * 6.2831853f + 4.18f));
    return RGB(r & 255, g & 255, b & 255);
}

static void paintEditor(HWND hwnd, EditorState* st)
{
    Plugin* p = st->p;
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    HDC dc = CreateCompatibleDC(hdc); HBITMAP bmp = CreateCompatibleBitmap(hdc, ED_W, ED_H);
    HGDIOBJ ob = SelectObject(dc, bmp); SetBkMode(dc, TRANSPARENT);
    RECT full = { 0, 0, ED_W, ED_H }; HBRUSH bgb = CreateSolidBrush(KW_BG);
    FillRect(dc, &full, bgb); DeleteObject(bgb);

    SetTextColor(dc, KW_ACCENT); TextOutA(dc, 16, 12, "* MATCH SLICER *", 16);
    SetTextColor(dc, KW_TEXT_DIM);
    TextOutA(dc, 180, 15, "guide's groove, main's sound  <3", 32);
    RECT er; exportRect(&er); drawBtn(dc, er, "EXPORT WAV",
        p->eng.live.load() ? KW_BTN_ON : KW_BTN_OFF);

    Slicer& s = p->eng;
    int gframes = s.guide.frames, mframes = s.main.frames;

    /* guide waveform + slice boundaries + playhead */
    RECT gr; guideRect(&gr);
    drawWave(dc, gr, s.guide.mono, gframes, KW_WAVE_G);
    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, gr.left, gr.top - 16, gframes > 1 ? "GUIDE" :
             "GUIDE  - drop a .wav (or double-click) -", gframes > 1 ? 5 : 40);
    if (gframes > 1 && !s.slices.empty()) {
        int w = gr.right - gr.left;
        HPEN sp = CreatePen(PS_SOLID, 1, KW_SLICE); HGDIOBJ o = SelectObject(dc, sp);
        for (auto& gs : s.slices) {
            int x = gr.left + (int)((int64_t)gs.start * w / gframes);
            MoveToEx(dc, x, gr.top, nullptr); LineTo(dc, x, gr.bottom);
        }
        SelectObject(dc, o); DeleteObject(sp);
    }

    /* main waveform + used regions tinted + onset markers */
    RECT mr; mainRect(&mr);
    drawWave(dc, mr, s.main.mono, mframes, KW_WAVE_M);
    SetTextColor(dc, KW_ACCENT);
    TextOutA(dc, mr.left, mr.top - 16, mframes > 1 ? "MAIN" :
             "MAIN  - drop a .wav (or double-click) -", mframes > 1 ? 4 : 39);
    /* main transient markers (only meaningful in main-transient mode) */
    if (mframes > 1 && !s.mainOnsets.empty() &&
        paramReal(pMainMode, p->params[pMainMode]) >= 0.5f) {
        int w = mr.right - mr.left;
        HPEN op = CreatePen(PS_SOLID, 1, KW_ONSET); HGDIOBJ o = SelectObject(dc, op);
        for (int on : s.mainOnsets) {
            int x = mr.left + (int)((int64_t)on * w / mframes);
            MoveToEx(dc, x, mr.top + 1, nullptr); LineTo(dc, x, mr.top + 9);
        }
        SelectObject(dc, o); DeleteObject(op);
    }
    if (mframes > 1 && !s.match.empty() && !s.cands.empty()) {
        int w = mr.right - mr.left, win = 0;
        { int a = 0; for (auto& gs : s.slices) a += gs.len; if (!s.slices.empty()) win = a / (int)s.slices.size(); }
        for (size_t i = 0; i < s.match.size(); i++) {
            int cs = s.cands[s.match[i]].start;
            int x0 = mr.left + (int)((int64_t)cs * w / mframes);
            int x1 = mr.left + (int)((int64_t)(cs + win) * w / mframes);
            RECT hb = { x0, mr.bottom - 6, x1 > x0 ? x1 : x0 + 1, mr.bottom - 1 };
            HBRUSH hbr = CreateSolidBrush(candColor(cs, mframes));
            FillRect(dc, &hb, hbr); DeleteObject(hbr);
        }
    }

    /* mapping strip: one cell per guide slice, colored by its source region */
    RECT mp; mapRect(&mp);
    HBRUSH mbg = CreateSolidBrush(KW_PANEL); FillRect(dc, &mp, mbg); DeleteObject(mbg);
    if (!s.slices.empty() && !s.match.empty() && gframes > 1) {
        int w = mp.right - mp.left;
        for (size_t i = 0; i < s.slices.size(); i++) {
            int x0 = mp.left + (int)((int64_t)s.slices[i].start * w / gframes);
            int end = (i + 1 < s.slices.size()) ? s.slices[i + 1].start : gframes;
            int x1 = mp.left + (int)((int64_t)end * w / gframes);
            RECT c = { x0, mp.top + 1, x1 - 1 > x0 ? x1 - 1 : x0 + 1, mp.bottom - 1 };
            HBRUSH cb = CreateSolidBrush(candColor(s.cands[s.match[i]].start, mframes));
            FillRect(dc, &c, cb); DeleteObject(cb);
        }
    }
    /* playhead across guide + map */
    float pp = s.playPos.load();
    if (s.live.load()) {
        int gx = gr.left + (int)(pp * (gr.right - gr.left));
        int mpx = mp.left + (int)(pp * (mp.right - mp.left));
        HPEN php = CreatePen(PS_SOLID, 2, KW_PLAY); HGDIOBJ o = SelectObject(dc, php);
        MoveToEx(dc, gx, gr.top, nullptr); LineTo(dc, gx, gr.bottom);
        MoveToEx(dc, mpx, mp.top, nullptr); LineTo(dc, mpx, mp.bottom);
        SelectObject(dc, o); DeleteObject(php);
    }

    /* MATCH button */
    RECT mtr; matchRect(&mtr);
    drawBtn(dc, mtr, s.haveBoth() ? "MATCH <3" : "MATCH (load both)",
            s.haveBoth() ? KW_BTN_ON : KW_BTN_OFF);
    SetTextColor(dc, KW_TEXT_DIM);
    { char info[96];
      if (!s.slices.empty()) snprintf(info, sizeof(info), "%d slices  ->  %d candidates",
                                      (int)s.slices.size(), (int)s.cands.size());
      else snprintf(info, sizeof(info), "load a guide + main, then MATCH");
      TextOutA(dc, 192, 346, info, (int)strlen(info)); }

    /* knobs */
    for (int k = 0; k < NKNOBS; k++) {
        KnobPos kp; knob(k, &kp); char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    /* toggles */
    static const int tprm[NTOGS] = { pSliceMode, pMainMode, pGainFollow, pPitchMatch, pStretchFit };
    static const char* tname[NTOGS] = { "GuideSlc", "MainSlc", "GainFollow", "PitchMatch", "StretchFit" };
    for (int t = 0; t < NTOGS; t++) {
        RECT r; togRect(t, &r); char v[16]; paramDisplay(p, tprm[t], v);
        char lbl[40]; snprintf(lbl, sizeof(lbl), "%s: %s", tname[t], v);
        bool on = paramReal(tprm[t], p->params[tprm[t]]) >= 0.5f;
        drawBtn(dc, r, lbl, on ? KW_BTN_ON : KW_BTN_OFF);
    }

    /* console */
    RECT con = { 16, 512, 984, 688 };
    HBRUSH cbg = CreateSolidBrush(KW_PANEL); FillRect(dc, &con, cbg); DeleteObject(cbg);
    HPEN cpen = CreatePen(PS_SOLID, 1, KW_BORDER);
    HGDIOBJ co = SelectObject(dc, cpen), cob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, con.left, con.top, con.right, con.bottom);
    SelectObject(dc, co); SelectObject(dc, cob); DeleteObject(cpen);
    SetTextColor(dc, KW_ONSET);
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

/* ---- actions ---- */
static bool browse(EditorState* st, char* out)
{
    out[0] = 0;
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = out; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) != 0;
}
static void saveWav(EditorState* st)
{
    Plugin* p = st->p;
    char file[MAX_PATH]; snprintf(file, sizeof(file), "mosaic.wav");
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0"; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "wav"; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) p->exportWav(file);
}

static void onLDown(EditorState* st, int x, int y)
{
    Plugin* p = st->p; RECT r;
    exportRect(&r); if (inRect(r, x, y)) { saveWav(st); return; }
    matchRect(&r);  if (inRect(r, x, y)) { p->rematch(); InvalidateRect(st->hwnd, nullptr, FALSE); return; }

    /* toggle buttons */
    static const int tprm[NTOGS] = { pSliceMode, pMainMode, pGainFollow, pPitchMatch, pStretchFit };
    for (int t = 0; t < NTOGS; t++) { togRect(t, &r);
        if (inRect(r, x, y)) {
            bool on = paramReal(tprm[t], p->params[tprm[t]]) >= 0.5f;
            p->setParamFromUI(tprm[t], on ? 0.0f : 1.0f);
            if (t == 0 || t == 1) p->rematch();   /* slice modes change the pattern */
            InvalidateRect(st->hwnd, nullptr, FALSE); return;
        } }

    /* knobs */
    for (int k = 0; k < NKNOBS; k++) { KnobPos kp; knob(k, &kp);
        if ((x-kp.cx)*(x-kp.cx) + (y-kp.cy)*(y-kp.cy) <= (KNOB_R+4)*(KNOB_R+4)) {
            st->dragParam = kp.param; st->dragStartY = y; st->dragStartVal = p->params[kp.param];
            return;
        } }
}
static void onLUp(EditorState* st)
{
    Plugin* p = st->p;
    if (st->dragParam >= 0) {
        /* params that change the slicing/matching -> rematch */
        if (st->dragParam == pDiv || st->dragParam == pBars ||
            st->dragParam == pXfade || st->dragParam == pVariation ||
            st->dragParam == pSpectralW || st->dragParam == pGuideThresh ||
            st->dragParam == pMainThresh)
            p->rematch();
    }
    st->dragParam = -1;
}
static void onMove(EditorState* st, int y)
{
    if (st->dragParam < 0) return;
    float v = st->dragStartVal + (st->dragStartY - y) / 200.0f;
    st->p->setParamFromUI(st->dragParam, v);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK MsEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorState* st = (EditorState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        DragAcceptFiles(hwnd, TRUE); SetTimer(hwnd, 1, 50, nullptr); return 0; }
    case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_PAINT: if (st) paintEditor(hwnd, st); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: if (st) { SetCapture(hwnd); onLDown(st, (short)LOWORD(lp), (short)HIWORD(lp)); } return 0;
    case WM_MOUSEMOVE: if (st) onMove(st, (short)HIWORD(lp)); return 0;
    case WM_LBUTTONUP: if (st) onLUp(st); ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK:
        if (st) { int x = (short)LOWORD(lp), y = (short)HIWORD(lp); RECT gr, mr;
            guideRect(&gr); mainRect(&mr); char f[MAX_PATH];
            if (inRect(gr, x, y) && browse(st, f)) { st->p->loadGuide(f); st->p->rematch(); }
            else if (inRect(mr, x, y) && browse(st, f)) { st->p->loadMain(f); st->p->rematch(); }
            InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_DROPFILES: {
        if (st) { HDROP h = (HDROP)wp; POINT pt; DragQueryPoint(h, &pt); char f[MAX_PATH];
            if (DragQueryFileA(h, 0, f, MAX_PATH)) {
                RECT gr, mr; guideRect(&gr); mainRect(&mr);
                if (pt.y < gr.bottom) st->p->loadGuide(f);
                else st->p->loadMain(f);
                st->p->rematch();
            }
            DragFinish(h); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0; }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void ensureClass()
{
    static bool done = false; if (done) return;
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS | CS_OWNDC; wc.lpfnWndProc = MsEditorProc;
    wc.hInstance = g_hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "MatchSlicerWnd"; RegisterClassA(&wc); done = true;
}
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin* p, void* parent)
{
    if (!parent) return false;
    ensureClass();
    EditorState* st = new EditorState(); st->p = p;
    HWND h = CreateWindowExA(0, "MatchSlicerWnd", "", WS_CHILD | WS_VISIBLE, 0, 0, ED_W, ED_H,
                             (HWND)parent, nullptr, g_hInst, st);
    if (!h) { delete st; return false; }
    st->hwnd = h; p->editor = h; p->logf("editor opened"); return true;
}
static void editorClose(Plugin* p)
{
    if (!p || !p->editor) return;
    HWND h = (HWND)p->editor;
    EditorState* st = (EditorState*)GetWindowLongPtr(h, GWLP_USERDATA);
    KillTimer(h, 1); DestroyWindow(h); delete st; p->editor = nullptr;
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
static ERect g_rect = { 0, 0, 700, 1000 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}
#endif

#endif /* MS_EDITOR_H */
