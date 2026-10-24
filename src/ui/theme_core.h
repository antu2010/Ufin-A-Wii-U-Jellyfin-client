// Shared theme maths for Ufin's global colour customisation: hex codes, HSV, the custom palette and the way
// its three colours are arranged, readability helpers, and "what is the dominant colour of this artwork".
// Pure logic (no ImGui, no GPU), host-tested in tests/host/test_theme_core.cpp.
//
// The three colours every renderer consumes are the same ones the premade accents have always had:
//   main   (text / progress / switches that are on), deep (focused rows and buttons), second (gradients).
// A custom palette just supplies them differently; nothing in a renderer knows which kind it got.
#pragma once
#include "m3e_core.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace ui {
namespace theme {

using m3e::Rgb8;

// ---- HSV ----------------------------------------------------------------------------------------------

struct Hsv {
    float h = 0, s = 0, v = 0; // h in turns (0..1), s and v in 0..1
};

inline Hsv toHsv(Rgb8 c) {
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    Hsv o;
    o.v = mx;
    const float d = mx - mn;
    if (mx <= 0.0f || d < 1e-6f) return o; // black or grey: hue and saturation stay 0
    o.s = d / mx;
    float h;
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    o.h = h / 6.0f;
    return o;
}

inline Rgb8 fromHsv(float h, float s, float v) {
    h = h - std::floor(h);
    s = std::min(1.0f, std::max(0.0f, s));
    v = std::min(1.0f, std::max(0.0f, v));
    const float c = v * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = v - c;
    auto q = [&](float f) { return (uint8_t)std::min(255.0f, std::max(0.0f, (f + m) * 255.0f + 0.5f)); };
    Rgb8 o;
    o.r = q(r);
    o.g = q(g);
    o.b = q(b);
    return o;
}

// ---- hex codes ----------------------------------------------------------------------------------------

inline bool hexDigit(char c, int& v) {
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
    else if (c >= 'A' && c <= 'F') v = 10 + c - 'A';
    else return false;
    return true;
}

// "#RRGGBB", "RRGGBB", "#RGB" or "RGB" (any case, surrounding spaces ignored). False for anything else, with
// `out` untouched.
inline bool parseHex(const std::string& text, Rgb8& out) {
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n')) b--;
    if (a < b && text[a] == '#') a++;
    const size_t n = b - a;
    if (n != 6 && n != 3) return false;
    int d[6];
    for (size_t i = 0; i < n; i++)
        if (!hexDigit(text[a + i], d[i])) return false;
    if (n == 3) {
        out.r = (uint8_t)(d[0] * 17);
        out.g = (uint8_t)(d[1] * 17);
        out.b = (uint8_t)(d[2] * 17);
    } else {
        out.r = (uint8_t)(d[0] * 16 + d[1]);
        out.g = (uint8_t)(d[2] * 16 + d[3]);
        out.b = (uint8_t)(d[4] * 16 + d[5]);
    }
    return true;
}

inline std::string toHex(Rgb8 c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

// ---- the custom palette -------------------------------------------------------------------------------

// Three colours the user edits ("1", "2", "3") and an arrangement that says which of them plays which role.
// Arrangement 0 is the natural one (1 = main, 2 = deep, 3 = second); the other five are the remaining
// orderings, so one set of colours gives six different gradients without re-picking anything.
struct Custom {
    Rgb8 c[3] = {{0x00, 0xA4, 0xDC}, {0x0B, 0x7F, 0xB0}, {0xAA, 0x5C, 0xC3}}; // the default is Jellyfin blue
    int arrange = 0;
};

static const int kArrangeCount = 6;

// role[0] = main, role[1] = deep, role[2] = second  ->  index into Custom::c
inline void arrangeRoles(int arrange, int role[3]) {
    static const int perms[kArrangeCount][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    if (arrange < 0 || arrange >= kArrangeCount) arrange = 0;
    for (int i = 0; i < 3; i++) role[i] = perms[arrange][i];
}

struct Roles {
    Rgb8 main, deep, second;
};

inline Roles rolesOf(const Custom& p) {
    int r[3];
    arrangeRoles(p.arrange, r);
    return {p.c[r[0]], p.c[r[1]], p.c[r[2]]};
}

// "Main 1  Deep 2  Second 3" -- what the arrangement row says.
inline std::string arrangeLabel(int arrange) {
    int r[3];
    arrangeRoles(arrange, r);
    char buf[64];
    snprintf(buf, sizeof(buf), "Main %d  Deep %d  Second %d", r[0] + 1, r[1] + 1, r[2] + 1);
    return buf;
}

// A deeper shade of a colour (focused rows, pressed buttons) and a partner hue for gradients: what "Auto" fills
// in when the user only cares about one colour.
inline Rgb8 autoDeep(Rgb8 main) {
    m3e::Hsl h = m3e::toHsl(main);
    return m3e::fromHsl(h.h, h.s, h.l * 0.72f);
}
inline Rgb8 autoSecond(Rgb8 main) {
    m3e::Hsl h = m3e::toHsl(main);
    return m3e::fromHsl(h.h + 1.0f / 6.0f, h.s, h.l);
}

// Makes `c` readable against `bg` (WCAG contrast ratio >= minRatio) by moving its lightness away from the
// background's, keeping its hue and saturation. Unchanged when it already is.
inline Rgb8 ensureContrast(Rgb8 c, Rgb8 bg, float minRatio) {
    if (m3e::contrast(c, bg) >= minRatio) return c;
    const m3e::Hsl h = m3e::toHsl(c);
    const bool lighten = m3e::luminance(bg) < 0.4f;
    Rgb8 best = c;
    for (int i = 1; i <= 40; i++) {
        const float l = lighten ? h.l + (1.0f - h.l) * (i / 40.0f) : h.l * (1.0f - i / 40.0f);
        best = m3e::fromHsl(h.h, h.s, l);
        if (m3e::contrast(best, bg) >= minRatio) return best;
    }
    return best; // pure white / black: the best that can be done
}

// White or near-black, whichever reads better on `bg`.
inline Rgb8 onColour(Rgb8 bg) {
    const Rgb8 white{0xFF, 0xFF, 0xFF}, dark{0x14, 0x16, 0x1A};
    return m3e::contrast(white, bg) >= m3e::contrast(dark, bg) ? white : dark;
}

// ---- artwork colour -----------------------------------------------------------------------------------

// The colour that best represents a picture: the most "colourful" hue in it, averaged. RGBA8 pixels, sampled on
// a coarse grid (at most ~48x48 samples, however big the image), so it costs a few thousand operations once per
// picture. False when the picture has no usable colour (greyscale, almost black / white, empty or invalid) --
// the caller then keeps the selected palette.
inline bool dominantColour(const uint8_t* rgba, int w, int h, Rgb8& out) {
    if (!rgba || w <= 0 || h <= 0) return false;
    const int stepX = std::max(1, w / 48), stepY = std::max(1, h / 48);
    const int kBins = 12;
    float weight[kBins] = {};
    float sr[kBins] = {}, sg[kBins] = {}, sb[kBins] = {};
    float total = 0.0f;
    int samples = 0;
    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            const uint8_t* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (p[3] < 128) continue; // transparent
            samples++;
            const Rgb8 px{p[0], p[1], p[2]};
            const Hsv v = toHsv(px);
            if (v.s < 0.22f || v.v < 0.20f) continue; // grey, black: no colour to speak of
            if (v.v > 0.97f && v.s < 0.35f) continue;  // near white
            const float wgt = v.s * (0.35f + 0.65f * std::min(1.0f, v.v * 1.4f));
            int bin = (int)(v.h * kBins);
            if (bin >= kBins) bin = kBins - 1;
            weight[bin] += wgt;
            sr[bin] += px.r * wgt;
            sg[bin] += px.g * wgt;
            sb[bin] += px.b * wgt;
            total += wgt;
        }
    }
    if (samples == 0 || total < 0.03f * (float)samples) return false; // (mostly colourless)
    // A hue wins together with its two neighbours, so a gradient of similar reds is one red, not three bins.
    int best = 0;
    float bestW = -1.0f;
    for (int b = 0; b < kBins; b++) {
        const float s = weight[b] + 0.5f * (weight[(b + 1) % kBins] + weight[(b + kBins - 1) % kBins]);
        if (s > bestW) { bestW = s; best = b; }
    }
    if (weight[best] <= 0.0f) return false;
    out.r = (uint8_t)std::min(255.0f, sr[best] / weight[best] + 0.5f);
    out.g = (uint8_t)std::min(255.0f, sg[best] / weight[best] + 0.5f);
    out.b = (uint8_t)std::min(255.0f, sb[best] / weight[best] + 0.5f);
    return true;
}

} // namespace theme
} // namespace ui
