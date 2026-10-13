// glitch_core.h - shared code: settings, effects, image loading/saving, operations table.
// Used by both the console program (glitch.cpp) and the Windows GUI (glitch_gui.cpp).
// All SETTINGS live in this file.
//
// Dependencies: stb_image.h and stb_image_write.h (https://github.com/nothings/stb)

#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Sorting modes applied to a detected run of pixels
enum class SortMode {
    PixelAsc,       // whole pixels by brightness, ascending
    PixelDesc,      // whole pixels by brightness, descending
    ChannelsAsc,    // R, G, B sorted independently, ascending
    ChannelsDesc,   // R, G, B sorted independently, descending
    ChannelsMixed,  // R, G, B sorted independently, random direction per channel
    None            // no sorting
};

// ======================= SETTINGS =======================
// These are plain variables (starting values). The GUI edits them at runtime
// through the "Set..." buttons; the console version just uses them as they are.

// RGB split
static int RGB_STRIP_MIN = 1;
static int RGB_STRIP_MAX = 4;
static int RGB_SHIFT_MIN = 1;
static int RGB_SHIFT_MAX = 8;

// Slicing
// (horizontal slicing shifts rows along X, vertical slicing shifts columns along Y)
static bool SLICE_SMOOTH = true;       // true: shift changes smoothly from line to line,
// false: every strip gets one constant shift (hard steps)
static int  SLICE_MIN = 40;             // strip thickness; in smooth mode: distance between
static int  SLICE_MAX = 200;             // control points (wave length) in px
static int  SLICE_SHIFT_MAX = 70;      // shift from -MAX to +MAX

// Contrast (>1 = higher contrast, 1.0 = no change)
static double CONTRAST_FACTOR = 1.5;

// Pixel sorting
static bool   SORT_HORIZONTAL = true;  // true: along rows, false: along columns
static int    SORT_STRIP_MIN = 1;      // strip thickness (px)
static int    SORT_STRIP_MAX = 1;
static double SORT_PROB = 1;                // probability that a strip is processed
static double SORT_THRESHOLD_PERCENT = 0;  // brightness threshold in percent (0..100)
static bool   SORT_ABOVE_THRESHOLD = true;    // true: pick pixels brighter than threshold, false: darker

// Sorting mode
static bool     SORT_MIX_MODES = false;                      // true: each strip gets its own random mode
static SortMode SORT_FIXED_MODE = SortMode::PixelDesc;  // mode used when SORT_MIX_MODES = false

// Modes to choose from when SORT_MIX_MODES = true
static std::vector<SortMode> SORT_MIX_LIST = {
    SortMode::PixelAsc,
    SortMode::PixelDesc,
    SortMode::ChannelsAsc,
    SortMode::ChannelsDesc,
    SortMode::ChannelsMixed
};

// Pixel smear
static bool   SMEAR_HORIZONTAL = true;  // true: stretch to the right, false: downwards
static int    SMEAR_STRIP_MIN = 1;
static int    SMEAR_STRIP_MAX = 4;
static double SMEAR_PROB = 0.5;         // probability that a strip gets smeared
static int    SMEAR_ANCHORS = 3;        // number of smear points per strip
static int    SMEAR_LEN_MIN = 50;       // smear length (px)
static int    SMEAR_LEN_MAX = 400;

// Pixel displacement (needs two images: the second one is the displacement map)
// A map value of 128 means "no displacement"; darker/brighter values push in opposite directions.
static double DISPLACE_SCALE_X = 40.0;  // max horizontal displacement in px (0 = none)
static double DISPLACE_SCALE_Y = 40.0;  // max vertical displacement in px (0 = none)
static bool   DISPLACE_FROM_LUMA = false; // false: map R -> X shift, map G -> Y shift
// true:  map brightness drives both axes
static bool   DISPLACE_WRAP = false; // true: wrap around the edges, false: repeat edge pixels

// JPEG compression corruption
static int JPEG_QUALITY       = 5;   // 1..100, lower = blockier
static int JPEG_PASSES        = 1;   // number of compress/decode cycles
static int JPEG_CORRUPT_BYTES = 20;  // damaged bytes in the compressed data per pass (0 = compression only)

// RGB channel multiplication (each channel is multiplied by its own factor)
static bool   RGB_MUL_RANDOM = false;  // true: random factors in [RGB_MUL_MIN, RGB_MUL_MAX] on every run
static double RGB_MUL_R = 1.4;
static double RGB_MUL_G = 0.9;
static double RGB_MUL_B = 1.3;
static double RGB_MUL_MIN = 0.5;
static double RGB_MUL_MAX = 2.0;

// Subtract blending (needs two images: result = image 1 - image 2 * strength)
static double SUBTRACT_STRENGTH = 1.0;
static bool   SUBTRACT_ABSOLUTE = false;  // true: |image 1 - image 2 * strength| (difference)

// RGB channel displacement (each channel is moved by its own offset, in px)
static bool CHDISP_RANDOM = false;  // true: random offsets in [-CHDISP_RANDOM_MAX, CHDISP_RANDOM_MAX]
static int  CHDISP_RANDOM_MAX = 20;
static int  CHDISP_R_X = -8, CHDISP_R_Y = 0;
static int  CHDISP_G_X = 0,  CHDISP_G_Y = 0;
static int  CHDISP_B_X = 8,  CHDISP_B_Y = 0;
static bool CHDISP_WRAP = false;    // true: wrap around the edges, false: repeat edge pixels
// =========================================================

// ---------- Editable parameters (used by the GUI settings dialogs) ----------
constexpr int SORT_MODE_COUNT = 6;  // number of values in SortMode
static const char* const SORT_MODE_NAMES[SORT_MODE_COUNT] = {
    "Pixel asc", "Pixel desc", "Channels asc", "Channels desc", "Channels mixed", "None"
};

enum class ParamKind { Int, Double, Bool, Choice, ModeList };

struct Param {
    const char* name;               // name of the setting
    ParamKind kind;
    void* ptr;                      // int*, double*, bool*, SortMode* (Choice) or std::vector<SortMode>* (ModeList)
    double minV, maxV;              // allowed range (Int / Double)
    const char* hint;               // short explanation
    double def = 0.0;               // value at program start (Int / Double / Bool / Choice)
    std::vector<SortMode> defList;  // value at program start (ModeList)
};

static double paramGet(const Param& p) {
    switch (p.kind) {
    case ParamKind::Int:    return *static_cast<int*>(p.ptr);
    case ParamKind::Double: return *static_cast<double*>(p.ptr);
    case ParamKind::Bool:   return *static_cast<bool*>(p.ptr) ? 1.0 : 0.0;
    case ParamKind::Choice: return static_cast<double>(static_cast<int>(*static_cast<SortMode*>(p.ptr)));
    case ParamKind::ModeList: break;
    }
    return 0.0;
}

// Sets a value, clamping it to the allowed range
static void paramSet(const Param& p, double v) {
    switch (p.kind) {
    case ParamKind::Int:
        *static_cast<int*>(p.ptr) = static_cast<int>(std::lround(std::clamp(v, p.minV, p.maxV)));
        break;
    case ParamKind::Double:
        *static_cast<double*>(p.ptr) = std::clamp(v, p.minV, p.maxV);
        break;
    case ParamKind::Bool:
        *static_cast<bool*>(p.ptr) = (v != 0.0);
        break;
    case ParamKind::Choice:
        *static_cast<SortMode*>(p.ptr) = static_cast<SortMode>(
            std::clamp(static_cast<int>(std::lround(v)), 0, SORT_MODE_COUNT - 1));
        break;
    case ParamKind::ModeList:
        break;
    }
}

// Parameters of every operation that has settings. They are matched with OPERATIONS by operation name
// (see settingsFor() at the end of this file), so the order of the operations can be changed freely.
// Built on first use; that moment also records the starting values used by "Defaults".
// (The GUI calls captureSettingDefaults() at startup, before anything can be edited.)
struct OpSettings {
    const char* opName;
    std::vector<Param> params;
};

static const std::vector<OpSettings>& settingsRegistry() {
    static const std::vector<OpSettings> reg = [] {
        auto I = [](const char* n, int* p, double lo, double hi, const char* h) {
            Param q{n, ParamKind::Int, p, lo, hi, h, 0.0, {}}; q.def = *p; return q; };
        auto D = [](const char* n, double* p, double lo, double hi, const char* h) {
            Param q{n, ParamKind::Double, p, lo, hi, h, 0.0, {}}; q.def = *p; return q; };
        auto B = [](const char* n, bool* p, const char* h) {
            Param q{n, ParamKind::Bool, p, 0, 1, h, 0.0, {}}; q.def = *p ? 1.0 : 0.0; return q; };
        auto C = [](const char* n, SortMode* p, const char* h) {
            Param q{n, ParamKind::Choice, p, 0, SORT_MODE_COUNT - 1, h, 0.0, {}}; q.def = static_cast<int>(*p); return q; };
        auto L = [](const char* n, std::vector<SortMode>* p, const char* h) {
            Param q{n, ParamKind::ModeList, p, 0, 0, h, 0.0, {}}; q.defList = *p; return q; };

        std::vector<OpSettings> r;

        r.push_back({"RGB split", {
            I("RGB_STRIP_MIN", &RGB_STRIP_MIN, 1, 5000, "min strip height (px)"),
            I("RGB_STRIP_MAX", &RGB_STRIP_MAX, 1, 5000, "max strip height (px)"),
            I("RGB_SHIFT_MIN", &RGB_SHIFT_MIN, 0, 5000, "min shift of a strip (px)"),
            I("RGB_SHIFT_MAX", &RGB_SHIFT_MAX, 0, 5000, "max shift of a strip (px)"),
        }});

        std::vector<Param> slicing = {
            B("SLICE_SMOOTH", &SLICE_SMOOTH, "smooth wave instead of hard steps"),
            I("SLICE_MIN", &SLICE_MIN, 1, 10000, "strip thickness / distance between wave points (px)"),
            I("SLICE_MAX", &SLICE_MAX, 1, 10000, "same, maximum (px)"),
            I("SLICE_SHIFT_MAX", &SLICE_SHIFT_MAX, 0, 10000, "shift from -MAX to +MAX (px)"),
        };
        r.push_back({"Horizontal slicing", slicing});
        r.push_back({"Vertical slicing", slicing});

        r.push_back({"Contrast", {
            D("CONTRAST_FACTOR", &CONTRAST_FACTOR, 0, 20, ">1 = more contrast, 1 = no change"),
        }});

        r.push_back({"Pixel sorting", {
            B("SORT_HORIZONTAL", &SORT_HORIZONTAL, "sort along rows (off = along columns)"),
            I("SORT_STRIP_MIN", &SORT_STRIP_MIN, 1, 5000, "min strip thickness (px)"),
            I("SORT_STRIP_MAX", &SORT_STRIP_MAX, 1, 5000, "max strip thickness (px)"),
            D("SORT_PROB", &SORT_PROB, 0, 1, "probability that a strip is sorted"),
            D("SORT_THRESHOLD_PERCENT", &SORT_THRESHOLD_PERCENT, 0, 100, "brightness threshold (%)"),
            B("SORT_ABOVE_THRESHOLD", &SORT_ABOVE_THRESHOLD, "sort pixels brighter than the threshold (off = darker)"),
            B("SORT_MIX_MODES", &SORT_MIX_MODES, "random mode for every strip (off = fixed mode)"),
            C("SORT_FIXED_MODE", &SORT_FIXED_MODE, "mode used when mixing is off"),
            L("SORT_MIX_LIST", &SORT_MIX_LIST, "modes picked from when mixing is on"),
        }});

        r.push_back({"Pixel smear", {
            B("SMEAR_HORIZONTAL", &SMEAR_HORIZONTAL, "stretch to the right (off = downwards)"),
            I("SMEAR_STRIP_MIN", &SMEAR_STRIP_MIN, 1, 5000, "min strip thickness (px)"),
            I("SMEAR_STRIP_MAX", &SMEAR_STRIP_MAX, 1, 5000, "max strip thickness (px)"),
            D("SMEAR_PROB", &SMEAR_PROB, 0, 1, "probability that a strip is smeared"),
            I("SMEAR_ANCHORS", &SMEAR_ANCHORS, 1, 1000, "smear points per strip"),
            I("SMEAR_LEN_MIN", &SMEAR_LEN_MIN, 1, 100000, "min smear length (px)"),
            I("SMEAR_LEN_MAX", &SMEAR_LEN_MAX, 1, 100000, "max smear length (px)"),
        }});

        r.push_back({"Pixel displacement", {
            D("DISPLACE_SCALE_X", &DISPLACE_SCALE_X, 0, 5000, "max horizontal displacement (px)"),
            D("DISPLACE_SCALE_Y", &DISPLACE_SCALE_Y, 0, 5000, "max vertical displacement (px)"),
            B("DISPLACE_FROM_LUMA", &DISPLACE_FROM_LUMA, "map brightness drives both axes (off = R->X, G->Y)"),
            B("DISPLACE_WRAP", &DISPLACE_WRAP, "wrap around the edges (off = repeat edge pixels)"),
        }});

        r.push_back({"JPEG compression corruption", {
            I("JPEG_QUALITY", &JPEG_QUALITY, 1, 100, "lower = blockier"),
            I("JPEG_PASSES", &JPEG_PASSES, 1, 20, "number of compress/decode cycles"),
            I("JPEG_CORRUPT_BYTES", &JPEG_CORRUPT_BYTES, 0, 1000000, "damaged bytes per pass (0 = none)"),
        }});

        r.push_back({"RGB channel multiplication", {
            B("RGB_MUL_RANDOM", &RGB_MUL_RANDOM, "random factors on every run"),
            D("RGB_MUL_R", &RGB_MUL_R, 0, 20, "red factor"),
            D("RGB_MUL_G", &RGB_MUL_G, 0, 20, "green factor"),
            D("RGB_MUL_B", &RGB_MUL_B, 0, 20, "blue factor"),
            D("RGB_MUL_MIN", &RGB_MUL_MIN, 0, 20, "random factor: minimum"),
            D("RGB_MUL_MAX", &RGB_MUL_MAX, 0, 20, "random factor: maximum"),
        }});

        r.push_back({"Subtract blending", {
            D("SUBTRACT_STRENGTH", &SUBTRACT_STRENGTH, 0, 20, "result = image - map * strength"),
            B("SUBTRACT_ABSOLUTE", &SUBTRACT_ABSOLUTE, "absolute difference |image - map * strength|"),
        }});

        r.push_back({"RGB channel displacement", {
            B("CHDISP_RANDOM", &CHDISP_RANDOM, "random offsets on every run"),
            I("CHDISP_RANDOM_MAX", &CHDISP_RANDOM_MAX, 0, 5000, "random offsets from -MAX to +MAX (px)"),
            I("CHDISP_R_X", &CHDISP_R_X, -5000, 5000, "red channel offset X (px)"),
            I("CHDISP_R_Y", &CHDISP_R_Y, -5000, 5000, "red channel offset Y (px)"),
            I("CHDISP_G_X", &CHDISP_G_X, -5000, 5000, "green channel offset X (px)"),
            I("CHDISP_G_Y", &CHDISP_G_Y, -5000, 5000, "green channel offset Y (px)"),
            I("CHDISP_B_X", &CHDISP_B_X, -5000, 5000, "blue channel offset X (px)"),
            I("CHDISP_B_Y", &CHDISP_B_Y, -5000, 5000, "blue channel offset Y (px)"),
            B("CHDISP_WRAP", &CHDISP_WRAP, "wrap around the edges (off = repeat edge pixels)"),
        }});

        return r;
    }();
    return reg;
}


struct Pixel { uint8_t c[3]; };
static_assert(sizeof(Pixel) == 3, "Pixel must be 3 bytes");

struct Image {
    int w = 0, h = 0;
    std::vector<Pixel> px;
    Pixel* row(int y) { return px.data() + static_cast<size_t>(y) * w; }
};

static std::mt19937 rng{std::random_device{}()};

static int randInt(int a, int b) {
    if (b < a) b = a;
    return std::uniform_int_distribution<int>(a, b)(rng);
}
static bool chance(double p) {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p;
}
static double randReal(double a, double b) {
    return std::uniform_real_distribution<double>(a, b)(rng);
}

static float luma(const Pixel& p) {
    return 0.299f * p.c[0] + 0.587f * p.c[1] + 0.114f * p.c[2];
}

// Access a pixel by (line, position along the line).
// horiz = true:  line = row y,    position = x
// horiz = false: line = column x, position = y
static Pixel& at(Image& img, int line, int pos, bool horiz) {
    return horiz ? img.px[static_cast<size_t>(line) * img.w + pos]
                 : img.px[static_cast<size_t>(pos) * img.w + line];
}

// Split the range [0, total) into strips of random thickness
static std::vector<std::pair<int, int>> makeStrips(int total, int mn, int mx) {
    std::vector<std::pair<int, int>> strips;
    mn = std::max(1, mn);
    mx = std::max(mn, mx);
    for (int pos = 0; pos < total;) {
        int end = std::min(total, pos + randInt(mn, mx));
        strips.emplace_back(pos, end);
        pos = end;
    }
    return strips;
}

// Circular right shift by k elements
template <class It>
static void rotateRight(It b, It e, int k) {
    int n = static_cast<int>(e - b);
    if (n <= 0) return;
    k = ((k % n) + n) % n;
    if (k) std::rotate(b, b + (n - k), e);
}

// ---------- RGB split ----------
// Each channel is cut into horizontal strips, and every strip is shifted along X
// by RGB_SHIFT_MIN..RGB_SHIFT_MAX px in a random direction.
static void rgbSplit(Image& img) {
    std::vector<uint8_t> buf(img.w);
    for (int ch = 0; ch < 3; ++ch) {
        for (auto [y0, y1] : makeStrips(img.h, RGB_STRIP_MIN, RGB_STRIP_MAX)) {
            int shift = randInt(RGB_SHIFT_MIN, RGB_SHIFT_MAX);
            if (chance(0.5)) shift = -shift;
            for (int y = y0; y < y1; ++y) {
                Pixel* r = img.row(y);
                for (int x = 0; x < img.w; ++x) buf[x] = r[x].c[ch];
                rotateRight(buf.begin(), buf.end(), shift);
                for (int x = 0; x < img.w; ++x) r[x].c[ch] = buf[x];
            }
        }
    }
}

// ---------- Slicing ----------
// Builds one shift value per line (row or column).
// Smooth mode: random shifts are placed at control points SLICE_MIN..SLICE_MAX px apart,
// and the lines in between are interpolated with smoothstep, so the shift changes
// gradually from line to line (a wavy distortion).
// Stepped mode: every strip gets one constant random shift.
static std::vector<int> makeSliceShifts(int total) {
    std::vector<int> shifts(total, 0);
    const int mn = std::max(1, SLICE_MIN);
    const int mx = std::max(mn, SLICE_MAX);

    if (SLICE_SMOOTH) {
        int a = 0;
        double va = randInt(-SLICE_SHIFT_MAX, SLICE_SHIFT_MAX);
        while (a < total) {
            int b = a + randInt(mn, mx);
            double vb = randInt(-SLICE_SHIFT_MAX, SLICE_SHIFT_MAX);
            for (int i = a; i < std::min(b, total); ++i) {
                double t = static_cast<double>(i - a) / (b - a);
                t = t * t * (3.0 - 2.0 * t);  // smoothstep
                shifts[i] = static_cast<int>(std::lround(va + (vb - va) * t));
            }
            a = b;
            va = vb;
        }
    } else {
        for (auto [s0, s1] : makeStrips(total, mn, mx)) {
            int shift = randInt(-SLICE_SHIFT_MAX, SLICE_SHIFT_MAX);
            for (int i = s0; i < s1; ++i) shifts[i] = shift;
        }
    }
    return shifts;
}

// Every row is shifted along X by its own value
static void sliceHorizontal(Image& img) {
    std::vector<int> shifts = makeSliceShifts(img.h);
    for (int y = 0; y < img.h; ++y) {
        if (shifts[y] == 0) continue;
        Pixel* r = img.row(y);
        rotateRight(r, r + img.w, shifts[y]);
    }
}

// Every column is shifted along Y by its own value
static void sliceVertical(Image& img) {
    std::vector<int> shifts = makeSliceShifts(img.w);
    std::vector<Pixel> col(img.h);
    for (int x = 0; x < img.w; ++x) {
        if (shifts[x] == 0) continue;
        for (int y = 0; y < img.h; ++y) col[y] = img.row(y)[x];
        rotateRight(col.begin(), col.end(), shifts[x]);
        for (int y = 0; y < img.h; ++y) img.row(y)[x] = col[y];
    }
}

// ---------- Contrast ----------
static void applyContrast(Image& img, double factor) {
    if (factor == 1.0) return;
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        double x = (v - 127.5) * factor + 127.5;
        lut[v] = static_cast<uint8_t>(std::clamp<long>(std::lround(x), 0, 255));
    }
    for (Pixel& p : img.px)
        for (int c = 0; c < 3; ++c) p.c[c] = lut[p.c[c]];
}

// ---------- Pixel sorting ----------
// Sort a single run [a, e) of a line using the given mode
static void sortRun(std::vector<Pixel>& line, int a, int e, SortMode mode,
                    std::vector<uint8_t>& vals) {
    auto first = line.begin() + a;
    auto last  = line.begin() + e;
    switch (mode) {
    case SortMode::PixelAsc:
        std::sort(first, last, [](const Pixel& x, const Pixel& y) { return luma(x) < luma(y); });
        break;
    case SortMode::PixelDesc:
        std::sort(first, last, [](const Pixel& x, const Pixel& y) { return luma(x) > luma(y); });
        break;
    case SortMode::ChannelsAsc:
    case SortMode::ChannelsDesc:
    case SortMode::ChannelsMixed:
        vals.resize(e - a);
        for (int ch = 0; ch < 3; ++ch) {
            bool asc = (mode == SortMode::ChannelsAsc) ||
                       (mode == SortMode::ChannelsMixed && chance(0.5));
            for (int i = a; i < e; ++i) vals[i - a] = line[i].c[ch];
            if (asc) std::sort(vals.begin(), vals.end());
            else     std::sort(vals.begin(), vals.end(), std::greater<uint8_t>());
            for (int i = a; i < e; ++i) line[i].c[ch] = vals[i - a];
        }
        break;
    case SortMode::None:
        break;
    }
}

// Line: find contiguous runs of pixels that pass the brightness threshold and sort each run
static void sortLine(std::vector<Pixel>& line, SortMode mode) {
    const int n = static_cast<int>(line.size());
    std::vector<uint8_t> selected(n);
    for (int i = 0; i < n; ++i) {
        double pct = luma(line[i]) / 255.0 * 100.0;
        selected[i] = SORT_ABOVE_THRESHOLD ? (pct > SORT_THRESHOLD_PERCENT)
                                           : (pct < SORT_THRESHOLD_PERCENT);
    }
    std::vector<uint8_t> vals;
    int i = 0;
    while (i < n) {
        if (!selected[i]) { ++i; continue; }
        int j = i;
        while (j < n && selected[j]) ++j;
        if (j - i >= 2) sortRun(line, i, j, mode, vals);
        i = j;
    }
}

static void pixelSort(Image& img) {
    const bool horiz = SORT_HORIZONTAL;
    const int lines = horiz ? img.h : img.w;
    const int len   = horiz ? img.w : img.h;
    std::vector<Pixel> line(len);

    for (auto [s0, s1] : makeStrips(lines, SORT_STRIP_MIN, SORT_STRIP_MAX)) {
        if (!chance(SORT_PROB)) continue;

        SortMode mode = (SORT_MIX_MODES && !SORT_MIX_LIST.empty())
            ? SORT_MIX_LIST[randInt(0, static_cast<int>(SORT_MIX_LIST.size()) - 1)]
            : SORT_FIXED_MODE;
        if (mode == SortMode::None) continue;

        for (int l = s0; l < s1; ++l) {
            for (int p = 0; p < len; ++p) line[p] = at(img, l, p, horiz);
            sortLine(line, mode);
            for (int p = 0; p < len; ++p) at(img, l, p, horiz) = line[p];
        }
    }
}

// ---------- Smear (pixel stretching) ----------
// At a random point in a strip, the pixel color of every line is stretched
// over a random length.
static void pixelSmear(Image& img) {
    const bool horiz = SMEAR_HORIZONTAL;
    const int lines = horiz ? img.h : img.w;
    const int len   = horiz ? img.w : img.h;

    for (auto [s0, s1] : makeStrips(lines, SMEAR_STRIP_MIN, SMEAR_STRIP_MAX)) {
        if (!chance(SMEAR_PROB)) continue;
        for (int k = 0; k < SMEAR_ANCHORS; ++k) {
            int p0 = randInt(0, len - 1);
            int p1 = std::min(len, p0 + randInt(SMEAR_LEN_MIN, SMEAR_LEN_MAX));
            for (int l = s0; l < s1; ++l) {
                Pixel src = at(img, l, p0, horiz);
                for (int p = p0 + 1; p < p1; ++p) at(img, l, p, horiz) = src;
            }
        }
    }
}

// ---------- Pixel displacement ----------
// Bilinear resize (used to make the map match the size of the primary image)
static Image resizeBilinear(const Image& in, int w, int h) {
    Image out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        float fy = (y + 0.5f) * in.h / h - 0.5f;
        int y0 = static_cast<int>(std::floor(fy));
        float ty = fy - y0;
        int ya = std::clamp(y0, 0, in.h - 1);
        int yb = std::clamp(y0 + 1, 0, in.h - 1);
        for (int x = 0; x < w; ++x) {
            float fx = (x + 0.5f) * in.w / w - 0.5f;
            int x0 = static_cast<int>(std::floor(fx));
            float tx = fx - x0;
            int xa = std::clamp(x0, 0, in.w - 1);
            int xb = std::clamp(x0 + 1, 0, in.w - 1);
            const Pixel& p00 = in.px[static_cast<size_t>(ya) * in.w + xa];
            const Pixel& p10 = in.px[static_cast<size_t>(ya) * in.w + xb];
            const Pixel& p01 = in.px[static_cast<size_t>(yb) * in.w + xa];
            const Pixel& p11 = in.px[static_cast<size_t>(yb) * in.w + xb];
            Pixel& o = out.px[static_cast<size_t>(y) * w + x];
            for (int c = 0; c < 3; ++c) {
                float top = p00.c[c] + (p10.c[c] - p00.c[c]) * tx;
                float bot = p01.c[c] + (p11.c[c] - p01.c[c]) * tx;
                o.c[c] = static_cast<uint8_t>(std::lround(top + (bot - top) * ty));
            }
        }
    }
    return out;
}

// The map says where to take each pixel from: output(x, y) = source(x + dx, y + dy),
// where dx/dy come from the map pixel at the same position.
static void pixelDisplace(Image& img, const Image& mapIn) {
    Image resized;
    const Image* mp = &mapIn;
    if (mapIn.w != img.w || mapIn.h != img.h) {
        resized = resizeBilinear(mapIn, img.w, img.h);
        mp = &resized;
    }
    const Image& map = *mp;
    const Image source = img;  // unmodified copy to sample from

    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const size_t idx = static_cast<size_t>(y) * img.w + x;
            const Pixel& m = map.px[idx];
            double vx, vy;
            if (DISPLACE_FROM_LUMA) { vx = vy = luma(m); }
            else                    { vx = m.c[0]; vy = m.c[1]; }

            int sx = x + static_cast<int>(std::lround((vx - 127.5) / 127.5 * DISPLACE_SCALE_X));
            int sy = y + static_cast<int>(std::lround((vy - 127.5) / 127.5 * DISPLACE_SCALE_Y));
            if (DISPLACE_WRAP) {
                sx = ((sx % img.w) + img.w) % img.w;
                sy = ((sy % img.h) + img.h) % img.h;
            } else {
                sx = std::clamp(sx, 0, img.w - 1);
                sy = std::clamp(sy, 0, img.h - 1);
            }
            img.px[idx] = source.px[static_cast<size_t>(sy) * img.w + sx];
        }
    }
}

// ---------- JPEG compression corruption ----------
static void appendBytes(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<unsigned char>*>(ctx);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    v->insert(v->end(), p, p + size);
}

// Decodes JPEG bytes into img (the size must stay the same). Returns false if the decoder fails.
static bool decodeJpegInto(const std::vector<unsigned char>& jpg, Image& img) {
    int w, h, n;
    unsigned char* pixels = stbi_load_from_memory(jpg.data(), static_cast<int>(jpg.size()),
                                                  &w, &h, &n, 3);
    if (!pixels) return false;
    if (w != img.w || h != img.h) { stbi_image_free(pixels); return false; }
    std::copy(pixels, pixels + static_cast<size_t>(w) * h * 3,
              reinterpret_cast<unsigned char*>(img.px.data()));
    stbi_image_free(pixels);
    return true;
}

// Offset of the first byte of entropy-coded data (right after the SOS header), or 0 if not found.
static size_t findScanStart(const std::vector<unsigned char>& jpg) {
    size_t i = 2;  // skip SOI
    while (i + 4 <= jpg.size() && jpg[i] == 0xFF) {
        size_t len = (static_cast<size_t>(jpg[i + 2]) << 8) | jpg[i + 3];
        if (jpg[i + 1] == 0xDA) return i + 2 + len;
        i += 2 + len;
    }
    return 0;
}

// Compresses the image to a very low quality JPEG, damages random bytes of the compressed
// data (never the headers), and decodes it back. If the decoder rejects the damaged file,
// the number of damaged bytes is halved until it is accepted.
static void jpegCorrupt(Image& img) {
    for (int pass = 0; pass < JPEG_PASSES; ++pass) {
        std::vector<unsigned char> jpg;
        if (!stbi_write_jpg_to_func(appendBytes, &jpg, img.w, img.h, 3, img.px.data(), JPEG_QUALITY))
            return;

        const size_t scanStart = findScanStart(jpg);
        const size_t scanEnd = jpg.size() >= 2 ? jpg.size() - 2 : 0;  // before EOI
        int count = (scanStart > 0 && scanStart < scanEnd) ? JPEG_CORRUPT_BYTES : 0;

        while (true) {
            std::vector<unsigned char> bad = jpg;
            for (int k = 0; k < count; ++k) {
                for (int tries = 0; tries < 20; ++tries) {
                    size_t pos = scanStart + static_cast<size_t>(randInt(0, static_cast<int>(scanEnd - scanStart) - 1));
                    // Don't touch 0xFF bytes or the 0x00 stuffing byte that follows them,
                    // otherwise we could create fake markers
                    if (bad[pos] == 0xFF || (pos > 0 && bad[pos - 1] == 0xFF)) continue;
                    unsigned char b;
                    do { b = static_cast<unsigned char>(randInt(0, 255)); } while (b == 0xFF);
                    bad[pos] = b;
                    break;
                }
            }
            if (decodeJpegInto(bad, img)) break;
            if (count == 0) return;  // even the undamaged file failed: leave the image as is
            count /= 2;
        }
    }
}

// ---------- RGB channel multiplication ----------
static void rgbMultiply(Image& img) {
    double f[3] = {RGB_MUL_R, RGB_MUL_G, RGB_MUL_B};
    if (RGB_MUL_RANDOM)
        for (double& v : f) v = randReal(RGB_MUL_MIN, RGB_MUL_MAX);

    uint8_t lut[3][256];
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v)
            lut[c][v] = static_cast<uint8_t>(std::clamp<long>(std::lround(v * f[c]), 0, 255));
    for (Pixel& p : img.px)
        for (int c = 0; c < 3; ++c) p.c[c] = lut[c][p.c[c]];
}

// ---------- Subtract blending (two images) ----------
// result = image - map * strength (clamped at 0). The map is resized to the image size.
static void subtractBlend(Image& img, const Image& mapIn) {
    Image resized;
    const Image* mp = &mapIn;
    if (mapIn.w != img.w || mapIn.h != img.h) {
        resized = resizeBilinear(mapIn, img.w, img.h);
        mp = &resized;
    }
    const Image& other = *mp;
    for (size_t i = 0; i < img.px.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            double v = img.px[i].c[c] - other.px[i].c[c] * SUBTRACT_STRENGTH;
            if (SUBTRACT_ABSOLUTE) v = std::fabs(v);
            img.px[i].c[c] = static_cast<uint8_t>(std::clamp<long>(std::lround(v), 0, 255));
        }
    }
}

// ---------- RGB channel displacement ----------
// Each color channel is moved by its own (x, y) offset: out(x, y).c = source(x - dx, y - dy).c
static void rgbChannelDisplace(Image& img) {
    int off[3][2] = {{CHDISP_R_X, CHDISP_R_Y}, {CHDISP_G_X, CHDISP_G_Y}, {CHDISP_B_X, CHDISP_B_Y}};
    if (CHDISP_RANDOM)
        for (auto& o : off)
            for (int& v : o) v = randInt(-CHDISP_RANDOM_MAX, CHDISP_RANDOM_MAX);

    const Image source = img;
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            Pixel& out = img.px[static_cast<size_t>(y) * img.w + x];
            for (int c = 0; c < 3; ++c) {
                int sx = x - off[c][0];
                int sy = y - off[c][1];
                if (CHDISP_WRAP) {
                    sx = ((sx % img.w) + img.w) % img.w;
                    sy = ((sy % img.h) + img.h) % img.h;
                } else {
                    sx = std::clamp(sx, 0, img.w - 1);
                    sy = std::clamp(sy, 0, img.h - 1);
                }
                out.c[c] = source.px[static_cast<size_t>(sy) * img.w + sx].c[c];
            }
        }
    }
}

// ---------- Invert colors ----------
static void invertColors(Image& img) {
    for (Pixel& p : img.px)
        for (int c = 0; c < 3; ++c) p.c[c] = static_cast<uint8_t>(255 - p.c[c]);
}

// ---------- Input/output ----------
static bool loadImage(const fs::path& path, Image& img) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    int w, h, n;
    unsigned char* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()),
                                                  &w, &h, &n, 3);
    if (!pixels) return false;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<size_t>(w) * h);
    std::copy(pixels, pixels + static_cast<size_t>(w) * h * 3,
              reinterpret_cast<unsigned char*>(img.px.data()));
    stbi_image_free(pixels);
    return true;
}

static void writeCallback(void* ctx, void* data, int size) {
    static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), size);
}

static bool saveImage(const fs::path& path, const Image& img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    int ok;
    if (ext == ".jpg" || ext == ".jpeg")
        ok = stbi_write_jpg_to_func(writeCallback, &f, img.w, img.h, 3, img.px.data(), 95);
    else
        ok = stbi_write_png_to_func(writeCallback, &f, img.w, img.h, 3, img.px.data(), img.w * 3);
    return ok != 0 && f.good();
}

// ---------- Operations menu ----------
struct Operation {
    const char* name;
    void (*run)(Image&);                       // regular operation (nullptr if it needs a map)
    void (*runWithMap)(Image&, const Image&);  // operation that needs the second image
};

static void runContrast(Image& img) { applyContrast(img, CONTRAST_FACTOR); }

// The index shown to the user is the position in this list + 1
// The index shown to the user is the position in this list + 1.
// Operations that need a second image (runWithMap) must stay together at the end:
// the GUI separates them from the others with a blank space.
static const std::vector<Operation> OPERATIONS = {
    {"RGB split",                   rgbSplit,           nullptr},
    {"Horizontal slicing",          sliceHorizontal,    nullptr},
    {"Vertical slicing",            sliceVertical,      nullptr},
    {"Contrast",                    runContrast,        nullptr},
    {"Pixel sorting",               pixelSort,          nullptr},
    {"Pixel smear",                 pixelSmear,         nullptr},
    {"RGB channel multiplication",  rgbMultiply,        nullptr},
    {"RGB channel displacement",    rgbChannelDisplace, nullptr},
    {"Invert colors",               invertColors,       nullptr},
    {"JPEG compression corruption", jpegCorrupt,        nullptr},
    // ---- operations that need two images ----
    {"Pixel displacement",          nullptr,            pixelDisplace},
    {"Subtract blending",           nullptr,            subtractBlend},
};

// Parameters of operation number opIndex (0-based, same order as OPERATIONS); empty if it has none
static const std::vector<Param>& settingsFor(size_t opIndex) {
    static const std::vector<Param> none;
    if (opIndex >= OPERATIONS.size()) return none;
    for (const OpSettings& s : settingsRegistry())
        if (std::strcmp(s.opName, OPERATIONS[opIndex].name) == 0) return s.params;
    return none;
}

// Remembers the starting values of all settings (used by the "Defaults" button)
static void captureSettingDefaults() { settingsRegistry(); }
