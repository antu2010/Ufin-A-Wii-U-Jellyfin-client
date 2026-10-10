#include "app_ui.h"
#include "glass_core.h"
#include "../help_content.h"
#include "model3d.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace ui {

// --- palette ---

namespace col {
// Mutable, not const: setLightMode() below reassigns the whole block in
// place so every one of the ~100 call sites elsewhere in this file that
// reads e.g. col::BG keeps working unchanged, dark or light. Semantic
// colours (error/live/white) don't need a light variant and stay const.
static ImU32 BG          = IM_COL32(0x14, 0x16, 0x1A, 0xFF);
static ImU32 SIDEBAR     = IM_COL32(0x1B, 0x1E, 0x24, 0xFF);
static ImU32 PANEL       = IM_COL32(0x23, 0x27, 0x2E, 0xFF);
static ImU32 PANEL_HI    = IM_COL32(0x2C, 0x31, 0x3A, 0xFF);

static ImU32 TEXT        = IM_COL32(0xEC, 0xEE, 0xF1, 0xFF);
static ImU32 MUTED       = IM_COL32(0x9A, 0xA0, 0xA8, 0xFF);
static ImU32 DIM         = IM_COL32(0x5F, 0x65, 0x6E, 0xFF);
static ImU32 TRACK       = IM_COL32(0x3A, 0x3F, 0x48, 0xFF);
static const ImU32 ERROR_RED   = IM_COL32(0xD9, 0x48, 0x5F, 0xFF);
static const ImU32 LIVE_RED    = IM_COL32(0xE5, 0x39, 0x35, 0xFF);
static const ImU32 WHITE       = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
} // namespace col

// --- theme (accent colour, ambient background, snow) ---

struct AccentColours {
    ImU32 main, deep, second;
};
static AccentColours g_accent = {IM_COL32(0x00, 0xA4, 0xDC, 0xFF), IM_COL32(0x0B, 0x7F, 0xB0, 0xFF),
                                 IM_COL32(0xAA, 0x5C, 0xC3, 0xFF)};
static bool g_rainbow = false;
static Background g_background = Background::Off;
static bool g_snow = false;
static bool g_light = false; // false = dark palette (default), true = light

static ImU32 accentMain() { return g_accent.main; }
static ImU32 accentDeep() { return g_accent.deep; }
static ImU32 accentSecond() { return g_accent.second; }

static ImU32 rgb(uint32_t v, int a = 0xFF) { return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, a); }

// Linear blend between two packed colours, t in 0..1.
static ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    auto ch = [&](int shift) -> int {
        int ca = (int)((a >> shift) & 0xFF), cb = (int)((b >> shift) & 0xFF);
        return ca + (int)((cb - ca) * t);
    };
    return IM_COL32(ch(0), ch(8), ch(16), ch(24));
}


static ImU32 hsv(float h, float sat, float val) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), sat, val, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

static Accent g_accentStyle = Accent::Blue;

const char* accentName(Accent a) {
    switch (a) {
        case Accent::Blue:    return "Jellyfin blue";
        case Accent::Purple:  return "Purple";
        case Accent::Green:   return "Green";
        case Accent::Orange:  return "Orange";
        case Accent::Pink:    return "Pink";
        case Accent::Teal:    return "Teal";
        case Accent::Crimson: return "Crimson";
        case Accent::Amber:   return "Amber";
        case Accent::Indigo:  return "Indigo";
        case Accent::Rainbow: return "Rainbow";
        default:              return "";
    }
}

void setAccent(Accent a) {
    g_accentStyle = a;
    g_rainbow = (a == Accent::Rainbow);
    switch (a) {
        case Accent::Purple:  g_accent = {rgb(0xAA5CC3), rgb(0x7E3F99), rgb(0x00A4DC)}; break;
        case Accent::Green:   g_accent = {rgb(0x2ECC71), rgb(0x1E8C4E), rgb(0x00A4DC)}; break;
        case Accent::Orange:  g_accent = {rgb(0xFF9F43), rgb(0xC7702A), rgb(0xE84393)}; break;
        case Accent::Pink:    g_accent = {rgb(0xFF6BB5), rgb(0xC2447F), rgb(0x7E57C2)}; break;
        // Four new colourways, same {main, deep, secondary} shape as the
        // originals above (secondary is only used for gradients/ambient).
        case Accent::Teal:    g_accent = {rgb(0x1ABC9C), rgb(0x128F76), rgb(0x3498DB)}; break;
        case Accent::Crimson: g_accent = {rgb(0xE74C3C), rgb(0xB03A2E), rgb(0xF39C12)}; break;
        case Accent::Amber:   g_accent = {rgb(0xF1C40F), rgb(0xB7950B), rgb(0xE67E22)}; break;
        case Accent::Indigo:  g_accent = {rgb(0x5C6BC0), rgb(0x3F4A8C), rgb(0x26C6DA)}; break;
        case Accent::Rainbow: tickTheme(0.0); break;
        default:              g_accent = {rgb(0x00A4DC), rgb(0x0B7FB0), rgb(0xAA5CC3)}; break;
    }
}

void tickTheme(double time) {
    if (!g_rainbow) return;
    float h = (float)std::fmod(time * 0.06, 1.0);
    g_accent = {hsv(h, 0.72f, 0.95f), hsv(h, 0.78f, 0.66f), hsv(h + 0.33f, 0.6f, 0.9f)};
}

const char* backgroundEffectName(Background b) {
    switch (b) {
        case Background::Off:        return "Off";
        case Background::Circles:    return "Circles";
        case Background::WiiBubbles: return "Bubbles";
        case Background::Gradient:   return "Gradient";
        case Background::Polygons:   return "Polygons";
        case Background::Starfield:  return "Starfield";
        case Background::Grid:       return "Grid";
        case Background::Aurora:     return "Aurora";
        case Background::Ripples:    return "Ripples";
        case Background::Beams:      return "Beams";
        case Background::Orbs:       return "Orbs";
        case Background::Waves:      return "Waves";
        default:                     return "";
    }
}

void setBackgroundEffect(Background b) { g_background = b; }

// Swaps the neutral half of the palette (col::BG/SIDEBAR/PANEL/PANEL_HI/
// TEXT/MUTED/DIM/TRACK) between the dark defaults and a light set, so
// every existing accent colourway gets a light counterpart for free --
// nothing that draws with col::* needs to know or care which is active.
void setLightMode(bool on) {
    g_light = on;
    if (on) {
        col::BG       = IM_COL32(0xF6, 0xF7, 0xF9, 0xFF);
        col::SIDEBAR  = IM_COL32(0xED, 0xEE, 0xF1, 0xFF);
        col::PANEL    = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
        col::PANEL_HI = IM_COL32(0xE7, 0xE9, 0xED, 0xFF);
        col::TEXT     = IM_COL32(0x1A, 0x1D, 0x21, 0xFF);
        col::MUTED    = IM_COL32(0x6B, 0x72, 0x80, 0xFF);
        col::DIM      = IM_COL32(0xC3, 0xC7, 0xCC, 0xFF);
        col::TRACK    = IM_COL32(0xD8, 0xDB, 0xE0, 0xFF);
    } else {
        col::BG       = IM_COL32(0x14, 0x16, 0x1A, 0xFF);
        col::SIDEBAR  = IM_COL32(0x1B, 0x1E, 0x24, 0xFF);
        col::PANEL    = IM_COL32(0x23, 0x27, 0x2E, 0xFF);
        col::PANEL_HI = IM_COL32(0x2C, 0x31, 0x3A, 0xFF);
        col::TEXT     = IM_COL32(0xEC, 0xEE, 0xF1, 0xFF);
        col::MUTED    = IM_COL32(0x9A, 0xA0, 0xA8, 0xFF);
        col::DIM      = IM_COL32(0x5F, 0x65, 0x6E, 0xFF);
        col::TRACK    = IM_COL32(0x3A, 0x3F, 0x48, 0xFF);
    }
    applyTheme(); // refresh ImGui's own style colours (little uses them)
}

// Soft drifting glows: each blob is a stack of faint circles, which reads
// as one blurred shape. The original (and only) animated background.
static void drawBgCircles(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const ImU32 colours[3] = {accentMain(), accentSecond(), accentDeep()};
    for (int i = 0; i < 3; i++) {
        float cx = 1280.0f * (0.45f + 0.32f * (float)std::sin(t * 0.045 + i * 2.1));
        float cy = 720.0f * (0.5f + 0.3f * (float)std::cos(t * 0.038 + i * 1.3));
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(colours[i]);
        for (int k = 0; k < 7; k++) {
            float r = 320.0f - k * 40.0f;
            d->AddCircleFilled(ImVec2(cx, cy), r, ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.045f)), 48);
        }
    }
}

// Wii U HOME Menu-style bubbles: small rings rising from the bottom of
// the screen and looping back around, instead of the "Circles" mode's
// slow-drifting glow blobs.
static void drawBgWiiBubbles(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < 26; i++) {
        uint32_t hsh = (uint32_t)i * 2654435761u;
        float r1 = (hsh & 0xFFFF) / 65535.0f, r2 = ((hsh >> 16) & 0xFFFF) / 65535.0f;
        float speed = 14.0f + r2 * 22.0f;
        float size = 10.0f + r1 * 34.0f;
        float y = 720.0f - (float)std::fmod(r2 * 720.0 + t * speed, 800.0);
        float x = r1 * 1280.0f + 26.0f * (float)std::sin(t * (0.25 + r2 * 0.4) + i * 1.7);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4((i % 2 == 0) ? accentMain() : accentSecond());
        d->AddCircle(ImVec2(x, y), size, ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.10f + 0.05f * r1)),
                     24, 1.6f + r1 * 1.4f);
        d->AddCircleFilled(ImVec2(x, y), size * 0.18f, ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.12f)), 12);
    }
}

// A slowly rotating, low-alpha diagonal gradient wash across the whole
// background. Kept faint (alpha ~0.14) and drawn before every panel/list
// row, so it never competes with foreground text.
static void drawBgGradient(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const float x0 = 0.0f, y0 = 0.0f, x1 = 1280.0f, y1 = 720.0f;
    d->PathRect(ImVec2(x0, y0), ImVec2(x1, y1), 0.0f);
    const int n = d->_Path.Size;
    if (n < 3) { d->PathClear(); return; }
    float angle = (float)std::fmod(t * 0.035, 1.0) * 6.2831853f;
    ImVec2 dir(std::cos(angle), std::sin(angle));
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(accentDeep());
    ImVec4 cb = ImGui::ColorConvertU32ToFloat4(accentSecond());
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) * 0.5f;
    auto colourAt = [&](const ImVec2& p) {
        float proj = ((p.x - cx) * dir.x + (p.y - cy) * dir.y) / diag;
        float tt = proj * 0.5f + 0.5f;
        tt = tt < 0.0f ? 0.0f : (tt > 1.0f ? 1.0f : tt);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * tt, ca.y + (cb.y - ca.y) * tt,
                                                     ca.z + (cb.z - ca.z) * tt, 0.14f));
    };
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve(n * 3, n + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(ImVec2(cx, cy), uv, colourAt(ImVec2(cx, cy)));
    for (int i = 0; i < n; i++) d->PrimWriteVtx(d->_Path[i], uv, colourAt(d->_Path[i]));
    for (int i = 0; i < n; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
    }
    d->PathClear();
}

// Faint drifting, slowly rotating polygons (triangles through hexagons).
static void drawBgPolygons(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < 8; i++) {
        uint32_t hsh = (uint32_t)i * 2654435761u;
        float r1 = (hsh & 0xFFFF) / 65535.0f, r2 = ((hsh >> 16) & 0xFFFF) / 65535.0f;
        float cx = 1280.0f * (0.1f + 0.8f * r1) + 40.0f * (float)std::sin(t * 0.05 + i * 1.9);
        float cy = 720.0f * (0.15f + 0.75f * r2) + 30.0f * (float)std::cos(t * 0.045 + i * 1.3);
        float rot = (float)(t * (0.08 + 0.05 * r1) + i);
        float size = 46.0f + 30.0f * r2;
        int sides = 3 + (i % 4); // 3..6
        ImVec4 c = ImGui::ColorConvertU32ToFloat4((i % 2 == 0) ? accentMain() : accentSecond());
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.07f));
        ImVec2 pts[6];
        for (int k = 0; k < sides; k++) {
            float a = rot + k * (6.2831853f / (float)sides);
            pts[k] = ImVec2(cx + std::cos(a) * size, cy + std::sin(a) * size);
        }
        d->AddConvexPolyFilled(pts, sides, col);
    }
}

// A field of small, gently twinkling points drifting sideways.
static void drawBgStarfield(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < 70; i++) {
        uint32_t hsh = (uint32_t)i * 2654435761u;
        float r1 = (hsh & 0xFFFF) / 65535.0f, r2 = ((hsh >> 16) & 0xFFFF) / 65535.0f;
        float speed = 4.0f + r2 * 10.0f;
        float x = (float)std::fmod(r1 * 1280.0 + t * speed, 1320.0) - 20.0f;
        float y = r2 * 720.0f;
        float twinkle = 0.35f + 0.35f * (float)std::sin(t * (1.2 + r1 * 1.5) + i);
        float size = 1.2f + r1 * 2.2f;
        ImVec4 c = ImGui::ColorConvertU32ToFloat4((i % 3 == 0) ? accentSecond() : col::TEXT);
        d->AddCircleFilled(ImVec2(x, y), size,
                            ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.10f + 0.12f * twinkle)), 8);
    }
}

// --- Liquid Glass showcase backgrounds ---
// Added after the originals (the old modes above are untouched). Liquid Glass bends a snapshot
// of whatever is drawn behind a panel, strongest at its rim, so these are built from crisp,
// structured shapes -- lines, rings, hard edges -- that visibly warp as a panel passes over
// them. Alphas stay low (<= ~0.5, mostly far less) so text on top stays readable in dark and light mode.

// Same RGB, new alpha (0..1).
static ImU32 bgTint(ImU32 c, float a) {
    return (c & 0x00FFFFFFu) | ((ImU32)(a * 255.0f + 0.5f) << 24);
}

// A slowly scrolling grid whose lines ripple. Straight lines are the classic refraction test:
// every panel edge visibly bends them.
static void drawBgGrid(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const float spacing = 64.0f;
    const float scrollX = (float)std::fmod(t * 9.0, (double)spacing);
    const float scrollY = (float)std::fmod(t * 6.0, (double)spacing);
    ImVec2 pts[17];
    for (int i = -1; i <= 21; i++) { // vertical lines, wavering sideways
        const float x0 = i * spacing + scrollX;
        const bool major = (((i % 4) + 4) % 4) == 0;
        for (int k = 0; k < 13; k++) {
            const float y = k * 60.0f;
            pts[k] = ImVec2(x0 + 13.0f * (float)std::sin(t * 0.5 + y * 0.012 + i * 0.4), y);
        }
        d->AddPolyline(pts, 13, bgTint((i % 2 == 0) ? accentMain() : accentSecond(), major ? 0.36f : 0.22f), 0,
                       major ? 2.2f : 1.5f);
    }
    for (int j = -1; j <= 12; j++) { // horizontal lines, rolling up and down
        const float y0 = j * spacing + scrollY;
        const bool major = (((j % 4) + 4) % 4) == 0;
        for (int k = 0; k < 17; k++) {
            const float x = k * 80.0f;
            pts[k] = ImVec2(x, y0 + 11.0f * (float)std::sin(t * 0.45 + x * 0.01 + j * 0.5));
        }
        d->AddPolyline(pts, 17, bgTint((j % 2 == 0) ? accentSecond() : accentMain(), major ? 0.36f : 0.22f), 0,
                       major ? 2.2f : 1.5f);
    }
}

// Soft northern-lights curtains: three translucent ribbons that sway and breathe. Each is one
// triangle strip, fading to nothing at its top and bottom edge.
static void drawBgAurora(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 tints[3] = {accentMain(), accentSecond(), accentDeep()};
    const int cols = 65;
    const float step = 1280.0f / (float)(cols - 1);
    for (int k = 0; k < 3; k++) {
        d->PrimReserve((cols - 1) * 12, cols * 3);
        const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
        const ImU32 mid = bgTint(tints[k], 0.34f), edge = bgTint(tints[k], 0.0f);
        for (int i = 0; i < cols; i++) {
            const float x = i * step;
            const float cy = 720.0f * (0.28f + 0.14f * k) + 70.0f * (float)std::sin(x * 0.004 + t * 0.07 + k * 2.0) +
                             38.0f * (float)std::sin(x * 0.011 - t * 0.05 + k);
            const float hh = 105.0f + 45.0f * (float)std::sin(x * 0.006 + t * 0.04 + k * 1.7);
            d->PrimWriteVtx(ImVec2(x, cy - hh), uv, edge);
            d->PrimWriteVtx(ImVec2(x, cy), uv, mid);
            d->PrimWriteVtx(ImVec2(x, cy + hh * 0.8f), uv, edge);
        }
        for (int i = 0; i < cols - 1; i++) {
            const ImDrawIdx a = (ImDrawIdx)(base + i * 3), b = (ImDrawIdx)(base + (i + 1) * 3);
            const ImDrawIdx q[12] = {a, b, (ImDrawIdx)(b + 1), a, (ImDrawIdx)(b + 1), (ImDrawIdx)(a + 1),
                                     (ImDrawIdx)(a + 1), (ImDrawIdx)(b + 1), (ImDrawIdx)(b + 2),
                                     (ImDrawIdx)(a + 1), (ImDrawIdx)(b + 2), (ImDrawIdx)(a + 2)};
            for (int n = 0; n < 12; n++) d->PrimWriteIdx(q[n]);
        }
    }
}

// Water ripples: rings spreading out from three drifting points and fading. Crisp circular arcs
// bend into oval shapes under a glass panel.
static void drawBgRipples(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const float maxR = 500.0f;
    for (int s = 0; s < 3; s++) {
        const float cx = 1280.0f * (0.5f + 0.34f * (float)std::sin(t * 0.031 + s * 2.3));
        const float cy = 720.0f * (0.5f + 0.30f * (float)std::cos(t * 0.027 + s * 1.7));
        for (int n = 0; n < 7; n++) {
            const float r = (float)std::fmod(t * 30.0 + n * (maxR / 7.0f) + s * 41.0, (double)maxR);
            const float life = 1.0f - r / maxR; // 1 at birth, 0 at the edge
            d->AddCircle(ImVec2(cx, cy), r + 4.0f, bgTint((s + n) % 2 == 0 ? accentMain() : accentSecond(), 0.45f * life * life),
                         80, 1.5f + 2.0f * life);
        }
    }
}

// Slanted shafts of light sweeping slowly across the screen. The hard edges of each beam are
// what refraction bends most visibly.
static void drawBgBeams(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < 6; i++) {
        const uint32_t hsh = (uint32_t)(i + 1) * 2654435761u;
        const float r1 = (hsh & 0xFFFF) / 65535.0f;
        const float x = 1280.0f * ((i + 0.5f) / 6.0f) + 110.0f * (float)std::sin(t * 0.045 + i * 1.7);
        const float w = 60.0f + 70.0f * r1;
        const float slant = 240.0f;
        const float pulse = 0.5f + 0.5f * (float)std::sin(t * (0.3 + 0.25 * r1) + i * 2.0);
        const ImU32 tint = (i % 2 == 0) ? accentMain() : accentSecond();
        const ImVec2 q[4] = {ImVec2(x - w * 0.5f + slant, -20.0f), ImVec2(x + w * 0.5f + slant, -20.0f),
                             ImVec2(x + w * 0.5f - slant, 740.0f), ImVec2(x - w * 0.5f - slant, 740.0f)};
        d->AddConvexPolyFilled(q, 4, bgTint(tint, 0.05f + 0.05f * pulse));
        d->AddLine(q[0], q[3], bgTint(tint, 0.14f + 0.10f * pulse), 1.6f);
        d->AddLine(q[1], q[2], bgTint(tint, 0.14f + 0.10f * pulse), 1.6f);
    }
}

// Glassy beads: big translucent discs with a bright rim and a highlight, wandering on slow
// Lissajous paths. A panel over one magnifies and bends its rim.
static void drawBgOrbs(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < 7; i++) {
        const uint32_t hsh = (uint32_t)(i + 1) * 2654435761u;
        const float r1 = (hsh & 0xFFFF) / 65535.0f, r2 = ((hsh >> 16) & 0xFFFF) / 65535.0f;
        const float x = 1280.0f * (0.5f + 0.42f * (float)std::sin(t * (0.030 + 0.012 * r1) + i * 1.9));
        const float y = 720.0f * (0.5f + 0.40f * (float)std::cos(t * (0.027 + 0.012 * r2) + i * 1.3));
        const float rad = 50.0f + 90.0f * r1;
        const ImU32 tint = (i % 3 == 0) ? accentSecond() : accentMain();
        d->AddCircleFilled(ImVec2(x, y), rad, bgTint(tint, 0.13f), 64);
        d->AddCircle(ImVec2(x, y), rad, bgTint(tint, 0.32f), 64, 2.5f);
        d->AddCircleFilled(ImVec2(x - rad * 0.38f, y - rad * 0.40f), rad * 0.22f, bgTint(col::TEXT, 0.10f), 24);
    }
}

// Three layers of rolling waves along the bottom of the screen, each a translucent fill under a
// crisp crest line. The crests are long curves that a glass panel visibly bends.
static void drawBgWaves(double t) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const int cols = 81;
    const float step = 1280.0f / (float)(cols - 1);
    ImVec2 crest[81];
    for (int layer = 0; layer < 3; layer++) {
        const float baseY = 720.0f * (0.58f + 0.12f * layer);
        const float amp = 26.0f + 8.0f * layer;
        const ImU32 tint = (layer == 1) ? accentSecond() : accentMain();
        const ImU32 top = bgTint(tint, 0.22f), bottom = bgTint(tint, 0.04f);
        for (int i = 0; i < cols; i++) {
            const float x = i * step;
            crest[i] = ImVec2(x, baseY + amp * (float)std::sin(x * 0.007 * (1.0 + 0.3 * layer) + t * (0.25 + 0.1 * layer) + layer * 2.0) +
                                     0.5f * amp * (float)std::sin(x * 0.016 - t * 0.3 + layer));
        }
        d->PrimReserve((cols - 1) * 6, cols * 2);
        const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
        for (int i = 0; i < cols; i++) {
            d->PrimWriteVtx(crest[i], uv, top);
            d->PrimWriteVtx(ImVec2(crest[i].x, 730.0f), uv, bottom);
        }
        for (int i = 0; i < cols - 1; i++) {
            const ImDrawIdx a = (ImDrawIdx)(base + i * 2), b = (ImDrawIdx)(base + (i + 1) * 2);
            const ImDrawIdx q[6] = {a, b, (ImDrawIdx)(b + 1), a, (ImDrawIdx)(b + 1), (ImDrawIdx)(a + 1)};
            for (int n = 0; n < 6; n++) d->PrimWriteIdx(q[n]);
        }
        d->AddPolyline(crest, cols, bgTint(tint, 0.48f), 0, 2.0f);
    }
}

static void drawBackgroundEffect() {
    if (g_background == Background::Off) return;
    const double t = ImGui::GetTime();
    switch (g_background) {
        case Background::Circles:    drawBgCircles(t); break;
        case Background::WiiBubbles: drawBgWiiBubbles(t); break;
        case Background::Gradient:   drawBgGradient(t); break;
        case Background::Polygons:   drawBgPolygons(t); break;
        case Background::Starfield:  drawBgStarfield(t); break;
        case Background::Grid:       drawBgGrid(t); break;
        case Background::Aurora:     drawBgAurora(t); break;
        case Background::Ripples:    drawBgRipples(t); break;
        case Background::Beams:      drawBgBeams(t); break;
        case Background::Orbs:       drawBgOrbs(t); break;
        case Background::Waves:      drawBgWaves(t); break;
        default: break;
    }
}

void drawSnow(double time) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    for (int i = 0; i < 90; i++) {
        // Cheap per-flake randomness from the index.
        uint32_t hsh = (uint32_t)i * 2654435761u;
        float r1 = (hsh & 0xFFFF) / 65535.0f, r2 = ((hsh >> 16) & 0xFFFF) / 65535.0f;
        float speed = 22.0f + r2 * 45.0f;
        float size = 1.4f + r1 * 2.6f;
        float y = (float)std::fmod(r2 * 740.0 + time * speed, 740.0) - 10.0f;
        float x = r1 * 1280.0f + 18.0f * (float)std::sin(time * (0.5 + r2) + i);
        fg->AddCircleFilled(ImVec2(x, y), size, IM_COL32(255, 255, 255, 150 + (int)(r1 * 90)), 8);
    }
}

static const float TEXT_L = 30.0f;  // titles

static const float TEXT_M = 24.0f;  // list names, body
static const float TEXT_S = 19.0f;  // details, hints

static ImDrawList* dl() { return ImGui::GetBackgroundDrawList(); }
static ImFont* font() { return ImGui::GetFont(); }

static ImVec2 textSize(float size, const std::string& text) {
    return font()->CalcTextSizeA(size, 1e9f, 0.0f, text.c_str());
}

std::string fitText(ImFont* f, float size, const std::string& text, float maxWidth) {
    if (maxWidth <= 0.0f) return "";
    if (f->CalcTextSizeA(size, 1e9f, 0.0f, text.c_str()).x <= maxWidth) return text;
    const std::string dots = "...";
    size_t len = text.size();
    while (len > 0) {
        // Step back one UTF-8 character.
        len--;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) len--;
        std::string candidate = text.substr(0, len) + dots;
        if (f->CalcTextSizeA(size, 1e9f, 0.0f, candidate.c_str()).x <= maxWidth) return candidate;
    }
    return f->CalcTextSizeA(size, 1e9f, 0.0f, dots.c_str()).x <= maxWidth ? dots : "";
}

static void text(float size, float x, float y, ImU32 color, const std::string& s, float maxWidth = 0.0f) {
    std::string t = maxWidth > 0.0f ? fitText(font(), size, s, maxWidth) : s;
    dl()->AddText(font(), size, ImVec2(x, y), color, t.c_str());
}

static void textRight(float size, float right, float y, ImU32 color, const std::string& s) {
    dl()->AddText(font(), size, ImVec2(right - textSize(size, s).x, y), color, s.c_str());
}

static void rect(const Rect& r, ImU32 color, float rounding = 0.0f) {
    dl()->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), color, rounding);
}

// Rounded rectangle with a diagonal gradient (top-left colour a to
// bottom-right colour b). ImGui's own gradient rect has square corners,
// so build it as a triangle fan over the rounded outline, colouring
// each vertex by where it sits along the diagonal.
static void gradientRoundRect(float x0, float y0, float x1, float y1, float rounding, ImU32 a, ImU32 b,
                              ImDrawList* onList = nullptr) {
    ImDrawList* d = onList ? onList : dl();
    d->PathRect(ImVec2(x0, y0), ImVec2(x1, y1), rounding);
    const int n = d->_Path.Size;
    if (n < 3) { d->PathClear(); return; }
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
    auto colourAt = [&](const ImVec2& p) {
        float t = ((p.x - x0) / (x1 - x0) + (p.y - y0) / (y1 - y0)) * 0.5f;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                                     ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
    };
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImVec2 centre((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    d->PrimReserve(n * 3, n + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(centre, uv, colourAt(centre));
    for (int i = 0; i < n; i++) d->PrimWriteVtx(d->_Path[i], uv, colourAt(d->_Path[i]));
    for (int i = 0; i < n; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
    }
    d->PathClear();
}


// --- Liquid Glass -----------------------------------------------------------
//
// "The Wii U system UI, given a modern glass treatment": rounded, translucent
// surfaces that sit on one shared, low-resolution blurred backdrop (see
// glass_core.h), softly lit from above with a slow highlight that drifts
// across whatever has focus. All of it is ordinary ImGui geometry -- a few
// rounded rectangles and one textured rounded rectangle per surface -- so it
// needs no shaders, render targets or per-frame allocations of its own.
// With glass off nothing here runs and every surface is drawn exactly as
// before (surfaceRect() falls straight through to rect()).

static bool g_glass = false;
static std::unordered_map<uint64_t, ImU32> g_artColours; // artwork texture -> average colour
static uint64_t g_artTarget = 0;                         // artwork the current screen wants to pick up
static glass::Rgb g_artCur;
static float g_artAmt = 0.0f;
static double g_glassLastTime = 0.0;
static float g_glassBlend = 1.0f; // cross-fade position between the previous and newest blur (see BackdropManager::blend)

// Real refraction (see glass_core.h): the platform's snapshot texture and the
// draw-list callback that fills it from the colour buffer. Unset = no refraction;
// glass then uses the soft blurred backdrop.
static uint64_t g_snapTex = 0;
static ImDrawCallback g_snapCb = nullptr;
static void* g_snapData = nullptr;
static glass::SnapshotPlanner g_snapPlanner;

void setGlassSnapshot(uint64_t texture, GlassSnapshotCallback callback, void* userData) {
    g_snapTex = (texture && callback) ? texture : 0;
    g_snapCb = g_snapTex ? callback : nullptr;
    g_snapData = g_snapTex ? userData : nullptr;
}
static bool refractionOn() { return g_glass && g_snapTex && g_snapCb; }
bool glassRefractionActive() { return refractionOn(); }
int glassSnapshotsThisFrame() { return g_snapPlanner.snapshotsThisFrame(); }

void setGlass(bool on) { g_glass = on; }
bool glassEnabled() { return g_glass; }
bool glassBlurAvailable() { return glass::backdrop().active(); }

static glass::Rgb toRgb(ImU32 c) {
    glass::Rgb r;
    r.r = (float)(c & 0xFF) / 255.0f;
    r.g = (float)((c >> 8) & 0xFF) / 255.0f;
    r.b = (float)((c >> 16) & 0xFF) / 255.0f;
    return r;
}

void glassNoteArtwork(uint64_t texture, const uint8_t* rgba, int width, int height) {
    if (!g_glass || !texture || !rgba || width <= 0 || height <= 0) return;
    // 8x8 grid sample, weighted towards colourful pixels so a mostly-black
    // poster still yields its dominant hue rather than grey.
    float sr = 0, sg = 0, sb = 0, sw = 0;
    for (int gy = 0; gy < 8; gy++) {
        for (int gx = 0; gx < 8; gx++) {
            const int x = (gx * 2 + 1) * width / 16, y = (gy * 2 + 1) * height / 16;
            const uint8_t* p = rgba + ((size_t)y * width + x) * 4;
            const float r = p[0], g = p[1], b = p[2];
            const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
            const float wgt = 0.15f + (mx - mn) / 255.0f;
            sr += r * wgt; sg += g * wgt; sb += b * wgt; sw += wgt;
        }
    }
    if (sw <= 0.0f) return;
    g_artColours[texture] = IM_COL32((int)(sr / sw), (int)(sg / sw), (int)(sb / sw), 0xFF);
}

void glassForgetArtwork(uint64_t texture) {
    g_artColours.erase(texture);
    if (g_artTarget == texture) g_artTarget = 0;
}

// The current screen's focused artwork (set while drawing, used next frame).
static void glassArtFocus(uint64_t texture) {
    if (g_glass && texture) g_artTarget = texture;
}

void glassFrame(double time, bool videoActive) {
    if (!g_glass) return;
    glass::BackdropManager& bd = glass::backdrop();
    double dt = time - g_glassLastTime;
    g_glassLastTime = time;
    if (!(dt > 0.0 && dt < 0.25)) dt = 1.0 / 60.0;
    if (!bd.active()) {
        g_artTarget = 0;
        return;
    }
    // Ease towards the focused artwork's colour (or away when there is none).
    auto it = g_artTarget ? g_artColours.find(g_artTarget) : g_artColours.end();
    const bool have = it != g_artColours.end();
    const float k = (float)std::min(1.0, dt * 2.5);
    if (have) {
        const glass::Rgb t = toRgb(it->second);
        g_artCur.r += (t.r - g_artCur.r) * k;
        g_artCur.g += (t.g - g_artCur.g) * k;
        g_artCur.b += (t.b - g_artCur.b) * k;
    }
    g_artAmt += ((have ? 1.0f : 0.0f) - g_artAmt) * k;
    if (g_artAmt < 0.01f) g_artAmt = 0.0f;
    g_artTarget = 0; // screens set it again while drawing this frame

    glass::BackdropParams p;
    p.time = time;
    p.base = toRgb(col::BG);
    p.accent = toRgb(g_accent.main);
    p.accent2 = toRgb(g_accent.second);
    p.art = g_artCur;
    p.artStrength = g_artAmt * 0.7f; // faint: the artwork must never be drowned out
    p.light = g_light;
    p.animated = true;
    bd.update(time, p, videoActive);
    g_glassBlend = bd.blend(time);
}

enum class GK { Panel, Row, RowFocus, RowFocusDim, Overlay, Button, ButtonHot, ButtonPressed, ButtonDisabled };

// Corner radius in glass mode: rounder, like Wii U tiles.
static float gr(float r) { return g_glass ? r * 1.45f : r; }

// --- Shape-aware geometry ---------------------------------------------------
//
// Every glass effect that lives *inside* a bubble (rim light, sheen, drifting
// highlight, accent strips, slider fills) is built from the bubble's own
// rounded outline, clipped to whatever band it needs, instead of from a plain
// rectangle or an axis-aligned clip rect. That way nothing can ever poke out
// of a rounded corner. The outline is generated here (fixed segment count) so
// every layer of a surface shares exactly the same vertices.

typedef std::vector<ImVec2> Poly;

// ImGui's own rounded rects clamp the radius to half the short side minus a
// pixel; use the same radius everywhere so all layers trace one shape.
static float glassRadius(float w, float h, float rad) {
    const float lim = std::min(w, h) * 0.5f - 1.0f;
    return std::max(0.0f, std::min(rad, lim));
}

// Arc segments per corner for a shape of corner radius r. Every ring of one
// shape (rim, glow, lens mesh) must use the SAME count -- derived from the
// shape's own radius, not each ring's smaller one -- or the rings have different
// vertex counts and cannot be stitched together.
static int arcSegments(float r) { return std::max(4, std::min(16, (int)std::ceil(r / 2.5f))); }

// Clockwise outline starting at the top-left corner's arc. `segs` > 0 forces the
// segment count (so concentric rings line up); 0 picks it from r.
static Poly roundRectPoly(float x0, float y0, float x1, float y1, float r, int segs = 0) {
    Poly p;
    if (x1 <= x0 || y1 <= y0) return p;
    r = std::max(0.0f, std::min(r, std::min(x1 - x0, y1 - y0) * 0.5f));
    if (segs <= 0) {
        if (r < 0.5f) {
            p = {ImVec2(x0, y0), ImVec2(x1, y0), ImVec2(x1, y1), ImVec2(x0, y1)};
            return p;
        }
        segs = arcSegments(r);
    }
    const float PI = 3.14159265f;
    const ImVec2 c[4] = {ImVec2(x0 + r, y0 + r), ImVec2(x1 - r, y0 + r), ImVec2(x1 - r, y1 - r), ImVec2(x0 + r, y1 - r)};
    const float a0[4] = {PI, 1.5f * PI, 0.0f, 0.5f * PI};
    p.reserve((size_t)(segs + 1) * 4);
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= segs; i++) {
            const float a = a0[k] + (0.5f * PI) * (float)i / (float)segs;
            p.push_back(ImVec2(c[k].x + std::cos(a) * r, c[k].y + std::sin(a) * r));
        }
    }
    return p;
}

// Sutherland-Hodgman against one axis-aligned half-plane. Clipping a convex
// polygon this way stays convex.
static Poly clipPoly(const Poly& in, bool axisX, bool keepGreater, float v) {
    Poly out;
    const size_t n = in.size();
    if (n < 3) return out;
    auto dist = [&](const ImVec2& q) { const float d = (axisX ? q.x : q.y) - v; return keepGreater ? d : -d; };
    for (size_t i = 0; i < n; i++) {
        const ImVec2 &a = in[i], &b = in[(i + 1) % n];
        const float da = dist(a), db = dist(b);
        if (da >= 0.0f) out.push_back(a);
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            out.push_back(ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
        }
    }
    return out;
}

// A triangle fan over a convex polygon, colour chosen per vertex. The colour
// function must be linear in position across the polygon for the result to be
// exact (every use below is).
template <class F>
static void fillFan(ImDrawList* d, const Poly& poly, F colourAt) {
    const int n = (int)poly.size();
    if (n < 3) return;
    ImVec2 centre(0, 0);
    for (const ImVec2& q : poly) { centre.x += q.x; centre.y += q.y; }
    centre.x /= (float)n; centre.y /= (float)n;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve(n * 3, n + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(centre, uv, colourAt(centre));
    for (int i = 0; i < n; i++) d->PrimWriteVtx(poly[(size_t)i], uv, colourAt(poly[(size_t)i]));
    for (int i = 0; i < n; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
    }
}

// A ring (between the outline and the outline inset by `thick`), colour chosen
// per vertex (the lambda also learns whether it is the inner edge) -- the glass
// rim and its soft inner glow. Both outlines have the same vertex count.
template <class F>
static void fillRing(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, float thick, F colourAt) {
    const int segs = arcSegments(rad);
    const Poly o = roundRectPoly(x0, y0, x1, y1, rad, segs);
    const Poly i = roundRectPoly(x0 + thick, y0 + thick, x1 - thick, y1 - thick, std::max(0.0f, rad - thick), segs);
    if (o.size() < 3 || o.size() != i.size()) return;
    const int n = (int)o.size();
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve(n * 6, n * 2);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    for (int k = 0; k < n; k++) {
        d->PrimWriteVtx(o[(size_t)k], uv, colourAt(o[(size_t)k], false));
        d->PrimWriteVtx(i[(size_t)k], uv, colourAt(i[(size_t)k], true));
    }
    for (int k = 0; k < n; k++) {
        const int k2 = (k + 1) % n;
        const ImDrawIdx a = (ImDrawIdx)(base + k * 2), b = (ImDrawIdx)(base + k * 2 + 1);
        const ImDrawIdx c = (ImDrawIdx)(base + k2 * 2), e = (ImDrawIdx)(base + k2 * 2 + 1);
        d->PrimWriteIdx(a); d->PrimWriteIdx(c); d->PrimWriteIdx(b);
        d->PrimWriteIdx(b); d->PrimWriteIdx(c); d->PrimWriteIdx(e);
    }
}

// A texture inside a glass shape, drawn as a mesh of concentric rounded rings
// whose texture coordinates are pulled towards the middle near the rim -- a
// cheap "thick glass" lens that needs no shader. With `refract` it is the real
// thing: `tex` is a snapshot of the frame, the band is wide and the bend strong,
// so what is behind the glass visibly warps. Without, it is the gentle warp of
// the soft backdrop. `a` is the layer's opacity (0..255). False = too small to
// bend (the caller draws it flat).
static bool glassLensMesh(ImDrawList* d, uint64_t tex, float x0, float y0, float x1, float y1, float rad, int a,
                          bool refract) {
    const float w = x1 - x0, h = y1 - y0;
    if (a <= 0) return true;
    glass::Lens lens;
    static const float kSoft[4] = {0.0f, 0.22f, 0.55f, 1.0f};
    const float* depth = refract ? glass::LENS_RING_DEPTH : kSoft;
    const int R = refract ? glass::LENS_RINGS - 1 : 3;
    if (refract) {
        lens = glass::lensFor(w, h, rad);
    } else {
        lens.edge = std::min(std::min(w, h) * 0.3f, std::min(16.0f, rad * 0.65f));
        lens.bend = 18.0f;
        lens.magnify = 1.0f;
    }
    if (lens.edge < 3.0f) return false;
    Poly ring[glass::LENS_RINGS];
    float inset[glass::LENS_RINGS];
    const int segs = arcSegments(rad);
    for (int i = 0; i <= R; i++) {
        inset[i] = lens.edge * depth[i];
        ring[i] = roundRectPoly(x0 + inset[i], y0 + inset[i], x1 - inset[i], y1 - inset[i], std::max(0.0f, rad - inset[i]), segs);
        if (ring[i].size() < 3 || ring[i].size() != ring[0].size()) return false;
    }
    const int n = (int)ring[0].size();
    const ImVec2 mid((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    const float sw = layout::SCREEN_W, sh = layout::SCREEN_H;
    auto uvAt = [&](const ImVec2& q, float t) { // t: 0 at the very rim .. 1 inside the band
        float dx = mid.x - q.x, dy = mid.y - q.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        const float bend = refract ? glass::lensShift(lens, t) : lens.bend * (1.0f - t) * (1.0f - t);
        float lx = q.x, ly = q.y;
        if (len > 1e-3f) { lx += dx / len * bend; ly += dy / len * bend; } // lookup pulled inwards
        lx = mid.x + (lx - mid.x) * lens.magnify; // faint interior magnification
        ly = mid.y + (ly - mid.y) * lens.magnify;
        float u = lx / sw, v = ly / sh;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return ImVec2(u, v);
    };
    // Bind the texture for exactly these triangles (they are written by hand, not
    // through AddImage, so nothing else would).
    d->PushTexture(ImTextureRef((ImTextureID)tex));
    const ImU32 col = IM_COL32(255, 255, 255, a);
    const int vtx = (R + 1) * n + 1, idx = R * n * 6 + n * 3;
    d->PrimReserve(idx, vtx);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    for (int i = 0; i <= R; i++) {
        const float t = depth[i];
        for (int k = 0; k < n; k++) d->PrimWriteVtx(ring[i][(size_t)k], uvAt(ring[i][(size_t)k], t), col);
    }
    d->PrimWriteVtx(mid, uvAt(mid, 1.0f), col);
    for (int i = 0; i < R; i++) {
        for (int k = 0; k < n; k++) {
            const int k2 = (k + 1) % n;
            const ImDrawIdx a0 = (ImDrawIdx)(base + i * n + k), a1 = (ImDrawIdx)(base + i * n + k2);
            const ImDrawIdx b0 = (ImDrawIdx)(base + (i + 1) * n + k), b1 = (ImDrawIdx)(base + (i + 1) * n + k2);
            d->PrimWriteIdx(a0); d->PrimWriteIdx(a1); d->PrimWriteIdx(b0);
            d->PrimWriteIdx(b0); d->PrimWriteIdx(a1); d->PrimWriteIdx(b1);
        }
    }
    const ImDrawIdx c = (ImDrawIdx)(base + (R + 1) * n);
    for (int k = 0; k < n; k++) {
        d->PrimWriteIdx(c);
        d->PrimWriteIdx((ImDrawIdx)(base + R * n + k));
        d->PrimWriteIdx((ImDrawIdx)(base + R * n + (k + 1) % n));
    }
    d->PopTexture();
    return true;
}

// One texture layer inside a glass shape: lens-warped, or plain if it is too small.
static void glassBackdropLayer(ImDrawList* d, uint64_t tex, float x0, float y0, float x1, float y1, float rad, int a,
                               bool refract = false) {
    if (a <= 0 || !tex) return;
    if (glassLensMesh(d, tex, x0, y0, x1, y1, rad, a, refract)) return;
    const glass::Uv uv = glass::uvForRect(x0, y0, x1 - x0, y1 - y0, layout::SCREEN_W, layout::SCREEN_H);
    d->AddImageRounded(ImTextureRef((ImTextureID)tex), ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1),
                       IM_COL32(255, 255, 255, a), rad);
}

// The blurred backdrop at opacity `a` (0..255) with the running cross-fade between
// the previous and newest blur, so its slow drift is smooth at frame rate.
// Two layers that add up to exactly `a` of the blended picture over what is behind.
static void glassBackdrop(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, int a) {
    glass::BackdropManager& bd = glass::backdrop();
    const uint64_t to = bd.texture(), from = bd.textureFrom();
    if (!to) return;
    const float f = g_glassBlend, al = (float)a / 255.0f;
    if (!from || from == to || f >= 0.999f) {
        glassBackdropLayer(d, to, x0, y0, x1, y1, rad, a);
        return;
    }
    const float aFrom = al * (1.0f - f) / (1.0f - al * f), aTo = al * f; // see BackdropManager::blend
    glassBackdropLayer(d, from, x0, y0, x1, y1, rad, (int)(aFrom * 255.0f + 0.5f));
    glassBackdropLayer(d, to, x0, y0, x1, y1, rad, (int)(aTo * 255.0f + 0.5f));
}

// A flat-coloured band of a rounded rectangle: only the part of the shape
// between bandY0 and bandY1 (anti-aliased). Used for accent strips and the
// pressed-in edge; unlike a plain rect it follows the rounded corners.
static void glassBand(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, float bandY0, float bandY1,
                      ImU32 colour) {
    rad = glassRadius(x1 - x0, y1 - y0, rad);
    Poly p = roundRectPoly(x0, y0, x1, y1, rad);
    p = clipPoly(p, false, true, bandY0);
    p = clipPoly(p, false, false, bandY1);
    if (p.size() >= 3) d->AddConvexPolyFilled(p.data(), (int)p.size(), colour);
}

static void glassSurface(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, GK k, bool blur,
                         float alpha = 1.0f) {
    const float w = x1 - x0, h = y1 - y0;
    if (w <= 1.0f || h <= 1.0f) return;
    rad = glassRadius(w, h, rad);
    const ImVec2 p0(x0, y0), p1(x1, y1);
    const bool focus = (k == GK::RowFocus || k == GK::ButtonHot);
    const bool pressed = (k == GK::ButtonPressed);
    if (k == GK::ButtonDisabled) alpha *= 0.45f; // reduced opacity, lighting and contrast
    auto A = [&](int a) { int v = (int)((float)a * alpha); return v < 0 ? 0 : (v > 255 ? 255 : v); };
    const uint64_t tex = blur ? glass::backdrop().texture() : 0;

    // Real refraction: make sure a snapshot of everything drawn so far exists
    // before this shape's own shadow or body goes down (one per frame is the
    // minimum; more only where glass sits on top of glass).
    const bool refract = refractionOn();
    if (refract && g_snapPlanner.onSurface((uint64_t)ImGui::GetFrameCount(), d, x0, y0, x1, y1)) {
        d->AddCallback(g_snapCb, g_snapData);
        d->AddCallback(ImDrawCallback_ResetRenderState, nullptr); // the copy may disturb the draw state
    }

    // Glass is mostly clear: a faint tint and sheen, and a backdrop layer that
    // only partly covers what is behind it, so the scene stays visible.
    int tintA = 0x34, sheenA = 0x22, blurA = 0x58;
    switch (k) {
        case GK::Panel: tintA = 0x34; sheenA = 0x22; break;
        case GK::Row: tintA = 0x30; sheenA = 0x18; break;
        case GK::RowFocus: tintA = 0x78; sheenA = 0x38; break;
        case GK::RowFocusDim: tintA = 0x44; sheenA = 0x1C; break;
        case GK::Overlay: tintA = 0x88; sheenA = 0x16; break;
        case GK::Button: tintA = 0x38; sheenA = 0x28; break;
        case GK::ButtonHot: tintA = 0x70; sheenA = 0x40; break;
        case GK::ButtonPressed: tintA = 0x60; sheenA = 0; break;
        case GK::ButtonDisabled: tintA = 0x38; sheenA = 0x10; break;
    }
    // Nothing soft to sit on (over video, or blur unavailable): give the
    // surface a little more body of its own so text stays readable.
    if (!tex && k != GK::Overlay) tintA = std::min(0xC0, tintA + 0x48);

    // Depth: a soft shadow and a focus glow, both as rings that lie entirely
    // OUTSIDE the shape -- a filled rect under a see-through body would show
    // through it.
    if (k != GK::Overlay) {
        const int layers = 3;
        for (int i = 0; i < layers; i++) {
            const float th = 2.0f, e = th * 0.5f + th * (float)i;
            const int a = (int)((pressed ? 0x16 : 0x24) * (1.0f - (float)i / (float)layers));
            d->AddRect(ImVec2(x0 - e, y0 - e), ImVec2(x1 + e, y1 + e + (pressed ? 1.0f : 3.0f)), IM_COL32(0, 0, 0, A(a)),
                       rad + e, 0, th);
        }
    }
    if (focus) {
        const ImU32 halo = (accentMain() & 0x00FFFFFF);
        for (int i = 0; i < 2; i++) {
            const float th = 2.0f, e = th * 0.5f + th * (float)i + 1.0f;
            d->AddRect(ImVec2(x0 - e, y0 - e), ImVec2(x1 + e, y1 + e), halo | ((ImU32)A(i == 0 ? 0x44 : 0x22) << 24), rad + e, 0, th);
        }
    }

    // Body. With refraction: the snapshot of what is behind, bent at the rim, with
    // only a faint frost of the soft backdrop on top. Without: the shared blurred
    // backdrop at this panel's own screen position, only partly opaque, bent a
    // little at the edges...
    if (refract) {
        glassBackdropLayer(d, g_snapTex, x0, y0, x1, y1, rad, A(255), true);
        if (tex) glassBackdrop(d, x0, y0, x1, y1, rad, A(0x30));
    } else if (tex) {
        glassBackdrop(d, x0, y0, x1, y1, rad, A(blurA));
    }
    // ...under a light tint.
    ImU32 tint;
    if (pressed) tint = IM_COL32(0, 0, 0, A(tintA));
    else if (focus) tint = (accentDeep() & 0x00FFFFFF) | ((ImU32)A(tintA) << 24);
    else if (g_light) tint = IM_COL32(0xFF, 0xFF, 0xFF, A(tintA));
    else tint = IM_COL32(0x28, 0x34, 0x48, A(tintA));
    d->AddRectFilled(p0, p1, tint, rad);

    // Light from above: a diagonal sheen from the top-left.
    if (sheenA > 0) gradientRoundRect(x0, y0, x1, y1, rad, IM_COL32(255, 255, 255, A(sheenA)), IM_COL32(255, 255, 255, 0), d);

    // Rim light, like the reference glass: a thin bright edge that is strongest
    // at the top-left and bottom-right corners and faint along the middle.
    {
        const int base = pressed ? 0x18 : (g_light ? 0x50 : 0x2C);
        const int peak = pressed ? 0x50 : (g_light ? 0xE0 : 0xC8);
        fillRing(d, x0 + 0.5f, y0 + 0.5f, x1 - 0.5f, y1 - 0.5f, std::max(0.0f, rad - 0.5f), focus ? 2.0f : 1.5f, [&](const ImVec2& q, bool) {
            const float u = ((q.x - x0) / w + (q.y - y0) / h) * 0.5f;
            float s = std::fabs(2.0f * u - 1.0f);
            s = s * s;
            return IM_COL32(255, 255, 255, A(base + (int)((float)(peak - base) * s)));
        });
        // Soft inner glow just inside the rim, fading to nothing: the thick-glass
        // edge, brightest at the same two corners.
        if (!pressed && std::min(w, h) > 12.0f) {
            const float gw = std::min(7.0f, std::min(w, h) * 0.22f);
            fillRing(d, x0 + 1.5f, y0 + 1.5f, x1 - 1.5f, y1 - 1.5f, std::max(0.0f, rad - 1.5f), gw, [&](const ImVec2& q, bool inner) {
                if (inner) return IM_COL32(255, 255, 255, 0);
                const float u = ((q.x - x0) / w + (q.y - y0) / h) * 0.5f;
                float s = std::fabs(2.0f * u - 1.0f);
                s = s * s;
                return IM_COL32(255, 255, 255, A((g_light ? 0x10 : 0x0C) + (int)((g_light ? 0x50 : 0x40) * s)));
            });
        }
    }
    if (pressed) { // pressed in: a dark inner top edge
        glassBand(d, x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y1 - 1.0f, rad - 1.0f, y0 + 1.0f, y0 + 4.0f, IM_COL32(0, 0, 0, A(0x60)));
    }

    // Focus: a highlight that drifts across the surface. It is cut from the
    // surface's own rounded outline (inset), so it follows the corners.
    if (focus && w > 8.0f) {
        const double period = 3.2;
        const float u = (float)(std::fmod(ImGui::GetTime(), period) / period); // 0..1
        const float bw = std::max(12.0f, w * 0.20f), cx = x0 - bw + (w + 2.0f * bw) * u;
        const float in = 2.0f;
        const Poly shape = roundRectPoly(x0 + in, y0 + in, x1 - in, y1 - in, std::max(0.0f, rad - in));
        const int hi = A(0x2A);
        Poly left = clipPoly(clipPoly(shape, true, true, cx - bw), true, false, cx);
        Poly right = clipPoly(clipPoly(shape, true, true, cx), true, false, cx + bw);
        fillFan(d, left, [&](const ImVec2& q) { return IM_COL32(255, 255, 255, (int)((float)hi * std::min(1.0f, std::max(0.0f, (q.x - (cx - bw)) / bw)))); });
        fillFan(d, right, [&](const ImVec2& q) { return IM_COL32(255, 255, 255, (int)((float)hi * std::min(1.0f, std::max(0.0f, 1.0f - (q.x - cx) / bw)))); });
    }

    // Edge.
    if (focus) {
        d->AddRect(p0, p1, (accentMain() & 0x00FFFFFF) | ((ImU32)A(0xFF) << 24), rad, 0, 2.0f);
    } else if (k == GK::RowFocusDim) {
        d->AddRect(p0, p1, (accentDeep() & 0x00FFFFFF) | ((ImU32)A(0xFF) << 24), rad, 0, 2.0f);
    } else {
        d->AddRect(p0, p1, g_light ? IM_COL32(0, 0, 0, A(0x24)) : IM_COL32(255, 255, 255, A(0x1C)), rad, 0, 1.0f);
    }
}

static void glassCircle(ImDrawList* d, float cx, float cy, float r, GK k, bool blur) {
    glassSurface(d, cx - r, cy - r, cx + r, cy + r, r, k, blur);
}

// A panel-ish rounded rectangle: the flat colour it always was, or glass.
static void surfaceRect(const Rect& r, ImU32 flat, float rounding, GK kind, bool blur = true) {
    if (!g_glass) {
        rect(r, flat, rounding);
        return;
    }
    glassSurface(dl(), r.x, r.y, r.right(), r.bottom(), gr(rounding), kind, blur);
}

// The screen's base: theme background, then (glass) the soft light field the
// panels frost, then the ambient effect.
// --- Jellyfin backdrop (see setScreenBackdrop in app_ui.h) ---
static Texture g_bdPrev = 0, g_bdCur = 0;
static float g_bdPrevAspect = 0.0f, g_bdCurAspect = 0.0f, g_bdCurAlpha = 0.0f;

void setScreenBackdrop(Texture prev, float prevAspect, Texture cur, float curAspect, float curAlpha) {
    g_bdPrev = prev;
    g_bdPrevAspect = prevAspect;
    g_bdCur = cur;
    g_bdCurAspect = curAspect;
    g_bdCurAlpha = curAlpha < 0.0f ? 0.0f : (curAlpha > 1.0f ? 1.0f : curAlpha);
}

// How much of the normal background the backdrop covers (0 = none, 1 = all of it).
static float backdropCoverage() {
    if (g_bdPrev) return 1.0f;
    return g_bdCur ? g_bdCurAlpha : 0.0f;
}

// Draws `tex` filling the whole screen ("cover": the overflow is cropped, never stretched).
static void drawCoverImage(Texture tex, float aspect, int alpha) {
    if (!tex || alpha <= 0) return;
    const float W = layout::SCREEN_W, H = layout::SCREEN_H, screen = W / H;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (aspect > screen) {            // wider than the screen: crop the sides
        const float vis = screen / aspect;
        uv0.x = (1.0f - vis) * 0.5f;
        uv1.x = 1.0f - uv0.x;
    } else if (aspect > 0.0f && aspect < screen) { // taller: crop top and bottom
        const float vis = aspect / screen;
        uv0.y = (1.0f - vis) * 0.5f;
        uv1.y = 1.0f - uv0.y;
    }
    dl()->AddImage(ImTextureRef((ImTextureID)tex), ImVec2(0, 0), ImVec2(W, H), uv0, uv1, IM_COL32(255, 255, 255, alpha));
}

static void drawBackdropLayers() {
    const float cov = backdropCoverage();
    if (cov <= 0.0f) return;
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    drawCoverImage(g_bdPrev, g_bdPrevAspect, 255);
    drawCoverImage(g_bdCur, g_bdCurAspect, (int)(g_bdCurAlpha * 255.0f + 0.5f));
    // Scrim in the theme's background colour (dark theme: darkens, light theme: veils), a little
    // heavier at the bottom where the button hints sit, so text stays readable on bright pictures.
    const ImU32 base = col::BG & 0x00FFFFFFu;
    dl()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), base | ((ImU32)(0.52f * cov * 255.0f) << 24));
    dl()->AddRectFilledMultiColor(ImVec2(0, H * 0.62f), ImVec2(W, H), base, base,
                                  base | ((ImU32)(0.38f * cov * 255.0f) << 24), base | ((ImU32)(0.38f * cov * 255.0f) << 24));
}

static void drawScreenBackdrop() {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    rect({0, 0, W, H}, col::BG);
    if (backdropCoverage() >= 0.999f) { // fully covered: skip the glass field and the animated effect
        drawBackdropLayers();
        return;
    }
    if (g_glass) {
        const glass::BackdropManager& bd = glass::backdrop();
        const uint64_t tex = bd.texture(), from = bd.textureFrom();
        if (tex) {
            if (from && from != tex && g_glassBlend < 0.999f) { // opaque previous blur, newest faded over it
                dl()->AddImage(ImTextureRef((ImTextureID)from), ImVec2(0, 0), ImVec2(W, H), ImVec2(0, 0), ImVec2(1, 1), col::WHITE);
                dl()->AddImage(ImTextureRef((ImTextureID)tex), ImVec2(0, 0), ImVec2(W, H), ImVec2(0, 0), ImVec2(1, 1),
                               IM_COL32(255, 255, 255, (int)(g_glassBlend * 255.0f + 0.5f)));
            } else {
                dl()->AddImage(ImTextureRef((ImTextureID)tex), ImVec2(0, 0), ImVec2(W, H), ImVec2(0, 0), ImVec2(1, 1), col::WHITE);
            }
        } else {
            // Flat glass (no blur texture): a plain accent-tinted gradient.
            const ImU32 top = (accentMain() & 0x00FFFFFF) | (0x28u << 24), clear = (accentMain() & 0x00FFFFFF);
            dl()->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.6f), top, top, clear, clear);
        }
    }
    drawBackgroundEffect();
    drawBackdropLayers(); // fading in or out over the normal background (no-op without a backdrop)
}

// --- icons (simple vector shapes, drawn in a size x size box) ---

static void drawIcon(Icon icon, float cx, float cy, float s, ImU32 c) {
    ImDrawList* d = dl();
    const float h = s * 0.5f;
    const float t = std::max(2.0f, s * 0.08f); // stroke
    switch (icon) {
        case Icon::Home: {
            d->AddTriangleFilled(ImVec2(cx, cy - h), ImVec2(cx - h, cy - h * 0.05f), ImVec2(cx + h, cy - h * 0.05f), c);
            d->AddRectFilled(ImVec2(cx - h * 0.68f, cy - h * 0.1f), ImVec2(cx + h * 0.68f, cy + h * 0.85f), c, 2.0f);
            d->AddRectFilled(ImVec2(cx - h * 0.18f, cy + h * 0.3f), ImVec2(cx + h * 0.18f, cy + h * 0.85f), col::SIDEBAR);
            break;
        }
        case Icon::Search: {
            d->AddCircle(ImVec2(cx - h * 0.15f, cy - h * 0.15f), h * 0.55f, c, 24, t);
            d->AddLine(ImVec2(cx + h * 0.25f, cy + h * 0.25f), ImVec2(cx + h * 0.85f, cy + h * 0.85f), c, t * 1.4f);
            break;
        }
        case Icon::LiveTv:
        case Icon::Channel: {
            d->AddRect(ImVec2(cx - h * 0.9f, cy - h * 0.6f), ImVec2(cx + h * 0.9f, cy + h * 0.6f), c, 4.0f, 0, t);
            d->AddLine(ImVec2(cx - h * 0.35f, cy + h * 0.85f), ImVec2(cx + h * 0.35f, cy + h * 0.85f), c, t);
            d->AddCircleFilled(ImVec2(cx, cy), h * 0.22f, col::LIVE_RED, 16);
            break;
        }
        case Icon::Library:
        case Icon::Collection: {
            for (int i = 0; i < 3; i++) {
                float x = cx - h * 0.8f + i * h * 0.58f;
                d->AddRectFilled(ImVec2(x, cy - h * (0.8f - i * 0.1f)), ImVec2(x + h * 0.42f, cy + h * 0.8f), c, 2.0f);
            }
            break;
        }
        case Icon::Folder: {
            d->AddRectFilled(ImVec2(cx - h * 0.9f, cy - h * 0.62f), ImVec2(cx - h * 0.1f, cy - h * 0.35f), c, 2.0f);
            d->AddRectFilled(ImVec2(cx - h * 0.9f, cy - h * 0.42f), ImVec2(cx + h * 0.9f, cy + h * 0.7f), c, 3.0f);
            break;
        }
        case Icon::Movie:
        case Icon::Video: {
            d->AddRectFilled(ImVec2(cx - h * 0.9f, cy - h * 0.62f), ImVec2(cx + h * 0.9f, cy + h * 0.62f), c, 3.0f);
            for (int i = 0; i < 4; i++) {
                float x = cx - h * 0.72f + i * h * 0.46f;
                d->AddRectFilled(ImVec2(x, cy - h * 0.52f), ImVec2(x + h * 0.2f, cy - h * 0.34f), col::BG);
                d->AddRectFilled(ImVec2(x, cy + h * 0.34f), ImVec2(x + h * 0.2f, cy + h * 0.52f), col::BG);
            }
            d->AddTriangleFilled(ImVec2(cx - h * 0.2f, cy - h * 0.22f), ImVec2(cx - h * 0.2f, cy + h * 0.22f),
                                 ImVec2(cx + h * 0.25f, cy), col::BG);
            break;
        }
        case Icon::Series:
        case Icon::Episode: {
            d->AddRectFilled(ImVec2(cx - h * 0.9f, cy - h * 0.6f), ImVec2(cx + h * 0.9f, cy + h * 0.5f), c, 4.0f);
            d->AddRectFilled(ImVec2(cx - h * 0.7f, cy - h * 0.42f), ImVec2(cx + h * 0.7f, cy + h * 0.32f), col::BG, 2.0f);
            d->AddLine(ImVec2(cx - h * 0.4f, cy + h * 0.8f), ImVec2(cx + h * 0.4f, cy + h * 0.8f), c, t);
            break;
        }
        case Icon::Music: {
            // Two beamed eighth notes.
            const float r = h * 0.26f;
            ImVec2 n1(cx - h * 0.45f, cy + h * 0.55f), n2(cx + h * 0.5f, cy + h * 0.4f);
            d->AddCircleFilled(n1, r, c, 16);
            d->AddCircleFilled(n2, r, c, 16);
            ImVec2 s1(n1.x + r * 0.85f, n1.y), s2(n2.x + r * 0.85f, n2.y);
            ImVec2 t1(s1.x, cy - h * 0.7f), t2(s2.x, cy - h * 0.85f);
            d->AddLine(s1, t1, c, t);
            d->AddLine(s2, t2, c, t);
            d->AddQuadFilled(ImVec2(t1.x - t * 0.5f, t1.y), ImVec2(t2.x + t * 0.5f, t2.y),
                             ImVec2(t2.x + t * 0.5f, t2.y + h * 0.28f), ImVec2(t1.x - t * 0.5f, t1.y + h * 0.28f), c);
            break;
        }
        case Icon::Album: {
            d->AddCircleFilled(ImVec2(cx, cy), h * 0.85f, c, 32);
            d->AddCircle(ImVec2(cx, cy), h * 0.55f, col::BG, 32, 1.5f);
            d->AddCircleFilled(ImVec2(cx, cy), h * 0.18f, col::BG, 16);
            break;
        }
        case Icon::Settings: {
            // Gear: ring with eight teeth.
            for (int i = 0; i < 8; i++) {
                float a = i * 3.14159265f / 4.0f;
                ImVec2 dir(std::cos(a), std::sin(a));
                d->AddLine(ImVec2(cx + dir.x * h * 0.45f, cy + dir.y * h * 0.45f),
                           ImVec2(cx + dir.x * h * 0.9f, cy + dir.y * h * 0.9f), c, t * 1.8f);
            }
            d->AddCircleFilled(ImVec2(cx, cy), h * 0.62f, c, 24);
            d->AddCircleFilled(ImVec2(cx, cy), h * 0.26f, col::BG, 16);
            break;
        }
        case Icon::User: {
            d->AddCircleFilled(ImVec2(cx, cy - h * 0.35f), h * 0.34f, c, 20);
            d->PathArcTo(ImVec2(cx, cy + h * 0.85f), h * 0.75f, 3.14159265f, 2.0f * 3.14159265f, 20);
            d->PathFillConvex(c);
            break;
        }
        case Icon::Info: {
            d->AddCircle(ImVec2(cx, cy), h * 0.85f, c, 28, t);
            d->AddCircleFilled(ImVec2(cx, cy - h * 0.4f), t * 0.9f, c, 10);
            d->AddLine(ImVec2(cx, cy - h * 0.1f), ImVec2(cx, cy + h * 0.5f), c, t * 1.2f);
            break;
        }
        case Icon::Palette: {
            d->AddCircleFilled(ImVec2(cx, cy), h * 0.85f, c, 28);
            const ImU32 dots[4] = {rgb(0xE84393), rgb(0xFF9F43), rgb(0x2ECC71), rgb(0x00A4DC)};
            for (int i = 0; i < 4; i++) {
                float a = -2.4f + i * 0.9f;
                d->AddCircleFilled(ImVec2(cx + std::cos(a) * h * 0.48f, cy + std::sin(a) * h * 0.48f), h * 0.17f,
                                   dots[i], 12);
            }
            d->AddCircleFilled(ImVec2(cx + h * 0.35f, cy + h * 0.35f), h * 0.2f, col::BG, 12);
            break;
        }
        case Icon::Snow: {
            for (int i = 0; i < 3; i++) {
                float a = i * 3.14159265f / 3.0f;
                ImVec2 dir(std::cos(a) * h * 0.85f, std::sin(a) * h * 0.85f);
                d->AddLine(ImVec2(cx - dir.x, cy - dir.y), ImVec2(cx + dir.x, cy + dir.y), c, t);
                for (int e = -1; e <= 1; e += 2) {
                    ImVec2 tip(cx + dir.x * e * 0.6f, cy + dir.y * e * 0.6f);
                    ImVec2 side(-dir.y * 0.25f, dir.x * 0.25f);
                    d->AddLine(tip, ImVec2(tip.x + dir.x * e * 0.35f + side.x, tip.y + dir.y * e * 0.35f + side.y), c, t * 0.8f);
                    d->AddLine(tip, ImVec2(tip.x + dir.x * e * 0.35f - side.x, tip.y + dir.y * e * 0.35f - side.y), c, t * 0.8f);
                }
            }
            break;
        }
        case Icon::Clock: {
            d->AddCircle(ImVec2(cx, cy), h * 0.85f, c, 28, t);
            d->AddLine(ImVec2(cx, cy), ImVec2(cx, cy - h * 0.55f), c, t);
            d->AddLine(ImVec2(cx, cy), ImVec2(cx + h * 0.4f, cy + h * 0.1f), c, t);
            break;
        }
        case Icon::Sparkle: {
            d->AddTriangleFilled(ImVec2(cx, cy - h * 0.9f), ImVec2(cx - h * 0.2f, cy), ImVec2(cx + h * 0.2f, cy), c);
            d->AddTriangleFilled(ImVec2(cx, cy + h * 0.9f), ImVec2(cx - h * 0.2f, cy), ImVec2(cx + h * 0.2f, cy), c);
            d->AddTriangleFilled(ImVec2(cx - h * 0.9f, cy), ImVec2(cx, cy - h * 0.2f), ImVec2(cx, cy + h * 0.2f), c);
            d->AddTriangleFilled(ImVec2(cx + h * 0.9f, cy), ImVec2(cx, cy - h * 0.2f), ImVec2(cx, cy + h * 0.2f), c);
            break;
        }
        case Icon::Heart: {
            d->AddCircleFilled(ImVec2(cx - h * 0.36f, cy - h * 0.2f), h * 0.42f, c, 16);
            d->AddCircleFilled(ImVec2(cx + h * 0.36f, cy - h * 0.2f), h * 0.42f, c, 16);
            d->AddTriangleFilled(ImVec2(cx - h * 0.76f, cy - h * 0.05f), ImVec2(cx + h * 0.76f, cy - h * 0.05f),
                                 ImVec2(cx, cy + h * 0.8f), c);
            break;
        }
        case Icon::Resume: {
            d->AddCircle(ImVec2(cx, cy), h * 0.85f, c, 28, t);
            d->AddTriangleFilled(ImVec2(cx - h * 0.25f, cy - h * 0.4f), ImVec2(cx - h * 0.25f, cy + h * 0.4f),
                                 ImVec2(cx + h * 0.45f, cy), c);
            break;
        }
        case Icon::Gamepad: {
            d->AddRectFilled(ImVec2(cx - h * 0.95f, cy - h * 0.55f), ImVec2(cx + h * 0.95f, cy + h * 0.55f), c, h * 0.3f);
            d->AddRectFilled(ImVec2(cx - h * 0.5f, cy - h * 0.38f), ImVec2(cx + h * 0.5f, cy + h * 0.38f), col::BG, 2.0f);
            break;
        }
        case Icon::Remote: {
            d->AddRectFilled(ImVec2(cx - h * 0.28f, cy - h * 0.9f), ImVec2(cx + h * 0.28f, cy + h * 0.9f), c, h * 0.2f);
            d->AddCircleFilled(ImVec2(cx, cy - h * 0.45f), h * 0.13f, col::BG, 10);
            d->AddCircleFilled(ImVec2(cx, cy + h * 0.2f), h * 0.1f, col::BG, 10);
            break;
        }
        case Icon::Help: {
            d->AddCircle(ImVec2(cx, cy), h * 0.85f, c, 28, t);
            d->PathArcTo(ImVec2(cx, cy - h * 0.22f), h * 0.30f, -3.3f, 0.35f, 12);
            d->PathLineTo(ImVec2(cx + h * 0.02f, cy + h * 0.18f));
            d->PathStroke(c, 0, t * 1.1f);
            d->AddCircleFilled(ImVec2(cx, cy + h * 0.5f), t * 0.9f, c, 10);
            break;
        }
        case Icon::None:
            break;
    }
}

// --- shared chrome ---

static void drawHints(const std::vector<Hint>& hints, float x, float y) {
    const float r = 14.0f;
    for (const Hint& hint : hints) {
        float bw = std::max(2 * r, textSize(TEXT_S, hint.button).x + 14.0f);
        if (g_glass) glassSurface(dl(), x, y, x + bw, y + 2 * r, r, GK::Button, true);
        else dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + 2 * r), col::PANEL_HI, r);
        ImVec2 bs = textSize(TEXT_S, hint.button);
        text(TEXT_S, x + (bw - bs.x) * 0.5f, y + r - bs.y * 0.5f, col::TEXT, hint.button);
        x += bw + 8.0f;
        text(TEXT_S, x, y + r - bs.y * 0.5f, col::MUTED, hint.label);
        x += textSize(TEXT_S, hint.label).x + 26.0f;
    }
}

static void drawLogo(float x, float y) {
    // A rounded tile with a play triangle in Jellyfin's two colours.
    const float s = 40.0f;
    gradientRoundRect(x, y, x + s, y + s, 10.0f, accentSecond(), accentMain());
    dl()->AddTriangleFilled(ImVec2(x + 15, y + 11), ImVec2(x + 15, y + 29), ImVec2(x + 30, y + 20), col::WHITE);
    text(34.0f, x + s + 14.0f, y + 1.0f, col::TEXT, "Ufin");
}

static void drawProgress(float x, float y, float w, float h, double position, double duration, bool live) {
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col::TRACK, h * 0.5f);
    if (duration > 0.0) {
        float f = (float)std::min(1.0, std::max(0.0, position / duration));
        if (f > 0.0f) dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w * f, y + h), accentMain(), h * 0.5f);
        dl()->AddCircleFilled(ImVec2(x + w * f, y + h * 0.5f), h * 1.1f, col::WHITE, 20);
    } else if (live) {
        dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col::LIVE_RED, h * 0.5f);
    } else {
        // Unknown length: a marker that sweeps so the screen visibly moves.
        float phase = (float)std::fmod(position, 8.0) / 8.0f;
        float mw = w * 0.08f;
        dl()->AddRectFilled(ImVec2(x + (w - mw) * phase, y), ImVec2(x + (w - mw) * phase + mw, y + h), accentMain(),
                            h * 0.5f);
    }
}

// Watched tick / unwatched count (top-right of the artwork) and the
// resume progress bar (along its bottom).
static void drawArtBadges(const ListEntry& e, const layout::Fit& f) {
    ImDrawList* d = dl();
    if (e.progress > 0.0f && e.progress < 1.0f) {
        float y = f.y + f.h - 5;
        d->AddRectFilled(ImVec2(f.x + 3, y), ImVec2(f.x + f.w - 3, y + 4), IM_COL32(0, 0, 0, 0xC0), 2.0f);
        d->AddRectFilled(ImVec2(f.x + 3, y), ImVec2(f.x + 3 + (f.w - 6) * e.progress, y + 4), accentMain(), 2.0f);
    }
    const ImVec2 corner(f.x + f.w - 2, f.y + 2);
    if (e.unplayed > 0) {
        std::string n = e.unplayed > 99 ? "99+" : std::to_string(e.unplayed);
        ImVec2 ts = textSize(15.0f, n);
        float w = std::max(20.0f, ts.x + 10), h = 20;
        d->AddRectFilled(ImVec2(corner.x - w + 4, corner.y - 6), ImVec2(corner.x + 4, corner.y - 6 + h), accentMain(), h * 0.5f);
        d->AddText(font(), 15.0f, ImVec2(corner.x - w + 4 + (w - ts.x) * 0.5f, corner.y - 6 + (h - ts.y) * 0.5f),
                   col::WHITE, n.c_str());
    } else if (e.watched) {
        ImVec2 c(corner.x, corner.y + 4);
        d->AddCircleFilled(c, 10.0f, accentMain(), 16);
        d->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x - 1.5f, c.y + 4), col::WHITE, 2.4f);
        d->AddLine(ImVec2(c.x - 1.5f, c.y + 4), ImVec2(c.x + 5, c.y - 4), col::WHITE, 2.4f);
    }
}

// --- Usage celebration (hidden: tap the sidebar's "Time used" counter five times) ---
// Frame-counted rather than clocked: the UI is vsync-paced at 60 Hz, and counting frames keeps the
// animation deterministic (and testable). The confetti is drawn in two waves around the glass
// bubble, so the bubble refracts the back wave and the front wave falls past it.
static bool g_celebActive = false;
static int g_celebFrame = 0;
static std::string g_celebBig, g_celebSmall;
static const int CELEB_MIN_FRAMES = 36;      // 0.6 s before a tap can dismiss it
static const float CELEB_MAX_SECONDS = 16.0f; // auto-dismiss

bool hitUsageCounter(float x, float y) { return layout::usageCounter().contains(x, y); }

void showUsageCelebration(const std::string& big, const std::string& small) {
    g_celebActive = true;
    g_celebFrame = 0;
    g_celebBig = big;
    g_celebSmall = small;
}

bool usageCelebrationActive() { return g_celebActive; }

void dismissUsageCelebration() {
    if (g_celebActive && g_celebFrame >= CELEB_MIN_FRAMES) g_celebActive = false;
}

void advanceUsageCelebration(int frames) {
    if (g_celebActive && frames > 0) g_celebFrame += frames;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 150 pieces, each fully determined by its index: staggered launch over ~2.4 s, then they sway and
// tumble down past the bottom edge. `frontLayer` picks every third piece for the layer in front.
static void drawConfetti(ImDrawList* d, float t, bool frontLayer) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    const ImU32 palette[8] = {accentMain(),
                              accentSecond(),
                              accentDeep(),
                              IM_COL32(255, 205, 60, 255),
                              IM_COL32(255, 110, 170, 255),
                              IM_COL32(90, 220, 140, 255),
                              IM_COL32(255, 150, 60, 255),
                              IM_COL32(245, 245, 250, 255)};
    for (int i = 0; i < 150; i++) {
        if (((i % 3) == 0) != frontLayer) continue;
        uint32_t h = (uint32_t)(i + 1) * 2654435761u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        const float r1 = (h & 0xFF) / 255.0f, r2 = ((h >> 8) & 0xFF) / 255.0f, r3 = ((h >> 16) & 0xFF) / 255.0f,
                    r4 = ((h >> 24) & 0xFF) / 255.0f;
        const float u = t - r1 * 2.4f; // seconds since this piece was launched
        if (u < 0.0f) continue;
        const float y = -30.0f + (150.0f + r3 * 200.0f) * u + 20.0f * u * u;
        if (y > H + 30.0f) continue;
        const float x = r2 * W + (float)std::sin(u * (1.5f + r4 * 2.5f) + r1 * 6.28f) * (18.0f + r3 * 40.0f) +
                        (r4 - 0.5f) * 60.0f * u;
        const float size = 7.0f + r4 * 8.0f;
        const ImU32 c = bgTint(palette[h % 8], 0.92f);
        if (i % 5 == 0) {
            d->AddCircleFilled(ImVec2(x, y), size * 0.45f, c, 10);
            continue;
        }
        // A tumbling strip: it spins in the plane and "flips" (its width follows |cos|).
        const float ang = u * (2.0f + r1 * 6.0f) * (r2 > 0.5f ? 1.0f : -1.0f) + r3 * 6.28f;
        const float flip = 0.25f + 0.75f * (float)std::fabs(std::cos(u * (2.0f + r4 * 4.0f)));
        const float hw = size * flip, hh = size * 0.5f;
        const float ca = (float)std::cos(ang), sa = (float)std::sin(ang);
        auto corner = [&](float cx, float cy) { return ImVec2(x + cx * ca - cy * sa, y + cx * sa + cy * ca); };
        d->AddQuadFilled(corner(-hw, -hh), corner(hw, -hh), corner(hw, hh), corner(-hw, hh), c);
    }
}

static void drawUsageCelebration() {
    if (!g_celebActive) return;
    const float t = (float)g_celebFrame / 60.0f;
    g_celebFrame++;
    if (t > CELEB_MAX_SECONDS) {
        g_celebActive = false;
        return;
    }
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;

    // Dim what is behind, easing in.
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, (int)(165.0f * clamp01(t / 0.35f))));
    drawConfetti(fg, t, false); // behind the bubble: the glass bends it

    // The bubble pops in with a small overshoot, then floats gently.
    const float a = clamp01(t / 0.55f);
    const float k = 1.70158f, p = a - 1.0f;
    const float scale = 1.0f + (k + 1.0f) * p * p * p + k * p * p; // easeOutBack
    const float R = 215.0f * scale;
    const float cx = W * 0.5f, cy = H * 0.5f + (float)std::sin(t * 1.6f) * 5.0f;
    if (R > 1.0f) {
        if (g_glass) {
            glassCircle(fg, cx, cy, R, GK::Overlay, true);
            // A faint tint inside the glass so the text stays legible over busy rows behind it.
            fg->AddCircleFilled(ImVec2(cx, cy), R * 0.97f, bgTint(col::BG, 0.30f), 72);
        } else {
            fg->AddCircleFilled(ImVec2(cx, cy), R, col::PANEL, 72);
            fg->AddCircle(ImVec2(cx, cy), R, accentMain(), 72, 3.0f);
        }
        // A soft highlight on the upper left, like light on a soap bubble.
        fg->AddCircleFilled(ImVec2(cx - R * 0.46f, cy - R * 0.50f), R * 0.10f, bgTint(col::WHITE, 0.16f), 24);
        fg->AddCircleFilled(ImVec2(cx - R * 0.46f, cy - R * 0.50f), R * 0.05f, bgTint(col::WHITE, 0.30f), 20);
    }

    // Text fades in once the bubble has mostly arrived.
    const float ta = clamp01((t - 0.30f) / 0.40f);
    if (ta > 0.0f) {
        auto centred = [&](float size, float y, ImU32 color, const std::string& str) {
            ImVec2 ts = font()->CalcTextSizeA(size, 1e9f, 0.0f, str.c_str());
            fg->AddText(font(), size, ImVec2(cx - ts.x * 0.5f, y), color, str.c_str());
        };
        centred(TEXT_M, cy - 104.0f, bgTint(col::MUTED, ta), "Time with Ufin");
        centred(76.0f, cy - 70.0f, bgTint(col::TEXT, ta), g_celebBig);
        centred(TEXT_S, cy + 44.0f, bgTint(col::MUTED, ta), g_celebSmall);
        if (t > 1.2f) centred(TEXT_S - 2, cy + 108.0f, bgTint(col::DIM, clamp01((t - 1.2f) / 0.5f)), "Tap or press any button");
    }

    drawConfetti(fg, t, true); // in front of everything
}

// --- browser ---

void drawBrowser(const BrowserModel& m) {
    drawScreenBackdrop();

    // Sidebar
    if (g_glass) {
        // A floating frosted panel; the items keep their exact positions.
        glassSurface(dl(), 8.0f, 10.0f, layout::SIDEBAR_W - 8.0f, layout::SCREEN_H - 10.0f, 26.0f, GK::Panel, true);
    } else {
        rect({0, 0, layout::SIDEBAR_W, layout::SCREEN_H}, col::SIDEBAR);
    }
    drawLogo(30.0f, 40.0f);
    for (int i = 0; i < (int)m.sidebar.size(); i++) {
        Rect r = layout::sidebarItem(i);
        bool current = (i == m.sidebarSelected);
        if (current && m.sidebarFocused) {
            surfaceRect(r, accentDeep(), 12.0f, GK::RowFocus);
        } else if (current) {
            surfaceRect(r, col::PANEL, 12.0f, GK::RowFocusDim);
            if (!g_glass) dl()->AddRectFilled(ImVec2(r.x, r.y + 12), ImVec2(r.x + 4, r.bottom() - 12), accentMain(), 2.0f);
        }
        ImU32 c = (current && m.sidebarFocused) ? col::WHITE : (current ? col::TEXT : col::MUTED);
        drawIcon(m.sidebar[i].icon, r.x + 32, r.y + r.h * 0.5f, 26.0f, c);
        text(TEXT_M, r.x + 58, r.y + (r.h - TEXT_M) * 0.5f - 1, c, m.sidebar[i].label, r.w - 66);
    }
    if (!m.footnote.empty()) {
        text(TEXT_S - 2, layout::SIDEBAR_PAD + 4, layout::SCREEN_H - 44, col::DIM, m.footnote,
             layout::SIDEBAR_W - 2 * layout::SIDEBAR_PAD);
    }
    if (!m.usageLabel.empty()) {
        // "Time used" counter; tapping it five times starts the celebration (see showUsageCelebration).
        const Rect u = layout::usageCounter();
        text(TEXT_S - 3, u.x + 4, u.y + 2, col::DIM, "Time used", u.w - 8);
        text(TEXT_S, u.x + 4, u.y + 2 + (TEXT_S - 3) + 4, col::MUTED, m.usageLabel, u.w - 8);
    }

    // Header
    const int total = (int)m.items.size();
    std::string counter;
    if (total > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", m.selected + 1, total);
        counter = buf;
    }
    float counterW = counter.empty() ? 0.0f : textSize(TEXT_S, counter).x + 24.0f;
    float clockW = m.clock.empty() ? 0.0f : textSize(TEXT_L, m.clock).x + 24.0f;
    text(TEXT_L + 6, layout::CONTENT_X, layout::HEADER_TITLE_Y, col::TEXT, m.title,
         layout::CONTENT_W - std::max(clockW, m.clock.empty() ? counterW : 0.0f));
    if (!m.clock.empty()) {
        textRight(TEXT_L, layout::CONTENT_RIGHT, layout::HEADER_TITLE_Y + 2, col::TEXT, m.clock);
        if (!counter.empty()) textRight(TEXT_S, layout::CONTENT_RIGHT, layout::HEADER_PATH_Y, col::MUTED, counter);
    } else if (!counter.empty()) {
        textRight(TEXT_S, layout::CONTENT_RIGHT, layout::HEADER_TITLE_Y + 12, col::MUTED, counter);
    }
    if (!m.path.empty()) {
        // Keep the deepest part of a long path: cut from the left.
        std::string p = m.path;
        const float pathMax = layout::CONTENT_W - (m.clock.empty() ? 0.0f : counterW);
        while (textSize(TEXT_S, p).x > pathMax && p.size() > 4) {
            size_t cut = 1;
            while (cut < p.size() && ((unsigned char)p[cut] & 0xC0) == 0x80) cut++;
            p = p.substr(cut);
        }
        if (p != m.path) p = "..." + p;
        text(TEXT_S, layout::CONTENT_X, layout::HEADER_PATH_Y, col::MUTED, p);
    }

    // List
    const int visible = layout::visibleRows();
    if (total == 0) {
        text(TEXT_M, layout::CONTENT_X + 8, layout::LIST_TOP + 20, col::MUTED, m.emptyMessage, layout::CONTENT_W);
    } else {
        ListWindow w = computeListWindow(total, m.selected, visible);
        for (int i = w.start; i < w.end; i++) {
            const ListEntry& e = m.items[(size_t)i];
            Rect r = layout::listRow(i - w.start);
            bool sel = (i == m.selected);
            if (sel) glassArtFocus(e.art);
            if (sel && !m.sidebarFocused) {
                surfaceRect(r, accentDeep(), 12.0f, GK::RowFocus);
            } else if (sel) {
                surfaceRect(r, col::PANEL, 12.0f, GK::RowFocusDim);
                if (!g_glass) dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentDeep(), 12.0f, 0, 2.0f);
            } else {
                surfaceRect(r, col::PANEL, 12.0f, GK::Row);
            }
            bool hot = sel && !m.sidebarFocused;
            float x = r.x + 10;
            if (e.art || e.icon != Icon::None) {
                // Artwork slot: the image fitted inside, or the icon until it loads.
                const float boxY = r.y + (r.h - layout::ROW_ART_H) * 0.5f;
                layout::Fit f{x + 8, boxY + 6, layout::ROW_ART_W - 16, layout::ROW_ART_H - 12};
                if (e.art) {
                    f = layout::fitInside(x, boxY, layout::ROW_ART_W, layout::ROW_ART_H, e.artAspect);
                    dl()->AddImageRounded(ImTextureRef((ImTextureID)e.art), ImVec2(f.x, f.y),
                                          ImVec2(f.x + f.w, f.y + f.h), ImVec2(0, 0), ImVec2(1, 1), col::WHITE, 6.0f);
                } else {
                    drawIcon(e.icon, x + layout::ROW_ART_W * 0.5f, r.y + r.h * 0.5f, 30.0f,
                             hot ? col::WHITE : col::MUTED);
                }
                drawArtBadges(e, f);
                x += layout::ROW_ART_W + 14;
            } else {
                x += 8;
            }
            float tagW = (e.tag.empty() ? 0.0f : std::min(textSize(TEXT_S, e.tag).x, r.w * 0.35f) + 24.0f) +
                         (e.favorite ? 30.0f : 0.0f);
            float nameW = r.right() - 18 - tagW - x;
            bool twoLines = !e.detail.empty();
            float nameY = twoLines ? r.y + 9 : r.y + (r.h - TEXT_M) * 0.5f;
            text(TEXT_M, x, nameY, hot ? col::WHITE : col::TEXT, e.name, nameW);
            if (twoLines) text(TEXT_S, x, r.y + 39, hot ? IM_COL32(0xDD, 0xEE, 0xF7, 0xFF) : col::MUTED, e.detail, nameW);
            float tagRight = r.right() - 18;
            if (e.favorite) {
                drawIcon(Icon::Heart, tagRight - 10, r.y + r.h * 0.5f, 18.0f, rgb(0xFF5C8A));
                tagRight -= 30;
            }
            if (!e.tag.empty()) {
                std::string tag = fitText(font(), TEXT_S, e.tag, r.w * 0.35f);
                textRight(TEXT_S, tagRight, r.y + (r.h - TEXT_S) * 0.5f, hot ? col::WHITE : col::MUTED, tag);
            }
        }
        if (total > visible) {
            Rect track = layout::scrollTrack();
            rect(track, col::PANEL, 3.0f);
            float thumbH = std::max(24.0f, track.h * visible / total);
            float thumbY = track.y + (track.h - thumbH) * w.start / (float)(total - visible);
            dl()->AddRectFilled(ImVec2(track.x, thumbY), ImVec2(track.right(), thumbY + thumbH), col::MUTED, 3.0f);
        }
    }

    drawHints(m.hints, layout::CONTENT_X, layout::FOOTER_Y);
    drawUsageCelebration(); // hidden easter egg: over everything, only while it is running
}

// --- message card ---

void drawMessage(const MessageModel& m) {
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);

    const float cardW = 820.0f;
    const float x = (layout::SCREEN_W - cardW) * 0.5f;
    const float pad = 36.0f;
    const float wrap = cardW - 2 * pad;

    // Height: title + wrapped lines.
    float bodyH = 0.0f;
    for (const std::string& line : m.lines) {
        bodyH += line.empty() ? TEXT_M * 0.6f
                              : font()->CalcTextSizeA(TEXT_M, 1e9f, wrap, line.c_str()).y + 8.0f;
    }
    float cardH = std::min(560.0f, pad * 2 + TEXT_L + 26.0f + bodyH);
    float y = (layout::SCREEN_H - cardH) * 0.5f - 10.0f;

    surfaceRect({x, y, cardW, cardH}, col::PANEL, 18.0f, GK::Panel);
    if (g_glass) {
        // Cut from the card's own rounded outline, so the corners follow it.
        glassBand(dl(), x, y, x + cardW, y + cardH, gr(18.0f), y, y + 6.0f, m.isError ? col::ERROR_RED : accentMain());
    } else {
        dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + cardW, y + 6), m.isError ? col::ERROR_RED : accentMain(), gr(18.0f),
                            ImDrawFlags_RoundCornersTop);
    }

    std::string title = m.title;
    if (m.busy) {
        int dots = (int)(ImGui::GetTime() * 3.0) % 4;
        title += std::string((size_t)dots, '.');
    }
    text(TEXT_L, x + pad, y + pad, m.isError ? col::ERROR_RED : col::TEXT, title, wrap);

    float ty = y + pad + TEXT_L + 26.0f;
    const float maxY = y + cardH - pad;
    for (const std::string& line : m.lines) {
        if (line.empty()) {
            ty += TEXT_M * 0.6f;
            continue;
        }
        float h = font()->CalcTextSizeA(TEXT_M, 1e9f, wrap, line.c_str()).y;
        if (ty + h > maxY) break;
        dl()->AddText(font(), TEXT_M, ImVec2(x + pad, ty), col::MUTED, line.c_str(), nullptr, wrap);
        ty += h + 8.0f;
    }

    drawHints(m.hints, x, y + cardH + 24.0f);
}

// --- now playing ---

void drawNowPlaying(const NowPlayingModel& m) {
    drawScreenBackdrop();
    glassArtFocus(m.art);
    drawLogo(30.0f, 40.0f);

    // Artwork: the album art, or a gradient tile with a note.
    const float art = layout::NP_ART;
    const float ax = layout::NP_ART_X, ay = layout::NP_ART_Y;
    if (m.art) {
        dl()->AddImageRounded(ImTextureRef((ImTextureID)m.art), ImVec2(ax, ay), ImVec2(ax + art, ay + art),
                              ImVec2(0, 0), ImVec2(1, 1), col::WHITE, 22.0f);
    } else {
        gradientRoundRect(ax, ay, ax + art, ay + art, 22.0f, accentSecond(), accentDeep());
        drawIcon(Icon::Music, ax + art * 0.5f, ay + art * 0.5f, 150.0f, IM_COL32(0xFF, 0xFF, 0xFF, 0xD0));
    }
    if (m.paused) {
        dl()->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + art, ay + art), IM_COL32(0, 0, 0, 0x70), 22.0f);
        float cx = ax + art * 0.5f, cy = ay + art * 0.5f;
        dl()->AddRectFilled(ImVec2(cx - 34, cy - 44), ImVec2(cx - 10, cy + 44), col::WHITE, 5.0f);
        dl()->AddRectFilled(ImVec2(cx + 10, cy - 44), ImVec2(cx + 34, cy + 44), col::WHITE, 5.0f);
    }

    const float tx = layout::NP_TEXT_X;
    const float tw = layout::NP_TEXT_W;
    text(TEXT_S, tx, layout::NP_LABEL_Y, accentMain(), m.paused ? "PAUSED" : "NOW PLAYING");
    dl()->AddText(font(), 40.0f, ImVec2(tx, layout::NP_LABEL_Y + 36.0f), col::TEXT, m.title.c_str(), nullptr, tw);
    text(TEXT_M, tx, layout::NP_SUBTITLE_Y, col::MUTED, m.subtitle, tw);

    drawProgress(tx, layout::NP_PROGRESS_Y, tw, 8.0f, m.positionSeconds, m.durationSeconds, false);
    text(TEXT_S, tx, layout::NP_TIME_Y, col::MUTED, formatTime(m.positionSeconds));
    if (m.durationSeconds > 0.0)
        textRight(TEXT_S, tx + tw, layout::NP_TIME_Y, col::MUTED, formatTime(m.durationSeconds));

    // Transport buttons: seek back / play-pause / seek forward / stop,
    // touch-tappable (see layout::npButtonRect / hitTestNowPlaying).
    // Slightly transparent so they read as an overlay on the artwork
    // row rather than solid chrome.
    for (int i = 0; i < (int)ui::NpButton::Count; i++) {
        ui::Rect r = ui::npButtonRect(i);
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        if (g_glass) {
            // Normal / pressed (finger down on it, from the existing touch
            // hit-test). Focus on this screen is the touch/remote itself, so
            // "hot" is not used here.
            glassCircle(dl(), cx, cy, r.w * 0.5f, m.pressedButton == i ? GK::ButtonPressed : GK::Button, true);
        } else {
            dl()->AddCircleFilled(ImVec2(cx, cy), r.w * 0.5f, IM_COL32(0x2C, 0x31, 0x3A, 0xB0), 32);
            dl()->AddCircle(ImVec2(cx, cy), r.w * 0.5f, IM_COL32(0x3A, 0x3F, 0x48, 0xA0), 32, 1.5f);
        }
        const ImU32 ic = IM_COL32(0xEC, 0xEE, 0xF1, 0xE6);
        switch ((ui::NpButton)i) {
            case ui::NpButton::SeekBack:
            case ui::NpButton::SeekForward: {
                const char* label = (ui::NpButton)i == ui::NpButton::SeekBack ? "-10" : "+30";
                const float fs = r.w * 0.34f;
                ImVec2 sz = font()->CalcTextSizeA(fs, 1e9f, 0.0f, label);
                dl()->AddText(font(), fs, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), ic, label);
                break;
            }
            case ui::NpButton::PlayPause: {
                const float s = r.w * 0.32f;
                if (m.paused) {
                    dl()->AddTriangleFilled(ImVec2(cx - s * 0.55f, cy - s), ImVec2(cx - s * 0.55f, cy + s),
                                            ImVec2(cx + s * 0.85f, cy), ic);
                } else {
                    dl()->AddRectFilled(ImVec2(cx - s * 0.7f, cy - s * 0.85f), ImVec2(cx - s * 0.15f, cy + s * 0.85f), ic, 2.0f);
                    dl()->AddRectFilled(ImVec2(cx + s * 0.15f, cy - s * 0.85f), ImVec2(cx + s * 0.7f, cy + s * 0.85f), ic, 2.0f);
                }
                break;
            }
            case ui::NpButton::Stop: {
                const float s = r.w * 0.32f;
                dl()->AddRectFilled(ImVec2(cx - s * 0.75f, cy - s * 0.75f), ImVec2(cx + s * 0.75f, cy + s * 0.75f), ic, 2.0f);
                break;
            }
            default: break;
        }
    }

    // Volume: a plain draggable track, no label needed -- its position
    // under the transport buttons and the speaker icon say what it is.
    // Touch handling for the drag lives in PlaybackControl::poll() (see
    // layout::npVolumeRect / npVolumeFraction).
    {
        float vol = m.volume < 0.0f ? 0.0f : (m.volume > 1.0f ? 1.0f : m.volume);
        ui::Rect vr = ui::npVolumeRect();
        float lineY = vr.y + vr.h * 0.5f;
        const float lineH = 6.0f;
        const float iconW = 30.0f;
        float trackX = vr.x + iconW, trackW = vr.w - iconW;
        drawIcon(Icon::Music, vr.x + iconW * 0.5f, lineY, iconW * 0.8f, IM_COL32(0xB8, 0xBC, 0xC4, 0xFF));
        if (g_glass) {
            // A glass groove with an accent fill cut from the groove's own
            // rounded outline, and a glass knob.
            const float gh = 14.0f, gy0 = lineY - gh * 0.5f, gy1 = lineY + gh * 0.5f;
            glassSurface(dl(), trackX, gy0, trackX + trackW, gy1, gh * 0.5f, GK::Row, true);
            if (vol > 0.0f) {
                const float r = glassRadius(trackW, gh, gh * 0.5f);
                Poly fill = clipPoly(roundRectPoly(trackX + 2.0f, gy0 + 2.0f, trackX + trackW - 2.0f, gy1 - 2.0f, std::max(0.0f, r - 2.0f)),
                                     true, false, trackX + trackW * vol);
                if (fill.size() >= 3) dl()->AddConvexPolyFilled(fill.data(), (int)fill.size(), (accentMain() & 0x00FFFFFF) | (0xD8u << 24));
            }
            glassCircle(dl(), trackX + trackW * vol, lineY, 14.0f, GK::Button, true);
        } else {
            dl()->AddRectFilled(ImVec2(trackX, lineY - lineH * 0.5f), ImVec2(trackX + trackW, lineY + lineH * 0.5f),
                                col::TRACK, lineH * 0.5f);
            if (vol > 0.0f) {
                dl()->AddRectFilled(ImVec2(trackX, lineY - lineH * 0.5f), ImVec2(trackX + trackW * vol, lineY + lineH * 0.5f),
                                    accentMain(), lineH * 0.5f);
            }
            dl()->AddCircleFilled(ImVec2(trackX + trackW * vol, lineY), lineH * 1.4f, col::WHITE, 20);
        }
    }

    if (!m.queueText.empty()) text(TEXT_S, tx, layout::NP_QUEUE_Y, col::TEXT, m.queueText, tw);
    if (!m.nextText.empty()) text(TEXT_S, tx, layout::NP_QUEUE_Y + 30.0f, col::MUTED, m.nextText, tw);

    drawHints(m.hints, 110.0f, layout::FOOTER_Y);
}

// --- music visualizer ---

void drawMusicVisualizer(const VisualizerModel& m) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);

    text(TEXT_S, 60.0f, 108.0f, accentMain(), m.paused ? "PAUSED" : "NOW PLAYING");
    text(TEXT_M, 60.0f, 140.0f, col::TEXT, m.title, W - 120.0f);

    // Bars, centered, leaving room for the title above and hints below.
    const int n = m.bandCount > 0 ? std::min(m.bandCount, VisualizerModel::MAX_BANDS) : 0;
    if (n > 0) {
        const float top = 240.0f, bottom = layout::FOOTER_Y - 60.0f;
        const float areaH = bottom - top;
        const float gap = 18.0f;
        const float barW = (W - 120.0f - gap * (n - 1)) / (float)n;
        for (int i = 0; i < n; i++) {
            float level = m.bands[i];
            if (level < 0.0f) level = 0.0f;
            if (level > 1.0f) level = 1.0f;
            // A small floor so a quiet passage still shows a sliver of
            // every bar rather than the row looking dead/broken.
            float h = areaH * (0.03f + 0.97f * level);
            float x0 = 60.0f + i * (barW + gap);
            float x1 = x0 + barW;
            float y1 = bottom;
            float y0 = bottom - h;
            // Low bands lean toward the deep accent, high bands toward
            // the secondary accent -- a cheap per-bar gradient rather
            // than a full per-pixel one, matching the same two-colour
            // palette used everywhere else (drawLogo, Now Playing art).
            float t = (float)i / (float)std::max(1, n - 1);
            ImU32 bandColor = lerpColor(accentDeep(), accentSecond(), t);
            gradientRoundRect(x0, y0, x1, y1, std::min(10.0f, barW * 0.3f), accentMain(), bandColor);
        }
    } else {
        // No audio data yet (e.g. right at track start) -- a calm empty
        // state rather than a blank gap.
        text(TEXT_S, 60.0f, 240.0f, col::MUTED, "Warming up...");
    }

    drawHints(m.hints, 110.0f, layout::FOOTER_Y);
}

void drawBufferingIndicator(double bufferedSeconds, double targetSeconds) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    const float cx = W * 0.5f, cy = H * 0.45f; // same spot as the pause icon
    const float ring = 30.0f, thick = 6.0f;
    const float PI2 = 6.2831853f;

    // Translucent disc so the wheel reads over any picture, then a dim full
    // ring with a bright ~110 degree arc spinning around it (~1 turn/s).
    if (g_glass) glassCircle(dl(), cx, cy, 64.0f, GK::Overlay, false);
    else dl()->AddCircleFilled(ImVec2(cx, cy), 64.0f, IM_COL32(0, 0, 0, 0x90), 48);
    dl()->AddCircle(ImVec2(cx, cy), ring, IM_COL32(0xFF, 0xFF, 0xFF, 0x40), 32, thick);
    const float a0 = (float)std::fmod(ImGui::GetTime() * 6.2, (double)PI2);
    dl()->PathArcTo(ImVec2(cx, cy), ring, a0, a0 + 1.9f, 20);
    dl()->PathStroke(col::WHITE, ImDrawFlags_None, thick);

    // "Buffering  4 / 12 s": calm wording, in a pill under the wheel.
    char buf[64];
    if (targetSeconds > 0.0) {
        int have = (int)(bufferedSeconds < 0.0 ? 0.0 : bufferedSeconds);
        snprintf(buf, sizeof(buf), "Buffering  %d / %d s", have, (int)(targetSeconds + 0.5));
    } else {
        snprintf(buf, sizeof(buf), "Buffering");
    }
    const ImVec2 s = textSize(TEXT_S, buf);
    const float bx = cx - s.x * 0.5f - 16.0f, by = cy + 64.0f + 12.0f;
    if (g_glass) glassSurface(dl(), bx, by, bx + s.x + 32.0f, by + s.y + 14.0f, gr(12.0f), GK::Overlay, false);
    else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + s.x + 32.0f, by + s.y + 14.0f), IM_COL32(0, 0, 0, 0xB0), 12.0f);
    text(TEXT_S, bx + 16.0f, by + 7.0f, col::WHITE, buf);
}

void drawVideoHud(const VideoHudModel& m) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;

    if (m.buffering && !m.paused) drawBufferingIndicator(m.bufferedSeconds, m.bufferingTargetSeconds);

    if (m.paused) {
        float cx = W * 0.5f, cy = H * 0.45f;
        if (g_glass) glassCircle(dl(), cx, cy, 64.0f, GK::Overlay, false);
        else dl()->AddCircleFilled(ImVec2(cx, cy), 64.0f, IM_COL32(0, 0, 0, 0x90), 48);
        dl()->AddRectFilled(ImVec2(cx - 24, cy - 30), ImVec2(cx - 7, cy + 30), col::WHITE, 4.0f);
        dl()->AddRectFilled(ImVec2(cx + 7, cy - 30), ImVec2(cx + 24, cy + 30), col::WHITE, 4.0f);
    }

    if (m.seekDelta != 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%d s   ->   %s", m.seekDelta > 0 ? "+" : "", (int)m.seekDelta,
                 formatTime(m.seekTarget).c_str());
        ImVec2 s = textSize(TEXT_L, buf);
        float bx = (W - s.x) * 0.5f - 28, by = 60.0f;
        if (g_glass) glassSurface(dl(), bx, by, bx + s.x + 56, by + s.y + 28, gr(16.0f), GK::Overlay, false);
        else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + s.x + 56, by + s.y + 28), IM_COL32(0, 0, 0, 0xB0), 16.0f);
        text(TEXT_L, bx + 28, by + 14, col::WHITE, buf);
    }

    if (!m.notice.empty()) {
        ImVec2 ns = textSize(TEXT_M, m.notice);
        float bx = (W - ns.x) * 0.5f - 24, by = 60.0f;
        if (m.seekDelta != 0.0) by += 70;
        if (g_glass) glassSurface(dl(), bx, by, bx + ns.x + 48, by + ns.y + 24, gr(14.0f), GK::Overlay, false);
        else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + ns.x + 48, by + ns.y + 24), IM_COL32(0, 0, 0, 0xB0), 14.0f);
        text(TEXT_M, bx + 24, by + 12, col::WHITE, m.notice);
    }

    if (m.trackMenu) {
        // Audio & subtitles panel, right side.
        const float pw = 560, px = W - pw - 50, py = 120, rowH = 74;
        if (g_glass) glassSurface(dl(), px, py, px + pw, py + 70 + 2 * (rowH + 10) + 20, gr(18.0f), GK::Overlay, false);
        else dl()->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 70 + 2 * (rowH + 10) + 20), IM_COL32(0x14, 0x16, 0x1A, 0xEE), 18.0f);
        text(TEXT_L, px + 28, py + 22, col::WHITE, "Audio & subtitles");
        const char* labels[2] = {"Audio", "Subtitles"};
        const std::string values[2] = {m.audioLabel, m.subtitleLabel};
        for (int i = 0; i < 2; i++) {
            float ry = py + 76 + i * (rowH + 10);
            bool sel = (i == m.trackRow);
            if (g_glass) glassSurface(dl(), px + 18, ry, px + pw - 18, ry + rowH, gr(12.0f), sel ? GK::RowFocus : GK::Row, false);
            else dl()->AddRectFilled(ImVec2(px + 18, ry), ImVec2(px + pw - 18, ry + rowH), sel ? accentDeep() : col::PANEL, 12.0f);
            text(TEXT_S, px + 36, ry + 10, sel ? col::WHITE : col::MUTED, labels[i]);
            text(TEXT_M, px + 60, ry + 36, col::WHITE, values[i], pw - 150);
            text(TEXT_M, px + 36, ry + 36, col::WHITE, "<");
            textRight(TEXT_M, px + pw - 36, ry + 36, col::WHITE, ">");
        }
    }

    if (!m.visible && !m.paused && m.seekDelta == 0.0 && !m.trackMenu) return;

    // Bottom panel: fade to dark, title, progress, times, hints.
    const float top = H - 190.0f;
    dl()->AddRectFilledMultiColor(ImVec2(0, top), ImVec2(W, H), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 0xE0), IM_COL32(0, 0, 0, 0xE0));
    const float x = 64.0f, w = W - 128.0f;
    float y = top + 46.0f;
    if (m.live) {
        dl()->AddRectFilled(ImVec2(x, y + 4), ImVec2(x + 58, y + 30), col::LIVE_RED, 6.0f);
        text(TEXT_S, x + 9, y + 6, col::WHITE, "LIVE");
        text(TEXT_L, x + 72, y, col::WHITE, m.title, w - 72);
    } else {
        text(TEXT_L, x, y, col::WHITE, m.title, w);
    }
    y += 50.0f;
    drawProgress(x, y, w, 6.0f, m.positionSeconds, m.durationSeconds, m.live);
    y += 18.0f;
    text(TEXT_S, x, y, IM_COL32(0xDD, 0xDD, 0xDD, 0xFF), formatTime(m.positionSeconds));
    if (m.durationSeconds > 0.0) {
        textRight(TEXT_S, x + w, y, IM_COL32(0xDD, 0xDD, 0xDD, 0xFF), formatTime(m.durationSeconds));
    }
    drawHints(m.hints, x, H - 44.0f);
}

// --- login ---

static void drawBrandPanel() {
    // Big logo on the left.
    const float s = 96.0f, x = 110.0f, y = 210.0f;
    gradientRoundRect(x, y, x + s, y + s, 24.0f, accentSecond(), accentMain());
    dl()->AddTriangleFilled(ImVec2(x + 36, y + 26), ImVec2(x + 36, y + 70), ImVec2(x + 72, y + 48), col::WHITE);
    text(64.0f, x, y + s + 24.0f, col::TEXT, "Ufin");
    text(TEXT_M, x, y + s + 100.0f, col::MUTED, "Jellyfin for the Wii U");
}

void drawLogin(const LoginModel& m) {
    drawScreenBackdrop();
    drawBrandPanel();

    text(TEXT_L + 6, layout::LOGIN_X, 70.0f, col::TEXT, "Sign in to Jellyfin");

    for (int i = 0; i < (int)m.rows.size(); i++) {
        const LoginField& f = m.rows[(size_t)i];
        Rect r = layout::loginRow(i);
        bool focus = (i == m.focused);
        if (f.isButton) {
            ImU32 fill = f.primary ? (focus ? accentMain() : accentDeep()) : (focus ? col::PANEL_HI : col::PANEL);
            surfaceRect(r, fill, 14.0f, (f.primary || focus) ? GK::ButtonHot : GK::Button);
            if (!g_glass && focus && !f.primary) {
                dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
            }
            ImVec2 ts = textSize(TEXT_M, f.label);
            text(TEXT_M, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - TEXT_M) * 0.5f - 1, col::WHITE, f.label);
            continue;
        }
        surfaceRect(r, focus ? col::PANEL_HI : col::PANEL, 14.0f, focus ? GK::ButtonHot : GK::Button);
        if (!g_glass && focus) dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
        text(TEXT_S, r.x + 20, r.y + 9, focus ? accentMain() : col::MUTED, f.label);
        std::string shown = f.value;
        if (f.password && !shown.empty()) {
            shown.clear();
            // one dot per character, not per UTF-8 byte
            for (char c : f.value) if (((unsigned char)c & 0xC0) != 0x80) shown += "*";
        }
        if (shown.empty()) text(TEXT_M, r.x + 20, r.y + 34, col::DIM, f.placeholder, r.w - 40);
        else text(TEXT_M, r.x + 20, r.y + 34, col::TEXT, shown, r.w - 40);
    }

    if (!m.message.empty()) {
        std::string msg = m.message;
        if (m.busy) msg += std::string((size_t)((int)(ImGui::GetTime() * 3.0) % 4), '.');
        float y = layout::loginRow((int)m.rows.size()).y + 4;
        dl()->AddText(font(), TEXT_S + 1, ImVec2(layout::LOGIN_X + 4, y),
                      m.messageIsError ? col::ERROR_RED : col::MUTED, msg.c_str(), nullptr, layout::LOGIN_W - 8);
    }
    drawHints(m.hints, layout::LOGIN_X, layout::FOOTER_Y);
}

void drawQuickConnect(const QuickConnectModel& m) {
    drawScreenBackdrop();
    drawBrandPanel();

    text(TEXT_L + 6, layout::LOGIN_X, 70.0f, col::TEXT, "Quick Connect");
    const float x = layout::LOGIN_X, w = layout::LOGIN_W;
    dl()->AddText(font(), TEXT_M, ImVec2(x, 130), col::MUTED,
                  "On a phone or computer that's signed in to Jellyfin, open your profile "
                  "-> Quick Connect and enter this code:", nullptr, w);

    // The code, big, in two groups of three.
    std::string code = m.code;
    if (code.size() == 6) code = code.substr(0, 3) + " " + code.substr(3);
    Rect card{x, 240, w, 170};
    surfaceRect(card, col::PANEL, 18.0f, GK::Panel);
    ImVec2 cs = font()->CalcTextSizeA(96.0f, 1e9f, 0.0f, code.c_str());
    dl()->AddText(font(), 96.0f, ImVec2(x + (w - cs.x) * 0.5f, card.y + (card.h - cs.y) * 0.5f), col::WHITE,
                  code.c_str());

    text(TEXT_S, x, 432, col::DIM, "Server: " + m.server, w);
    if (!m.message.empty()) {
        std::string msg = m.message;
        if (!m.messageIsError) msg += std::string((size_t)((int)(ImGui::GetTime() * 3.0) % 4), '.');
        dl()->AddText(font(), TEXT_S + 1, ImVec2(x, 470), m.messageIsError ? col::ERROR_RED : col::MUTED,
                      msg.c_str(), nullptr, w);
    }
    drawHints(m.hints, x, layout::FOOTER_Y);
}

void drawToast(const std::string& t, double age, double lifetime) {
    if (t.empty() || age < 0.0 || age > lifetime) return;
    float alpha = (float)std::min(1.0, (lifetime - age) / 0.5);
    ImVec2 ts = textSize(TEXT_M, t);
    float w = ts.x + 48, h = 50, x = (layout::SCREEN_W - w) * 0.5f, y = 24;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (g_glass) {
        glassSurface(fg, x, y, x + w, y + h, h * 0.5f, GK::Button, true, alpha);
    } else {
        fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x2C, 0x31, 0x3A, (int)(245 * alpha)), h * 0.5f);
        fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x00, 0xA4, 0xDC, (int)(255 * alpha)), h * 0.5f, 0, 2.0f);
    }
    fg->AddText(font(), TEXT_M, ImVec2(x + 24, y + (h - ts.y) * 0.5f), IM_COL32(0xEC, 0xEE, 0xF1, (int)(255 * alpha)),
                t.c_str());
}

void drawCrtOverlay(double time) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;

    // Scanlines: a dark line every 3 pixels.
    for (float y = 0; y < H; y += 3.0f) {
        fg->AddRectFilled(ImVec2(0, y), ImVec2(W, y + 1.0f), IM_COL32(0, 0, 0, 90));
    }
    // Faint phosphor tint and a slow flicker.
    int flicker = 10 + (int)(4.0 * std::sin(time * 50.0));
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(60, 255, 150, flicker));
    // A soft bright band rolling down the screen.
    float band = (float)std::fmod(time * 90.0, (double)(H + 160)) - 80.0f;
    fg->AddRectFilledMultiColor(ImVec2(0, band - 60), ImVec2(W, band), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 18));
    fg->AddRectFilledMultiColor(ImVec2(0, band), ImVec2(W, band + 60), IM_COL32(255, 255, 255, 18),
                                IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    // Vignette: darker towards every edge.
    const ImU32 clear = IM_COL32(0, 0, 0, 0), dark = IM_COL32(0, 0, 0, 150);
    const float e = 150.0f;
    fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, e), dark, dark, clear, clear);
    fg->AddRectFilledMultiColor(ImVec2(0, H - e), ImVec2(W, H), clear, clear, dark, dark);
    fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(e, H), dark, clear, clear, dark);
    fg->AddRectFilledMultiColor(ImVec2(W - e, 0), ImVec2(W, H), clear, dark, dark, clear);
    // Rounded tube corners: a thick black rounded frame.
    fg->AddRect(ImVec2(-30, -30), ImVec2(W + 30, H + 30), IM_COL32(0, 0, 0, 255), 90.0f, 0, 64.0f);
}

void drawChoice(const ChoiceModel& m) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(layout::SCREEN_W, layout::SCREEN_H), IM_COL32(0, 0, 0, 0xB0));
    const int n = (int)m.options.size();
    Rect first = choiceRow(0, n), last = choiceRow(n > 0 ? n - 1 : 0, n);
    float cardX = first.x - 40, cardW = first.w + 80;
    float cardY = first.y - 130, cardH = (last.bottom() - cardY) + 40;
    if (m.art) {
        cardX -= 130;
        cardW += 130;
    }
    if (g_glass) glassSurface(fg, cardX, cardY, cardX + cardW, cardY + cardH, gr(18.0f), GK::Panel, true);
    else fg->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH), col::PANEL, 18.0f);
    if (g_glass) glassBand(fg, cardX, cardY, cardX + cardW, cardY + cardH, gr(18.0f), cardY, cardY + 6.0f, accentMain());
    else fg->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + 6), accentMain(), gr(18.0f),
                           ImDrawFlags_RoundCornersTop);
    float tx = first.x;
    if (m.art) {
        layout::Fit f = layout::fitInside(cardX + 30, cardY + 30, 110, cardH - 60, m.artAspect);
        fg->AddImageRounded(ImTextureRef((ImTextureID)m.art), ImVec2(f.x, f.y), ImVec2(f.x + f.w, f.y + f.h),
                            ImVec2(0, 0), ImVec2(1, 1), col::WHITE, 8.0f);
    }
    fg->AddText(font(), TEXT_L, ImVec2(tx, cardY + 30), col::TEXT, fitText(font(), TEXT_L, m.title, first.w).c_str());
    fg->AddText(font(), TEXT_S, ImVec2(tx, cardY + 76), col::MUTED, fitText(font(), TEXT_S, m.subtitle, first.w).c_str());
    for (int i = 0; i < n; i++) {
        Rect r = choiceRow(i, n);
        bool sel = (i == m.selected);
        if (g_glass) glassSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(14.0f), sel ? GK::ButtonHot : GK::Button, true);
        else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), sel ? accentDeep() : col::PANEL_HI, 14.0f);
        ImVec2 ts = textSize(TEXT_M, m.options[(size_t)i]);
        fg->AddText(font(), TEXT_M, ImVec2(r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - ts.y) * 0.5f), col::WHITE,
                    m.options[(size_t)i].c_str());
    }
    // Hints under the card (drawn on top, like the card).
    float hx = cardX;
    const float rr = 14.0f;
    for (const Hint& hint : m.hints) {
        float bw = std::max(2 * rr, textSize(TEXT_S, hint.button).x + 14.0f);
        float hy = cardY + cardH + 18;
        if (g_glass) glassSurface(fg, hx, hy, hx + bw, hy + 2 * rr, rr, GK::Button, true);
        else fg->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + bw, hy + 2 * rr), col::PANEL_HI, rr);
        ImVec2 bs = textSize(TEXT_S, hint.button);
        fg->AddText(font(), TEXT_S, ImVec2(hx + (bw - bs.x) * 0.5f, hy + rr - bs.y * 0.5f), col::TEXT, hint.button.c_str());
        hx += bw + 8;
        fg->AddText(font(), TEXT_S, ImVec2(hx, hy + rr - bs.y * 0.5f), col::MUTED, hint.label.c_str());
        hx += textSize(TEXT_S, hint.label).x + 26;
    }
}

// --- Help window ----------------------------------------------------------------------------
//
// A rotating 3D controller (a .glb from the SD card, projected by hand and drawn as ImGui
// triangles -- no GPU work of its own) beside a scrolling list of what each button does. The model
// swings left and right, then does a full turn now and then. No file: "Missing model".

namespace {

struct V3 { float x, y, z; };

V3 rotate3(V3 p, float yaw, float pitch) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float x = p.x * cy + p.z * sy, z = -p.x * sy + p.z * cy;
    return {x, p.y * cp - z * sp, p.y * sp + z * cp};
}

// Yaw/pitch of the model at animation frame `frame`: it swings +-45 degrees, then every 14 s
// turns all the way round (showing the back) before swinging again.
void helpPose(int frame, float& yaw, float& pitch) {
    const float t = (float)frame / 60.0f;
    const float cycle = 14.0f, p = std::fmod(t, cycle) / cycle;
    auto smooth = [](float a, float b, float x) { float u = clamp01((x - a) / (b - a)); return u * u * (3 - 2 * u); };
    const float amp = smooth(0.0f, 0.1f, p) * (1.0f - smooth(0.4f, 0.5f, p));
    yaw = amp * 0.80f * std::sin(t * 1.1f);
    if (p >= 0.5f) {
        const float u = (p - 0.5f) * 2.0f;
        yaw += 6.2831853f * (u * u * (3 - 2 * u));
    }
    pitch = 0.20f + 0.07f * std::sin(t * 0.7f);
}

// ---- the models: .glb files from the SD card, loaded on a worker thread ----
struct ModelSlot {
    enum { Idle, Loading, Decoded, Ready, Missing, Failed };
    std::atomic<int> state{Idle};
    std::thread worker;
    std::string err;            // written by the worker before it publishes `state`
    Model3D model;
    std::vector<uint64_t> tex;  // GPU textures, parallel to model.textures
    ~ModelSlot() { if (worker.joinable()) worker.join(); }
};
ModelSlot g_slots[help::kControllerCount];
std::string g_modelDir = help::kModelDir;
std::function<uint64_t(const uint8_t*, int, int)> g_modelUpload;
std::function<void(uint64_t)> g_modelRelease;

// Starts loading on first use, uploads the textures on this (the render) thread once decoded.
ModelSlot& pollModel(int kind) {
    kind = kind < 0 ? 0 : (kind >= help::kControllerCount ? help::kControllerCount - 1 : kind);
    ModelSlot& s = g_slots[kind];
    const int st = s.state.load();
    if (st == ModelSlot::Idle) {
        s.state.store(ModelSlot::Loading);
        const std::string path = g_modelDir + help::modelFile(help::controllerAt(kind));
        ModelOrient o;
        o.tall = (help::controllerAt(kind) == help::Controller::WiiRemote);
        if (s.worker.joinable()) s.worker.join();
        s.worker = std::thread([&s, path, o] {
            const ModelLoad r = loadModelFile(path, o, s.model, s.err);
            s.state.store(r == ModelLoad::Ok ? ModelSlot::Decoded : (r == ModelLoad::Missing ? ModelSlot::Missing : ModelSlot::Failed));
        });
    } else if (st == ModelSlot::Decoded) {
        if (s.worker.joinable()) s.worker.join();
        for (ModelTexture& t : s.model.textures) {
            s.tex.push_back(g_modelUpload ? g_modelUpload(t.rgba.data(), t.w, t.h) : 0);
            std::vector<uint8_t>().swap(t.rgba); // the GPU has its own copy now
        }
        s.state.store(ModelSlot::Ready);
    }
    return s;
}

// Draws a loaded model centred at (cx, cy); `scale` = pixels per model unit. Painter's algorithm:
// back faces dropped, triangles sorted far to near, each run of one texture is one ImGui draw command.
void drawLoadedModel(ImDrawList* d, const ModelSlot& s, float cx, float cy, float scale, int frame) {
    const Model3D& m = s.model;
    const size_t nv = m.vertexCount(), nt = m.triangleCount();
    if (!nv || !nt) return;
    float yaw, pitch;
    helpPose(frame, yaw, pitch);
    const V3 light = {-0.35f, 0.55f, 0.75f};
    static std::vector<ImVec2> sp; // per vertex: screen position, depth, brightness (reused every frame)
    static std::vector<float> sz, lit;
    static std::vector<std::pair<float, uint32_t>> order;
    sp.resize(nv); sz.resize(nv); lit.resize(nv);
    for (size_t i = 0; i < nv; i++) {
        const V3 r = rotate3({m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}, yaw, pitch);
        const float k = scale * 6.0f / (6.0f - r.z);
        sp[i] = ImVec2(cx + r.x * k, cy - r.y * k);
        sz[i] = r.z;
        const V3 n = rotate3({m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]}, yaw, pitch);
        const float dot = n.x * light.x + n.y * light.y + n.z * light.z;
        lit[i] = 0.55f + 0.45f * (dot < 0 ? -dot * 0.4f : dot);
    }
    order.clear();
    for (size_t t = 0; t < nt; t++) {
        const uint32_t i0 = m.idx[t * 3], i1 = m.idx[t * 3 + 1], i2 = m.idx[t * 3 + 2];
        const float area = (sp[i1].x - sp[i0].x) * (sp[i2].y - sp[i0].y) - (sp[i1].y - sp[i0].y) * (sp[i2].x - sp[i0].x);
        // glTF front faces run counter-clockwise; the screen's y axis points down, so they come out negative.
        const bool dbl = m.materials[m.triMat[t]].doubleSided;
        if (area == 0 || (area > 0 && !dbl)) continue;
        order.emplace_back((sz[i0] + sz[i1] + sz[i2]) * (1.0f / 3.0f), (uint32_t)t);
    }
    std::sort(order.begin(), order.end());
    const size_t kMaxDrawn = 13000; // 3 vertices each: stays well inside one draw list's 16-bit indices
    if (order.size() > kMaxDrawn) order.erase(order.begin(), order.end() - (std::ptrdiff_t)kMaxDrawn);

    auto texOf = [&](uint32_t t) -> uint64_t {
        const int ti = m.materials[m.triMat[t]].tex;
        return (ti >= 0 && (size_t)ti < s.tex.size()) ? s.tex[(size_t)ti] : 0;
    };
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    bool pushed = false;
    for (size_t i = 0; i < order.size();) {
        const uint64_t h = texOf(order[i].second);
        size_t j = i;
        while (j < order.size() && texOf(order[j].second) == h) j++;
        if (pushed) { d->PopTexture(); pushed = false; }
        if (h) { d->PushTexture(ImTextureRef((ImTextureID)h)); pushed = true; }
        d->PrimReserve((int)(j - i) * 3, (int)(j - i) * 3);
        for (size_t k = i; k < j; k++) {
            const uint32_t t = order[k].second;
            const ModelMaterial& mat = m.materials[m.triMat[t]];
            for (int c = 0; c < 3; c++) {
                const uint32_t vi = m.idx[t * 3 + (size_t)c];
                auto ch = [&](float f) { const float v = f * lit[vi] * 255.0f + 0.5f; return (int)(v > 255 ? 255 : (v < 0 ? 0 : v)); };
                const int a = mat.blend ? (int)(std::min(1.0f, std::max(0.0f, mat.color[3])) * 255.0f + 0.5f) : 255;
                const ImVec2 uv = h ? ImVec2(m.uv[vi * 2], m.uv[vi * 2 + 1]) : white;
                d->PrimVtx(sp[vi], uv, IM_COL32(ch(mat.color[0]), ch(mat.color[1]), ch(mat.color[2]), a));
            }
        }
        i = j;
    }
    if (pushed) d->PopTexture();
}

// ---- layout shared by drawing and hit testing ----
struct HelpGeom {
    Rect card, stage, list;
    std::vector<Rect> tabs;
    std::vector<float> rowY, rowH; // top of each row in content space, and its height
    float contentH = 0, scroll = 0;
};

HelpGeom helpGeom(const HelpModel& m) {
    HelpGeom g;
    g.card = {60, 44, layout::SCREEN_W - 120, layout::SCREEN_H - 88};
    const float pad = 28.0f, top = g.card.y + 84.0f, bottom = g.card.bottom() - 70.0f;
    g.stage = {g.card.x + pad, top, 380.0f, bottom - top};
    g.list = {g.stage.right() + pad, top, g.card.right() - pad - (g.stage.right() + pad), bottom - top};
    // Tabs, right-aligned in the header.
    float x = g.card.right() - pad;
    g.tabs.assign(m.tabs.size(), Rect{0, 0, 0, 0});
    for (int i = (int)m.tabs.size() - 1; i >= 0; i--) {
        const float w = textSize(TEXT_S, m.tabs[(size_t)i]).x + 30.0f;
        x -= w;
        g.tabs[(size_t)i] = {x, g.card.y + 20.0f, w, 34.0f};
        x -= 8.0f;
    }
    float y = 0;
    for (const HelpRow& r : m.rows) {
        const float h = r.header ? 46.0f : 36.0f;
        g.rowY.push_back(y);
        g.rowH.push_back(h);
        y += h;
    }
    g.contentH = y + 8.0f;
    if (m.cursor >= 0 && m.cursor < (int)m.rows.size()) {
        const float centre = g.rowY[(size_t)m.cursor] + g.rowH[(size_t)m.cursor] * 0.5f;
        g.scroll = std::max(0.0f, std::min(centre - g.list.h * 0.5f, g.contentH - g.list.h));
    }
    return g;
}

} // namespace

HelpHit hitTestHelp(const HelpModel& m, float x, float y) {
    HelpHit hit;
    const HelpGeom g = helpGeom(m);
    if (!g.card.contains(x, y)) {
        hit.kind = HelpHit::Close;
        return hit;
    }
    for (size_t i = 0; i < g.tabs.size(); i++)
        if (g.tabs[i].contains(x, y)) {
            hit.kind = HelpHit::Tab;
            hit.index = (int)i;
            return hit;
        }
    if (g.list.contains(x, y)) {
        for (size_t i = 0; i < m.rows.size(); i++) {
            const float ry = g.list.y + g.rowY[i] - g.scroll;
            if (!m.rows[i].header && y >= ry && y < ry + g.rowH[i]) {
                hit.kind = HelpHit::Row;
                hit.index = (int)i;
                return hit;
            }
        }
    }
    return hit;
}

void setModelDir(const std::string& dir) { g_modelDir = dir; }
void setModelBackend(std::function<uint64_t(const uint8_t*, int, int)> upload, std::function<void(uint64_t)> release) {
    g_modelUpload = std::move(upload);
    g_modelRelease = std::move(release);
}
void resetHelpModels() {
    for (ModelSlot& s : g_slots) {
        if (s.worker.joinable()) s.worker.join();
        if (g_modelRelease) for (uint64_t t : s.tex) if (t) g_modelRelease(t);
        s.tex.clear();
        s.model = Model3D();
        s.err.clear();
        s.state.store(ModelSlot::Idle);
    }
}

void drawHelp(const HelpModel& m) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    const HelpGeom g = helpGeom(m);
    const Rect& c = g.card;

    // Everything behind is dimmed; in Liquid Glass mode a full-screen sheet frosts it first.
    if (g_glass) glassSurface(fg, 0, 0, W, H, 0.0f, GK::Overlay, true, 0.55f);
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, g_glass ? 0x90 : 0xC8));

    // The card.
    if (g_glass) {
        glassSurface(fg, c.x, c.y, c.right(), c.bottom(), gr(22.0f), GK::Panel, true);
        // A faint tint inside the glass so the text stays readable over whatever is behind it.
        fg->AddRectFilled(ImVec2(c.x + 2, c.y + 2), ImVec2(c.right() - 2, c.bottom() - 2), bgTint(col::BG, 0.40f), gr(22.0f));
    } else {
        fg->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.right(), c.bottom()), col::PANEL, 22.0f);
    }
    if (g_glass) glassBand(fg, c.x, c.y, c.right(), c.bottom(), gr(22.0f), c.y, c.y + 6.0f, accentMain());
    else fg->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.right(), c.y + 6), accentMain(), 22.0f, ImDrawFlags_RoundCornersTop);

    // Header: title, controller tabs.
    fg->AddText(font(), TEXT_L, ImVec2(c.x + 28, c.y + 18), col::TEXT, "Help");
    for (size_t i = 0; i < g.tabs.size(); i++) {
        const Rect& t = g.tabs[i];
        const bool sel = ((int)i == m.tab);
        if (g_glass) glassSurface(fg, t.x, t.y, t.right(), t.bottom(), t.h * 0.5f, sel ? GK::ButtonHot : GK::Button, true);
        else fg->AddRectFilled(ImVec2(t.x, t.y), ImVec2(t.right(), t.bottom()), sel ? accentDeep() : col::PANEL_HI, t.h * 0.5f);
        const ImVec2 ts = textSize(TEXT_S, m.tabs[i]);
        fg->AddText(font(), TEXT_S, ImVec2(t.x + (t.w - ts.x) * 0.5f, t.y + (t.h - ts.y) * 0.5f), sel ? col::WHITE : col::MUTED,
                    m.tabs[i].c_str());
    }

    // Stage: the rotating model, then its name and a note.
    fg->AddRectFilled(ImVec2(g.stage.x, g.stage.y), ImVec2(g.stage.right(), g.stage.bottom()), bgTint(col::BG, 0.45f), 18.0f);
    fg->AddRect(ImVec2(g.stage.x, g.stage.y), ImVec2(g.stage.right(), g.stage.bottom()), bgTint(col::TEXT, 0.10f), 18.0f, 0, 1.0f);
    {
        ModelSlot& slot = pollModel(m.tab);
        const float cx = g.stage.x + g.stage.w * 0.5f, cy = g.stage.y + g.stage.h * 0.40f;
        const int st = slot.state.load();
        if (st == ModelSlot::Ready) {
            const float hx = std::hypot(slot.model.ext[0], slot.model.ext[2]), hy = slot.model.ext[1]; // widest while turning
            const float scale = std::min(g.stage.w * 0.92f / (hx * 2.0f), g.stage.h * 0.62f / (hy * 2.0f));
            fg->PushClipRect(ImVec2(g.stage.x, g.stage.y), ImVec2(g.stage.right(), g.stage.bottom()), true);
            drawLoadedModel(fg, slot, cx, cy, scale, m.frame);
            fg->PopClipRect();
        } else {
            // No picture: say why. (Loading takes a moment the first time; Missing = no file on the SD card.)
            const bool missing = (st == ModelSlot::Missing), failed = (st == ModelSlot::Failed);
            const char* title = missing ? "Missing model" : (failed ? "Can't load model" : "Loading model...");
            std::string detail = missing ? std::string(help::kModelDirShown) + help::modelFile(help::controllerAt(m.tab))
                                         : (failed ? slot.err : std::string());
            const ImVec2 ts = textSize(TEXT_L, title);
            fg->AddText(font(), TEXT_L, ImVec2(cx - ts.x * 0.5f, cy - ts.y), missing || failed ? col::TEXT : col::MUTED, title);
            if (!detail.empty()) {
                detail = fitText(font(), TEXT_S, detail, g.stage.w - 36.0f);
                const ImVec2 ds = textSize(TEXT_S, detail);
                fg->AddText(font(), TEXT_S, ImVec2(cx - ds.x * 0.5f, cy + 8.0f), col::MUTED, detail.c_str());
            }
        }
        const ImVec2 ns = textSize(TEXT_L, m.name);
        fg->AddText(font(), TEXT_L, ImVec2(g.stage.x + (g.stage.w - ns.x) * 0.5f, g.stage.bottom() - 118.0f), col::TEXT, m.name.c_str());
        fg->AddText(font(), TEXT_S, ImVec2(g.stage.x + 18, g.stage.bottom() - 74.0f), col::MUTED, m.note.c_str(), nullptr, g.stage.w - 36.0f);
    }

    // The list of buttons, scrolled so the selected row stays in view.
    fg->PushClipRect(ImVec2(g.list.x, g.list.y), ImVec2(g.list.right(), g.list.bottom()), true);
    const float chipX = g.list.x + 6.0f, actionX = g.list.x + 250.0f;
    for (size_t i = 0; i < m.rows.size(); i++) {
        const HelpRow& r = m.rows[i];
        const float y = g.list.y + g.rowY[i] - g.scroll, h = g.rowH[i];
        if (y + h < g.list.y || y > g.list.bottom()) continue;
        if (r.header) {
            fg->AddText(font(), TEXT_M, ImVec2(chipX, y + 14.0f), accentMain(), r.action.c_str());
            fg->AddLine(ImVec2(chipX, y + h - 2.0f), ImVec2(g.list.right() - 14.0f, y + h - 2.0f), bgTint(col::TEXT, 0.12f), 1.0f);
            continue;
        }
        const bool sel = ((int)i == m.cursor);
        if (sel) {
            if (g_glass) glassSurface(fg, g.list.x, y + 1, g.list.right() - 12.0f, y + h - 1, gr(10.0f), GK::RowFocus, false);
            else fg->AddRectFilled(ImVec2(g.list.x, y + 1), ImVec2(g.list.right() - 12.0f, y + h - 1), col::PANEL_HI, 10.0f);
        }
        const float cw = std::max(34.0f, textSize(TEXT_S, r.button).x + 22.0f), ch = 26.0f, cy = y + (h - ch) * 0.5f;
        fg->AddRectFilled(ImVec2(chipX + 6, cy), ImVec2(chipX + 6 + cw, cy + ch), sel ? accentDeep() : col::PANEL_HI, ch * 0.5f);
        const ImVec2 bs = textSize(TEXT_S, r.button);
        fg->AddText(font(), TEXT_S, ImVec2(chipX + 6 + (cw - bs.x) * 0.5f, cy + (ch - bs.y) * 0.5f), sel ? col::WHITE : col::TEXT,
                    r.button.c_str());
        fg->AddText(font(), TEXT_S, ImVec2(actionX, y + (h - textSize(TEXT_S, r.action).y) * 0.5f), sel ? col::TEXT : col::MUTED,
                    fitText(font(), TEXT_S, r.action, g.list.right() - actionX - 16.0f).c_str());
    }
    fg->PopClipRect();
    if (g.contentH > g.list.h) { // scrollbar
        const float trackX = g.list.right() - 6.0f, th = std::max(30.0f, g.list.h * g.list.h / g.contentH);
        const float ty = g.list.y + (g.list.h - th) * (g.scroll / std::max(1.0f, g.contentH - g.list.h));
        fg->AddRectFilled(ImVec2(trackX, g.list.y), ImVec2(trackX + 4, g.list.bottom()), col::TRACK, 2.0f);
        fg->AddRectFilled(ImVec2(trackX, ty), ImVec2(trackX + 4, ty + th), col::MUTED, 2.0f);
    }

    // Button hints along the bottom of the card.
    float hx = c.x + 28.0f;
    const float rr = 14.0f, hy = c.bottom() - 52.0f;
    for (const Hint& hint : m.hints) {
        const float bw = std::max(2 * rr, textSize(TEXT_S, hint.button).x + 14.0f);
        if (g_glass) glassSurface(fg, hx, hy, hx + bw, hy + 2 * rr, rr, GK::Button, true);
        else fg->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + bw, hy + 2 * rr), col::PANEL_HI, rr);
        const ImVec2 bs = textSize(TEXT_S, hint.button);
        fg->AddText(font(), TEXT_S, ImVec2(hx + (bw - bs.x) * 0.5f, hy + rr - bs.y * 0.5f), col::TEXT, hint.button.c_str());
        hx += bw + 8;
        fg->AddText(font(), TEXT_S, ImVec2(hx, hy + rr - bs.y * 0.5f), col::MUTED, hint.label.c_str());
        hx += textSize(TEXT_S, hint.label).x + 26;
    }
}

void drawUpNext(const UpNextModel& m) {
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);
    const float artW = 300, artH = 420, ax = 150, ay = 150;
    if (m.art) {
        layout::Fit f = layout::fitInside(ax, ay, artW, artH, m.artAspect);
        dl()->AddImageRounded(ImTextureRef((ImTextureID)m.art), ImVec2(f.x, f.y), ImVec2(f.x + f.w, f.y + f.h),
                              ImVec2(0, 0), ImVec2(1, 1), col::WHITE, 14.0f);
    } else {
        gradientRoundRect(ax, ay + 60, ax + artW, ay + 60 + artW * 0.75f, 16.0f, accentSecond(), accentDeep());
        drawIcon(Icon::Episode, ax + artW * 0.5f, ay + 60 + artW * 0.375f, 110.0f, IM_COL32(255, 255, 255, 0xD0));
    }
    const float tx = ax + artW + 70, tw = layout::SCREEN_W - 110 - tx;
    text(TEXT_S, tx, 200, accentMain(), "UP NEXT");
    if (!m.showName.empty()) text(TEXT_M, tx, 236, col::MUTED, m.showName, tw);
    dl()->AddText(font(), 40.0f, ImVec2(tx, 276), col::TEXT, m.episodeTitle.c_str(), nullptr, tw);
    text(TEXT_S, tx, 390, col::MUTED, m.detail, tw);

    // Countdown ring with the seconds in the middle.
    const ImVec2 c(tx + 60, 520);
    const float r = 50;
    dl()->AddCircle(c, r, col::TRACK, 48, 8.0f);
    float frac = m.totalSeconds > 0 ? (float)std::max(0.0, std::min(1.0, m.secondsLeft / m.totalSeconds)) : 0.0f;
    if (frac > 0) {
        dl()->PathArcTo(c, r, -1.5708f, -1.5708f + 6.28318f * frac, 48);
        dl()->PathStroke(accentMain(), 0, 8.0f);
    }
    std::string secs = std::to_string((int)std::ceil(std::max(0.0, m.secondsLeft)));
    ImVec2 ss = textSize(TEXT_L + 6, secs);
    text(TEXT_L + 6, c.x - ss.x * 0.5f, c.y - ss.y * 0.5f, col::TEXT, secs);
    text(TEXT_M, c.x + r + 30, c.y - TEXT_M * 0.5f, col::MUTED, "Starting automatically");
    drawHints(m.hints, tx, layout::FOOTER_Y);
}

void applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.FrameRounding = 10.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(col::BG);
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(col::TEXT);
    c[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(col::PANEL);
}

} // namespace ui

// The "Who's watching?" screen, built from the helpers above (see profiles_ui.h).
#include "profiles_ui.inc"
