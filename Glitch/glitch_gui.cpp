#define _CRT_SECURE_NO_WARNINGS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <commctrl.h>

#include <cstdio>
#include <cwctype>
#include <deque>
#include <exception>

#include "glitch_core.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

// ======================= GUI SETTINGS =======================
constexpr int PANEL_W = 300;                                // left panel width (at 96 DPI)
constexpr int STATUS_H = 26;                                // status bar height (at 96 DPI)
constexpr size_t HISTORY_MAX_STEPS = 40;                    // max number of undo steps
constexpr unsigned long long HISTORY_MAX_BYTES = 1024ull * 1024 * 1024;  // memory cap for undo
// ============================================================

enum : int {
    ID_OPEN = 100, ID_SAVE, ID_LOADMAP, ID_CLEARMAP, ID_UNDO, ID_REDO, ID_RESET, ID_FIT,
    ID_SWAP, ID_SMOOTH, ID_EXIT, ID_BG,
    ID_OP_BASE = 200, ID_SET_BASE = 400, ID_DLG_DEFAULTS = 900, ID_BG_PICK = 910, ID_BG_EDIT = 920
};

struct App {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HMENU menu = nullptr;
    bool dirty = false;  // true when the current image was changed since it was loaded / saved
    HFONT iconFont = nullptr;
    const wchar_t* glyphGear = L"";
    const wchar_t* glyphUndo = L"";
    const wchar_t* glyphRedo = L"";
    HWND tip = nullptr;                  // tooltip control
    std::deque<std::wstring> tipTexts;   // tooltip texts must stay alive
    int dpi = 96;

    HWND btnOpen = nullptr, btnSave = nullptr, btnLoadMap = nullptr, btnClearMap = nullptr;
    HWND btnUndo = nullptr, btnRedo = nullptr, btnReset = nullptr, btnFit = nullptr;
    HWND btnSwap = nullptr, btnSmooth = nullptr;
    std::vector<HWND> opButtons, setButtons;

    Image original, cur, map;
    bool hasImage = false, hasMap = false;
    fs::path srcPath, mapPath;
    Image mapOriginal;                      // original + history of the image in the map slot
    std::deque<Image> mapUndo, mapRedo;     // (they travel with the image when swapping)
    std::deque<Image> undo, redo;
    std::vector<uint32_t> bgra, mapBgra;  // cached 0x00RRGGBB pixels for painting

    // view (zoom / pan)
    bool fit = true;
    double zoom = 1.0, offX = 0.0, offY = 0.0;
    bool dragging = false, fastPaint = false;
    POINT dragStart{0, 0};
    double dragOffX = 0.0, dragOffY = 0.0;

    std::wstring status = L"Drag & drop an image into the window to start";

    // layout
    RECT view{}, statusRect{}, mapSlot{}, opsLabelRect{}, mapLabelRect{};
};

App g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

std::wstring widen(const char* s) {
    std::wstring w;
    while (*s) w += static_cast<wchar_t>(static_cast<unsigned char>(*s++));
    return w;
}

// ---------- small helpers ----------
void invalidateView() {
    InvalidateRect(g.hwnd, &g.view, FALSE);
    InvalidateRect(g.hwnd, &g.statusRect, FALSE);
}

void setStatus(const std::wstring& s) {
    g.status = s;
    InvalidateRect(g.hwnd, &g.statusRect, FALSE);
}

void toBgra(const Image& im, std::vector<uint32_t>& out) {
    out.resize(im.px.size());
    for (size_t i = 0; i < im.px.size(); ++i) {
        const Pixel& p = im.px[i];
        out[i] = (static_cast<uint32_t>(p.c[0]) << 16) | (static_cast<uint32_t>(p.c[1]) << 8) | p.c[2];
    }
}

void refreshCur() { toBgra(g.cur, g.bgra); }
void refreshMap() { toBgra(g.map, g.mapBgra); }

void updateButtons() {
    auto en = [](HWND h, bool on) { EnableWindow(h, on ? TRUE : FALSE); };
    en(g.btnSave, g.hasImage);
    en(g.btnUndo, !g.undo.empty());
    en(g.btnRedo, !g.redo.empty());
    en(g.btnReset, g.hasImage);
    en(g.btnFit, g.hasImage);
    en(g.btnClearMap, g.hasMap);
    en(g.btnSwap, g.hasImage && g.hasMap);
    if (g.menu) {
        auto enm = [](int id, bool on) {
            EnableMenuItem(g.menu, static_cast<UINT>(id), MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
        };
        enm(ID_SAVE, g.hasImage);
        enm(ID_UNDO, !g.undo.empty());
        enm(ID_REDO, !g.redo.empty());
        enm(ID_RESET, g.hasImage);
        enm(ID_FIT, g.hasImage);
        enm(ID_CLEARMAP, g.hasMap);
        enm(ID_SWAP, g.hasImage && g.hasMap);
    }
    for (size_t i = 0; i < g.opButtons.size(); ++i)
        en(g.opButtons[i], g.hasImage && (!OPERATIONS[i].runWithMap || g.hasMap));
}

// ---------- view transform ----------
void currentTransform(double& z, double& ox, double& oy) {
    if (g.fit) {
        const double vw = std::max<LONG>(1, g.view.right - g.view.left);
        const double vh = std::max<LONG>(1, g.view.bottom - g.view.top);
        z = std::min(vw / g.cur.w, vh / g.cur.h);
        ox = g.view.left + (vw - g.cur.w * z) / 2.0;
        oy = g.view.top + (vh - g.cur.h * z) / 2.0;
    } else {
        z = g.zoom;
        ox = g.offX;
        oy = g.offY;
    }
}

// Keeps at least a part of the image inside the view, so it can never be dragged out of reach
void clampOffsets() {
    if (g.fit || !g.hasImage) return;
    const double vl = g.view.left, vr = g.view.right, vt = g.view.top, vb = g.view.bottom;
    const double iw = g.cur.w * g.zoom, ih = g.cur.h * g.zoom;
    const double mx = std::min({static_cast<double>(S(60)), iw, vr - vl});
    const double my = std::min({static_cast<double>(S(60)), ih, vb - vt});
    g.offX = std::clamp(g.offX, vl + mx - iw, vr - mx);
    g.offY = std::clamp(g.offY, vt + my - ih, vb - my);
}

void zoomAt(POINT pt, double factor) {
    double z, ox, oy;
    currentTransform(z, ox, oy);
    const double nz = std::clamp(z * factor, 0.02, 64.0);
    g.fit = false;
    g.zoom = nz;
    g.offX = pt.x - (pt.x - ox) * nz / z;
    g.offY = pt.y - (pt.y - oy) * nz / z;
    clampOffsets();
    invalidateView();
}

// ---------- layout ----------
void layout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = S(8), gap = S(5), bh = S(26), panelW = S(PANEL_W);
    const int inner = panelW - 2 * m, half = (inner - gap) / 2;

    int y = m;
    auto row2 = [&](HWND a, HWND b) {
        MoveWindow(a, m, y, half, bh, TRUE);
        MoveWindow(b, m + half + gap, y, inner - half - gap, bh, TRUE);
        y += bh + gap;
    };
    row2(g.btnUndo, g.btnRedo);
    row2(g.btnOpen, g.btnSave);
    row2(g.btnLoadMap, g.btnClearMap);
    row2(g.btnReset, g.btnFit);
    row2(g.btnSwap, g.btnSmooth);

    y += S(4);
    g.opsLabelRect = RECT{m, y, panelW - m, y + S(20)};
    y += S(22);
    const int setW = S(40);
    for (size_t i = 0; i < g.opButtons.size(); ++i) {
        // blank line before the first operation that needs two images
        if (OPERATIONS[i].runWithMap && (i == 0 || !OPERATIONS[i - 1].runWithMap)) y += S(16);
        MoveWindow(g.opButtons[i], m, y, inner - setW - gap, bh, TRUE);
        MoveWindow(g.setButtons[i], m + inner - setW, y, setW, bh, TRUE);
        y += bh + S(3);
    }

    y += S(6);
    g.mapLabelRect = RECT{m, y, panelW - m, y + S(20)};
    y += S(22);
    const int slotH = std::max(S(60), std::min(S(110), H - y - m));
    g.mapSlot = RECT{m, y, panelW - m, y + slotH};

    g.view = RECT{panelW, 0, W, std::max(0, H - S(STATUS_H))};
    g.statusRect = RECT{panelW, std::max(0, H - S(STATUS_H)), W, H};
}

// ---------- drawing ----------
BITMAPINFO makeBmi(int w, int h) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return bmi;
}

constexpr uint32_t VIEW_BG = (36u << 16) | (36u << 8) | 38u;  // 0x00RRGGBB, same as RGB(36,36,38)

// BEGIN renderImage
// Renders an image (cached 0x00RRGGBB pixels) into a w x h buffer. The image's top-left corner is at
// (ox, oy) in buffer coordinates and the scale is z. Everything outside the image is filled with bg.
// Sampling is done here instead of by GDI: nearest neighbour when zoomed in (or when `fast`),
// box filter (up to 4x4 taps per output pixel) when zoomed out.
void renderImage(std::vector<uint32_t>& out, int w, int h, const std::vector<uint32_t>& px, int iw, int ih,
                 double z, double ox, double oy, bool fast, uint32_t bg) {
    out.assign(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), bg);
    if (w <= 0 || h <= 0 || iw <= 0 || ih <= 0 || z <= 0.0 ||
        px.size() < static_cast<size_t>(iw) * static_cast<size_t>(ih))
        return;

    const bool smooth = (z < 1.0) && !fast;
    const int n = smooth ? std::min(4, static_cast<int>(std::ceil(1.0 / z))) : 1;

    // destination range actually covered by the image
    const int dx0 = std::max(0, static_cast<int>(std::floor(ox)));
    const int dx1 = std::min(w, static_cast<int>(std::ceil(ox + iw * z)));
    const int dy0 = std::max(0, static_cast<int>(std::floor(oy)));
    const int dy1 = std::min(h, static_cast<int>(std::ceil(oy + ih * z)));
    if (dx1 <= dx0 || dy1 <= dy0) return;

    // source column for every tap of every output column (-1 = outside the image)
    std::vector<int> colIdx(static_cast<size_t>(dx1 - dx0) * n);
    for (int dx = dx0; dx < dx1; ++dx) {
        for (int k = 0; k < n; ++k) {
            const double t = smooth ? (k + 0.5) / n : 0.5;
            const int ix = static_cast<int>(std::floor((dx + t - ox) / z));
            colIdx[static_cast<size_t>(dx - dx0) * n + k] = (ix >= 0 && ix < iw) ? ix : -1;
        }
    }

    std::vector<int> rowIdx(n);
    for (int dy = dy0; dy < dy1; ++dy) {
        for (int k = 0; k < n; ++k) {
            const double t = smooth ? (k + 0.5) / n : 0.5;
            const int iy = static_cast<int>(std::floor((dy + t - oy) / z));
            rowIdx[k] = (iy >= 0 && iy < ih) ? iy : -1;
        }
        uint32_t* dst = &out[static_cast<size_t>(dy) * w];
        for (int dx = dx0; dx < dx1; ++dx) {
            const int* ci = &colIdx[static_cast<size_t>(dx - dx0) * n];
            if (n == 1) {
                if (ci[0] >= 0 && rowIdx[0] >= 0) dst[dx] = px[static_cast<size_t>(rowIdx[0]) * iw + ci[0]];
                continue;
            }
            unsigned sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int ky = 0; ky < n; ++ky) {
                if (rowIdx[ky] < 0) continue;
                const uint32_t* srow = &px[static_cast<size_t>(rowIdx[ky]) * iw];
                for (int kx = 0; kx < n; ++kx) {
                    if (ci[kx] < 0) continue;
                    const uint32_t p = srow[ci[kx]];
                    sr += (p >> 16) & 255u;
                    sg += (p >> 8) & 255u;
                    sb += p & 255u;
                    ++cnt;
                }
            }
            if (cnt) dst[dx] = ((sr / cnt) << 16) | ((sg / cnt) << 8) | (sb / cnt);
        }
    }
}
// END renderImage

// Draws the image into `area` (client coordinates). The image is rendered on the CPU and blitted 1:1,
// so no GDI stretching (and its clipping quirks) is involved.
void drawImage(HDC dc, const std::vector<uint32_t>& px, int iw, int ih,
               double z, double ox, double oy, const RECT& area, bool fast) {
    const int w = static_cast<int>(area.right - area.left);
    const int h = static_cast<int>(area.bottom - area.top);
    if (w <= 0 || h <= 0) return;
    static std::vector<uint32_t> buf;
    renderImage(buf, w, h, px, iw, ih, z, ox - area.left, oy - area.top, fast, VIEW_BG);
    BITMAPINFO bmi = makeBmi(w, h);
    StretchDIBits(dc, area.left, area.top, w, h, 0, 0, w, h, buf.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

void drawCentered(HDC dc, const wchar_t* text, const RECT& area) {
    RECT calc = area;
    DrawTextW(dc, text, -1, &calc, DT_CALCRECT | DT_CENTER | DT_WORDBREAK);
    const int th = calc.bottom - calc.top;
    RECT dst = area;
    dst.top = area.top + std::max<LONG>(0, ((area.bottom - area.top) - th) / 2);
    DrawTextW(dc, text, -1, &dst, DT_CENTER | DT_WORDBREAK);
}

void drawWindow(HDC dc, const RECT& rc) {
    HBRUSH face = GetSysColorBrush(COLOR_BTNFACE);
    RECT panel{0, 0, S(PANEL_W), rc.bottom};
    FillRect(dc, &panel, face);
    FillRect(dc, &g.statusRect, face);

    HBRUSH dark = CreateSolidBrush(RGB(36, 36, 38));
    FillRect(dc, &g.view, dark);
    FillRect(dc, &g.mapSlot, dark);

    SelectObject(dc, g.font);
    SetBkMode(dc, TRANSPARENT);

    // panel labels
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(dc, L"Operations", -1, &g.opsLabelRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextW(dc, L"Displacement map / 2nd image", -1, &g.mapLabelRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // map slot
    if (g.hasMap) {
        const double sw = g.mapSlot.right - g.mapSlot.left, sh = g.mapSlot.bottom - g.mapSlot.top;
        const double z = std::min(sw / g.map.w, sh / g.map.h);
        const double ox = g.mapSlot.left + (sw - g.map.w * z) / 2.0;
        const double oy = g.mapSlot.top + (sh - g.map.h * z) / 2.0;
        drawImage(dc, g.mapBgra, g.map.w, g.map.h, z, ox, oy, g.mapSlot, false);
    } else {
        SetTextColor(dc, RGB(170, 170, 170));
        RECT inner = g.mapSlot;
        InflateRect(&inner, -S(6), -S(4));
        drawCentered(dc, L"Drop a second image here\n(or use Load map...)\nNeeded for operations that use a map", inner);
    }
    HBRUSH frame = CreateSolidBrush(RGB(120, 120, 120));
    FrameRect(dc, &g.mapSlot, frame);
    DeleteObject(frame);

    // main view
    if (g.hasImage) {
        double z, ox, oy;
        currentTransform(z, ox, oy);
        drawImage(dc, g.bgra, g.cur.w, g.cur.h, z, ox, oy, g.view, g.fastPaint);
    } else {
        SetTextColor(dc, RGB(190, 190, 190));
        RECT inner = g.view;
        InflateRect(&inner, -S(30), -S(30));
        drawCentered(dc,
                     L"Drag & drop an image here\n\n"
                     L"Drop two images at once to also load a displacement map.\n"
                     L"You can also press Ctrl+O.",
                     inner);
    }
    DeleteObject(dark);

    // status bar
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    RECT left = g.statusRect;
    left.left += S(8);
    left.right -= S(300);
    DrawTextW(dc, g.status.c_str(), -1, &left, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (g.hasImage) {
        double z, ox, oy;
        currentTransform(z, ox, oy);
        std::wstring info = std::to_wstring(g.cur.w) + L"x" + std::to_wstring(g.cur.h) +
                            L"   zoom " + std::to_wstring(static_cast<int>(std::lround(z * 100))) +
                            L"%   undo " + std::to_wstring(g.undo.size());
        RECT right = g.statusRect;
        right.right -= S(8);
        DrawTextW(dc, info.c_str(), -1, &right, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
}

// ---------- history ----------
void trimHistory() {
    const unsigned long long bytesPer = static_cast<unsigned long long>(g.cur.px.size()) * 3;
    while (g.undo.size() > 1 &&
           (g.undo.size() > HISTORY_MAX_STEPS || g.undo.size() * bytesPer > HISTORY_MAX_BYTES))
        g.undo.pop_front();
}

void pushUndo() {
    g.undo.push_back(g.cur);
    g.redo.clear();
    trimHistory();
}

void doUndo() {
    if (g.undo.empty()) return;
    g.redo.push_back(std::move(g.cur));
    g.cur = std::move(g.undo.back());
    g.undo.pop_back();
    refreshCur();
    updateButtons();
    g.dirty = true;
    setStatus(L"Undo");
    invalidateView();
}

void doRedo() {
    if (g.redo.empty()) return;
    g.undo.push_back(std::move(g.cur));
    g.cur = std::move(g.redo.back());
    g.redo.pop_back();
    refreshCur();
    updateButtons();
    g.dirty = true;
    setStatus(L"Redo");
    invalidateView();
}

void doReset() {
    if (!g.hasImage) return;
    pushUndo();
    g.cur = g.original;
    refreshCur();
    updateButtons();
    g.dirty = true;
    setStatus(L"Reset to the original image (you can undo this)");
    invalidateView();
}

// ---------- operations ----------
void applyOp(size_t idx) {
    if (!g.hasImage || idx >= OPERATIONS.size()) return;
    const Operation& op = OPERATIONS[idx];
    if (op.runWithMap && !g.hasMap) {
        setStatus(L"This operation needs a second image (displacement map)");
        return;
    }

    HCURSOR oldCursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const ULONGLONG t0 = GetTickCount64();
    pushUndo();
    try {
        if (op.runWithMap) op.runWithMap(g.cur, g.map);
        else               op.run(g.cur);
    } catch (const std::exception& e) {
        if (!g.undo.empty()) {
            g.cur = std::move(g.undo.back());
            g.undo.pop_back();
        }
        SetCursor(oldCursor);
        setStatus(L"Operation failed: " + widen(e.what()));
        return;
    }
    refreshCur();
    updateButtons();
    SetCursor(oldCursor);
    g.dirty = true;
    setStatus(L"Applied: " + widen(op.name) + L"  (" + std::to_wstring(GetTickCount64() - t0) + L" ms)");
    invalidateView();
}

// ---------- files ----------
bool loadMain(const fs::path& p) {
    Image im;
    if (!loadImage(p, im)) {
        setStatus(L"Failed to read image: " + p.filename().wstring());
        return false;
    }
    g.original = im;
    g.cur = std::move(im);
    g.undo.clear();
    g.redo.clear();
    g.hasImage = true;
    g.srcPath = p;
    g.fit = true;
    refreshCur();
    updateButtons();
    SetWindowTextW(g.hwnd, (L"Glitch - " + p.filename().wstring()).c_str());
    g.dirty = false;
    setStatus(L"Loaded " + p.filename().wstring());
    InvalidateRect(g.hwnd, nullptr, FALSE);
    return true;
}

bool loadMapImage(const fs::path& p) {
    Image im;
    if (!loadImage(p, im)) {
        setStatus(L"Failed to read map image: " + p.filename().wstring());
        return false;
    }
    g.map = std::move(im);
    g.mapOriginal = g.map;
    g.mapUndo.clear();
    g.mapRedo.clear();
    g.mapPath = p;
    g.hasMap = true;
    refreshMap();
    updateButtons();
    setStatus(L"Loaded map: " + p.filename().wstring());
    InvalidateRect(g.hwnd, nullptr, FALSE);
    return true;
}

void clearMap() {
    g.hasMap = false;
    g.map = Image{};
    g.mapOriginal = Image{};
    g.mapUndo.clear();
    g.mapRedo.clear();
    g.mapPath.clear();
    g.mapBgra.clear();
    updateButtons();
    setStatus(L"Displacement map cleared");
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Exchanges the main image and the map (each one keeps its original, undo and redo history)
void doSwap() {
    if (!g.hasImage || !g.hasMap) {
        setStatus(L"Load both an image and a map to swap them");
        return;
    }
    std::swap(g.cur, g.map);
    std::swap(g.original, g.mapOriginal);
    std::swap(g.undo, g.mapUndo);
    std::swap(g.redo, g.mapRedo);
    std::swap(g.srcPath, g.mapPath);
    g.fit = true;
    refreshCur();
    refreshMap();
    updateButtons();
    SetWindowTextW(g.hwnd, (L"Glitch - " + g.srcPath.filename().wstring()).c_str());
    g.dirty = true;
    setStatus(L"Swapped image and map");
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void updateSmoothLabel() {
    SendMessageW(g.btnSmooth, BM_SETCHECK, SLICE_SMOOTH ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g.btnSmooth, SLICE_SMOOTH ? L"Slicing: smooth" : L"Slicing: stepped");
}

bool pickOpenFile(const wchar_t* title, fs::path& out) {
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring dir = g.srcPath.parent_path().wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.psd)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.psd\0"
                      L"All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = fs::path(buf);
    return true;
}

void doOpen() {
    fs::path p;
    if (pickOpenFile(L"Open image", p)) loadMain(p);
}

void doLoadMap() {
    fs::path p;
    if (pickOpenFile(L"Open displacement map / second image", p)) loadMapImage(p);
}

void doSave() {
    if (!g.hasImage) return;
    wchar_t buf[MAX_PATH * 4] = {};
    const std::wstring def = g.srcPath.stem().wstring() + L"_glitch.png";
    def.copy(buf, MAX_PATH * 4 - 1);
    std::wstring dir = g.srcPath.parent_path().wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Save image";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    fs::path p(buf);
    std::wstring ext = p.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg")
        p += (ofn.nFilterIndex == 2 ? L".jpg" : L".png");

    if (saveImage(p, g.cur)) {
        g.dirty = false;
        setStatus(L"Saved: " + p.wstring());
    } else {
        setStatus(L"Failed to save: " + p.wstring());
    }
}

void handleDrop(const std::vector<fs::path>& files, POINT pt) {
    if (files.empty()) return;
    if (files.size() == 1 && PtInRect(&g.mapSlot, pt)) {
        loadMapImage(files[0]);
        return;
    }
    const bool ok = loadMain(files[0]);
    if (ok && files.size() >= 2) {
        loadMapImage(files[1]);
        if (files.size() > 2) setStatus(L"Loaded image + map (extra files ignored)");
    }
}

// ---------- settings dialog ----------
struct ParamUi {
    HWND ctrl = nullptr;
    std::vector<HWND> checks;  // checkboxes of a mode list
};

struct DlgState {
    HWND hwnd = nullptr;
    const std::vector<Param>* params = nullptr;
    std::vector<ParamUi> ui;
    bool done = false;
    bool accepted = false;
};
DlgState* g_dlg = nullptr;

std::wstring fmtNum(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return widen(s.c_str());
}

HWND dlgCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle,
            int x, int y, int w, int h, int id = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

// Puts the current values (or the starting values) into the dialog controls
void fillControls(bool defaults) {
    const std::vector<Param>& params = *g_dlg->params;
    for (size_t i = 0; i < params.size(); ++i) {
        const Param& p = params[i];
        ParamUi& u = g_dlg->ui[i];
        const double v = defaults ? p.def : paramGet(p);
        switch (p.kind) {
        case ParamKind::Int:
        case ParamKind::Double:
            SetWindowTextW(u.ctrl, fmtNum(v).c_str());
            break;
        case ParamKind::Bool:
            SendMessageW(u.ctrl, BM_SETCHECK, v != 0.0 ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case ParamKind::Choice:
            SendMessageW(u.ctrl, CB_SETCURSEL, static_cast<WPARAM>(v), 0);
            break;
        case ParamKind::ModeList: {
            const std::vector<SortMode>& list =
                defaults ? p.defList : *static_cast<std::vector<SortMode>*>(p.ptr);
            for (int k = 0; k < SORT_MODE_COUNT; ++k) {
                const bool on = std::find(list.begin(), list.end(), static_cast<SortMode>(k)) != list.end();
                SendMessageW(u.checks[k], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            break;
        }
        }
    }
}

// Validates everything first, then applies it. Returns false (and shows a message) if something is wrong.
bool applyFromControls() {
    const std::vector<Param>& params = *g_dlg->params;
    const size_t n = params.size();
    std::vector<double> vals(n, 0.0);
    std::vector<std::vector<SortMode>> lists(n);

    for (size_t i = 0; i < n; ++i) {
        const Param& p = params[i];
        ParamUi& u = g_dlg->ui[i];
        switch (p.kind) {
        case ParamKind::Int:
        case ParamKind::Double: {
            wchar_t buf[64] = {};
            GetWindowTextW(u.ctrl, buf, 63);
            for (wchar_t* c = buf; *c; ++c)
                if (*c == L',') *c = L'.';  // accept a decimal comma
            wchar_t* end = nullptr;
            const double v = std::wcstod(buf, &end);
            while (end && *end == L' ') ++end;
            if (end == buf || (end && *end != L'\0')) {
                MessageBoxW(g_dlg->hwnd, (L"Invalid number in field " + widen(p.name)).c_str(),
                            L"Settings", MB_OK | MB_ICONWARNING);
                SetFocus(u.ctrl);
                return false;
            }
            vals[i] = v;
            break;
        }
        case ParamKind::Bool:
            vals[i] = (SendMessageW(u.ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1.0 : 0.0;
            break;
        case ParamKind::Choice: {
            const LRESULT sel = SendMessageW(u.ctrl, CB_GETCURSEL, 0, 0);
            vals[i] = sel < 0 ? 0.0 : static_cast<double>(sel);
            break;
        }
        case ParamKind::ModeList:
            for (int k = 0; k < SORT_MODE_COUNT; ++k)
                if (SendMessageW(u.checks[k], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    lists[i].push_back(static_cast<SortMode>(k));
            if (lists[i].empty()) {
                MessageBoxW(g_dlg->hwnd, (L"Select at least one mode in " + widen(p.name)).c_str(),
                            L"Settings", MB_OK | MB_ICONWARNING);
                return false;
            }
            break;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        const Param& p = params[i];
        if (p.kind == ParamKind::ModeList) *static_cast<std::vector<SortMode>*>(p.ptr) = lists[i];
        else                               paramSet(p, vals[i]);
    }
    return true;
}

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (g_dlg) {
            switch (LOWORD(wp)) {
            case IDOK:
                if (applyFromControls()) { g_dlg->accepted = true; g_dlg->done = true; }
                return 0;
            case IDCANCEL:
                g_dlg->done = true;
                return 0;
            case ID_DLG_DEFAULTS:
                fillControls(true);
                return 0;
            }
        }
        break;
    case 0x0400:  // DM_GETDEFID: tells IsDialogMessage which button Enter should press
        return static_cast<LRESULT>(IDOK) | (static_cast<LRESULT>(0x534B) << 16);  // DC_HASDEFID
    case WM_CLOSE:
        if (g_dlg) g_dlg->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Modal window with all settings of one operation
void showSettings(size_t opIdx) {
    const std::vector<Param>& params = settingsFor(opIdx);
    if (params.empty() || g_dlg) return;

    DlgState st;
    st.params = &params;
    st.ui.resize(params.size());

    const int m = S(12), rowH = S(26), step = rowH + S(4);
    const int labelW = S(210), ctrlW = S(130), hintX = m + labelW + ctrlW + S(10), hintW = S(330);
    const int clientW = hintX + hintW + m;
    int rows = 0;
    for (const Param& p : params) rows += (p.kind == ParamKind::ModeList) ? 1 + SORT_MODE_COUNT : 1;
    const int clientH = m + rows * step + S(10) + S(30) + m;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT cr{0, 0, clientW, clientH};
    AdjustWindowRectEx(&cr, style, FALSE, WS_EX_DLGMODALFRAME);
    const int w = static_cast<int>(cr.right - cr.left), h = static_cast<int>(cr.bottom - cr.top);
    RECT orc{};
    GetWindowRect(g.hwnd, &orc);
    const int x = std::max(0, static_cast<int>(orc.left + ((orc.right - orc.left) - w) / 2));
    const int y = std::max(0, static_cast<int>(orc.top + ((orc.bottom - orc.top) - h) / 2));

    const std::wstring opName = widen(OPERATIONS[opIdx].name);
    const std::wstring title = L"Settings - " + opName;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"GlitchSettings", title.c_str(), style, x, y, w, h,
                               g.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return;
    st.hwnd = dlg;
    g_dlg = &st;

    int cy = m;
    for (size_t i = 0; i < params.size(); ++i) {
        const Param& p = params[i];
        ParamUi& u = st.ui[i];
        const std::wstring name = widen(p.name);
        std::wstring hint = widen(p.hint);
        if (p.kind == ParamKind::Int || p.kind == ParamKind::Double)
            hint += L"  [" + fmtNum(p.minV) + L".." + fmtNum(p.maxV) + L"]";
        const int rowY = cy;

        switch (p.kind) {
        case ParamKind::Int:
        case ParamKind::Double:
            dlgCtl(dlg, L"STATIC", name.c_str(), SS_LEFT | SS_NOPREFIX, 0, m, cy + S(3), labelW, S(20));
            u.ctrl = dlgCtl(dlg, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                            m + labelW, cy, ctrlW, S(24));
            break;
        case ParamKind::Bool:
            u.ctrl = dlgCtl(dlg, L"BUTTON", name.c_str(), WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                            m, cy, labelW + ctrlW, S(24));
            break;
        case ParamKind::Choice:
            dlgCtl(dlg, L"STATIC", name.c_str(), SS_LEFT | SS_NOPREFIX, 0, m, cy + S(3), labelW, S(20));
            u.ctrl = dlgCtl(dlg, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                            m + labelW, cy, ctrlW, S(200));
            for (int k = 0; k < SORT_MODE_COUNT; ++k)
                SendMessageW(u.ctrl, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(SORT_MODE_NAMES[k]).c_str()));
            break;
        case ParamKind::ModeList:
            dlgCtl(dlg, L"STATIC", name.c_str(), SS_LEFT | SS_NOPREFIX, 0, m, cy + S(3), labelW, S(20));
            for (int k = 0; k < SORT_MODE_COUNT; ++k) {
                cy += step;
                u.checks.push_back(dlgCtl(dlg, L"BUTTON", widen(SORT_MODE_NAMES[k]).c_str(),
                                          WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                          m + S(18), cy, labelW + ctrlW - S(18), S(24)));
            }
            break;
        }
        dlgCtl(dlg, L"STATIC", hint.c_str(), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0,
               hintX, rowY + S(3), hintW, S(20));
        cy += step;
    }

    const int by = clientH - m - S(30), bw = S(100), bh = S(30);
    dlgCtl(dlg, L"BUTTON", L"Defaults", WS_TABSTOP | BS_PUSHBUTTON, 0, m, by, bw, bh, ID_DLG_DEFAULTS);
    dlgCtl(dlg, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, clientW - m - 2 * bw - S(8), by, bw, bh, IDOK);
    dlgCtl(dlg, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, clientW - m - bw, by, bw, bh, IDCANCEL);

    fillControls(false);

    EnableWindow(g.hwnd, FALSE);  // modal: block the main window while the dialog is open
    ShowWindow(dlg, SW_SHOW);
    if (st.ui[0].ctrl) SetFocus(st.ui[0].ctrl);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    const bool quitRequested = !st.done;  // the loop ended because of WM_QUIT
    EnableWindow(g.hwnd, TRUE);
    DestroyWindow(dlg);
    g_dlg = nullptr;
    if (quitRequested) {
        PostQuitMessage(static_cast<int>(msg.wParam));
        return;
    }
    SetForegroundWindow(g.hwnd);
    if (st.accepted) {
        updateSmoothLabel();  // the slicing toggle on the main panel follows SLICE_SMOOTH
        setStatus(L"Settings saved: " + opName + L" (applies to the next use of the operation)");
    }
}

// ---------- close confirmation ----------
bool confirmExit() {
    const wchar_t* text = g.dirty ? L"You have unsaved changes. Close the program anyway?"
                                  : L"Close the program?";
    return MessageBoxW(g.hwnd, text, L"Glitch", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

// ---------- solid color background ----------
struct BgState {
    HWND hwnd = nullptr;
    HWND edit[5] = {};  // width, height, red, green, blue
    RECT swatch{};      // color preview
    long v[5] = {0, 0, 0, 0, 0};
    bool done = false;
    bool accepted = false;
};
BgState* g_bg = nullptr;

constexpr int BG_COUNT = 5;
const wchar_t* const BG_NAMES[BG_COUNT] = {L"Width", L"Height", L"Red", L"Green", L"Blue"};
constexpr long BG_MIN[BG_COUNT] = {1, 1, 0, 0, 0};
constexpr long BG_MAX[BG_COUNT] = {20000, 20000, 255, 255, 255};
constexpr long long BG_MAX_PIXELS = 80000000LL;  // 80 megapixels

bool parseIntField(HWND ctl, long& out) {
    wchar_t buf[32] = {};
    GetWindowTextW(ctl, buf, 31);
    wchar_t* end = nullptr;
    const long v = std::wcstol(buf, &end, 10);
    while (end && *end == L' ') ++end;
    if (end == buf || (end && *end != L'\0')) return false;
    out = v;
    return true;
}

// Color currently typed into the R/G/B fields (invalid fields count as 0, values are clamped)
COLORREF bgCurrentColor() {
    long c[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        long v = 0;
        if (parseIntField(g_bg->edit[2 + i], v)) c[i] = std::clamp(v, 0L, 255L);
    }
    return RGB(c[0], c[1], c[2]);
}

void bgPickColor() {
    static COLORREF custom[16] = {};
    const COLORREF cur = bgCurrentColor();
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_bg->hwnd;
    cc.rgbResult = cur;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return;
    SetWindowTextW(g_bg->edit[2], std::to_wstring(GetRValue(cc.rgbResult)).c_str());
    SetWindowTextW(g_bg->edit[3], std::to_wstring(GetGValue(cc.rgbResult)).c_str());
    SetWindowTextW(g_bg->edit[4], std::to_wstring(GetBValue(cc.rgbResult)).c_str());
}

bool bgApply() {
    long v[BG_COUNT] = {};
    for (int i = 0; i < BG_COUNT; ++i) {
        if (!parseIntField(g_bg->edit[i], v[i])) {
            MessageBoxW(g_bg->hwnd, (std::wstring(L"Invalid number in field ") + BG_NAMES[i]).c_str(),
                        L"Solid color background", MB_OK | MB_ICONWARNING);
            SetFocus(g_bg->edit[i]);
            return false;
        }
        if (v[i] < BG_MIN[i] || v[i] > BG_MAX[i]) {
            MessageBoxW(g_bg->hwnd,
                        (std::wstring(BG_NAMES[i]) + L" must be between " + std::to_wstring(BG_MIN[i]) +
                         L" and " + std::to_wstring(BG_MAX[i])).c_str(),
                        L"Solid color background", MB_OK | MB_ICONWARNING);
            SetFocus(g_bg->edit[i]);
            return false;
        }
    }
    if (static_cast<long long>(v[0]) * v[1] > BG_MAX_PIXELS) {
        MessageBoxW(g_bg->hwnd, L"The image is too large (limit: 80 megapixels).",
                    L"Solid color background", MB_OK | MB_ICONWARNING);
        SetFocus(g_bg->edit[0]);
        return false;
    }
    for (int i = 0; i < BG_COUNT; ++i) g_bg->v[i] = v[i];
    return true;
}

LRESULT CALLBACK BgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (g_bg) {
            const int id = LOWORD(wp);
            if (id >= ID_BG_EDIT && id < ID_BG_EDIT + BG_COUNT && HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(hwnd, &g_bg->swatch, FALSE);  // update the color preview
                return 0;
            }
            switch (id) {
            case IDOK:
                if (bgApply()) { g_bg->accepted = true; g_bg->done = true; }
                return 0;
            case IDCANCEL:
                g_bg->done = true;
                return 0;
            case ID_BG_PICK:
                bgPickColor();
                return 0;
            }
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_bg) {
            HBRUSH fill = CreateSolidBrush(bgCurrentColor());
            FillRect(dc, &g_bg->swatch, fill);
            DeleteObject(fill);
            HBRUSH frame = CreateSolidBrush(RGB(110, 110, 110));
            FrameRect(dc, &g_bg->swatch, frame);
            DeleteObject(frame);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case 0x0400:  // DM_GETDEFID
        return static_cast<LRESULT>(IDOK) | (static_cast<LRESULT>(0x534B) << 16);
    case WM_CLOSE:
        if (g_bg) g_bg->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Replaces the main image by a plain color of the given size (the old image and its history are dropped)
void setBackgroundImage(int w, int h, int r, int gr, int b) {
    try {
        Image im;
        im.w = w;
        im.h = h;
        im.px.assign(static_cast<size_t>(w) * h,
                     Pixel{{static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b)}});
        g.original = im;
        g.cur = std::move(im);
    } catch (const std::exception&) {
        setStatus(L"Not enough memory for an image of this size");
        return;
    }
    g.undo.clear();
    g.redo.clear();
    g.hasImage = true;
    g.srcPath = fs::path(L"background.png");  // gives the default name for "Save as..."
    g.fit = true;
    g.dirty = true;                           // a generated image is not saved yet
    refreshCur();
    updateButtons();
    const std::wstring desc = L"Background RGB(" + std::to_wstring(r) + L", " + std::to_wstring(gr) +
                              L", " + std::to_wstring(b) + L") " + std::to_wstring(w) + L"x" + std::to_wstring(h);
    SetWindowTextW(g.hwnd, (L"Glitch - " + desc).c_str());
    setStatus(L"Created: " + desc);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void showBackgroundDialog() {
    if (g_bg || g_dlg) return;
    BgState st;

    long init[BG_COUNT] = {1920, 1080, 0, 0, 0};
    static long lastColor[3] = {0, 0, 0};  // remembers the last color that was used
    if (g.hasImage)    { init[0] = g.cur.w; init[1] = g.cur.h; }
    else if (g.hasMap) { init[0] = g.map.w; init[1] = g.map.h; }
    for (int i = 0; i < 3; ++i) init[2 + i] = lastColor[i];

    const int m = S(12), rowH = S(26), step = rowH + S(4);
    const int labelW = S(120), editW = S(90), btnW = S(100), btnH = S(30);
    const int clientW = 2 * m + S(250);
    const int clientH = m + 6 * step + S(10) + btnH + m;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT cr{0, 0, clientW, clientH};
    AdjustWindowRectEx(&cr, style, FALSE, WS_EX_DLGMODALFRAME);
    const int w = static_cast<int>(cr.right - cr.left), h = static_cast<int>(cr.bottom - cr.top);
    RECT orc{};
    GetWindowRect(g.hwnd, &orc);
    const int x = std::max(0, static_cast<int>(orc.left + ((orc.right - orc.left) - w) / 2));
    const int y = std::max(0, static_cast<int>(orc.top + ((orc.bottom - orc.top) - h) / 2));

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"GlitchBackground", L"Solid color background",
                               style, x, y, w, h, g.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return;
    st.hwnd = dlg;
    g_bg = &st;

    int cy = m;
    st.swatch = RECT{m + S(140), cy + 5 * step, m + S(140) + S(100), cy + 5 * step + rowH};  // set before the edits exist
    for (int i = 0; i < BG_COUNT; ++i) {
        const std::wstring label = std::wstring(BG_NAMES[i]) + (i < 2 ? L" (px)" : L" (0-255)");
        dlgCtl(dlg, L"STATIC", label.c_str(), SS_LEFT | SS_NOPREFIX, 0, m, cy + S(3), labelW, S(20));
        st.edit[i] = dlgCtl(dlg, L"EDIT", std::to_wstring(init[i]).c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                            WS_EX_CLIENTEDGE, m + labelW, cy, editW, S(24), ID_BG_EDIT + i);
        cy += step;
    }
    dlgCtl(dlg, L"BUTTON", L"Pick color...", WS_TABSTOP | BS_PUSHBUTTON, 0, m, cy, S(130), rowH, ID_BG_PICK);
    cy += step;

    const int by = clientH - m - btnH;
    dlgCtl(dlg, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, clientW - m - 2 * btnW - S(8), by, btnW, btnH, IDOK);
    dlgCtl(dlg, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, clientW - m - btnW, by, btnW, btnH, IDCANCEL);

    EnableWindow(g.hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit[0]);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    const bool quitRequested = !st.done;
    EnableWindow(g.hwnd, TRUE);
    DestroyWindow(dlg);
    g_bg = nullptr;
    if (quitRequested) {
        PostQuitMessage(static_cast<int>(msg.wParam));
        return;
    }
    SetForegroundWindow(g.hwnd);
    if (st.accepted) {
        for (int i = 0; i < 3; ++i) lastColor[i] = st.v[2 + i];
        setBackgroundImage(static_cast<int>(st.v[0]), static_cast<int>(st.v[1]),
                           static_cast<int>(st.v[2]), static_cast<int>(st.v[3]), static_cast<int>(st.v[4]));
    }
}

// ---------- menu bar ----------
void createMenu() {
    auto add = [](HMENU menu, int id, const wchar_t* text) { AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(id), text); };
    auto sep = [](HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };

    HMENU file = CreatePopupMenu();
    add(file, ID_OPEN, L"Open image...\tCtrl+O");
    add(file, ID_SAVE, L"Save as...\tCtrl+S");
    sep(file);
    add(file, ID_EXIT, L"Exit");

    HMENU edit = CreatePopupMenu();
    add(edit, ID_UNDO, L"Undo\tCtrl+Z");
    add(edit, ID_REDO, L"Redo\tCtrl+Y");
    sep(edit);
    add(edit, ID_RESET, L"Reset to original");

    HMENU image = CreatePopupMenu();
    add(image, ID_BG, L"Solid color background...\tCtrl+B");
    sep(image);
    add(image, ID_LOADMAP, L"Load map...");
    add(image, ID_CLEARMAP, L"Clear map");
    add(image, ID_SWAP, L"Swap image and map");

    HMENU view = CreatePopupMenu();
    add(view, ID_FIT, L"Fit to window\tF");

    g.menu = CreateMenu();
    AppendMenuW(g.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(g.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"Edit");
    AppendMenuW(g.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(image), L"Image");
    AppendMenuW(g.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    SetMenu(g.hwnd, g.menu);
}

// ---------- window ----------
bool fontInstalled(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfHeight = -S(16);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t actual[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, actual);  // Windows substitutes another font if `face` is missing
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(nullptr, dc);
    return lstrcmpiW(actual, face) == 0;
}

// Standard Windows icons: Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10).
// If neither font exists, plain Unicode symbols from Segoe UI Symbol are used instead.
void initIcons() {
    const wchar_t* face;
    if (fontInstalled(L"Segoe Fluent Icons")) {
        face = L"Segoe Fluent Icons";
        g.glyphGear = L"\uE713";
        g.glyphUndo = L"\uE7A7";
        g.glyphRedo = L"\uE7A6";
    } else if (fontInstalled(L"Segoe MDL2 Assets")) {
        face = L"Segoe MDL2 Assets";
        g.glyphGear = L"\uE713";
        g.glyphUndo = L"\uE7A7";
        g.glyphRedo = L"\uE7A6";
    } else {
        face = L"Segoe UI Symbol";
        g.glyphGear = L"\u2699";
        g.glyphUndo = L"\u21B6";
        g.glyphRedo = L"\u21B7";
    }
    LOGFONTW lf{};
    lf.lfHeight = -S(16);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    g.iconFont = CreateFontIndirectW(&lf);
}

void initTooltips() {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    g.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            g.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void addTip(HWND ctl, const std::wstring& text) {
    if (!g.tip) return;
    g.tipTexts.push_back(text);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g.hwnd;
    ti.uId = reinterpret_cast<UINT_PTR>(ctl);
    ti.lpszText = const_cast<LPWSTR>(g.tipTexts.back().c_str());
    SendMessageW(g.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

HWND makeButton(HWND parent, const wchar_t* text, int id, DWORD style = BS_PUSHBUTTON) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                             0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return h;
}

HWND makeIconButton(HWND parent, const wchar_t* glyph, int id) {
    HWND h = makeButton(parent, glyph, id);
    if (g.iconFont) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.iconFont), TRUE);
    return h;
}

void createControls(HWND hwnd) {
    g.hwnd = hwnd;
    captureSettingDefaults();  // remember the starting values for the "Defaults" button

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    initIcons();
    initTooltips();
    createMenu();

    g.btnOpen = makeButton(hwnd, L"Open...", ID_OPEN);
    g.btnSave = makeButton(hwnd, L"Save as...", ID_SAVE);
    g.btnLoadMap = makeButton(hwnd, L"Load map...", ID_LOADMAP);
    g.btnClearMap = makeButton(hwnd, L"Clear map", ID_CLEARMAP);
    g.btnUndo = makeIconButton(hwnd, g.glyphUndo, ID_UNDO);
    g.btnRedo = makeIconButton(hwnd, g.glyphRedo, ID_REDO);
    addTip(g.btnUndo, L"Undo (Ctrl+Z)");
    addTip(g.btnRedo, L"Redo (Ctrl+Y)");
    g.btnReset = makeButton(hwnd, L"Reset", ID_RESET);
    g.btnFit = makeButton(hwnd, L"Fit", ID_FIT);
    g.btnSwap = makeButton(hwnd, L"Swap image/map", ID_SWAP);
    g.btnSmooth = makeButton(hwnd, L"Slicing: smooth", ID_SMOOTH, BS_AUTOCHECKBOX | BS_PUSHLIKE);
    updateSmoothLabel();

    for (size_t i = 0; i < OPERATIONS.size(); ++i) {
        const std::wstring label = std::to_wstring(i + 1) + L". " + widen(OPERATIONS[i].name);
        g.opButtons.push_back(makeButton(hwnd, label.c_str(), ID_OP_BASE + static_cast<int>(i)));
        g.setButtons.push_back(makeIconButton(hwnd, g.glyphGear, ID_SET_BASE + static_cast<int>(i)));
        addTip(g.setButtons.back(), L"Settings: " + widen(OPERATIONS[i].name));
        EnableWindow(g.setButtons.back(), settingsFor(i).empty() ? FALSE : TRUE);  // e.g. Invert has none
    }
    layout();
    updateButtons();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        createControls(hwnd);
        return 0;

    case WM_SIZE:
        if (g.hwnd) {
            layout();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_GETMINMAXINFO: {
        RECT r{0, 0, S(PANEL_W + 520), S(676)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);  // TRUE: the window has a menu bar
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            IntersectClipRect(mem, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
            drawWindow(mem, rc);
            BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                   mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        switch (id) {
        case ID_OPEN:     doOpen(); break;
        case ID_SAVE:     doSave(); break;
        case ID_LOADMAP:  doLoadMap(); break;
        case ID_CLEARMAP: clearMap(); break;
        case ID_UNDO:     doUndo(); break;
        case ID_REDO:     doRedo(); break;
        case ID_RESET:    doReset(); break;
        case ID_FIT:
            if (g.hasImage) { g.fit = true; invalidateView(); }
            break;
        case ID_SWAP:     doSwap(); break;
        case ID_BG:       showBackgroundDialog(); break;
        case ID_EXIT:     SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case ID_SMOOTH:
            SLICE_SMOOTH = (SendMessageW(g.btnSmooth, BM_GETCHECK, 0, 0) == BST_CHECKED);
            updateSmoothLabel();
            setStatus(SLICE_SMOOTH ? L"Slicing mode: smooth (shift changes gradually between lines)"
                                    : L"Slicing mode: stepped (each strip has one constant shift)");
            break;
        default:
            if (id >= ID_OP_BASE && id < ID_OP_BASE + static_cast<int>(OPERATIONS.size()))
                applyOp(static_cast<size_t>(id - ID_OP_BASE));
            else if (id >= ID_SET_BASE && id < ID_SET_BASE + static_cast<int>(OPERATIONS.size()))
                showSettings(static_cast<size_t>(id - ID_SET_BASE));
            break;
        }
        SetFocus(hwnd);  // so that Space/Enter never re-press the last button
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        POINT pt{};
        DragQueryPoint(drop, &pt);
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<fs::path> files;
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len, L'\0');
            DragQueryFileW(drop, i, s.data(), len + 1);
            files.emplace_back(s);
        }
        DragFinish(drop);
        handleDrop(files, pt);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!g.hasImage) return 0;
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&g.view, pt)) {
            const double notches = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
            zoomAt(pt, std::pow(1.25, notches));
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (g.hasImage && PtInRect(&g.view, pt)) {
            double z, ox, oy;
            currentTransform(z, ox, oy);
            g.fit = false;
            g.zoom = z;
            g.offX = ox;
            g.offY = oy;
            g.dragging = true;
            g.fastPaint = true;
            g.dragStart = pt;
            g.dragOffX = ox;
            g.dragOffY = oy;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g.dragging) {
            const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
            g.offX = g.dragOffX + (x - g.dragStart.x);
            g.offY = g.dragOffY + (y - g.dragStart.y);
            clampOffsets();
            invalidateView();
        }
        return 0;

    case WM_LBUTTONUP:
        if (g.dragging) {
            g.dragging = false;
            g.fastPaint = false;
            ReleaseCapture();
            invalidateView();
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (g.hasImage && PtInRect(&g.view, pt)) {
            g.fit = true;
            invalidateView();
        }
        return 0;
    }

    case WM_CLOSE:
        if (g.hasImage && !confirmExit()) return 0;  // nothing to lose when no image is loaded
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g.font) DeleteObject(g.font);
        if (g.iconFont) DeleteObject(g.iconFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    SetProcessDPIAware();

    HDC sdc = GetDC(nullptr);
    g.dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);

    // Allow drag & drop even if the program runs elevated
    ChangeWindowMessageFilter(WM_DROPFILES, MSGFLT_ADD);
    ChangeWindowMessageFilter(0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ADD);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"GlitchGuiWindow";
    RegisterClassExW(&wc);

    WNDCLASSEXW dc = wc;
    dc.style = 0;
    dc.lpfnWndProc = DlgProc;
    dc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    dc.lpszClassName = L"GlitchSettings";
    RegisterClassExW(&dc);
    dc.lpfnWndProc = BgProc;
    dc.lpszClassName = L"GlitchBackground";
    RegisterClassExW(&dc);

    RECT r{0, 0, S(1180), S(760)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);  // TRUE: the window has a menu bar
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Glitch",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    ACCEL accel[] = {
        {FCONTROL | FVIRTKEY, 'O', ID_OPEN},
        {FCONTROL | FVIRTKEY, 'S', ID_SAVE},
        {FCONTROL | FVIRTKEY, 'Z', ID_UNDO},
        {FCONTROL | FVIRTKEY, 'Y', ID_REDO},
        {FCONTROL | FVIRTKEY, 'B', ID_BG},
        {FVIRTKEY, 'F', ID_FIT},
    };
    HACCEL hAccel = CreateAcceleratorTableW(accel, static_cast<int>(sizeof(accel) / sizeof(accel[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(hAccel);
    return static_cast<int>(msg.wParam);
}
