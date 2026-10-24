#include "app_ui.h"
#include "glass_core.h"
#include "m3e_core.h"
#include "theme_core.h"
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
// The selected palette is separate from what is drawn: the premade accent or the custom palette is the BASE;
// g_accent (above) is the base after the artwork blend and the readability pass -- the one place every renderer
// reads the three theme colours from (accentMain/Deep/Second, g_m3c, the glass pack).
static m3e::Rgb8 unpack(ImU32 c) { return {(uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF), (uint8_t)((c >> 16) & 0xFF)}; }
static ImU32 pack(m3e::Rgb8 c) { return IM_COL32(c.r, c.g, c.b, 0xFF); }
static bool g_custom = false;                // "Custom Palette" is the selected palette
static theme::Custom g_customPal;
static bool g_artPref = false;               // Settings: artwork-reactive accent (M3E only)
static bool g_artContext = false;            // a music / video player is on screen
static bool g_artHave = false;               // the playing item gave a usable colour
static float g_artGoal[3] = {0, 0, 0}, g_artNow[3] = {0, 0, 0}; // 0..255
static float g_artMix = 0.0f;                // 0 = the selected palette, 1 = the artwork colour
static double g_themeTime = 0.0, g_lastTick = -1.0;
static Background g_background = Background::Off;
static bool g_snow = false;
static bool g_light = false; // false = dark palette (default), true = light

// --- Material 3 Expressive state (see m3e_core.h, m3e_draw.inc) ---
// In M3E mode the accent roles below are served from a palette derived from the SAME user-selected accent
// (g_accent), so every screen that draws with accentMain()/accentDeep()/accentSecond() follows the theme:
//   main   = primary             (text in the accent colour, progress, switches that are on)
//   deep   = primaryContainer    (focused rows and buttons: a tonal fill, not a saturated one)
//   second = tertiary            (the second colour of gradients)
// and onMain()/onFocus() give the matching foreground. With M3E off they are the old values and white.
static bool g_m3e = false;
// Button shapes (Settings > Customization > Customize shapes; M3E only -- see m3e_draw.inc). Default is each
// button's own look; the rest are Pill, Rounded, Square and the scalloped Clover, Cookie and Flower (round buttons).
enum { SH_DEFAULT, SH_PILL, SH_ROUNDED, SH_SQUARE, SH_CLOVER, SH_COOKIE, SH_FLOWER, SH_COUNT };
static float g_btnSize[4][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}}; // width, height scale per button kind
static int g_btnShape[4] = {0, 0, 0, 0}; // per button kind: play/pause, seek & skip, toggles, dialog & login
struct M3Colours {
    ImU32 primary, onPrimary, primaryContainer, onPrimaryContainer, secondaryContainer, onSecondaryContainer,
          tertiary, tertiaryContainer, onTertiaryContainer, errorContainer, onErrorContainer;
};
static M3Colours g_m3c = {};

static ImU32 accentMain() { return g_m3e ? g_m3c.primary : g_accent.main; }
static ImU32 accentDeep() { return g_m3e ? g_m3c.primaryContainer : g_accent.deep; }
static ImU32 accentSecond() { return g_m3e ? g_m3c.tertiary : g_accent.second; }
// Text on an accent fill: white on every premade accent (as ever); a custom palette may be pale, so there it is
// whichever of white / near-black reads better.
static ImU32 onMain() {
    if (g_m3e) return g_m3c.onPrimary;
    return g_custom ? pack(theme::onColour(unpack(g_accent.main))) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
}
// Text and glyphs on the translucent overlay surfaces of the video HUD: white as ever, except M3E whose
// overlays follow its light/dark surface.
static uint64_t g_extraPasses = 0;
void noteExtraPass() { g_extraPasses++; }

static ImU32 onOverlay() { return g_m3e ? col::TEXT : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF); }
static ImU32 onFocus() {
    if (g_m3e) return g_m3c.onPrimaryContainer;
    return g_custom ? pack(theme::onColour(unpack(g_accent.deep))) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
}

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


void applyTheme(); // (defined at the end of the file)

// Sets the neutral half of the palette (col::BG/SIDEBAR/PANEL/PANEL_HI/TEXT/MUTED/DIM/TRACK) and, in M3E
// mode, the accent roles. Plain and Liquid Glass: the dark defaults or the light set, exactly as before.
// M3E: tonal surfaces derived from the accent, so every existing col::* user is coordinated with it for
// free. Called whenever the accent (Rainbow: every frame), the light flag or the style changes.
static void refreshPalette() {
    if (g_m3e) {
        const m3e::Rgb8 seed = unpack(g_accent.second);
        const m3e::Palette p = m3e::derive(unpack(g_accent.main), g_light, (g_custom || g_artMix > 0.0f) ? &seed : nullptr);
        col::BG       = pack(p.surface);
        col::SIDEBAR  = pack(p.surfaceLow);
        col::PANEL    = pack(p.surfaceContainer);
        col::PANEL_HI = pack(p.surfaceHigh);
        col::TEXT     = pack(p.onSurface);
        col::MUTED    = pack(p.onSurfaceVariant);
        col::DIM      = pack(p.outline);
        col::TRACK    = pack(p.surfaceHighest);
        g_m3c = {pack(p.primary), pack(p.onPrimary), pack(p.primaryContainer), pack(p.onPrimaryContainer),
                 pack(p.secondaryContainer), pack(p.onSecondaryContainer), pack(p.tertiary),
                 pack(p.tertiaryContainer), pack(p.onTertiaryContainer), pack(p.errorContainer),
                 pack(p.onErrorContainer)};
    } else if (g_light) {
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

static AccentColours premadeColours(Accent a, double time) {
    switch (a) {
        case Accent::Purple:  return {rgb(0xAA5CC3), rgb(0x7E3F99), rgb(0x00A4DC)};
        case Accent::Green:   return {rgb(0x2ECC71), rgb(0x1E8C4E), rgb(0x00A4DC)};
        case Accent::Orange:  return {rgb(0xFF9F43), rgb(0xC7702A), rgb(0xE84393)};
        case Accent::Pink:    return {rgb(0xFF6BB5), rgb(0xC2447F), rgb(0x7E57C2)};
        // Four more colourways, same {main, deep, secondary} shape as the originals above (secondary is only
        // used for gradients/ambient).
        case Accent::Teal:    return {rgb(0x1ABC9C), rgb(0x128F76), rgb(0x3498DB)};
        case Accent::Crimson: return {rgb(0xE74C3C), rgb(0xB03A2E), rgb(0xF39C12)};
        case Accent::Amber:   return {rgb(0xF1C40F), rgb(0xB7950B), rgb(0xE67E22)};
        case Accent::Indigo:  return {rgb(0x5C6BC0), rgb(0x3F4A8C), rgb(0x26C6DA)};
        case Accent::Rainbow: {
            const float h = (float)std::fmod(time * 0.06, 1.0);
            return {hsv(h, 0.72f, 0.95f), hsv(h, 0.78f, 0.66f), hsv(h + 0.33f, 0.6f, 0.9f)};
        }
        default:              return {rgb(0x00A4DC), rgb(0x0B7FB0), rgb(0xAA5CC3)};
    }
}

// The one place g_accent is computed: base palette (premade or custom) -> artwork blend (M3E, while playing)
// -> readability (a custom colour that would vanish into the background is lifted) -> neutral palette.
static void rebuildAccent() {
    AccentColours b = g_custom ? AccentColours{pack(theme::rolesOf(g_customPal).main), pack(theme::rolesOf(g_customPal).deep),
                                               pack(theme::rolesOf(g_customPal).second)}
                               : premadeColours(g_accentStyle, g_themeTime);
    if (g_m3e && g_artMix > 0.0f) {
        const m3e::Rgb8 art{(uint8_t)(g_artNow[0] + 0.5f), (uint8_t)(g_artNow[1] + 0.5f), (uint8_t)(g_artNow[2] + 0.5f)};
        b.main = lerpColor(b.main, pack(art), g_artMix);
        b.deep = lerpColor(b.deep, pack(theme::autoDeep(art)), g_artMix);
        b.second = lerpColor(b.second, pack(theme::autoSecond(art)), g_artMix);
    }
    if (g_m3e) {
        g_accent = b; // (M3E derives tonal roles; they are readable by construction)
        refreshPalette();
    } else {
        g_accent = b;
        refreshPalette(); // the neutral half first: custom colours are checked against its background
        if (g_custom) {
            g_accent.main = pack(theme::ensureContrast(unpack(b.main), unpack(col::BG), 3.0f));
            g_accent.second = pack(theme::ensureContrast(unpack(b.second), unpack(col::BG), 2.0f));
        }
    }
}

// Button shapes (M3E). Kind 0 = play / pause, 1 = seek & skip, 2 = toggles (shuffle, repeat, lyrics), 3 = dialog and
// login buttons (wide: no scalloped shapes).
static void syncPlayerRow();
void setButtonShapes(const int shapes[4]) {
    for (int i = 0; i < 4; i++) {
        const int v = shapes[i];
        g_btnShape[i] = (v >= 0 && v < buttonShapeCount(i)) ? v : 0;
    }
    syncPlayerRow();
}
int buttonSizeMin() { return 60; }
// (A dialog button's height stops where its neighbours' 12 px gap does: taller would overlap the row above / below.)
int buttonSizeMax(int index) { return index == 7 ? 112 : 130; }
// The player's button row re-spaces itself around the sizes and shapes in use (layout.h npCustom): only in M3E,
// and only while something is customised, so everything else keeps the original layout exactly.
static void syncPlayerRow() {
    NpCustom& c = npCustom();
    bool custom = false;
    for (int k = 0; k < 4; k++) custom = custom || g_btnShape[k] != 0 || g_btnSize[k][0] != 1.0f || g_btnSize[k][1] != 1.0f;
    c.active = g_m3e && custom;
    for (int k = 0; k < 3; k++) {
        c.sw[k] = c.active ? g_btnSize[k][0] : 1.0f;
        c.sh[k] = c.active ? g_btnSize[k][1] : 1.0f;
    }
}
void setButtonSizes(const int sizes[8]) {
    for (int i = 0; i < 8; i++) {
        const int v = std::max(buttonSizeMin(), std::min(buttonSizeMax(i), sizes[i]));
        g_btnSize[i / 2][i % 2] = (float)v / 100.0f;
    }
    syncPlayerRow();
}
int buttonShapeCount(int kind) { return kind == 3 ? 4 : (int)SH_COUNT; }
const char* buttonShapeName(int shape) {
    static const char* names[] = {"Default", "Pill", "Rounded", "Square", "Clover", "Cookie", "Flower"};
    return (shape >= 0 && shape < (int)SH_COUNT) ? names[shape] : "";
}
const char* buttonKindName(int kind) {
    static const char* names[] = {"Play / pause", "Seek & skip", "Toggles", "Dialog buttons"};
    return (kind >= 0 && kind < 4) ? names[kind] : "";
}

// The colour a premade accent shows as in the Customization list (0xRRGGBB; bit 24 marks the rainbow chip).
uint32_t accentSwatchColour(Accent a) {
    if (a == Accent::Rainbow) return 0x01000000u;
    const ImU32 c = premadeColours(a, 0.0).main;
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

void setAccent(Accent a) {
    g_accentStyle = a;
    g_rainbow = (a == Accent::Rainbow);
    rebuildAccent();
    if (g_m3e) applyTheme(); // M3E derives its whole palette from the accent
}

// Custom palette: three colours (0xRRGGBB) and which of them is main / deep / second. `on` false goes back to the
// premade accent chosen with setAccent (which is kept all along).
void setCustomPalette(bool on, const uint32_t rgbColours[3], int arrange) {
    g_custom = on;
    for (int i = 0; i < 3; i++) {
        const uint32_t v = rgbColours[i];
        g_customPal.c[i] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    }
    g_customPal.arrange = (arrange >= 0 && arrange < theme::kArrangeCount) ? arrange : 0;
    rebuildAccent();
    if (g_m3e) applyTheme();
}
bool customPaletteActive() { return g_custom; }

// Artwork-reactive accent. The preference is the Settings switch (only M3E honours it); the context says whether a
// player is on screen; the colour is the playing item's dominant colour (valid = false: none, e.g. no artwork).
void setArtworkAccent(bool enabled) { g_artPref = enabled; }
void setArtworkContext(bool playing) { g_artContext = playing; }
void setArtworkColour(bool valid, uint32_t rgbColour) {
    g_artHave = valid;
    if (!valid) return; // (the blend fades back to the selected palette; the old colour stays for that fade)
    g_artGoal[0] = (float)((rgbColour >> 16) & 0xFF);
    g_artGoal[1] = (float)((rgbColour >> 8) & 0xFF);
    g_artGoal[2] = (float)(rgbColour & 0xFF);
    if (g_artMix <= 0.001f) for (int i = 0; i < 3; i++) g_artNow[i] = g_artGoal[i]; // appearing: no slide from a stale colour
}
bool artworkAccentActive() { return g_m3e && g_artPref && g_artContext && g_artHave; }
float artworkMix() { return g_artMix; }

void tickTheme(double time) {
    const double dt = g_lastTick < 0.0 ? 0.0 : std::min(0.1, std::max(0.0, time - g_lastTick));
    g_lastTick = time;
    bool dirty = false;
    if (g_rainbow && !g_custom) { g_themeTime = time; dirty = true; }
    // Artwork blend: eases in and out, and follows the colour of the next item smoothly. Costs nothing once settled.
    const float goal = artworkAccentActive() ? 1.0f : 0.0f;
    const float k = 1.0f - std::exp(-(float)dt * 4.5f);
    if (g_artMix != goal) {
        g_artMix += (goal - g_artMix) * k;
        if (std::fabs(goal - g_artMix) < 0.004f) g_artMix = goal;
        dirty = true;
    }
    if (g_artMix > 0.0f) {
        for (int i = 0; i < 3; i++) {
            const float d = g_artGoal[i] - g_artNow[i];
            if (std::fabs(d) > 0.4f) { g_artNow[i] += d * k; dirty = true; }
            else if (d != 0.0f) { g_artNow[i] = g_artGoal[i]; dirty = true; }
        }
    }
    if (dirty) rebuildAccent(); // (a few dozen flops; ImGui's own style colours are not touched per frame)
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
    rebuildAccent();
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

// M3E has weight contrast (headlines and the focused label are heavier) but the console has one font file, so
// heavier text is the same glyphs drawn twice, 0.7 px apart. `g_bold` marks an emphasised run (the focused
// row's name); headline sizes are heavier automatically.
static bool g_bold = false;
static bool boldText(float size) { return g_m3e && (g_bold || size >= TEXT_L - 0.5f); }
static void text(float size, float x, float y, ImU32 color, const std::string& s, float maxWidth = 0.0f) {
    std::string t = maxWidth > 0.0f ? fitText(font(), size, s, maxWidth) : s;
    if (boldText(size)) dl()->AddText(font(), size, ImVec2(x + std::max(0.7f, size * 0.022f), y), color, t.c_str());
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
static uint64_t g_snapBlurTex = 0; // the snapshot downsampled (see Gfx): what the refraction samples when the frost is on
static int g_glassBlurLevel = 1;   // 0 sharp, 1 half, 2 quarter, 3 eighth resolution
void setGlassBlur(int level) { g_glassBlurLevel = level < 0 ? 0 : (level > 3 ? 3 : level); }
void setGlassBlurTexture(uint64_t texture) { g_snapBlurTex = texture; }
int glassBlurLevel() { return g_glassBlurLevel; }
static bool refractionOn() { return g_glass && g_snapTex && g_snapCb; }
bool glassRefractionActive() { return refractionOn(); }
int glassSnapshotsThisFrame() { return g_snapPlanner.snapshotsThisFrame(); }

// The three looks are mutually exclusive: turning one on turns the other off (Plain = neither flag).
void setGlass(bool on) {
    g_glass = on;
    if (on && g_m3e) { g_m3e = false; rebuildAccent(); applyTheme(); }
}
bool glassEnabled() { return g_glass; }
void setExpressive(bool on) {
    if (on) g_glass = false;
    if (g_m3e != on) { g_m3e = on; rebuildAccent(); applyTheme(); }
    syncPlayerRow();
}
bool expressiveEnabled() { return g_m3e; }
void setStyle(Style st) {
    setGlass(st == Style::LiquidGlass);
    setExpressive(st == Style::Expressive);
    syncPlayerRow();
}
Style style() { return g_m3e ? Style::Expressive : (g_glass ? Style::LiquidGlass : Style::Plain); }
const char* styleName(Style st) {
    switch (st) {
        case Style::LiquidGlass: return "Liquid Glass";
        case Style::Expressive:  return "Material 3 Expressive";
        default:                 return "Plain";
    }
}
bool glassBlurAvailable() { return glass::backdrop().active(); }

static glass::Rgb toRgb(ImU32 c) {
    glass::Rgb r;
    r.r = (float)(c & 0xFF) / 255.0f;
    r.g = (float)((c >> 8) & 0xFF) / 255.0f;
    r.b = (float)((c >> 16) & 0xFF) / 255.0f;
    return r;
}

void glassNoteArtwork(uint64_t texture, const uint8_t* rgba, int width, int height) {
    if (!g_glass || !texture || !rgba || width <= 0 || height <= 0) return; // (M3E does not tint by artwork)
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
    if (!g_glass && !g_m3e) return; // Plain: nothing runs. M3E shares the (calm, static) blur for its frosted dialogs.
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
    p.animated = g_glass; // M3E's frost is still: regenerated only when the accent or light/dark changes
    if (g_m3e) {
        // M3E: the field is the theme's own surface colour with the primary tone; the accent is quantised
        // (5 bits a channel) so Rainbow does not restart the blur every frame.
        auto q = [](float v) { return std::floor(v * 31.0f + 0.5f) / 31.0f; };
        p.base = toRgb(col::BG);
        p.accent = toRgb(g_m3c.primaryContainer);
        p.accent2 = toRgb(g_m3c.tertiaryContainer);
        p.accent.r = q(p.accent.r); p.accent.g = q(p.accent.g); p.accent.b = q(p.accent.b);
        p.accent2.r = q(p.accent2.r); p.accent2.g = q(p.accent2.g); p.accent2.b = q(p.accent2.b);
        p.artStrength = 0.0f;
    }
    bd.update(time, p, videoActive);
    g_glassBlend = bd.blend(time);
}

// Dialog is M3E's frosted card over dimmed content (Liquid Glass draws such cards as Panel).
enum class GK { Panel, Row, RowFocus, RowFocusDim, Overlay, Button, ButtonHot, ButtonPressed, ButtonDisabled, Dialog };

// Corner radius in glass mode: rounder, like Wii U tiles.
static float gr(float r) { return g_glass ? r * 1.45f : (g_m3e ? r * 1.7f : r); } // (M3E: larger shape tokens)

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
        case GK::Dialog: tintA = 0x34; sheenA = 0x22; break;
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
        glassBackdropLayer(d, (g_glassBlurLevel > 0 && g_snapBlurTex) ? g_snapBlurTex : g_snapTex, x0, y0, x1, y1, rad, A(255), true);
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

// "A theme draws the surfaces" (Liquid Glass or M3E) as opposed to Plain's flat colours. Every themed
// surface goes through themedSurface(), which hands it to the active look; the screens above that
// only say WHAT the surface is (GK) and where it sits.
static bool themed() { return g_glass || g_m3e; }
static void m3eSurface(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, GK k, bool blur, float alpha);
static void m3eBackdropShapes();
static void m3eButton(const Rect& r, ImU32 fill, bool focus, ImDrawList* onList = nullptr);
static void drawFabs(const BrowserModel& m);
static void drawSortMenu(const BrowserModel& m);
static void drawRowAdd(const Rect& row, bool hot);
static void m3eTransportButton(ImDrawList* d, float cx, float cy, float halfW, float halfH, float sw, float sh, ImU32 fill, bool pressed, float radFrac, int slot, int shape);
static void m3eFillShape(ImDrawList* d, float cx, float cy, float hw, float hh, int shape, float R, float press, ImU32 fill,
                         float sw = 1.0f, float sh = 1.0f);
static float shapeCorner(int shape);
static void m3eCookie(ImDrawList* d, float cx, float cy, float r, int lobes, float amp, float rot, ImU32 col);
static void m3eLoader(ImDrawList* d, float cx, float cy, float r, ImU32 col, double time);
struct M3eFxScope { // icon animation scope; begin() before drawing an icon, end() after (m3e_draw.inc)
    ImDrawList* d = nullptr;
    int v0 = 0;
    float cx = 0, cy = 0;
    m3e::IconFx fx;
    bool on = false;
    void begin(uint32_t key, bool active, const m3e::IconFxStyle& st, float size, float x, float y, uint32_t tag = 0);
    void end();
};
static void m3eGroupMotion(int idx, int rank, int pressedRank, float& push, float& grow);
static float m3eDt();
static double g_pulseTime = -10.0;
// Moves every vertex drawn since construction by dy: one row's worth of geometry (a few hundred vertices at
// most), only while the press bounce is running.
struct M3eRowShift {
    ImDrawList* d = dl();
    int v0 = d->VtxBuffer.Size;
    void end(float dy) {
        if (std::fabs(dy) < 0.05f) return;
        for (int i = v0; i < d->VtxBuffer.Size; i++) d->VtxBuffer.Data[i].pos.y += dy;
    }
};
static float m3eRowPress();
static float m3eRowPush(int rank, float p);
static uint32_t g_pulseCount = 0;
void pulse() { g_pulseTime = ImGui::GetTime(); g_pulseCount++; }
static void m3eCookieImage(ImDrawList* d, ImTextureID tex, float cx, float cy, float r, int lobes, float amp, float rot, ImVec2 uv0, ImVec2 uv1, ImU32 tint);
static void m3eProgressLine(ImDrawList* d, float x, float y, float w, float h, float frac, bool moving);
static void m3eEntranceBegin(ImDrawList* d, uint32_t key, int& vtxStart, float& s);
static void m3eEntranceEnd(ImDrawList* d, int vtxStart, float s, float cx, float cy);
static void m3eDialogScrim(ImDrawList* d, float amt, int& vtxStart);
static void m3eMessageBadge(float x, float y, bool busy, bool error);
static void themedSurface(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, GK k, bool blur,
                          float alpha = 1.0f) {
    if (g_m3e) m3eSurface(d, x0, y0, x1, y1, rad, k, blur, alpha);
    else glassSurface(d, x0, y0, x1, y1, rad, k, blur, alpha);
}

static void glassCircle(ImDrawList* d, float cx, float cy, float r, GK k, bool blur) {
    themedSurface(d, cx - r, cy - r, cx + r, cy + r, r, k, blur);
}

// A panel-ish rounded rectangle: the flat colour it always was, or the active theme's surface.
static void surfaceRect(const Rect& r, ImU32 flat, float rounding, GK kind, bool blur = true) {
    if (!themed()) {
        rect(r, flat, rounding);
        return;
    }
    themedSurface(dl(), r.x, r.y, r.right(), r.bottom(), gr(rounding), kind, blur);
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
    if (g_glass) { // (M3E keeps a flat near-black / near-white surface: no accent-coloured glass field)
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
    if (g_m3e && g_background == Background::Off) m3eBackdropShapes(); // (a chosen effect replaces the decoration)
    drawBackgroundEffect();
    drawBackdropLayers(); // fading in or out over the normal background (no-op without a backdrop)
}

// --- icons (simple vector shapes, drawn in a size x size box) ---

static void drawIconRaw(Icon icon, float cx, float cy, float s, ImU32 c) {
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

// M3E icons play a short animation when they come on (see m3e_draw.inc); `g_iconActive` says whether the icon
// being drawn is the focused / selected one. Plain and Liquid Glass draw the icon as before.
static bool g_iconActive = false;
static bool g_iconHoldless = false;
static uint32_t g_iconTag = 0; // bumped by the caller for every event (a press) that should replay the icon's animation
static void m3eIconAnimated(Icon icon, float cx, float cy, float s, ImU32 c, bool active);
static void drawIcon(Icon icon, float cx, float cy, float s, ImU32 c) {
    if (g_m3e) m3eIconAnimated(icon, cx, cy, s, c, g_iconActive);
    else drawIconRaw(icon, cx, cy, s, c);
}

// --- shared chrome ---

static void drawHints(const std::vector<Hint>& hints, float x, float y) {
    const float r = 14.0f, right = layout::SCREEN_W - 20.0f, rowStep = 34.0f;
    auto widthOf = [&](const Hint& h) {
        return std::max(2 * r, textSize(TEXT_S, h.button).x + 14.0f) + 8.0f + textSize(TEXT_S, h.label).x + 26.0f;
    };
    // Legends that do not fit in one row wrap onto a second one (the first row moves up to make room), instead of
    // running off the screen.
    int rows = 1;
    float probe = x;
    for (const Hint& hint : hints) {
        const float w = widthOf(hint);
        if (probe + w - 26.0f > right && probe > x) { rows++; probe = x; }
        probe += w;
    }
    float rowY = y - (rows - 1) * 18.0f;
    const float x0 = x;
    for (const Hint& hint : hints) {
        float bw = std::max(2 * r, textSize(TEXT_S, hint.button).x + 14.0f);
        if (x + widthOf(hint) - 26.0f > right && x > x0) { x = x0; rowY += rowStep; }
        if (themed()) themedSurface(dl(), x, rowY, x + bw, rowY + 2 * r, r, GK::Button, true);
        else dl()->AddRectFilled(ImVec2(x, rowY), ImVec2(x + bw, rowY + 2 * r), col::PANEL_HI, r);
        ImVec2 bs = textSize(TEXT_S, hint.button);
        text(TEXT_S, x + (bw - bs.x) * 0.5f, rowY + r - bs.y * 0.5f, col::TEXT, hint.button);
        x += bw + 8.0f;
        text(TEXT_S, x, rowY + r - bs.y * 0.5f, col::MUTED, hint.label);
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

static void drawProgress(float x, float y, float w, float h, double position, double duration, bool live,
                         bool moving = true) {
    if (g_m3e && duration > 0.0) { // M3E: a wavy played part (it flattens when paused), a handle, a calm track
        m3eProgressLine(dl(), x, y, w, h, (float)std::min(1.0, std::max(0.0, position / duration)), moving);
        return;
    }
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
                   onMain(), n.c_str());
    } else if (e.watched) {
        ImVec2 c(corner.x, corner.y + 4);
        d->AddCircleFilled(c, 10.0f, accentMain(), 16);
        d->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x - 1.5f, c.y + 4), onMain(), 2.4f);
        d->AddLine(ImVec2(c.x - 1.5f, c.y + 4), ImVec2(c.x + 5, c.y - 4), onMain(), 2.4f);
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
        if (g_m3e) {
            // M3E: a big slowly turning cookie in the primary container instead of a glass bubble.
            m3eCookie(fg, cx, cy, R, 12, 0.07f, t * 0.25f, accentDeep());
        } else if (g_glass) {
            glassCircle(fg, cx, cy, R, GK::Overlay, true);
            // A faint tint inside the glass so the text stays legible over busy rows behind it.
            fg->AddCircleFilled(ImVec2(cx, cy), R * 0.97f, bgTint(col::BG, 0.30f), 72);
        } else {
            fg->AddCircleFilled(ImVec2(cx, cy), R, col::PANEL, 72);
            fg->AddCircle(ImVec2(cx, cy), R, accentMain(), 72, 3.0f);
        }
        // A soft highlight on the upper left, like light on a soap bubble.
        if (!g_m3e) { // (the soap-bubble highlight; a cookie is not shiny)
            fg->AddCircleFilled(ImVec2(cx - R * 0.46f, cy - R * 0.50f), R * 0.10f, bgTint(col::WHITE, 0.16f), 24);
            fg->AddCircleFilled(ImVec2(cx - R * 0.46f, cy - R * 0.50f), R * 0.05f, bgTint(col::WHITE, 0.30f), 20);
        }
    }

    // Text fades in once the bubble has mostly arrived.
    const float ta = clamp01((t - 0.30f) / 0.40f);
    if (ta > 0.0f) {
        auto centred = [&](float size, float y, ImU32 color, const std::string& str) {
            ImVec2 ts = font()->CalcTextSizeA(size, 1e9f, 0.0f, str.c_str());
            fg->AddText(font(), size, ImVec2(cx - ts.x * 0.5f, y), color, str.c_str());
        };
        const ImU32 fgText = g_m3e ? g_m3c.onPrimaryContainer : col::TEXT; // on the cookie
        const ImU32 fgMuted = g_m3e ? g_m3c.onPrimaryContainer : col::MUTED;
        centred(TEXT_M, cy - 104.0f, bgTint(fgMuted, ta), "Time with Ufin");
        centred(76.0f, cy - 70.0f, bgTint(fgText, ta), g_celebBig);
        centred(TEXT_S, cy + 44.0f, bgTint(fgMuted, ta), g_celebSmall);
        if (t > 1.2f) centred(TEXT_S - 2, cy + 108.0f, bgTint(g_m3e ? fgMuted : col::DIM, clamp01((t - 1.2f) / 0.5f)), "Tap or press any button");
    }

    drawConfetti(fg, t, true); // in front of everything
}

// --- list-row widgets (Settings > Customization) ---

// Left edge of a widget (the row's name must stay left of it).
static float rowWidgetLeft(const ListEntry& e, const Rect& r) {
    switch (e.widget.kind) {
        case RowWidget::Kind::Swatches:
        case RowWidget::Kind::Shapes: {
            const int n = e.widget.count();
            return r.right() - 12.0f - (float)n * (layout::swatchSize(r, n) + 6.0f) - 8.0f;
        }
        case RowWidget::Kind::Slider:  return layout::rowSliderMinus(r).x - 8.0f;
        case RowWidget::Kind::Preview: return r.x + 330.0f;
        case RowWidget::Kind::ButtonPreview: return r.x + 290.0f;
        default:                       return r.right();
    }
}

// Pie-slice rainbow disc: the "Rainbow" accent chip.
static void rainbowDisc(ImDrawList* d, float cx, float cy, float rad, float alpha) {
    for (int i = 0; i < 6; i++) {
        const float a0 = (float)i / 6.0f * 6.2831853f, a1 = (float)(i + 1) / 6.0f * 6.2831853f;
        d->PathLineTo(ImVec2(cx, cy));
        d->PathArcTo(ImVec2(cx, cy), rad, a0, a1, 4);
        d->PathFillConvex(bgTint(hsv((float)i / 6.0f, 0.75f, 0.95f), alpha));
    }
}

static void drawRowWidget(const ListEntry& e, const Rect& r, bool hot) {
    const RowWidget& w = e.widget;
    ImDrawList* d = dl();
    const float a = e.dimmed ? 0.38f : 1.0f;
    const ImU32 ring = bgTint(hot ? onFocus() : col::TEXT, a);
    const float cy = r.y + r.h * 0.5f;
    if (w.kind == RowWidget::Kind::Swatches) {
        const int n = (int)w.colours.size();
        const float sz = layout::swatchSize(r, n);
        for (int i = 0; i < n; i++) {
            const Rect c = layout::rowSwatch(r, i, n);
            const float cx = c.x + c.w * 0.5f;
            const uint32_t v = w.colours[(size_t)i];
            if (v & 0x01000000u) rainbowDisc(d, cx, cy, sz * 0.5f, a);
            else d->AddCircleFilled(ImVec2(cx, cy), sz * 0.5f, bgTint(rgb(v & 0xFFFFFF), a), 24);
            if (i == w.selected) d->AddCircle(ImVec2(cx, cy), sz * 0.5f + 4.0f, ring, 28, 2.5f);
            if (w.numbered) {
                const char label[2] = {(char)('1' + i), 0};
                const m3e::Rgb8 bg{(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
                const ImVec2 ts = textSize(TEXT_S, label);
                text(TEXT_S, cx - ts.x * 0.5f, cy - TEXT_S * 0.5f - 1.0f, bgTint(pack(theme::onColour(bg)), a), label);
            }
        }
    } else if (w.kind == RowWidget::Kind::Shapes) {
        // One sample of each shape on offer, in the theme colour; the chosen one is ringed.
        const int n = w.count();
        const float sz = layout::swatchSize(r, n);
        for (int i = 0; i < n; i++) {
            const Rect c = layout::rowSwatch(r, i, n);
            const float cx = c.x + c.w * 0.5f;
            const int sh = w.shapes[(size_t)i];
            const float R = sh == SH_DEFAULT ? sz * 0.34f : std::min(sz * 0.5f, shapeCorner(sh) * sz);
            m3eFillShape(d, cx, cy, sz * 0.5f, sz * 0.5f, sh, R, 0.0f, bgTint(accentMain(), a));
            if (i == w.selected) d->AddCircle(ImVec2(cx, cy), sz * 0.5f + 4.0f, ring, 28, 2.5f);
        }
    } else if (w.kind == RowWidget::Kind::ButtonPreview && w.shapes.size() >= 4 && w.sizes.size() >= 8) {
        // The four kinds of button as the player and the dialogs draw them: the chosen shape at the chosen size.
        // (Same fills; the corners are the shapes' resting ones.)
        static const float baseHW[4] = {38.0f, 22.0f, 21.0f, 54.0f}, baseHH[4] = {22.0f, 22.0f, 21.0f, 17.0f};
        const float x0 = r.x + 290.0f + 56.0f;
        static const float off[4] = {0.0f, 118.0f, 190.0f, 292.0f};
        for (int k = 0; k < 4; k++) {
            const float sw = (float)w.sizes[(size_t)k * 2] / 100.0f, sh = (float)w.sizes[(size_t)k * 2 + 1] / 100.0f;
            const float hw = baseHW[k] * sw, hh = baseHH[k] * sh;
            const int shp = w.shapes[(size_t)k];
            const float R = shp == SH_DEFAULT ? (k == 3 ? std::min(hh, 14.0f) : hh * 0.8f) : std::min(hh, shapeCorner(shp) * 2.0f * hh);
            const ImU32 fill = (k == 0 || k == 3) ? accentMain() : g_m3c.secondaryContainer;
            m3eFillShape(d, x0 + off[k], cy, hw, hh, shp, R, 0.0f, bgTint(fill, a), sw, sh);
        }
    } else if (w.kind == RowWidget::Kind::Slider) {
        const Rect t = layout::rowSliderTrack(r);
        const float x0 = t.x + 8.0f, x1 = t.right() - 8.0f, th = 5.0f;
        if (w.hueTrack) {
            for (int i = 0; i < 6; i++) {
                const float fa = (float)i / 6.0f, fb = (float)(i + 1) / 6.0f;
                const ImU32 ca = bgTint(hsv(fa, 0.8f, 0.95f), a), cb = bgTint(hsv(fb, 0.8f, 0.95f), a);
                d->AddRectFilledMultiColor(ImVec2(x0 + (x1 - x0) * fa, cy - th), ImVec2(x0 + (x1 - x0) * fb, cy + th), ca, cb, cb, ca);
            }
        } else {
            const ImU32 lo = bgTint(rgb(w.trackLo), a), hi = bgTint(rgb(w.trackHi), a);
            d->AddRectFilledMultiColor(ImVec2(x0, cy - th), ImVec2(x1, cy + th), lo, hi, hi, lo);
        }
        d->AddRect(ImVec2(x0, cy - th), ImVec2(x1, cy + th), bgTint(ring, 0.5f * a), 0.0f, 0, 1.0f);
        const float hx = x0 + (x1 - x0) * clamp01(w.value);
        d->AddCircleFilled(ImVec2(hx, cy), 11.0f, bgTint(col::WHITE, a), 20);
        d->AddCircle(ImVec2(hx, cy), 11.0f, bgTint(IM_COL32(0, 0, 0, 255), 0.55f * a), 20, 2.0f);
        for (int k = 0; k < 2; k++) {
            const Rect b = k == 0 ? layout::rowSliderMinus(r) : layout::rowSliderPlus(r);
            const float bx = b.x + b.w * 0.5f;
            d->AddCircle(ImVec2(bx, cy), 17.0f, ring, 24, 2.0f);
            d->AddLine(ImVec2(bx - 7.0f, cy), ImVec2(bx + 7.0f, cy), ring, 2.5f);
            if (k == 1) d->AddLine(ImVec2(bx, cy - 7.0f), ImVec2(bx, cy + 7.0f), ring, 2.5f);
        }
    } else if (w.kind == RowWidget::Kind::Preview) {
        // The live theme, not a copy of it: the gradient, a button, a focused row and accent text exactly as
        // every screen draws them right now.
        const float x = r.x + 340.0f;
        const float bh = 26.0f, y0 = cy - bh * 0.5f;
        const ImU32 cm = accentMain(), cd = accentDeep(), cs = accentSecond();
        d->AddRectFilledMultiColor(ImVec2(x, y0), ImVec2(x + 70.0f, y0 + bh), cm, cd, cd, cm);
        d->AddRectFilledMultiColor(ImVec2(x + 70.0f, y0), ImVec2(x + 140.0f, y0 + bh), cd, cs, cs, cd);
        d->AddRect(ImVec2(x, y0), ImVec2(x + 140.0f, y0 + bh), bgTint(ring, 0.4f), 6.0f, 0, 1.0f);
        const float bx = x + 156.0f;
        d->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + 92.0f, y0 + bh), cm, bh * 0.5f);
        const ImVec2 t1 = textSize(TEXT_S, "Button");
        text(TEXT_S, bx + (92.0f - t1.x) * 0.5f, cy - TEXT_S * 0.5f - 1.0f, onMain(), "Button");
        const float fx = bx + 104.0f;
        d->AddRectFilled(ImVec2(fx, y0), ImVec2(fx + 92.0f, y0 + bh), cd, 8.0f);
        const ImVec2 t2 = textSize(TEXT_S, "Focus");
        text(TEXT_S, fx + (92.0f - t2.x) * 0.5f, cy - TEXT_S * 0.5f - 1.0f, onFocus(), "Focus");
        text(TEXT_S, fx + 108.0f, cy - TEXT_S * 0.5f - 1.0f, cm, "Text");
    }
}

// --- browser ---

void drawBrowser(const BrowserModel& m) {
    layout::listInset() = layout::fabInset((int)m.fabs.size()); // rows stop short of the floating buttons
    drawScreenBackdrop();
    const float press = g_m3e ? m3eRowPress() : 0.0f; // M3E: an activation bounces the neighbouring rows

    // Sidebar
    if (themed()) {
        // A floating panel (frosted glass / M3E navigation rail); the items keep their exact positions.
        themedSurface(dl(), 8.0f, 10.0f, layout::SIDEBAR_W - 8.0f, layout::SCREEN_H - 10.0f, g_m3e ? 32.0f : 26.0f, GK::Panel, true);
    } else {
        rect({0, 0, layout::SIDEBAR_W, layout::SCREEN_H}, col::SIDEBAR);
    }
    drawLogo(30.0f, 40.0f);
    for (int i = 0; i < (int)m.sidebar.size(); i++) {
        M3eRowShift shift;
        Rect r = layout::sidebarItem(i);
        bool current = (i == m.sidebarSelected);
        if (current && m.sidebarFocused) {
            surfaceRect(r, accentDeep(), 12.0f, GK::RowFocus);
        } else if (current) {
            surfaceRect(r, col::PANEL, 12.0f, GK::RowFocusDim);
            if (!themed()) dl()->AddRectFilled(ImVec2(r.x, r.y + 12), ImVec2(r.x + 4, r.bottom() - 12), accentMain(), 2.0f);
        }
        ImU32 c = (current && m.sidebarFocused) ? onFocus() : (current ? (g_m3e ? g_m3c.onSecondaryContainer : col::TEXT) : col::MUTED);
        g_iconActive = current;
        g_iconTag = current ? g_pulseCount : 0;
        drawIcon(m.sidebar[i].icon, r.x + 32, r.y + r.h * 0.5f, 26.0f, c);
        g_iconActive = false;
        g_iconTag = 0;
        g_bold = g_m3e && current; // M3E: the current destination is set heavier
        text(TEXT_M, r.x + 58, r.y + (r.h - TEXT_M) * 0.5f - 1, c, m.sidebar[i].label, r.w - 66);
        g_bold = false;
        if (g_m3e && m.sidebarFocused) shift.end(m3eRowPush(i - m.sidebarSelected, press));
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
        snprintf(buf, sizeof(buf), "%d / %d", m.selected + 1, std::max(total, m.total));
        counter = buf;
    }
    float counterW = counter.empty() ? 0.0f : textSize(TEXT_S, counter).x + 24.0f;
    float clockW = m.clock.empty() ? 0.0f : textSize(TEXT_L, m.clock).x + 24.0f;
    text(TEXT_L + 6, layout::CONTENT_X, layout::HEADER_TITLE_Y, col::TEXT, m.title,
         layout::CONTENT_W - std::max(clockW, m.clock.empty() ? counterW : 0.0f) - (!m.clock.empty() && !m.tabs.empty() ? counterW : 0.0f));
    if (!m.clock.empty()) {
        textRight(TEXT_L, layout::CONTENT_RIGHT, layout::HEADER_TITLE_Y + 2, col::TEXT, m.clock);
        // (with page / view tabs under the title, the counter stays up beside the clock: down there it would run
        // into the tabs' "R" key)
        if (!counter.empty()) {
            if (m.tabs.empty()) textRight(TEXT_S, layout::CONTENT_RIGHT, layout::HEADER_PATH_Y, col::MUTED, counter);
            else textRight(TEXT_S, layout::CONTENT_RIGHT - (clockW > 0 ? clockW : 0.0f), layout::HEADER_TITLE_Y + 12, col::MUTED, counter);
        }
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

    // Settings pages: a tab per page, the current one filled; L and R switch them.
    if (!m.tabs.empty()) {
        const Rect first = layout::settingsTab(0);
        const Rect last = layout::settingsTab((int)m.tabs.size() - 1);
        auto keyHint = [&](float x, const char* label) {
            dl()->AddCircle(ImVec2(x, first.y + first.h * 0.5f), 11.0f, col::DIM, 18, 1.5f);
            const ImVec2 ts = textSize(TEXT_S - 3, label);
            text(TEXT_S - 3, x - ts.x * 0.5f, first.y + first.h * 0.5f - (TEXT_S - 3) * 0.5f - 1.0f, col::MUTED, label);
        };
        keyHint(first.x - 18.0f, "L");
        keyHint(last.right() + 18.0f, "R");
        for (int i = 0; i < (int)m.tabs.size(); i++) {
            M3eRowShift tabShift;
            const Rect r = layout::settingsTab(i);
            const bool cur = (i == m.tabSelected);
            if (cur) surfaceRect(r, accentDeep(), r.h * 0.5f, GK::RowFocus);
            else surfaceRect(r, col::PANEL, r.h * 0.5f, GK::Row);
            const ImVec2 ts = textSize(TEXT_S, m.tabs[(size_t)i]);
            g_bold = g_m3e && cur;
            text(TEXT_S, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - TEXT_S) * 0.5f - 1.0f, cur ? onFocus() : col::MUTED, m.tabs[(size_t)i]);
            g_bold = false;
        }
    }

    // List
    const int visible = layout::visibleRows();
    if (total == 0) {
        text(TEXT_M, layout::CONTENT_X + 8, layout::LIST_TOP + 20, col::MUTED, m.emptyMessage, layout::CONTENT_W);
    } else {
        ListWindow w = computeListWindow(total, m.selected, visible);
        // Grouped rows (the palette editor) are ONE long panel, not a button each: the panel is drawn first, and
        // inside it only the selected row gets a (slightly inset) highlight.
        for (int i = w.start; i < w.end;) {
            if (!m.items[(size_t)i].grouped) { i++; continue; }
            int j = i;
            while (j + 1 < w.end && m.items[(size_t)(j + 1)].grouped) j++;
            const Rect a = layout::listRow(i - w.start), b = layout::listRow(j - w.start);
            surfaceRect({a.x, a.y, a.w, b.bottom() - a.y}, col::PANEL, 12.0f, GK::Row);
            i = j + 1;
        }
        for (int i = w.start; i < w.end; i++) {
            M3eRowShift shift;
            const ListEntry& e = m.items[(size_t)i];
            Rect r = layout::listRow(i - w.start);
            bool sel = (i == m.selected);
            if (sel) glassArtFocus(e.art);
            const Rect fr = e.grouped ? Rect{r.x + 6.0f, r.y + 2.0f, r.w - 12.0f, r.h - 4.0f} : r;
            if (sel && !m.sidebarFocused) {
                surfaceRect(fr, accentDeep(), e.grouped ? 10.0f : 12.0f, GK::RowFocus);
            } else if (sel) {
                surfaceRect(fr, e.grouped ? col::PANEL_HI : col::PANEL, e.grouped ? 10.0f : 12.0f, GK::RowFocusDim);
                if (!themed()) dl()->AddRect(ImVec2(fr.x, fr.y), ImVec2(fr.right(), fr.bottom()), accentDeep(), e.grouped ? 10.0f : 12.0f, 0, 2.0f);
            } else if (!e.grouped) {
                surfaceRect(r, col::PANEL, 12.0f, GK::Row);
            }
            bool hot = sel && !m.sidebarFocused;
            const ImU32 onHot = onFocus(); // text on the focused row ("white" on Plain and glass)
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
                    g_iconActive = hot;
                    g_iconTag = hot ? g_pulseCount : 0;
                    drawIcon(e.icon, x + layout::ROW_ART_W * 0.5f, r.y + r.h * 0.5f, 30.0f,
                             hot ? onHot : col::MUTED);
                    g_iconActive = false;
                    g_iconTag = 0;
                }
                drawArtBadges(e, f);
                x += layout::ROW_ART_W + 14;
            } else {
                x += 8;
            }
            // Chip rows keep the chips clear of any text: their tag moves into the name ("Accent colour  -  Teal").
            const bool chipRow = e.widget.kind == RowWidget::Kind::Swatches || e.widget.kind == RowWidget::Kind::Shapes;
            const std::string shownName = (chipRow && !e.tag.empty()) ? e.name + "  -  " + e.tag : e.name;
            float tagW = (chipRow || e.tag.empty() ? 0.0f : std::min(textSize(TEXT_S, e.tag).x, r.w * 0.35f) + 24.0f) +
                         (e.favorite ? 30.0f : 0.0f) + (e.addable ? 50.0f : 0.0f);
            float nameW = r.right() - 18 - tagW - x;
            if (e.widget.kind != RowWidget::Kind::None) nameW = std::min(nameW, rowWidgetLeft(e, r) - x);
            bool twoLines = !e.detail.empty();
            float nameY = twoLines ? r.y + 9 : r.y + (r.h - TEXT_M) * 0.5f;
            // A greyed row (an option that does not apply right now) keeps its place but fades.
            auto fade = [&](ImU32 c) { return e.dimmed ? bgTint(c, 0.4f) : c; };
            g_bold = g_m3e && hot;
            text(TEXT_M, x, nameY, fade(hot ? onHot : col::TEXT), shownName, nameW);
            g_bold = false;
            // (the old fixed pale blue is only right on the plain accent fills; a custom palette can be anything)
            if (twoLines) text(TEXT_S, x, r.y + 39, fade(hot ? ((g_m3e || g_custom) ? onHot : IM_COL32(0xDD, 0xEE, 0xF7, 0xFF)) : col::MUTED), e.detail, nameW);
            if (e.widget.kind != RowWidget::Kind::None) drawRowWidget(e, r, hot);
            float tagRight = r.right() - 18;
            if (e.addable) { drawRowAdd(r, hot); tagRight -= 50; }
            if (e.favorite) {
                drawIcon(Icon::Heart, tagRight - 10, r.y + r.h * 0.5f, 18.0f, rgb(0xFF5C8A));
                tagRight -= 30;
            }
            if (!e.tag.empty() && !chipRow) {
                std::string tag = fitText(font(), TEXT_S, e.tag, r.w * 0.35f);
                textRight(TEXT_S, tagRight, r.y + (r.h - TEXT_S) * 0.5f, fade(hot ? onHot : col::MUTED), tag);
            }
            if (g_m3e && !m.sidebarFocused) shift.end(m3eRowPush(i - m.selected, press));
        }
        if (total > visible) {
            Rect track = layout::scrollTrack();
            rect(track, col::PANEL, 3.0f);
            float thumbH = std::max(24.0f, track.h * visible / total);
            float thumbY = track.y + (track.h - thumbH) * w.start / (float)(total - visible);
            dl()->AddRectFilled(ImVec2(track.x, thumbY), ImVec2(track.right(), thumbY + thumbH), col::MUTED, 3.0f);
        }
    }

    drawFabs(m);
    drawHints(m.hints, layout::CONTENT_X, layout::FOOTER_Y);
    drawSortMenu(m);
    drawUsageCelebration(); // hidden easter egg: over everything, only while it is running
}

// --- message card ---

void drawMessage(const MessageModel& m) {
    if (m.overlay && g_m3e) {
        int v = 0; float sc = 1.0f; // (the card of a message does not spring; only its backdrop fades in)
        m3eEntranceBegin(dl(), m3e::hashRect(0, 0, 1, 1), v, sc);
        m3eDialogScrim(dl(), m3e::entranceAlpha(sc), v);
    } else if (m.overlay) {
        dl()->AddRectFilled(ImVec2(0, 0), ImVec2(layout::SCREEN_W, layout::SCREEN_H), IM_COL32(0, 0, 0, 150));
    } else {
        drawScreenBackdrop();
        drawLogo(30.0f, 40.0f);
    }

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

    // A card over the plain screen is an opaque tonal container; over a dimmed screen (overlay) it is the frosted dialog.
    surfaceRect({x, y, cardW, cardH}, col::PANEL, 18.0f, (g_m3e && m.overlay) ? GK::Dialog : GK::Panel);
    if (g_m3e) {
        // M3E has no accent strip: a busy card shows the loading indicator, an error card an alert cookie.
        if (m.busy || m.isError) m3eMessageBadge(x + cardW - pad - 28.0f, y + pad + 16.0f, m.busy, m.isError);
    } else if (g_glass) {
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
    text(TEXT_L, x + pad, y + pad, m.isError ? (g_m3e ? g_m3c.onErrorContainer : col::ERROR_RED) : col::TEXT, title,
         (g_m3e && (m.busy || m.isError)) ? wrap - 72.0f : wrap); // (room for the badge)

    float ty = y + pad + TEXT_L + 26.0f;
    const float maxY = y + cardH - pad;
    m.linesShown = 0;
    for (size_t i = (size_t)std::max(0, m.firstLine); i < m.lines.size(); i++) {
        const std::string& line = m.lines[i];
        if (line.empty()) {
            ty += TEXT_M * 0.6f;
            m.linesShown++;
            continue;
        }
        float h = font()->CalcTextSizeA(TEXT_M, 1e9f, wrap, line.c_str()).y;
        if (ty + h > maxY) break;
        dl()->AddText(font(), TEXT_M, ImVec2(x + pad, ty), col::MUTED, line.c_str(), nullptr, wrap);
        ty += h + 8.0f;
        m.linesShown++;
    }

    drawHints(m.hints, x, y + cardH + 24.0f);
}

// --- now playing ---

// --- lyrics (Now Playing) ---------------------------------------------------------------------

static ImU32 scaleAlpha(ImU32 c, float f) {
    const int a = (int)((float)(c >> 24) * std::max(0.0f, std::min(1.0f, f)) + 0.5f);
    return (c & 0x00FFFFFFu) | ((ImU32)a << 24);
}

// Multiplies the alpha of every vertex drawn since `v0` -- how a whole group (the lyrics panel, the sync
// switch) fades in and out with one pass over a few hundred vertices, whatever the style drew it with.
static void fadeVertices(ImDrawList* d, int v0, float f) {
    if (f >= 0.999f) return;
    f = std::max(0.0f, f);
    const unsigned mul = (unsigned)(f * 256.0f);
    for (int i = v0; i < d->VtxBuffer.Size; i++) {
        ImU32& c = d->VtxBuffer[i].col;
        const unsigned a = ((c >> 24) * mul) >> 8;
        c = (c & 0x00FFFFFFu) | (a << 24);
    }
}

// The lyrics panel: the lines scroll so the one being sung sits in the middle, which grows and brightens
// while the rest dim towards the panel's edges. Synced lyrics follow the song; plain ones (or with the sync
// switch off) drift through the whole text as the song plays. Everything is eased by one smoothed position,
// so a seek or a new line glides instead of jumping. No allocation: a few CalcTextSize calls per frame.
static void drawLyricsPanel(const NowPlayingModel& m, float t) {
    if (t <= 0.004f || !m.lyrics || !m.lyrics->available()) return;
    const lyrics::Lyrics& ly = *m.lyrics;
    const int n = (int)ly.lines.size();
    const Rect P = npLyricsPanel();
    const float slide = 36.0f * (1.0f - t);
    const float px = P.x + slide;
    ImDrawList* d = dl();
    const int v0 = d->VtxBuffer.Size;

    if (themed()) themedSurface(d, px, P.y, px + P.w, P.y + P.h, gr(24.0f), GK::Panel, true);
    else d->AddRectFilled(ImVec2(px, P.y), ImVec2(px + P.w, P.y + P.h), scaleAlpha(col::PANEL, 0.88f), 24.0f);
    text(TEXT_S, px + 34.0f, P.y + 18.0f, accentMain(), m.lyricsSync && ly.synced ? "LYRICS  -  SYNCED" : "LYRICS");

    // Where the song is: the line being sung (synced), or how far through the song we are (plain).
    const bool sync = ly.synced && m.lyricsSync;
    const int cur = sync ? lyrics::currentLine(ly, m.positionSeconds) : -1;
    const float target = sync ? (float)std::max(cur, 0)
                              : (m.durationSeconds > 0.0 ? (float)std::max(0.0, std::min(1.0, m.positionSeconds / m.durationSeconds)) * (float)(n - 1) : 0.0f);
    static float s = 0.0f, hl = -1.0f, hlAmt = 0.0f;
    static float vel = 0.0f, idle = 99.0f; // touch scrolling: momentum (lines / second) and seconds since the last touch
    static std::string seenTitle;
    static int seenCount = -1;
    if (m.title != seenTitle || n != seenCount) { seenTitle = m.title; seenCount = n; s = target; hl = (float)cur; hlAmt = sync ? 1.0f : 0.0f; vel = 0.0f; idle = 99.0f; }
    const float dt = m3eDt();
    // Touch scrolling: dragging moves the lyrics with the finger, letting go carries on with a little momentum,
    // and a few seconds after the last touch the lyrics glide back to following the song.
    const float avgStep = 26.0f * 1.5f + 20.0f;
    if (m.lyricsTouching || m.lyricsDragDy != 0.0f) {
        const float dLines = -m.lyricsDragDy / avgStep;
        s += dLines;
        vel += (dLines / std::max(dt, 0.004f) - vel) * 0.5f;
        idle = 0.0f;
    } else {
        idle += dt;
        s += vel * dt;
        vel *= std::exp(-dt * 4.0f);
        if (std::fabs(vel) < 0.02f) vel = 0.0f;
    }
    s = std::max(0.0f, std::min((float)(n - 1), s));
    if (idle > 3.0f) s += (target - s) * (1.0f - std::exp(-dt * (sync ? 6.0f : 2.5f))); // back to following the song
    if (sync) hl += ((float)cur - hl) * (1.0f - std::exp(-dt * 9.0f));
    hlAmt += ((sync ? 1.0f : 0.0f) - hlAmt) * (1.0f - std::exp(-dt * 8.0f));

    const float clipTop = P.y + 56.0f, clipBot = P.y + P.h - 18.0f, cy = (clipTop + clipBot) * 0.5f;
    const float wrapW = P.w - 76.0f, gap = 20.0f, fadeH = 96.0f;
    auto emph = [&](int i) { return hlAmt * std::max(0.0f, 1.0f - std::fabs((float)i - hl)); };
    auto sizeOf = [&](int i) { return 26.0f + 9.0f * emph(i); };
    auto textOf = [&](int i) -> const char* { const std::string& x = ly.lines[(size_t)i].text; return x.empty() ? "..." : x.c_str(); };
    auto heightOf = [&](int i) { return font()->CalcTextSizeA(sizeOf(i), 1e9f, wrapW, textOf(i)).y; };

    d->PushClipRect(ImVec2(px + 8.0f, clipTop), ImVec2(px + P.w - 8.0f, clipBot), true);
    const int a = std::max(0, std::min(n - 1, (int)std::floor(s)));
    const float f = s - (float)a;
    const float ha = heightOf(a);
    const float stepA = a + 1 < n ? (ha + heightOf(a + 1)) * 0.5f + gap : ha + gap;
    auto drawLine = [&](int i, float c, float h) {
        const float e = emph(i);
        const float edge = std::min((c - clipTop), (clipBot - c)) / fadeH;
        const float fade = std::max(0.0f, std::min(1.0f, edge));
        const float base = sync ? 0.50f + 0.50f * e : 0.82f;
        ImU32 colr = lerpColor(col::MUTED, col::TEXT, sync ? e : 0.85f);
        colr = scaleAlpha(colr, base * fade);
        d->AddText(font(), sizeOf(i), ImVec2(px + 34.0f, c - h * 0.5f), colr, textOf(i), nullptr, wrapW);
    };
    float c = cy - f * stepA;
    drawLine(a, c, ha);
    float cc = c, hh = ha;
    for (int i = a + 1; i < n; i++) { // downwards
        const float hi = heightOf(i);
        cc += hh * 0.5f + gap + hi * 0.5f;
        hh = hi;
        if (cc - hi * 0.5f > clipBot) break;
        drawLine(i, cc, hi);
    }
    cc = c; hh = ha;
    for (int i = a - 1; i >= 0; i--) { // upwards
        const float hi = heightOf(i);
        cc -= hh * 0.5f + gap + hi * 0.5f;
        hh = hi;
        if (cc + hi * 0.5f < clipTop) break;
        drawLine(i, cc, hi);
    }
    d->PopClipRect();
    fadeVertices(d, v0, t);
}

static void drawQueueButton(const NowPlayingModel& m);
void drawQueueFlyout(const NowPlayingModel& m);
void drawQueueBrowser(const NowPlayingModel& m);
void drawVideoSettings(const NowPlayingModel& m);

void drawNowPlaying(const NowPlayingModel& m) {
    drawScreenBackdrop();
    glassArtFocus(m.art);
    drawLogo(30.0f, 40.0f);

    // Lyrics mode: the player glides into a column on the left and the lyrics open on the right. One eased
    // value (0 normal .. 1 lyrics) drives every position (see npLayout), and is published for touch hit-testing.
    // M3E eases it with a soft spring (a hint of settle); the other styles with a smooth ease-in-out.
    float lyrT;
    {
        static m3e::Spring lyrSpring;
        static float lyrEase = 0.0f;
        if (!m.lyricsCapable) { lyrEase = 0.0f; lyrSpring.reset(0.0f); } // the movie remote never shows lyrics: no slide
        const float lyrTarget = (m.lyricsOpen && m.lyrics && m.lyrics->available()) ? 1.0f : 0.0f;
        if (g_m3e) {
            lyrSpring.step(m3eDt(), lyrTarget, 210.0f, 24.0f);
            lyrEase = lyrSpring.x;
            lyrT = std::max(0.0f, std::min(1.0f, lyrSpring.x));
        } else {
            lyrEase += (lyrTarget - lyrEase) * (1.0f - std::exp(-m3eDt() * 9.0f));
            if (std::fabs(lyrTarget - lyrEase) < 0.0005f) lyrEase = lyrTarget;
            lyrT = lyrEase * lyrEase * (3.0f - 2.0f * lyrEase);
            lyrSpring.reset(lyrEase); // keeps the spring in step if the style changes
        }
        npLyr() = lyrT;
    }
    const NpLayout L = npLayout(lyrT);

    // Events the icons answer with an animation: a jump in the position is a seek (back or forward), a change of
    // the touched button is a press, a change of volume step is a nudge. (Only used by M3E.)
    static std::string evTitle;
    static double evPos = 0.0;
    static uint32_t evBack = 0, evFwd = 0, evPress[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    static int evLastPressed = -1;
    if (m.title != evTitle) { evTitle = m.title; evPos = m.positionSeconds; }
    else {
        const double dpos = m.positionSeconds - evPos;
        if (dpos > 2.0) evFwd++; else if (dpos < -2.0) evBack++;
        evPos = m.positionSeconds;
    }
    if (m.pressedButton != evLastPressed) {
        if (m.pressedButton >= 0 && m.pressedButton < 8) evPress[m.pressedButton]++;
        evLastPressed = m.pressedButton;
    }
    const uint32_t volStep = (uint32_t)(std::max(0.0f, std::min(1.0f, m.volume)) * 12.0f + 0.5f);

    // Artwork: the album art, or a gradient tile with a note.
    const float art = L.art;
    const float ax = L.artX, ay = L.artY;
    if (m.art && g_m3e) {
        // Cover art keeps the signature shape: the picture is clipped to the same slowly turning cookie as the default tile.
        m3eCookieImage(dl(), (ImTextureID)m.art, ax + art * 0.5f, ay + art * 0.5f, art * 0.5f / 1.085f, 9, 0.085f, (float)ImGui::GetTime() * 0.12f,
                       ImVec2(0, 0), ImVec2(1, 1), col::WHITE);
    } else if (m.art) {
        dl()->AddImageRounded(ImTextureRef((ImTextureID)m.art), ImVec2(ax, ay), ImVec2(ax + art, ay + art),
                              ImVec2(0, 0), ImVec2(1, 1), col::WHITE, 22.0f);
    } else if (g_m3e) {
        // No cover art: the featured tile is a slowly turning cookie holding the note (M3E's signature shape).
        m3eCookie(dl(), ax + art * 0.5f, ay + art * 0.5f, art * 0.5f, 9, 0.085f, (float)ImGui::GetTime() * 0.12f, accentDeep());
        g_iconActive = true;
        drawIcon(Icon::Music, ax + art * 0.5f, ay + art * 0.5f, 150.0f, g_m3c.onPrimaryContainer);
        g_iconActive = false;
    } else {
        gradientRoundRect(ax, ay, ax + art, ay + art, 22.0f, accentSecond(), accentDeep());
        drawIcon(Icon::Music, ax + art * 0.5f, ay + art * 0.5f, 150.0f, IM_COL32(0xFF, 0xFF, 0xFF, 0xD0));
    }
    if (m.paused) {
        if (g_m3e) m3eCookie(dl(), ax + art * 0.5f, ay + art * 0.5f, art * 0.5f / 1.085f, 9, 0.085f, (float)ImGui::GetTime() * 0.12f, IM_COL32(0, 0, 0, 0x70));
        else dl()->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + art, ay + art), IM_COL32(0, 0, 0, 0x70), 22.0f);
        float cx = ax + art * 0.5f, cy = ay + art * 0.5f;
        dl()->AddRectFilled(ImVec2(cx - 34, cy - 44), ImVec2(cx - 10, cy + 44), col::WHITE, 5.0f);
        dl()->AddRectFilled(ImVec2(cx + 10, cy - 44), ImVec2(cx + 34, cy + 44), col::WHITE, 5.0f);
    }

    const float tx = L.textX, tw = L.textW;     // label, title, subtitle
    const float cx0 = L.ctlX, cw = L.ctlW;      // progress bar, buttons, volume, queue lines
    text(TEXT_S, tx, L.labelY, accentMain(), m.paused ? "PAUSED" : "NOW PLAYING");
    dl()->PushClipRect(ImVec2(tx - 4.0f, L.labelY + 30.0f), ImVec2(tx + tw + 4.0f, L.subtitleY - 2.0f), true);
    dl()->AddText(font(), L.titleSize, ImVec2(tx, L.labelY + 36.0f), col::TEXT, m.title.c_str(), nullptr, tw);
    dl()->PopClipRect();
    text(TEXT_M, tx, L.subtitleY, col::MUTED, m.subtitle, tw);

    const double shownPos = m.scrubbing ? m.scrubSeconds : m.positionSeconds; // a finger on the bar: the time it would seek to
    drawProgress(cx0, L.progressY, cw, 8.0f, shownPos, m.durationSeconds, false, !m.paused && !m.scrubbing);
    text(TEXT_S, cx0, L.timeY, m.scrubbing ? accentMain() : col::MUTED, formatTime(shownPos));
    if (m.scrubbing && m.durationSeconds > 0.0) { // a bubble over the knob says where lifting the finger will go
        const std::string t = formatTime(shownPos);
        const ImVec2 ts = textSize(TEXT_M, t);
        const float f = (float)std::max(0.0, std::min(1.0, shownPos / m.durationSeconds));
        const float bw = ts.x + 28.0f, bh = ts.y + 14.0f;
        const float bx = std::max(cx0 - 6.0f, std::min(cx0 + cw + 6.0f - bw, cx0 + cw * f - bw * 0.5f)), by = L.progressY - 22.0f - bh;
        if (g_m3e) dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), accentMain(), bh * 0.5f);
        else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), accentDeep(), bh * 0.5f);
        dl()->AddText(font(), TEXT_M, ImVec2(bx + 14.0f, by + 7.0f), g_m3e ? onMain() : col::WHITE, t.c_str());
    }
    if (m.durationSeconds > 0.0)
        textRight(TEXT_S, cx0 + cw, L.timeY, col::MUTED, formatTime(m.durationSeconds));

    // Transport buttons: seek back / play-pause / seek forward / stop,
    // touch-tappable (see layout::npButtonRect / hitTestNowPlaying).
    // Slightly transparent so they read as an overlay on the artwork
    // row rather than solid chrome.
    for (int i = 0; i < (int)ui::NpButton::Count; i++) {
        const ui::Rect rr = ui::npButtonRect(i); // (touch rectangle: the user's width / height)
        ui::Rect r = rr;
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        r.w = r.h = std::min(rr.w, rr.h); // (the icons are sized from the smaller side)
        const bool isPlay = ((ui::NpButton)i == ui::NpButton::PlayPause);
        float grow = 0.0f;
        if (g_m3e) { // connected button group: neighbours of a pressed button are pushed aside, it swells
            static const int rankOf[6] = {1, 2, 3, 4, 0, 5}; // left-to-right place of each NpButton
            float push = 0.0f;
            m3eGroupMotion(i, rankOf[i], m.pressedButton >= 0 && m.pressedButton < 6 ? rankOf[m.pressedButton] : -1, push, grow);
            cx += push;
        }
        if (g_m3e) {
            // M3E media player: seek buttons are tonal rounded rectangles; play/pause is the wide primary-filled
            // one (drawn wider than its touch rect, into the gap), a pill when it offers "play" and a rounded
            // rectangle while playing; pressing squares the corners.
            const int kind = isPlay ? 0 : 1;
            const float hw = (isPlay ? rr.w * 0.70f : rr.w * 0.5f) * (1.0f + 0.14f * grow);
            m3eTransportButton(dl(), cx, cy, hw, rr.h * 0.5f * (1.0f + 0.04f * grow), g_btnSize[kind][0], g_btnSize[kind][1], isPlay ? accentMain() : g_m3c.secondaryContainer,
                               m.pressedButton == i, isPlay ? (m.paused ? 0.5f : 0.44f) : 0.44f, i, g_btnShape[isPlay ? 0 : 1]);
        } else if (g_glass) {
            // Normal / pressed (finger down on it, from the existing touch
            // hit-test). Focus on this screen is the touch/remote itself, so
            // "hot" is not used here.
            glassCircle(dl(), cx, cy, r.w * 0.5f, m.pressedButton == i ? GK::ButtonPressed : GK::Button, true);
        } else {
            dl()->AddCircleFilled(ImVec2(cx, cy), r.w * 0.5f, IM_COL32(0x2C, 0x31, 0x3A, 0xB0), 32);
            dl()->AddCircle(ImVec2(cx, cy), r.w * 0.5f, IM_COL32(0x3A, 0x3F, 0x48, 0xA0), 32, 1.5f);
        }
        const ImU32 ic = g_m3e ? (isPlay ? onMain() : g_m3c.onSecondaryContainer) : IM_COL32(0xEC, 0xEE, 0xF1, 0xE6);
        M3eFxScope fxs;
        switch ((ui::NpButton)i) {
            case ui::NpButton::SeekBack:
            case ui::NpButton::SeekForward: {
                const bool back = (ui::NpButton)i == ui::NpButton::SeekBack;
                const char* label = back ? "10" : "30";
                const float fs = r.w * 0.27f;
                ImVec2 sz = font()->CalcTextSizeA(fs, 1e9f, 0.0f, label);
                if (g_m3e) {
                    // Android's replay-10 / forward-30: a circular arrow round the number. When a seek happens (or the
                    // button is pressed) the arrow turns one full lap in the direction of the seek.
                    fxs.begin(3000 + i, true, {0.16f, 0.0f, 0.0f, back ? -6.2832f : 6.2832f, 0.0f, true}, r.w, cx, cy,
                              (back ? evBack : evFwd) + evPress[i]);
                    const float rr = r.w * 0.36f, sgn = back ? -1.0f : 1.0f, th = 2.6f;
                    ImVec2 arc[26];
                    for (int k = 0; k < 26; k++) {
                        const float a = (-65.0f + 290.0f * (float)k / 25.0f) * 0.0174533f;
                        arc[k] = ImVec2(cx + sgn * std::cos(a) * rr, cy + std::sin(a) * rr);
                    }
                    dl()->AddPolyline(arc, 26, ic, 0, th);
                    const float ea = 225.0f * 0.0174533f;
                    const ImVec2 P = arc[25];
                    const ImVec2 T(sgn * -std::sin(ea), std::cos(ea)), N(sgn * std::cos(ea), std::sin(ea));
                    const float hl = r.w * 0.17f, hw = r.w * 0.11f;
                    dl()->AddTriangleFilled(ImVec2(P.x + T.x * hl, P.y + T.y * hl), ImVec2(P.x + N.x * hw, P.y + N.y * hw),
                                            ImVec2(P.x - N.x * hw, P.y - N.y * hw), ic);
                    fxs.end();
                    dl()->AddText(font(), fs, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), ic, label);
                    break;
                }
                label = back ? "-10" : "+30";
                sz = font()->CalcTextSizeA(r.w * 0.34f, 1e9f, 0.0f, label);
                dl()->AddText(font(), r.w * 0.34f, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), ic, label);
                break;
            }
            case ui::NpButton::PlayPause: {
                const float s = r.w * 0.32f;
                if (g_m3e) {
                    // The play / pause icon morphs (two quads that turn into the triangle and back) on a spring.
                    static m3e::Spring pp;
                    pp.step(m3eDt(), m.paused ? 0.0f : 1.0f);
                    float q[16];
                    m3e::playPausePoints(std::max(0.0f, std::min(1.15f, pp.x)), q);
                    for (int k = 0; k < 2; k++) {
                        const ImVec2 poly[4] = {ImVec2(cx + q[8 * k] * s, cy + q[8 * k + 1] * s), ImVec2(cx + q[8 * k + 2] * s, cy + q[8 * k + 3] * s),
                                                ImVec2(cx + q[8 * k + 4] * s, cy + q[8 * k + 5] * s), ImVec2(cx + q[8 * k + 6] * s, cy + q[8 * k + 7] * s)};
                        dl()->AddConvexPolyFilled(poly, 4, ic);
                    }
                } else if (m.paused) {
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
                if (g_m3e) fxs.begin(3003, true, {0.30f, 0.0f, 0.0f, 0.0f, 0.0f, true}, r.w, cx, cy, evPress[3]);
                dl()->AddRectFilled(ImVec2(cx - s * 0.75f, cy - s * 0.75f), ImVec2(cx + s * 0.75f, cy + s * 0.75f), ic, g_m3e ? s * 0.3f : 2.0f);
                break;
            }
            default: break;
        }
        fxs.end();
    }

    // Shuffle / Repeat (music queues only). OFF: a plain dim button, icon grey, caption "off". ON: the button is
    // filled with the accent colour, the icon white, the caption written in the accent colour -- so it is clear at
    // a glance which are active. Repeat has a third state: "repeat one" is lit too, with a 1 on the icon.
    if (m.queueControls) {
        for (int w = (int)ui::NpButton::Shuffle; w <= (int)ui::NpButton::Repeat; w++) {
            const bool isShuffle = (w == (int)ui::NpButton::Shuffle);
            const bool on = isShuffle ? m.shuffleOn : m.repeatMode != 0;
            const ui::Rect r = ui::npToggleRect((ui::NpButton)w);
            float cx = r.x + r.w * 0.5f;
            const float cy = r.y + r.h * 0.5f, rad = std::min(r.w, r.h) * 0.5f;
            float growm = 0.0f;
            if (g_m3e) {
                // M3E toggle button: tonal when off, primary when on, and the ON state is a rounded square (selected
                // toggles change shape, not only colour -- so it reads without relying on colour alone).
                static const int rankOf[6] = {1, 2, 3, 4, 0, 5};
                float push = 0.0f;
                m3eGroupMotion(w, rankOf[w], m.pressedButton >= 0 && m.pressedButton < 6 ? rankOf[m.pressedButton] : -1, push, growm);
                cx += push;
                m3eTransportButton(dl(), cx, cy, r.w * 0.5f * (1.0f + 0.14f * growm), r.h * 0.5f * (1.0f + 0.04f * growm), g_btnSize[2][0], g_btnSize[2][1],
                                   on ? accentMain() : g_m3c.secondaryContainer, m.pressedButton == w, on ? 0.42f : 0.5f, w, g_btnShape[2]);
            } else if (g_glass) {
                glassCircle(dl(), cx, cy, rad, m.pressedButton == w ? GK::ButtonPressed : (on ? GK::ButtonHot : GK::Button), true);
                if (on) dl()->AddCircleFilled(ImVec2(cx, cy), rad * 0.94f, (accentMain() & 0x00FFFFFF) | (0x9Cu << 24), 32);
            } else if (on) {
                dl()->AddCircleFilled(ImVec2(cx, cy), rad, accentMain(), 32);
                dl()->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0xFF, 0xFF, 0xFF, 0x70), 32, 1.5f);
            } else {
                dl()->AddCircleFilled(ImVec2(cx, cy), rad, IM_COL32(0x2C, 0x31, 0x3A, 0x90), 32);
                dl()->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0x3A, 0x3F, 0x48, 0x90), 32, 1.5f);
            }
            const ImU32 ic = g_m3e ? (on ? onMain() : g_m3c.onSecondaryContainer)
                                   : (on ? IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) : IM_COL32(0xA0, 0xA5, 0xAE, 0xB0));
            M3eFxScope fxs;
            if (g_m3e) // the icon pops (shuffle wiggles, repeat spins a turn) when its toggle comes on or changes mode
                // shuffle flips over (the order is swapped), repeat turns a full lap -- on every press, whatever the state
                fxs.begin(isShuffle ? 3100u : 3200u, true,
                          isShuffle ? m3e::IconFxStyle{0.14f, 0.0f, 0.0f, 0.0f, 5.2f, true} : m3e::IconFxStyle{0.12f, 0.0f, 0.0f, 6.2832f, 0.0f, true},
                          rad * 2.0f, cx, cy, isShuffle ? (uint32_t)m.shuffleOn + 2u * evPress[w] : (uint32_t)m.repeatMode + 4u * evPress[w]);
            const float s = rad * 0.52f, th = 2.6f;
            ImDrawList* d = dl();
            auto arrowRight = [&](float x, float y) { d->AddTriangleFilled(ImVec2(x + s * 0.42f, y), ImVec2(x - s * 0.1f, y - s * 0.32f), ImVec2(x - s * 0.1f, y + s * 0.32f), ic); };
            auto arrowLeft = [&](float x, float y) { d->AddTriangleFilled(ImVec2(x - s * 0.42f, y), ImVec2(x + s * 0.1f, y + s * 0.32f), ImVec2(x + s * 0.1f, y - s * 0.32f), ic); };
            if (isShuffle) {
                // Two arrows crossing: the order is mixed.
                d->AddLine(ImVec2(cx - s, cy - s * 0.55f), ImVec2(cx + s * 0.55f, cy + s * 0.55f), ic, th);
                d->AddLine(ImVec2(cx - s, cy + s * 0.55f), ImVec2(cx + s * 0.55f, cy - s * 0.55f), ic, th);
                arrowRight(cx + s * 0.78f, cy + s * 0.55f);
                arrowRight(cx + s * 0.78f, cy - s * 0.55f);
            } else {
                // Two arrows chasing each other round a loop.
                const ImVec2 top[3] = {ImVec2(cx - s, cy + s * 0.15f), ImVec2(cx - s, cy - s * 0.55f), ImVec2(cx + s * 0.55f, cy - s * 0.55f)};
                const ImVec2 bot[3] = {ImVec2(cx + s, cy - s * 0.15f), ImVec2(cx + s, cy + s * 0.55f), ImVec2(cx - s * 0.55f, cy + s * 0.55f)};
                d->AddPolyline(top, 3, ic, 0, th);
                d->AddPolyline(bot, 3, ic, 0, th);
                arrowRight(cx + s * 0.78f, cy - s * 0.55f);
                arrowLeft(cx - s * 0.78f, cy + s * 0.55f);
                if (m.repeatMode == 2) { // repeat one: a 1 in the middle of the loop
                    const float fs = rad * 0.72f;
                    const ImVec2 sz = font()->CalcTextSizeA(fs, 1e9f, 0.0f, "1");
                    d->AddText(font(), fs, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), ic, "1");
                }
            }
            fxs.end();
            // The state, written under the button.
            const char* name = isShuffle ? (on ? "Shuffle on" : "Shuffle off")
                                         : (m.repeatMode == 0 ? "Repeat off" : (m.repeatMode == 1 ? "Repeat all" : "Repeat one"));
            const float fs = 16.0f;
            const ImVec2 ns = font()->CalcTextSizeA(fs, 1e9f, 0.0f, name);
            d->AddText(font(), fs, ImVec2(cx - ns.x * 0.5f, r.y + r.h + 6.0f), on ? accentMain() : col::MUTED, name);
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
        g_iconActive = true;
        g_iconTag = volStep + 1u;
        g_iconHoldless = true;
        drawIcon(Icon::Music, vr.x + iconW * 0.5f, lineY, iconW * 0.8f, g_m3e ? col::MUTED : IM_COL32(0xB8, 0xBC, 0xC4, 0xFF));
        g_iconActive = false;
        g_iconTag = 0;
        g_iconHoldless = false;
        if (g_m3e) {
            // M3E slider: a thick track, a primary fill, and a tall handle with a gap round it. Only the OUTER ends are
            // rounded (the fill's left end, the track's right end, which also carries the small end dot); the ends
            // that face the handle are nearly square.
            const float th = 16.0f, y0 = lineY - th * 0.5f, y1 = lineY + th * 0.5f, hx = trackX + trackW * vol;
            const float trackEnd = trackX + trackW, gap = 6.0f, small = 3.0f;
            const float fillEnd = std::max(trackX + th * 0.5f, hx - gap), trackStart = hx + gap; // (at full volume nothing is left: no stub behind the handle)
            if (vol <= 0.0f) {
                dl()->AddRectFilled(ImVec2(trackX, y0), ImVec2(trackEnd, y1), col::TRACK, th * 0.5f);
            } else {
                // fill: round on the left, a small radius where it meets the handle
                dl()->AddRectFilled(ImVec2(trackX, y0), ImVec2(fillEnd, y1), accentMain(), th * 0.5f, ImDrawFlags_RoundCornersLeft);
                dl()->AddRectFilled(ImVec2(std::max(trackX + th * 0.5f, fillEnd - th), y0), ImVec2(fillEnd, y1), accentMain(), small, ImDrawFlags_RoundCornersRight);
                // track: a small radius where it meets the handle, round on the far right
                if (trackStart < trackEnd - 2.0f) {
                    dl()->AddRectFilled(ImVec2(trackStart, y0), ImVec2(trackEnd, y1), col::TRACK, th * 0.5f, ImDrawFlags_RoundCornersRight);
                    dl()->AddRectFilled(ImVec2(trackStart, y0), ImVec2(std::min(trackEnd - th * 0.5f, trackStart + th), y1), col::TRACK, small, ImDrawFlags_RoundCornersLeft);
                }
            }
            if (trackEnd - trackStart > th) dl()->AddCircleFilled(ImVec2(trackEnd - th * 0.5f, lineY), 2.5f, accentMain(), 10); // the end dot (only when there is track to hold it)
            dl()->AddRectFilled(ImVec2(hx - 3.0f, lineY - 18.0f), ImVec2(hx + 3.0f, lineY + 18.0f), accentMain(), 3.0f);
        } else if (g_glass) {
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

    if (!m.queueText.empty()) text(TEXT_S, cx0, L.queueY, col::TEXT, m.queueText, cw);
    if (!m.nextText.empty()) text(TEXT_S, cx0, L.queueY + 30.0f, col::MUTED, m.nextText, cw);

    // Lyrics: the panel on the right, the sync switch beside it, and the Lyrics button at the end of the
    // volume row (greyed when the song has no lyrics).
    drawLyricsPanel(m, lyrT);
    {
        const bool avail = m.lyrics && m.lyrics->available();
        const bool hasSync = avail && m.lyrics->synced;
        // One round button, in the style's own look: off (dim), on (accent), disabled (greyed).
        auto roundButton = [&](const ui::Rect& r, bool on, bool disabled, bool pressed, int slot) {
            const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, rad = std::min(r.w, r.h) * 0.5f;
            if (g_m3e) {
                const ImU32 fill = on ? accentMain() : (disabled ? lerpColor(col::PANEL, g_m3c.secondaryContainer, 0.35f) : g_m3c.secondaryContainer);
                m3eTransportButton(dl(), cx, cy, r.w * 0.5f, r.h * 0.5f, g_btnSize[2][0], g_btnSize[2][1], fill, pressed && !disabled, on ? 0.42f : 0.5f, slot, g_btnShape[2]);
            } else if (g_glass) {
                glassCircle(dl(), cx, cy, rad, disabled ? GK::ButtonDisabled : (pressed ? GK::ButtonPressed : (on ? GK::ButtonHot : GK::Button)), true);
                if (on && !disabled) dl()->AddCircleFilled(ImVec2(cx, cy), rad * 0.94f, (accentMain() & 0x00FFFFFF) | (0x9Cu << 24), 32);
            } else if (on && !disabled) {
                dl()->AddCircleFilled(ImVec2(cx, cy), rad, accentMain(), 32);
                dl()->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0xFF, 0xFF, 0xFF, 0x70), 32, 1.5f);
            } else {
                dl()->AddCircleFilled(ImVec2(cx, cy), rad, IM_COL32(0x2C, 0x31, 0x3A, disabled ? 0x48 : 0x90), 32);
                dl()->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0x3A, 0x3F, 0x48, disabled ? 0x50 : 0x90), 32, 1.5f);
            }
        };
        auto iconColor = [&](bool on, bool disabled) -> ImU32 {
            if (disabled) return scaleAlpha(g_m3e ? col::MUTED : IM_COL32(0xA0, 0xA5, 0xAE, 0xFF), 0.40f);
            return g_m3e ? (on ? onMain() : g_m3c.onSecondaryContainer) : (on ? IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) : IM_COL32(0xA0, 0xA5, 0xAE, 0xB0));
        };

        // Lyrics button: a speech bubble with two lines of text.
        if (m.lyricsCapable) {
            const ui::Rect r = ui::npLyricsRect();
            const bool on = m.lyricsOpen && avail, dis = !avail;
            roundButton(r, on, dis, m.pressedButton == (int)ui::NpButton::Lyrics, 3300);
            const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, s = std::min(r.w, r.h) * 0.27f, th = 2.4f;
            const ImU32 ic = iconColor(on, dis);
            M3eFxScope fxs;
            if (g_m3e && !dis) fxs.begin(3300u, true, {0.16f, 0.0f, 0.0f, 0.0f, 0.0f, true}, r.w, cx, cy, (uint32_t)on + 2u * evPress[(int)ui::NpButton::Lyrics]);
            ImDrawList* d = dl();
            d->AddRect(ImVec2(cx - s * 1.1f, cy - s * 0.85f), ImVec2(cx + s * 1.1f, cy + s * 0.6f), ic, s * 0.35f, 0, th);
            d->AddTriangleFilled(ImVec2(cx - s * 0.6f, cy + s * 0.55f), ImVec2(cx - s * 0.1f, cy + s * 0.55f), ImVec2(cx - s * 0.75f, cy + s * 1.1f), ic);
            d->AddLine(ImVec2(cx - s * 0.6f, cy - s * 0.35f), ImVec2(cx + s * 0.6f, cy - s * 0.35f), ic, th);
            d->AddLine(ImVec2(cx - s * 0.6f, cy + s * 0.05f), ImVec2(cx + s * 0.2f, cy + s * 0.05f), ic, th);
            fxs.end();
        }

        // Sync switch: left of the lyrics, a clock; lit while the lyrics follow the song, greyed when the lyrics
        // carry no timing at all.
        if (lyrT > 0.004f && avail) {
            const int v0 = dl()->VtxBuffer.Size;
            const ui::Rect r = ui::npLyricsSyncRect();
            const bool on = hasSync && m.lyricsSync, dis = !hasSync;
            roundButton(r, on, dis, m.pressedButton == (int)ui::NpButton::LyricsSync, 3301);
            const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, s = std::min(r.w, r.h) * 0.27f, th = 2.4f;
            const ImU32 ic = iconColor(on, dis);
            M3eFxScope fxs;
            if (g_m3e && !dis) fxs.begin(3301u, true, {0.12f, 0.0f, 0.0f, 6.2832f, 0.0f, true}, r.w, cx, cy, (uint32_t)on + 2u * evPress[(int)ui::NpButton::LyricsSync]);
            ImDrawList* d = dl();
            d->AddCircle(ImVec2(cx, cy), s * 1.15f, ic, 28, th);
            d->AddLine(ImVec2(cx, cy), ImVec2(cx, cy - s * 0.75f), ic, th);
            d->AddLine(ImVec2(cx, cy), ImVec2(cx + s * 0.5f, cy + s * 0.2f), ic, th);
            fxs.end();
            const char* cap = !hasSync ? "Plain only" : (m.lyricsSync ? "Synced" : "Plain");
            const ImVec2 cs = font()->CalcTextSizeA(15.0f, 1e9f, 0.0f, cap);
            d->AddText(font(), 15.0f, ImVec2(cx - cs.x * 0.5f, r.y + r.h + 7.0f), on ? accentMain() : col::MUTED, cap);
            fadeVertices(d, v0, lyrT);
        }
    }

    drawQueueButton(m);
    drawHints(m.hints, 110.0f, layout::FOOTER_Y);
    drawQueueFlyout(m);   // (popups: on the foreground list, above everything)
    drawQueueBrowser(m);
    drawVideoSettings(m);
}

// --- music visualizer ---

void drawMusicVisualizer(const VisualizerModel& m) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);

    text(TEXT_S, 60.0f, 108.0f, accentMain(), m.paused ? "PAUSED" : "NOW PLAYING");
    text(TEXT_M, 60.0f, 140.0f, col::TEXT, m.title, W - 120.0f);

    // The picture, centred, leaving room for the title above and hints below.
    const int n = m.bandCount > 0 ? std::min(m.bandCount, VisualizerModel::MAX_BANDS) : 0;
    if (n > 0) {
        const float left = 60.0f, right = W - 60.0f;
        const float top = 240.0f, bottom = layout::FOOTER_Y - 60.0f;
        const float areaH = bottom - top;
        auto lvl = [&](int i) {
            float level = m.bands[std::max(0, std::min(n - 1, i))];
            return level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
        };
        // Low bands lean toward the deep accent, high bands toward the secondary accent: a cheap per-bar
        // gradient in the same two-colour palette as drawLogo and the Now Playing art.
        auto bandColor = [&](float t) { return lerpColor(accentDeep(), accentSecond(), std::max(0.0f, std::min(1.0f, t))); };
        const int style = ((m.style % VisualizerModel::STYLE_COUNT) + VisualizerModel::STYLE_COUNT) % VisualizerModel::STYLE_COUNT;
        const float gap = 18.0f;
        const float barW = (right - left - gap * (n - 1)) / (float)n;
        const float midY = (top + bottom) * 0.5f;
        if (style == 0) { // Bars
            for (int i = 0; i < n; i++) {
                // A small floor so a quiet passage still shows a sliver of every bar rather than looking dead.
                const float h = areaH * (0.03f + 0.97f * lvl(i));
                const float x0 = left + i * (barW + gap), x1 = x0 + barW;
                gradientRoundRect(x0, bottom - h, x1, bottom, g_m3e ? std::min(barW * 0.5f, h * 0.5f) : std::min(10.0f, barW * 0.3f),
                                  accentMain(), bandColor((float)i / (float)std::max(1, n - 1)));
            }
        } else if (style == 1) { // Terrain (3D): the spectrum as a ridge landscape, each moment rolling away into the distance
            static const int ROWS = 11;
            static float hist[ROWS][VisualizerModel::MAX_BANDS];
            static double lastPush = -1.0;
            const double nowT = ImGui::GetTime();
            if (lastPush < 0.0 || nowT - lastPush > 0.075 || nowT < lastPush) { // a new row about 13 times a second
                for (int r = ROWS - 1; r > 0; r--) for (int i = 0; i < VisualizerModel::MAX_BANDS; i++) hist[r][i] = hist[r - 1][i];
                for (int i = 0; i < VisualizerModel::MAX_BANDS; i++) hist[0][i] = i < n ? lvl(i) : 0.0f;
                lastPush = nowT;
            }
            const float cx = (left + right) * 0.5f, halfFront = (right - left) * 0.5f;
            const float yFront = bottom, yBack = top + areaH * 0.30f;
            for (int r = ROWS - 1; r >= 0; r--) { // far to near, so nearer rows cover the ones behind
                const float t = (float)r / (float)(ROWS - 1);
                const float persp = 1.0f - 0.55f * t;                       // things shrink with distance
                const float y = yFront + (yBack - yFront) * std::pow(t, 0.85f);
                const float halfW = halfFront * persp;
                const float hMax = areaH * 0.46f * persp;
                ImVec2 ridge[VisualizerModel::MAX_BANDS];
                for (int i = 0; i < n; i++) {
                    const float u = n > 1 ? (float)i / (float)(n - 1) : 0.5f;
                    const float level = std::max(0.0f, std::min(1.0f, hist[r][i]));
                    ridge[i] = ImVec2(cx + (u - 0.5f) * 2.0f * halfW, y - hMax * (0.03f + 0.97f * level));
                }
                for (int i = 0; i + 1 < n; i++) {
                    const float u = (float)i / (float)std::max(1, n - 1);
                    const ImU32 body = lerpColor(bandColor(u), col::BG, 0.25f + 0.65f * t); // fades into the backdrop with distance
                    dl()->AddQuadFilled(ridge[i], ridge[i + 1], ImVec2(ridge[i + 1].x, y), ImVec2(ridge[i].x, y), body);
                }
                const float thick = std::max(1.2f, 3.2f * persp);
                dl()->AddPolyline(ridge, n, lerpColor(accentSecond(), col::TEXT, 0.35f * (1.0f - t)), 0, thick);
            }
        } else if (style == 2) { // Segments: stacks of small blocks, like an LED level meter
            const int segs = 16;
            const float segGap = 5.0f, segH = (areaH - segGap * (segs - 1)) / (float)segs;
            for (int i = 0; i < n; i++) {
                const float x0 = left + i * (barW + gap), x1 = x0 + barW;
                const int lit = (int)std::ceil(lvl(i) * (float)segs - 0.001f);
                for (int sI = 0; sI < segs; sI++) {
                    const float y1 = bottom - sI * (segH + segGap), y0 = y1 - segH;
                    const bool on = sI < lit;
                    const ImU32 c = on ? lerpColor(accentDeep(), accentSecond(), (float)sI / (float)(segs - 1)) : col::PANEL;
                    dl()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), c, g_m3e ? segH * 0.5f : 3.0f);
                }
            }
        } else if (style == 3) { // Radial: bars around a circle, mirrored left / right so it stays balanced
            const float cx = W * 0.5f, cy = midY;
            const float r0 = areaH * 0.20f, rMax = areaH * 0.5f - 6.0f;
            const int total = n * 2;
            float avg = 0.0f;
            for (int i = 0; i < n; i++) avg += lvl(i);
            avg /= (float)n;
            // a soft disc that breathes with the overall level
            dl()->AddCircleFilled(ImVec2(cx, cy), r0 * (0.85f + 0.35f * avg), (accentDeep() & 0x00FFFFFFu) | 0x60000000u, 48);
            const float th = std::max(4.0f, (6.2831853f * r0) / (float)total * 0.62f);
            for (int k = 0; k < total; k++) {
                const int band = k < n ? k : total - 1 - k; // low bands at the top, spreading down both sides
                const float a = -1.5707963f + (6.2831853f * (float)k) / (float)total;
                const float len = (rMax - r0) * (0.04f + 0.96f * lvl(band));
                const float ca = std::cos(a), sa = std::sin(a);
                dl()->AddLine(ImVec2(cx + ca * r0, cy + sa * r0), ImVec2(cx + ca * (r0 + len), cy + sa * (r0 + len)),
                              bandColor((float)band / (float)std::max(1, n - 1)), th);
                dl()->AddCircleFilled(ImVec2(cx + ca * (r0 + len), cy + sa * (r0 + len)), th * 0.5f,
                                      bandColor((float)band / (float)std::max(1, n - 1)), 10);
            }
        } else { // Wave: a smooth curve through the levels, filled underneath
            const int steps = 96;
            static ImVec2 pts[97];
            for (int s2 = 0; s2 <= steps; s2++) {
                const float u = (float)s2 / (float)steps * (float)(n - 1);
                const int i1 = (int)std::floor(u), i0 = i1 - 1, i2 = i1 + 1, i3 = i1 + 2;
                const float t = u - (float)i1;
                // Catmull-Rom between the four nearest levels
                const float p0 = lvl(i0), p1 = lvl(i1), p2 = lvl(std::min(i2, n - 1)), p3 = lvl(i3);
                float v = 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                                  (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
                v = std::max(0.0f, std::min(1.0f, v));
                pts[s2] = ImVec2(left + (right - left) * (float)s2 / (float)steps, bottom - areaH * (0.03f + 0.97f * v));
            }
            for (int s2 = 0; s2 < steps; s2++) { // fill: one quad per step, tinted along the width
                const ImU32 c = (bandColor((float)s2 / (float)steps) & 0x00FFFFFFu) | 0x70000000u;
                dl()->AddQuadFilled(pts[s2], pts[s2 + 1], ImVec2(pts[s2 + 1].x, bottom), ImVec2(pts[s2].x, bottom), c);
            }
            dl()->AddPolyline(pts, steps + 1, accentMain(), ImDrawFlags_None, g_m3e ? 6.0f : 4.0f);
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
    if (themed()) glassCircle(dl(), cx, cy, 64.0f, GK::Overlay, false);
    else dl()->AddCircleFilled(ImVec2(cx, cy), 64.0f, IM_COL32(0, 0, 0, 0x90), 48);
    if (g_m3e) {
        m3eLoader(dl(), cx, cy, 34.0f, accentMain(), ImGui::GetTime()); // M3E loading indicator: a morphing cookie
    } else {
        dl()->AddCircle(ImVec2(cx, cy), ring, IM_COL32(0xFF, 0xFF, 0xFF, 0x40), 32, thick);
        const float a0 = (float)std::fmod(ImGui::GetTime() * 6.2, (double)PI2);
        dl()->PathArcTo(ImVec2(cx, cy), ring, a0, a0 + 1.9f, 20);
        dl()->PathStroke(col::WHITE, ImDrawFlags_None, thick);
    }

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
    if (themed()) themedSurface(dl(), bx, by, bx + s.x + 32.0f, by + s.y + 14.0f, gr(12.0f), GK::Overlay, false);
    else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + s.x + 32.0f, by + s.y + 14.0f), IM_COL32(0, 0, 0, 0xB0), 12.0f);
    text(TEXT_S, bx + 16.0f, by + 7.0f, onOverlay(), buf);
}

void drawVideoHud(const VideoHudModel& m) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;

    if (m.buffering && !m.paused) drawBufferingIndicator(m.bufferedSeconds, m.bufferingTargetSeconds);

    if (m.paused) {
        float cx = W * 0.5f, cy = H * 0.45f;
        if (themed()) glassCircle(dl(), cx, cy, 64.0f, GK::Overlay, false);
        else dl()->AddCircleFilled(ImVec2(cx, cy), 64.0f, IM_COL32(0, 0, 0, 0x90), 48);
        dl()->AddRectFilled(ImVec2(cx - 24, cy - 30), ImVec2(cx - 7, cy + 30), onOverlay(), g_m3e ? 8.0f : 4.0f);
        dl()->AddRectFilled(ImVec2(cx + 7, cy - 30), ImVec2(cx + 24, cy + 30), onOverlay(), g_m3e ? 8.0f : 4.0f);
    }

    if (m.seekDelta != 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%d s   ->   %s", m.seekDelta > 0 ? "+" : "", (int)m.seekDelta,
                 formatTime(m.seekTarget).c_str());
        ImVec2 s = textSize(TEXT_L, buf);
        float bx = (W - s.x) * 0.5f - 28, by = 60.0f;
        if (themed()) themedSurface(dl(), bx, by, bx + s.x + 56, by + s.y + 28, gr(16.0f), GK::Overlay, false);
        else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + s.x + 56, by + s.y + 28), IM_COL32(0, 0, 0, 0xB0), 16.0f);
        text(TEXT_L, bx + 28, by + 14, onOverlay(), buf);
    }

    if (!m.notice.empty()) {
        ImVec2 ns = textSize(TEXT_M, m.notice);
        float bx = (W - ns.x) * 0.5f - 24, by = 60.0f;
        if (m.seekDelta != 0.0) by += 70;
        if (themed()) themedSurface(dl(), bx, by, bx + ns.x + 48, by + ns.y + 24, gr(14.0f), GK::Overlay, false);
        else dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + ns.x + 48, by + ns.y + 24), IM_COL32(0, 0, 0, 0xB0), 14.0f);
        text(TEXT_M, bx + 24, by + 12, onOverlay(), m.notice);
    }

    if (m.trackMenu) {
        // Audio & subtitles panel, right side.
        const float pw = 560, px = W - pw - 50, py = 120, rowH = 74;
        if (themed()) themedSurface(dl(), px, py, px + pw, py + 70 + 2 * (rowH + 10) + 20, gr(18.0f), GK::Overlay, false);
        else dl()->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 70 + 2 * (rowH + 10) + 20), IM_COL32(0x14, 0x16, 0x1A, 0xEE), 18.0f);
        text(TEXT_L, px + 28, py + 22, onOverlay(), "Audio & subtitles");
        const char* labels[2] = {"Audio", "Subtitles"};
        const std::string values[2] = {m.audioLabel, m.subtitleLabel};
        for (int i = 0; i < 2; i++) {
            float ry = py + 76 + i * (rowH + 10);
            bool sel = (i == m.trackRow);
            if (themed()) themedSurface(dl(), px + 18, ry, px + pw - 18, ry + rowH, gr(12.0f), sel ? GK::RowFocus : GK::Row, false);
            else dl()->AddRectFilled(ImVec2(px + 18, ry), ImVec2(px + pw - 18, ry + rowH), sel ? accentDeep() : col::PANEL, 12.0f);
            const ImU32 rowText = sel ? onFocus() : onOverlay();
            text(TEXT_S, px + 36, ry + 10, sel ? onFocus() : col::MUTED, labels[i]);
            text(TEXT_M, px + 60, ry + 36, rowText, values[i], pw - 150);
            text(TEXT_M, px + 36, ry + 36, rowText, "<");
            textRight(TEXT_M, px + pw - 36, ry + 36, rowText, ">");
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
    drawProgress(x, y, w, 6.0f, m.positionSeconds, m.durationSeconds, m.live, !m.paused);
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
            if (g_m3e) {
                // M3E buttons: the primary one is filled (primary), the others are tonal; focus morphs them to a pill.
                m3eButton(r, f.primary ? accentMain() : g_m3c.secondaryContainer, focus);
            } else {
                surfaceRect(r, fill, 14.0f, (f.primary || focus) ? GK::ButtonHot : GK::Button);
            }
            if (!themed() && focus && !f.primary) {
                dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
            }
            ImVec2 ts = textSize(TEXT_M, f.label);
            g_bold = g_m3e;
            text(TEXT_M, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - TEXT_M) * 0.5f - 1,
                 g_m3e ? (f.primary ? onMain() : g_m3c.onSecondaryContainer) : col::WHITE, f.label);
            g_bold = false;
            continue;
        }
        surfaceRect(r, focus ? col::PANEL_HI : col::PANEL, 14.0f, focus ? GK::ButtonHot : GK::Button);
        if (!themed() && focus) dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
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
    if (g_m3e) {
        // M3E snackbar: a tonal pill, no outline.
        themedSurface(fg, x, y, x + w, y + h, h * 0.5f, GK::Button, false, alpha);
    } else if (g_glass) {
        glassSurface(fg, x, y, x + w, y + h, h * 0.5f, GK::Button, true, alpha);
    } else {
        fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x2C, 0x31, 0x3A, (int)(245 * alpha)), h * 0.5f);
        fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x00, 0xA4, 0xDC, (int)(255 * alpha)), h * 0.5f, 0, 2.0f);
    }
    fg->AddText(font(), TEXT_M, ImVec2(x + 24, y + (h - ts.y) * 0.5f),
                g_m3e ? ((col::TEXT & 0x00FFFFFFu) | ((ImU32)(255 * alpha) << 24)) : IM_COL32(0xEC, 0xEE, 0xF1, (int)(255 * alpha)),
                t.c_str());
}

void drawProgressToast(const std::string& t, float fraction, float y) {
    fraction = std::min(1.0f, std::max(0.0f, fraction));
    ImVec2 ts = textSize(TEXT_M, t);
    const float barW = std::max(220.0f, ts.x), barH = 6.0f;
    const float w = barW + 48, h = 68, x = (layout::SCREEN_W - w) * 0.5f;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (themed()) {
        themedSurface(fg, x, y, x + w, y + h, 24.0f, GK::Button, !g_m3e, 1.0f);
    } else {
        fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x2C, 0x31, 0x3A, 245), 24.0f);
        fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x00, 0xA4, 0xDC, 255), 24.0f, 0, 2.0f);
    }
    fg->AddText(font(), TEXT_M, ImVec2(x + (w - ts.x) * 0.5f, y + 10), g_m3e ? col::TEXT : IM_COL32(0xEC, 0xEE, 0xF1, 255), t.c_str());
    const float bx = x + 24, by = y + h - 18;
    fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + barH), col::TRACK, barH * 0.5f);
    if (fraction > 0.0f)
        fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW * fraction, by + barH), accentMain(), barH * 0.5f);
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
    if (!g_m3e) fg->AddRectFilled(ImVec2(0, 0), ImVec2(layout::SCREEN_W, layout::SCREEN_H), IM_COL32(0, 0, 0, 0xB0));
    const int n = (int)m.options.size();
    Rect first = choiceRow(0, n), last = choiceRow(n > 0 ? n - 1 : 0, n);
    float cardX = first.x - 40, cardW = first.w + 80;
    float cardY = first.y - 130, cardH = (last.bottom() - cardY) + 40;
    if (m.art) {
        cardX -= 130;
        cardW += 130;
    }
    int entV = 0; float entS = 1.0f;
    if (g_m3e) m3eEntranceBegin(fg, m3e::hashRect(cardX, cardY, cardW, cardH), entV, entS); // the card springs in
    if (g_m3e) m3eDialogScrim(fg, m3e::entranceAlpha(entS), entV); // the screen behind blurs and takes the accent wash
    if (themed()) themedSurface(fg, cardX, cardY, cardX + cardW, cardY + cardH, gr(18.0f), g_m3e ? GK::Dialog : GK::Panel, true);
    else fg->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH), col::PANEL, 18.0f);
    if (g_m3e) { /* (no accent strip in M3E) */ }
    else if (g_glass) glassBand(fg, cardX, cardY, cardX + cardW, cardY + cardH, gr(18.0f), cardY, cardY + 6.0f, accentMain());
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
        if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(14.0f), sel ? GK::ButtonHot : GK::Button, true);
        else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), sel ? accentDeep() : col::PANEL_HI, 14.0f);
        ImVec2 ts = textSize(TEXT_M, m.options[(size_t)i]);
        const ImU32 optText = g_m3e ? (sel ? onFocus() : col::TEXT) : col::WHITE;
        if (g_m3e && sel) fg->AddText(font(), TEXT_M, ImVec2(r.x + (r.w - ts.x) * 0.5f + 0.7f, r.y + (r.h - ts.y) * 0.5f), optText, m.options[(size_t)i].c_str());
        fg->AddText(font(), TEXT_M, ImVec2(r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - ts.y) * 0.5f), optText,
                    m.options[(size_t)i].c_str());
    }
    // Hints under the card (drawn on top, like the card).
    float hx = cardX;
    const float rr = 14.0f;
    for (const Hint& hint : m.hints) {
        float bw = std::max(2 * rr, textSize(TEXT_S, hint.button).x + 14.0f);
        float hy = cardY + cardH + 18;
        if (themed()) themedSurface(fg, hx, hy, hx + bw, hy + 2 * rr, rr, GK::Button, true);
        else fg->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + bw, hy + 2 * rr), col::PANEL_HI, rr);
        ImVec2 bs = textSize(TEXT_S, hint.button);
        fg->AddText(font(), TEXT_S, ImVec2(hx + (bw - bs.x) * 0.5f, hy + rr - bs.y * 0.5f), col::TEXT, hint.button.c_str());
        hx += bw + 8;
        fg->AddText(font(), TEXT_S, ImVec2(hx, hy + rr - bs.y * 0.5f), col::MUTED, hint.label.c_str());
        hx += textSize(TEXT_S, hint.label).x + 26;
    }
    if (g_m3e) m3eEntranceEnd(fg, entV, entS, cardX + cardW * 0.5f, cardY + cardH * 0.5f);
}

void drawTrackPrompt(const TrackPromptModel& m) {
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);
    const Rect card = trackPromptCard();
    surfaceRect(card, col::PANEL, 18.0f, GK::Panel);
    if (!g_m3e) {
        if (g_glass) glassBand(dl(), card.x, card.y, card.right(), card.bottom(), gr(18.0f), card.y, card.y + 6.0f, accentMain());
        else dl()->AddRectFilled(ImVec2(card.x, card.y), ImVec2(card.right(), card.y + 6), accentMain(), gr(18.0f), ImDrawFlags_RoundCornersTop);
    }
    text(TEXT_L, card.x + 30.0f, card.y + 28.0f, col::TEXT, m.title, card.w - 60.0f);
    text(TEXT_S, card.x + 30.0f, card.y + 74.0f, col::MUTED, m.subtitle, card.w - 60.0f);

    const char* labels[2] = {"Audio", "Subtitles"};
    const std::string values[2] = {m.audioLabel, m.subtitleLabel};
    for (int i = 0; i < 2; i++) {
        const Rect r = trackPromptSlot(i);
        const bool sel = (m.selected == i);
        surfaceRect(r, sel ? accentDeep() : col::PANEL_HI, 14.0f, sel ? GK::RowFocus : GK::Row);
        const ImU32 rowText = sel ? onFocus() : col::TEXT;
        text(TEXT_S, r.x + 22.0f, r.y + 9.0f, sel ? onFocus() : col::MUTED, labels[i]);
        text(TEXT_M, r.x + 22.0f, r.y + 35.0f, rowText, "<");
        textRight(TEXT_M, r.right() - 22.0f, r.y + 35.0f, rowText, ">");
        const float vw = r.w - 130.0f;
        const ImVec2 vs = textSize(TEXT_M, fitText(font(), TEXT_M, values[i], vw));
        text(TEXT_M, r.x + (r.w - vs.x) * 0.5f, r.y + 35.0f, rowText, values[i], vw);
    }

    const char* buttons[2] = {"OK", "Always use default"};
    for (int i = 0; i < 2; i++) {
        const Rect r = trackPromptSlot(2 + i);
        const bool focus = (m.selected == 2 + i), primary = (i == 0);
        if (g_m3e) {
            m3eButton(r, primary ? accentMain() : g_m3c.secondaryContainer, focus);
        } else {
            const ImU32 fill = primary ? (focus ? accentMain() : accentDeep()) : (focus ? col::PANEL_HI : col::PANEL);
            surfaceRect(r, fill, 14.0f, (primary || focus) ? GK::ButtonHot : GK::Button);
            if (!themed() && focus && !primary) dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
        }
        const ImVec2 ts = textSize(TEXT_M, buttons[i]);
        g_bold = g_m3e;
        text(TEXT_M, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - TEXT_M) * 0.5f - 1,
             g_m3e ? (primary ? onMain() : g_m3c.onSecondaryContainer) : col::WHITE, buttons[i]);
        g_bold = false;
    }
    drawHints(m.hints, card.x, card.bottom() + 22.0f);
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
    // Every triangle the loader accepts (kModelMaxTriangles) must be drawn: this list is sorted far to near,
    // so dropping any would cut away the farthest part of the model along a straight line that sweeps
    // across it as it turns (what a 13000 cap did to the 15-16k triangle Meshy controllers, whose
    // double-sided materials keep every triangle). A single draw command is kept under the 16-bit vertex
    // index range below instead; the draw list switches to a new vertex offset by itself beyond that.
    const size_t kMaxDrawn = kModelMaxTriangles;
    if (order.size() > kMaxDrawn) order.erase(order.begin(), order.end() - (std::ptrdiff_t)kMaxDrawn);
    const size_t kRunMax = 20000; // triangles per PrimReserve: 60000 vertices < 65536

    auto texOf = [&](uint32_t t) -> uint64_t {
        const int ti = m.materials[m.triMat[t]].tex;
        return (ti >= 0 && (size_t)ti < s.tex.size()) ? s.tex[(size_t)ti] : 0;
    };
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    bool pushed = false;
    for (size_t i = 0; i < order.size();) {
        const uint64_t h = texOf(order[i].second);
        size_t j = i;
        while (j < order.size() && j - i < kRunMax && texOf(order[j].second) == h) j++;
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
    int entV = 0; float entS = 1.0f;
    if (g_glass) glassSurface(fg, 0, 0, W, H, 0.0f, GK::Overlay, true, 0.55f);
    if (!g_m3e) fg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, g_glass ? 0x90 : 0xC8));
    if (g_m3e) m3eEntranceBegin(fg, m3e::hashRect(c.x, c.y, c.w, c.h), entV, entS); // the card springs in (scale + fade)
    if (g_m3e) m3eDialogScrim(fg, m3e::entranceAlpha(entS), entV);

    // The card.
    if (g_m3e) {
        themedSurface(fg, c.x, c.y, c.right(), c.bottom(), gr(22.0f), GK::Dialog, true);
    } else if (g_glass) {
        glassSurface(fg, c.x, c.y, c.right(), c.bottom(), gr(22.0f), GK::Panel, true);
        // A faint tint inside the glass so the text stays readable over whatever is behind it.
        fg->AddRectFilled(ImVec2(c.x + 2, c.y + 2), ImVec2(c.right() - 2, c.bottom() - 2), bgTint(col::BG, 0.40f), gr(22.0f));
    } else {
        fg->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.right(), c.bottom()), col::PANEL, 22.0f);
    }
    if (g_m3e) { /* (no accent strip in M3E) */ }
    else if (g_glass) glassBand(fg, c.x, c.y, c.right(), c.bottom(), gr(22.0f), c.y, c.y + 6.0f, accentMain());
    else fg->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.right(), c.y + 6), accentMain(), 22.0f, ImDrawFlags_RoundCornersTop);

    // Header: title, controller tabs.
    fg->AddText(font(), TEXT_L, ImVec2(c.x + 28, c.y + 18), col::TEXT, "Help");
    if (g_m3e) fg->AddText(font(), TEXT_L, ImVec2(c.x + 28.7f, c.y + 18), col::TEXT, "Help"); // (heavier headline)
    for (size_t i = 0; i < g.tabs.size(); i++) {
        const Rect& t = g.tabs[i];
        const bool sel = ((int)i == m.tab);
        if (themed()) themedSurface(fg, t.x, t.y, t.right(), t.bottom(), t.h * 0.5f, sel ? GK::ButtonHot : GK::Button, true);
        else fg->AddRectFilled(ImVec2(t.x, t.y), ImVec2(t.right(), t.bottom()), sel ? accentDeep() : col::PANEL_HI, t.h * 0.5f);
        const ImVec2 ts = textSize(TEXT_S, m.tabs[i]);
        fg->AddText(font(), TEXT_S, ImVec2(t.x + (t.w - ts.x) * 0.5f, t.y + (t.h - ts.y) * 0.5f), sel ? onFocus() : col::MUTED,
                    m.tabs[i].c_str());
    }

    // Stage: the rotating model, then its name and a note.
    const float stageR = g_m3e ? 28.0f : 18.0f;
    fg->AddRectFilled(ImVec2(g.stage.x, g.stage.y), ImVec2(g.stage.right(), g.stage.bottom()), bgTint(col::BG, g_m3e ? 0.70f : 0.45f), stageR);
    if (!g_m3e) fg->AddRect(ImVec2(g.stage.x, g.stage.y), ImVec2(g.stage.right(), g.stage.bottom()), bgTint(col::TEXT, 0.10f), stageR, 0, 1.0f);
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
            if (themed()) themedSurface(fg, g.list.x, y + 1, g.list.right() - 12.0f, y + h - 1, gr(10.0f), GK::RowFocus, false);
            else fg->AddRectFilled(ImVec2(g.list.x, y + 1), ImVec2(g.list.right() - 12.0f, y + h - 1), col::PANEL_HI, 10.0f);
        }
        const float cw = std::max(34.0f, textSize(TEXT_S, r.button).x + 22.0f), ch = 26.0f, cy = y + (h - ch) * 0.5f;
        fg->AddRectFilled(ImVec2(chipX + 6, cy), ImVec2(chipX + 6 + cw, cy + ch), sel ? (g_m3e ? accentMain() : accentDeep()) : col::PANEL_HI, ch * 0.5f);
        const ImVec2 bs = textSize(TEXT_S, r.button);
        fg->AddText(font(), TEXT_S, ImVec2(chipX + 6 + (cw - bs.x) * 0.5f, cy + (ch - bs.y) * 0.5f), sel ? (g_m3e ? onMain() : onFocus()) : col::TEXT,
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
        if (themed()) themedSurface(fg, hx, hy, hx + bw, hy + 2 * rr, rr, GK::Button, true);
        else fg->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + bw, hy + 2 * rr), col::PANEL_HI, rr);
        const ImVec2 bs = textSize(TEXT_S, hint.button);
        fg->AddText(font(), TEXT_S, ImVec2(hx + (bw - bs.x) * 0.5f, hy + rr - bs.y * 0.5f), col::TEXT, hint.button.c_str());
        hx += bw + 8;
        fg->AddText(font(), TEXT_S, ImVec2(hx, hy + rr - bs.y * 0.5f), col::MUTED, hint.label.c_str());
        hx += textSize(TEXT_S, hint.label).x + 26;
    }
    if (g_m3e) m3eEntranceEnd(fg, entV, entS, c.x + c.w * 0.5f, c.y + c.h * 0.5f);
}

void drawUpNext(const UpNextModel& m) {
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);
    const float artW = 300, artH = 420, ax = 150, ay = 150;
    if (m.art && g_m3e) {
        // Square centre crop of the poster inside the cookie.
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (m.artAspect > 0.0f && m.artAspect < 1.0f) { const float v = (1.0f - m.artAspect) * 0.5f; uv0.y = v; uv1.y = 1.0f - v; }
        else if (m.artAspect > 1.0f) { const float u = (1.0f - 1.0f / m.artAspect) * 0.5f; uv0.x = u; uv1.x = 1.0f - u; }
        m3eCookieImage(dl(), (ImTextureID)m.art, ax + artW * 0.5f, ay + 60 + artW * 0.375f, artW * 0.48f, 9, 0.085f,
                       (float)ImGui::GetTime() * 0.12f, uv0, uv1, col::WHITE);
    } else if (m.art) {
        layout::Fit f = layout::fitInside(ax, ay, artW, artH, m.artAspect);
        dl()->AddImageRounded(ImTextureRef((ImTextureID)m.art), ImVec2(f.x, f.y), ImVec2(f.x + f.w, f.y + f.h),
                              ImVec2(0, 0), ImVec2(1, 1), col::WHITE, g_m3e ? 28.0f : 14.0f);
    } else if (g_m3e) {
        m3eCookie(dl(), ax + artW * 0.5f, ay + 60 + artW * 0.375f, artW * 0.42f, 9, 0.085f, (float)ImGui::GetTime() * 0.12f, accentDeep());
        g_iconActive = true;
        drawIcon(Icon::Episode, ax + artW * 0.5f, ay + 60 + artW * 0.375f, 110.0f, g_m3c.onPrimaryContainer);
        g_iconActive = false;
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
#include "m3e_draw.inc"
#include "profiles_ui.inc"
#include "library_ui.inc"
#include "queue_ui.inc"
