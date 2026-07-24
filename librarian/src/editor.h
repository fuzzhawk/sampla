/*
 * editor.h — Win32/GDI editor for the Sample Librarian.
 *
 * Included from plugin.cpp after the Plugin struct is defined. Layout:
 *   top bar     library path (click to browse), SCAN button, progress
 *   settings    MinLen / MaxLen / MaxMB knobs, TuneKey toggle, RANDOMIZE
 *   left        constellation of the library (similar sounds cluster);
 *               the selected combo is drawn as a connected star shape
 *   right       palette of 12 combo buttons (layer count + score)
 *   bottom      layer controls for the selected combo: per-layer name,
 *               Vol/Pan/Tune knobs; global Master/Atk/Rel; EXPORT WAV
 *
 * The library scan runs on a Win32 worker thread (the engine call itself is
 * blocking and platform-neutral; tests call it directly).
 */
#ifndef LIB_EDITOR_H
#define LIB_EDITOR_H

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <math.h>

struct ERect { int16_t top, left, bottom, right; };

static const int ED_W = 1000;
static const int ED_H = 704;
static const int KNOB_R = 13;

/* constellation view */
static const int CV_X = 12, CV_Y = 76, CV_W = 628, CV_H = 424;
/* palette */
static const int PAL_X = 656, PAL_Y = 96, PAL_BW = 104, PAL_BH = 74, PAL_GAP = 8;
/* layer strip area */
static const int LAY_Y = 512;

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
    LibIndex* constFor = nullptr;   /* which index the bitmap was built for */
};

/* ---- geometry ---- */

static inline void pathBoxRect(RECT* r)  { r->left = 130; r->right = 700; r->top = 6;  r->bottom = 26; }
static inline void scanBtnRect(RECT* r)  { r->left = 710; r->right = 770; r->top = 6;  r->bottom = 26; }
static inline void tuneBtnRect(RECT* r)  { r->left = 210; r->right = 320; r->top = 40; r->bottom = 62; }
static inline void randBtnRect(RECT* r)  { r->left = PAL_X; r->right = PAL_X + 3 * PAL_BW + 2 * PAL_GAP; r->top = 44; r->bottom = 88; }
static inline void exportBtnRect(RECT* r){ r->left = 850; r->right = 985; r->top = LAY_Y + 6; r->bottom = LAY_Y + 46; }
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

/* settings + voice knobs: {param, cx, cy} */
struct KnobPos { int param, cx, cy; const char* label; };
static const int NSKNOBS = 3;
static inline void settingsKnob(int k, KnobPos* kp)
{
    static const int prm[NSKNOBS] = { pMinLen, pMaxLen, pMaxMB };
    static const char* lbl[NSKNOBS] = { "MinLen", "MaxLen", "MaxMB" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 40 + k * 60; kp->cy = 50;
}
static const int NVKNOBS = 3;
static inline void voiceKnob(int k, KnobPos* kp)
{
    static const int prm[NVKNOBS] = { pMaster, pAttack, pRelease };
    static const char* lbl[NVKNOBS] = { "Master", "Atk", "Rel" };
    kp->param = prm[k]; kp->label = lbl[k];
    kp->cx = 890 + (k % 2) * 60 - (k / 2) * 60; kp->cy = LAY_Y + 90 + (k / 2) * 0;
    /* simple row: */
    kp->cx = 780 + k * 70; kp->cy = LAY_Y + 110;
}
static inline void layerKnob(int lay, int off, KnobPos* kp)
{
    static const char* lbl[PPLAY] = { "Vol", "Pan", "Tune" };
    kp->param = PARAM_LAYER0 + lay * PPLAY + off;
    kp->label = lbl[off];
    kp->cx = 560 + off * 60;
    kp->cy = LAY_Y + 34 + lay * 44;
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
    RECT tl = { cx - 26, cy - KNOB_R - 14, cx + 26, cy - KNOB_R - 1 };
    DrawTextA(dc, label, -1, &tl, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(205, 210, 222));
    RECT vl = { cx - 26, cy + KNOB_R + 1, cx + 26, cy + KNOB_R + 14 };
    DrawTextA(dc, value, -1, &vl, DT_CENTER | DT_SINGLELINE);
}

static void drawTextBtn(HDC dc, const RECT& r, const char* text, COLORREF bg)
{
    HBRUSH bb = CreateSolidBrush(bg);
    FillRect(dc, &r, bb); DeleteObject(bb);
    SetTextColor(dc, RGB(220, 224, 236));
    DrawTextA(dc, text, -1, (RECT*)&r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* build (or reuse) the cached constellation bitmap for the current index */
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
    HBRUSH bg = CreateSolidBrush(RGB(10, 12, 20));
    FillRect(dc, &full, bg); DeleteObject(bg);

    if (ix) {
        for (size_t i = 0; i < ix->drawList.size(); i++) {
            const FileFeat& f = ix->files[ix->drawList[i]];
            int x = (int)(f.cx * CV_W);
            int y = (int)(f.cy * CV_H);
            /* brightness by pitch confidence, hue-ish by brightness band */
            int lum = 90 + (int)(f.pitchConf * 130.0f);
            if (lum > 235) lum = 235;
            SetPixel(dc, x, y, RGB(lum / 2, lum * 3 / 4, lum));
            SetPixel(dc, x + 1, y, RGB(lum / 3, lum / 2, lum * 3 / 4));
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

    HPEN border = CreatePen(PS_SOLID, 1, RGB(60, 66, 84));
    HGDIOBJ op = SelectObject(dc, border);
    HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, CV_X - 1, CV_Y - 1, CV_X + CV_W + 1, CV_Y + CV_H + 1);
    SelectObject(dc, op); SelectObject(dc, obr); DeleteObject(border);

    /* overlay the selected combo as a little constellation */
    LibIndex* ix = p->indexLive.load();
    if (ix && p->selected >= 0) {
        const Combo& c = p->combos[p->selected];
        HPEN star = CreatePen(PS_SOLID, 1, RGB(250, 220, 120));
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
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(dc, &full, bg); DeleteObject(bg);

    SetTextColor(dc, RGB(230, 234, 244));
    TextOutA(dc, 12, 8, "SAMPLE LIBRARIAN", 16);

    /* path box + scan */
    RECT pb; pathBoxRect(&pb);
    HBRUSH pbb = CreateSolidBrush(RGB(40, 44, 58));
    FillRect(dc, &pb, pbb); DeleteObject(pbb);
    SetTextColor(dc, RGB(200, 206, 220));
    std::string shown = p->libPath.empty()
        ? std::string("  (click to choose your library folder)")
        : "  " + p->libPath;
    DrawTextA(dc, shown.c_str(), -1, &pb, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT sc; scanBtnRect(&sc);
    drawTextBtn(dc, sc, p->prog.running.load() ? "..." : "SCAN", RGB(60, 90, 60));

    /* progress + status line */
    char status[160];
    LibIndex* ix = p->indexLive.load();
    if (p->prog.running.load())
        snprintf(status, sizeof(status), "scanning: %d found, %d done, %d kept",
                 p->prog.found.load(), p->prog.done.load(), p->prog.kept.load());
    else if (ix)
        snprintf(status, sizeof(status), "index: %d files", (int)ix->files.size());
    else
        snprintf(status, sizeof(status), "no index — choose a folder and SCAN");
    SetTextColor(dc, RGB(150, 190, 150));
    TextOutA(dc, 340, 44, status, (int)strlen(status));

    /* settings knobs + tune-key toggle */
    for (int k = 0; k < NSKNOBS; k++) {
        KnobPos kp; settingsKnob(k, &kp);
        char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    RECT tb; tuneBtnRect(&tb);
    bool tk = paramReal(pTuneKey, p->params[pTuneKey]) >= 0.5f;
    drawTextBtn(dc, tb, tk ? "Tune to key: ON" : "Tune to key: OFF",
                tk ? RGB(70, 90, 130) : RGB(48, 52, 66));

    drawConstellation(dc, st);

    /* randomize + palette */
    RECT rb; randBtnRect(&rb);
    drawTextBtn(dc, rb, "RANDOMIZE  \x07  12 combos", RGB(120, 70, 110));
    for (int i = 0; i < 12; i++) {
        RECT r; palBtnRect(i, &r);
        const Combo& c = p->combos[i];
        COLORREF col = c.nLayers ? (i == p->selected ? RGB(90, 120, 170)
                                                     : RGB(56, 62, 82))
                                 : RGB(38, 40, 50);
        HBRUSH b = CreateSolidBrush(col);
        FillRect(dc, &r, b); DeleteObject(b);
        char t1[32];
        if (c.nLayers)
            snprintf(t1, sizeof(t1), "#%d  %dx  %.0f%%", i + 1, c.nLayers,
                     c.score * 100.0f);
        else
            snprintf(t1, sizeof(t1), "#%d  —", i + 1);
        SetTextColor(dc, RGB(220, 224, 236));
        RECT tr = r; tr.bottom = tr.top + 24;
        DrawTextA(dc, t1, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (c.nLayers) {
            SetTextColor(dc, RGB(150, 158, 176));
            for (int l = 0; l < c.nLayers && l < 3; l++) {
                size_t sl = c.lay[l].path.find_last_of("/\\");
                std::string nm = sl == std::string::npos ? c.lay[l].path
                                                         : c.lay[l].path.substr(sl + 1);
                if (nm.size() > 15) nm = nm.substr(0, 14) + "…";
                RECT lr = r; lr.top += 24 + l * 15; lr.bottom = lr.top + 15;
                lr.left += 4;
                DrawTextA(dc, nm.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE);
            }
        }
    }

    /* layer controls for the selected combo */
    SetTextColor(dc, RGB(120, 200, 250));
    TextOutA(dc, 12, LAY_Y + 2, "COMBO LAYERS", 12);
    if (p->selected >= 0) {
        const Combo& c = p->combos[p->selected];
        for (int l = 0; l < 4; l++) {
            int y = LAY_Y + 22 + l * 44;
            SetTextColor(dc, RGB(190, 196, 208));
            if (l < c.nLayers) {
                size_t sl = c.lay[l].path.find_last_of("/\\");
                std::string nm = sl == std::string::npos ? c.lay[l].path
                                                         : c.lay[l].path.substr(sl + 1);
                char line[200];
                snprintf(line, sizeof(line), "L%d  %s  (%+.1f st)", l + 1,
                         nm.c_str(), c.lay[l].semis);
                TextOutA(dc, 14, y + 4, line, (int)strlen(line));
            } else {
                char line[16]; snprintf(line, sizeof(line), "L%d  —", l + 1);
                TextOutA(dc, 14, y + 4, line, (int)strlen(line));
            }
            for (int o = 0; o < PPLAY; o++) {
                KnobPos kp; layerKnob(l, o, &kp);
                char val[16]; paramDisplay(p, kp.param, val);
                drawKnob(dc, kp.cx, kp.cy + 4, p->params[kp.param], kp.label, val);
            }
        }
    } else {
        SetTextColor(dc, RGB(150, 156, 170));
        TextOutA(dc, 14, LAY_Y + 30,
                 "click a combo in the palette to open its layer controls",
                 55);
    }

    /* voice knobs + export */
    for (int k = 0; k < NVKNOBS; k++) {
        KnobPos kp; voiceKnob(k, &kp);
        char val[16]; paramDisplay(p, kp.param, val);
        drawKnob(dc, kp.cx, kp.cy, p->params[kp.param], kp.label, val);
    }
    RECT eb; exportBtnRect(&eb);
    drawTextBtn(dc, eb, "EXPORT WAV", RGB(70, 110, 80));

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
    if (p->libPath.empty() || p->prog.running.load()) return;
    if (st->scanThread) { CloseHandle(st->scanThread); st->scanThread = nullptr; }
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

static void doExport(EditorState* st)
{
    Plugin* p = st->p;
    if (p->selected < 0) return;
    char file[MAX_PATH] = { 0 };
    snprintf(file, sizeof(file), "combo_%02d.wav", p->selected + 1);
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "wav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn))
        p->exportCombo(file);
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
    exportBtnRect(&r);
    if (inRect(r, x, y)) { doExport(st); return; }

    for (int i = 0; i < 12; i++) {
        palBtnRect(i, &r);
        if (inRect(r, x, y)) {
            if (p->combos[i].nLayers) p->selectCombo(i);
            InvalidateRect(st->hwnd, nullptr, FALSE); return;
        }
    }

    /* knobs: settings + voice + layers */
    KnobPos kp;
    for (int k = 0; k < NSKNOBS + NVKNOBS + 4 * PPLAY; k++) {
        if (k < NSKNOBS) settingsKnob(k, &kp);
        else if (k < NSKNOBS + NVKNOBS) voiceKnob(k - NSKNOBS, &kp);
        else {
            int j = k - NSKNOBS - NVKNOBS;
            layerKnob(j / PPLAY, j % PPLAY, &kp);
            kp.cy += 4;
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
    (void)x;
    if (st->dragParam < 0) return;
    float v = st->dragStartVal + (st->dragStartY - y) / 200.0f;
    st->p->setParamFromUI(st->dragParam, v);
    InvalidateRect(st->hwnd, nullptr, FALSE);
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
        if (st) st->dragParam = -1;
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
    if (reason == DLL_PROCESS_ATTACH) g_hInst = inst;
    return TRUE;
}

#else  /* !_WIN32 — headless build for CI */

struct ERect { int16_t top, left, bottom, right; };
static ERect g_rect = { 0, 0, 704, 1000 };
static ERect* editorRect() { return &g_rect; }
static bool editorOpen(Plugin*, void*) { return false; }
static void editorClose(Plugin*) {}

#endif /* _WIN32 */

#endif /* LIB_EDITOR_H */
