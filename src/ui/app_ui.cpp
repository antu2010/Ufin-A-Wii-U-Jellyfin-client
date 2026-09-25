#include "app_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

// --- palette ---

namespace col {
static const ImU32 BG          = IM_COL32(0x14, 0x16, 0x1A, 0xFF);
static const ImU32 SIDEBAR     = IM_COL32(0x1B, 0x1E, 0x24, 0xFF);
static const ImU32 PANEL       = IM_COL32(0x23, 0x27, 0x2E, 0xFF);
static const ImU32 PANEL_HI    = IM_COL32(0x2C, 0x31, 0x3A, 0xFF);


static const ImU32 TEXT        = IM_COL32(0xEC, 0xEE, 0xF1, 0xFF);
static const ImU32 MUTED       = IM_COL32(0x9A, 0xA0, 0xA8, 0xFF);
static const ImU32 DIM         = IM_COL32(0x5F, 0x65, 0x6E, 0xFF);
static const ImU32 TRACK       = IM_COL32(0x3A, 0x3F, 0x48, 0xFF);
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
static bool g_ambient = false;
static bool g_snow = false;

static ImU32 accentMain() { return g_accent.main; }
static ImU32 accentDeep() { return g_accent.deep; }
static ImU32 accentSecond() { return g_accent.second; }

static ImU32 rgb(uint32_t v, int a = 0xFF) { return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, a); }


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
        case Accent::Rainbow: return "Rainbow";
        default:              return "";
    }
}

void setAccent(Accent a) {
    g_accentStyle = a;
    g_rainbow = (a == Accent::Rainbow);
    switch (a) {
        case Accent::Purple: g_accent = {rgb(0xAA5CC3), rgb(0x7E3F99), rgb(0x00A4DC)}; break;
        case Accent::Green:  g_accent = {rgb(0x2ECC71), rgb(0x1E8C4E), rgb(0x00A4DC)}; break;
        case Accent::Orange: g_accent = {rgb(0xFF9F43), rgb(0xC7702A), rgb(0xE84393)}; break;
        case Accent::Pink:   g_accent = {rgb(0xFF6BB5), rgb(0xC2447F), rgb(0x7E57C2)}; break;
        case Accent::Rainbow: tickTheme(0.0); break;
        default:             g_accent = {rgb(0x00A4DC), rgb(0x0B7FB0), rgb(0xAA5CC3)}; break;
    }
}

void tickTheme(double time) {
    if (!g_rainbow) return;
    float h = (float)std::fmod(time * 0.06, 1.0);
    g_accent = {hsv(h, 0.72f, 0.95f), hsv(h, 0.78f, 0.66f), hsv(h + 0.33f, 0.6f, 0.9f)};
}

void setAmbientBackground(bool on) { g_ambient = on; }

// Soft drifting glows: each blob is a stack of faint circles, which reads
// as one blurred shape.
static void drawAmbient() {
    if (!g_ambient) return;
    const double t = ImGui::GetTime();
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
static void gradientRoundRect(float x0, float y0, float x1, float y1, float rounding, ImU32 a, ImU32 b) {
    ImDrawList* d = dl();
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
        case Icon::None:
            break;
    }
}

// --- shared chrome ---

static void drawHints(const std::vector<Hint>& hints, float x, float y) {
    const float r = 14.0f;
    for (const Hint& hint : hints) {
        float bw = std::max(2 * r, textSize(TEXT_S, hint.button).x + 14.0f);
        dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + 2 * r), col::PANEL_HI, r);
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

// --- browser ---

void drawBrowser(const BrowserModel& m) {
    rect({0, 0, layout::SCREEN_W, layout::SCREEN_H}, col::BG);
    drawAmbient();

    // Sidebar
    rect({0, 0, layout::SIDEBAR_W, layout::SCREEN_H}, col::SIDEBAR);
    drawLogo(30.0f, 40.0f);
    for (int i = 0; i < (int)m.sidebar.size(); i++) {
        Rect r = layout::sidebarItem(i);
        bool current = (i == m.sidebarSelected);
        if (current && m.sidebarFocused) {
            rect(r, accentDeep(), 12.0f);
        } else if (current) {
            rect(r, col::PANEL, 12.0f);
            dl()->AddRectFilled(ImVec2(r.x, r.y + 12), ImVec2(r.x + 4, r.bottom() - 12), accentMain(), 2.0f);
        }
        ImU32 c = (current && m.sidebarFocused) ? col::WHITE : (current ? col::TEXT : col::MUTED);
        drawIcon(m.sidebar[i].icon, r.x + 32, r.y + r.h * 0.5f, 26.0f, c);
        text(TEXT_M, r.x + 58, r.y + (r.h - TEXT_M) * 0.5f - 1, c, m.sidebar[i].label, r.w - 66);
    }
    if (!m.footnote.empty()) {
        text(TEXT_S - 2, layout::SIDEBAR_PAD + 4, layout::SCREEN_H - 44, col::DIM, m.footnote,
             layout::SIDEBAR_W - 2 * layout::SIDEBAR_PAD);
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
            if (sel && !m.sidebarFocused) {
                rect(r, accentDeep(), 12.0f);
            } else if (sel) {
                rect(r, col::PANEL, 12.0f);
                dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentDeep(), 12.0f, 0, 2.0f);
            } else {
                rect(r, col::PANEL, 12.0f);
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
}

// --- message card ---

void drawMessage(const MessageModel& m) {
    rect({0, 0, layout::SCREEN_W, layout::SCREEN_H}, col::BG);
    drawAmbient();
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

    rect({x, y, cardW, cardH}, col::PANEL, 18.0f);
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + cardW, y + 6), m.isError ? col::ERROR_RED : accentMain(), 18.0f,
                        ImDrawFlags_RoundCornersTop);

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
    rect({0, 0, layout::SCREEN_W, layout::SCREEN_H}, col::BG);
    drawAmbient();
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

    const float tx = ax + art + 60.0f;
    const float tw = layout::SCREEN_W - 90.0f - tx;
    float y = ay + 20.0f;
    text(TEXT_S, tx, y, accentMain(), m.paused ? "PAUSED" : "NOW PLAYING");
    y += 36.0f;
    dl()->AddText(font(), 40.0f, ImVec2(tx, y), col::TEXT, m.title.c_str(), nullptr, tw);
    y += std::min(100.0f, font()->CalcTextSizeA(40.0f, 1e9f, tw, m.title.c_str()).y) + 10.0f;
    text(TEXT_M, tx, y, col::MUTED, m.subtitle, tw);
    y += 60.0f;

    drawProgress(tx, y, tw, 8.0f, m.positionSeconds, m.durationSeconds, false);
    y += 22.0f;
    text(TEXT_S, tx, y, col::MUTED, formatTime(m.positionSeconds));
    if (m.durationSeconds > 0.0) textRight(TEXT_S, tx + tw, y, col::MUTED, formatTime(m.durationSeconds));
    y += 52.0f;

    if (!m.queueText.empty()) {
        text(TEXT_S, tx, y, col::TEXT, m.queueText, tw);
        y += 30.0f;
    }
    if (!m.nextText.empty()) text(TEXT_S, tx, y, col::MUTED, m.nextText, tw);

    drawHints(m.hints, 110.0f, layout::FOOTER_Y);
}

// --- video HUD ---

void drawVideoHud(const VideoHudModel& m) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;

    if (m.paused) {
        float cx = W * 0.5f, cy = H * 0.45f;
        dl()->AddCircleFilled(ImVec2(cx, cy), 64.0f, IM_COL32(0, 0, 0, 0x90), 48);
        dl()->AddRectFilled(ImVec2(cx - 24, cy - 30), ImVec2(cx - 7, cy + 30), col::WHITE, 4.0f);
        dl()->AddRectFilled(ImVec2(cx + 7, cy - 30), ImVec2(cx + 24, cy + 30), col::WHITE, 4.0f);
    }

    if (m.seekDelta != 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%d s   ->   %s", m.seekDelta > 0 ? "+" : "", (int)m.seekDelta,
                 formatTime(m.seekTarget).c_str());
        ImVec2 s = textSize(TEXT_L, buf);
        float bx = (W - s.x) * 0.5f - 28, by = 60.0f;
        dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + s.x + 56, by + s.y + 28), IM_COL32(0, 0, 0, 0xB0), 16.0f);
        text(TEXT_L, bx + 28, by + 14, col::WHITE, buf);
    }

    if (!m.notice.empty()) {
        ImVec2 ns = textSize(TEXT_M, m.notice);
        float bx = (W - ns.x) * 0.5f - 24, by = 60.0f;
        if (m.seekDelta != 0.0) by += 70;
        dl()->AddRectFilled(ImVec2(bx, by), ImVec2(bx + ns.x + 48, by + ns.y + 24), IM_COL32(0, 0, 0, 0xB0), 14.0f);
        text(TEXT_M, bx + 24, by + 12, col::WHITE, m.notice);
    }

    if (m.trackMenu) {
        // Audio & subtitles panel, right side.
        const float pw = 560, px = W - pw - 50, py = 120, rowH = 74;
        dl()->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 70 + 2 * (rowH + 10) + 20), IM_COL32(0x14, 0x16, 0x1A, 0xEE), 18.0f);
        text(TEXT_L, px + 28, py + 22, col::WHITE, "Audio & subtitles");
        const char* labels[2] = {"Audio", "Subtitles"};
        const std::string values[2] = {m.audioLabel, m.subtitleLabel};
        for (int i = 0; i < 2; i++) {
            float ry = py + 76 + i * (rowH + 10);
            bool sel = (i == m.trackRow);
            dl()->AddRectFilled(ImVec2(px + 18, ry), ImVec2(px + pw - 18, ry + rowH), sel ? accentDeep() : col::PANEL, 12.0f);
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
    rect({0, 0, layout::SCREEN_W, layout::SCREEN_H}, col::BG);
    drawAmbient();
    drawBrandPanel();

    text(TEXT_L + 6, layout::LOGIN_X, 70.0f, col::TEXT, "Sign in to Jellyfin");

    for (int i = 0; i < (int)m.rows.size(); i++) {
        const LoginField& f = m.rows[(size_t)i];
        Rect r = layout::loginRow(i);
        bool focus = (i == m.focused);
        if (f.isButton) {
            ImU32 fill = f.primary ? (focus ? accentMain() : accentDeep()) : (focus ? col::PANEL_HI : col::PANEL);
            rect(r, fill, 14.0f);
            if (focus && !f.primary) {
                dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
            }
            ImVec2 ts = textSize(TEXT_M, f.label);
            text(TEXT_M, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - TEXT_M) * 0.5f - 1, col::WHITE, f.label);
            continue;
        }
        rect(r, focus ? col::PANEL_HI : col::PANEL, 14.0f);
        if (focus) dl()->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
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
    rect({0, 0, layout::SCREEN_W, layout::SCREEN_H}, col::BG);
    drawAmbient();
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
    rect(card, col::PANEL, 18.0f);
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
    fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x2C, 0x31, 0x3A, (int)(245 * alpha)), h * 0.5f);
    fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0x00, 0xA4, 0xDC, (int)(255 * alpha)), h * 0.5f, 0, 2.0f);
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
    fg->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH), col::PANEL, 18.0f);
    fg->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + 6), accentMain(), 18.0f,
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
        fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), sel ? accentDeep() : col::PANEL_HI, 14.0f);
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
        fg->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + bw, hy + 2 * rr), col::PANEL_HI, rr);
        ImVec2 bs = textSize(TEXT_S, hint.button);
        fg->AddText(font(), TEXT_S, ImVec2(hx + (bw - bs.x) * 0.5f, hy + rr - bs.y * 0.5f), col::TEXT, hint.button.c_str());
        hx += bw + 8;
        fg->AddText(font(), TEXT_S, ImVec2(hx, hy + rr - bs.y * 0.5f), col::MUTED, hint.label.c_str());
        hx += textSize(TEXT_S, hint.label).x + 26;
    }
}

void drawUpNext(const UpNextModel& m) {
    rect({0, 0, layout::SCREEN_W, layout::SCREEN_H}, col::BG);
    drawAmbient();
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
