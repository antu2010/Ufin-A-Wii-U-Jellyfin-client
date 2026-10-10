// profiles_ui.inc -- the "Who's watching?" screen (see profiles_ui.h).
//
// Included at the END of app_ui.cpp, after `} // namespace ui`, because it is built from that file's
// private helpers (dl(), text(), surfaceRect(), drawHints(), col::, g_glass, ...). It is not compiled
// on its own.
#include "profiles_ui.h"

namespace ui {

uint32_t profileColor(int i) {
    static const uint32_t palette[kProfileColors] = {0x0A9BD4, 0xA25BC0, 0xE0483A, 0x27AE60,
                                                     0xF28A30, 0xE0438C, 0x16A596, 0x5568C0};
    return palette[((i % kProfileColors) + kProfileColors) % kProfileColors];
}

namespace {

// ---- layout (1280x720 space), shared by drawing and hit testing ----
const float PF_TILE_W = 190.0f;   // width of one profile column
const float PF_GAP = 28.0f;       // between columns
const float PF_D = 140.0f;        // picture diameter
const float PF_GROUND = 410.0f;   // y of the ground line: the bottom of a picture at rest
const float PF_NAME_Y = 430.0f;
const float PF_CHECK_Y = 476.0f;
const float PF_CHECK_H = 36.0f;
const float PF_LOGOUT_Y = PF_CHECK_Y + PF_CHECK_H + 8.0f;
const float PF_LOGOUT_H = 36.0f;

float pfColumnX(int count, int i) {
    const float total = (float)count * PF_TILE_W + (float)(count > 0 ? count - 1 : 0) * PF_GAP;
    return (layout::SCREEN_W - total) * 0.5f + (float)i * (PF_TILE_W + PF_GAP);
}
Rect pfPictureRect(int count, int i) {
    return {pfColumnX(count, i), PF_GROUND - PF_D - 24.0f, PF_TILE_W, PF_D + 24.0f + (PF_NAME_Y + 34.0f - PF_GROUND)};
}
Rect pfCheckRect(int count, int i) { return {pfColumnX(count, i), PF_CHECK_Y, PF_TILE_W, PF_CHECK_H}; }
Rect pfLogoutRect(int count, int i) { return {pfColumnX(count, i), PF_LOGOUT_Y, PF_TILE_W, PF_LOGOUT_H}; }

// ---- the deformed round picture ----
// A unit-disc point (u right, v up, |(u,v)| <= 1) mapped to the screen for this frame's squash,
// stretch and lean. The bottom of the picture stays on the ground line (lifted by `lift`), the
// shape narrows toward the top while it is stretched and bulges at the bottom while it is squashed.
struct PfBlob {
    float cx = 0, centreY = 0, r = 0, sx = 1, sy = 1, shear = 0, stretch = 0, squash = 0;

    PfBlob(float cx_, float ground, float D, const BounceShape& b) {
        cx = cx_;
        r = D * 0.5f;
        sx = b.sx;
        sy = b.sy;
        shear = b.shear;
        stretch = std::max(0.0f, std::min(1.0f, (b.sy - 1.0f) / 0.24f));
        squash = std::max(0.0f, std::min(1.0f, (1.0f - b.sy) / 0.30f));
        centreY = ground - b.lift * D - r * b.sy;
    }
    ImVec2 at(float u, float v) const {
        float w = sx * (1.0f - 0.10f * stretch * std::max(0.0f, v) + 0.08f * squash * std::max(0.0f, -v));
        return ImVec2(cx + u * r * w + shear * r * (v + 1.0f) * 0.5f, centreY - v * r * sy);
    }
};

const int PF_SEG = 72;

// Rim points, clockwise on the screen (what ImGui's anti-aliased fills expect).
void pfRim(const PfBlob& b, float scale, std::vector<ImVec2>& out) {
    out.clear();
    for (int i = 0; i < PF_SEG; i++) {
        const float a = -6.2831853f * (float)i / (float)PF_SEG;
        out.push_back(b.at(std::cos(a) * scale, std::sin(a) * scale));
    }
}

// Fills the blob: a fan from the centre, with a colour (or a texture coordinate) per rim point.
// picture != 0 -> the user's photo, centre-cropped to a circle (it stretches with the blob);
// otherwise a diagonal gradient between `light` and `dark`.
void pfFill(ImDrawList* d, const PfBlob& b, uint64_t picture, float aspect, ImU32 light, ImU32 dark) {
    std::vector<ImVec2> rim;
    pfRim(b, 1.0f, rim);
    const int n = (int)rim.size();
    const ImVec2 centre = b.at(0.0f, 0.0f);
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    if (picture) d->PushTexture(ImTextureRef((ImTextureID)picture));
    d->PrimReserve(n * 3, n + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    auto uvOf = [&](float u, float v) {
        const float a = aspect > 0.01f ? aspect : 1.0f;
        if (a >= 1.0f) return ImVec2(0.5f + u * 0.5f / a, 0.5f - v * 0.5f);
        return ImVec2(0.5f + u * 0.5f, 0.5f - v * 0.5f * a);
    };
    auto tint = [&](float u, float v) {
        if (picture) return IM_COL32(255, 255, 255, 255);
        return lerpColor(dark, light, 0.5f + 0.5f * (v - u) * 0.5f);
    };
    d->PrimWriteVtx(centre, picture ? uvOf(0, 0) : white, tint(0, 0));
    for (int i = 0; i < n; i++) {
        const float a = -6.2831853f * (float)i / (float)n, u = std::cos(a), v = std::sin(a);
        d->PrimWriteVtx(rim[(size_t)i], picture ? uvOf(u, v) : white, tint(u, v));
    }
    for (int i = 0; i < n; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
    }
    if (picture) d->PopTexture();
}

// A plain filled shape inside the blob, given in unit-disc coordinates.
void pfShape(ImDrawList* d, const PfBlob& b, const std::vector<ImVec2>& unit, ImU32 colour) {
    std::vector<ImVec2> pts;
    for (const ImVec2& q : unit) pts.push_back(b.at(q.x, q.y));
    d->AddConcavePolyFilled(pts.data(), (int)pts.size(), colour);
}

// The plus on the "Add profile" disc: two plain rectangles (each convex, so no triangulation tricks), given
// clockwise on the screen, mapped through the blob so it squashes and leans with the disc.
void pfPlus(ImDrawList* d, const PfBlob& b, ImU32 colour) {
    const float a = 0.36f, w = 0.075f;
    auto bar = [&](float u0, float v0, float u1, float v1) {
        const ImVec2 q[4] = {b.at(u0, v1), b.at(u1, v1), b.at(u1, v0), b.at(u0, v0)}; // TL, TR, BR, BL on screen
        d->AddConvexPolyFilled(q, 4, colour);
    };
    bar(-a, -w, a, w);
    bar(-w, -a, w, a);
}

void pfAvatar(ImDrawList* d, const ProfileTile& t, float cx, bool hovered) {
    const PfBlob b(cx, PF_GROUND, PF_D, t.bounce);
    const float r = b.r;

    // Shadow on the ground: smaller and fainter the higher the picture is.
    {
        const float up = std::min(1.0f, t.bounce.lift / 0.35f);
        const float k = 1.0f - 0.45f * up;
        const float strength = (g_light ? 0.70f : 1.0f) * (1.0f - 0.55f * up);
        for (int i = 0; i < 3; i++) {
            const float grow = 1.0f + 0.16f * (float)i;
            d->AddEllipseFilled(ImVec2(cx, PF_GROUND + 7.0f), ImVec2(r * 0.88f * k * grow, 8.5f * k * grow),
                                IM_COL32(0, 0, 0, (int)((i == 0 ? 46.0f : 22.0f) * strength)), 0.0f, 40);
        }
    }

    // Body.
    if (t.add) {
        if (g_glass) {
            // Real Liquid Glass (refraction, blur, lit edge) like every other panel. The glass shape is a
            // rounded box with full-radius corners, so it follows the bounce's squash and stretch too.
            const ImVec2 c = b.at(0.0f, 0.0f);
            glassSurface(d, c.x - r * b.sx, c.y - r * b.sy, c.x + r * b.sx, c.y + r * b.sy, r,
                         hovered ? GK::ButtonHot : GK::Button, true);
        } else {
            // A translucent disc (so the screen behind shows through).
            std::vector<ImVec2> disc;
            pfRim(b, 1.0f, disc);
            d->AddConvexPolyFilled(disc.data(), (int)disc.size(), bgTint(col::TEXT, 0.10f)); // one polygon: no fan seams
        }
        pfPlus(d, b, hovered ? col::TEXT : col::MUTED);
    } else if (t.picture) {
        pfFill(d, b, t.picture, t.pictureAspect, 0, 0);
    } else {
        const ImU32 base = rgb(t.color);
        pfFill(d, b, 0, 1.0f, lerpColor(base, col::WHITE, 0.20f), lerpColor(base, IM_COL32(0, 0, 0, 255), 0.30f));
        // The initial, drawn upright (text cannot lean) but sized and placed with the blob.
        const float size = PF_D * 0.46f * 0.5f * (b.sx + b.sy);
        const ImVec2 c = b.at(0.0f, 0.0f);
        const ImVec2 ts = font()->CalcTextSizeA(size, 1e9f, 0.0f, t.letter.c_str());
        d->AddText(font(), size, ImVec2(c.x - ts.x * 0.5f + 1.5f, c.y - ts.y * 0.5f + 1.5f), IM_COL32(0, 0, 0, 60), t.letter.c_str());
        d->AddText(font(), size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col::WHITE, t.letter.c_str());
    }

    // Liquid Glass: a lit rim and a soft highlight, so the picture reads as a glass bead.
    std::vector<ImVec2> rim;
    if (g_glass && !t.add) { // (the Add tile is a real glass surface, which has its own lit edge)
        ImVec2 arc[24];
        for (int i = 0; i < 24; i++) {
            const float a = (105.0f + 85.0f * (float)i / 23.0f) * 0.0174533f;
            arc[i] = b.at(std::cos(a) * 0.86f, std::sin(a) * 0.86f);
        }
        d->AddPolyline(arc, 24, bgTint(col::WHITE, 0.45f), 0, 4.0f);
        std::vector<ImVec2> gloss;
        for (int i = 0; i < 16; i++) {
            const float a = -6.2831853f * (float)i / 16.0f;
            gloss.push_back(ImVec2(-0.40f + std::cos(a) * 0.20f, 0.52f + std::sin(a) * 0.11f));
        }
        pfShape(d, b, gloss, bgTint(col::WHITE, 0.26f));
    }

    // Rim; the hovered picture gets the accent colour and a glow.
    if (hovered) {
        for (int i = 2; i >= 1; i--) {
            pfRim(b, 1.0f + 0.045f * (float)i, rim);
            d->AddPolyline(rim.data(), (int)rim.size(), bgTint(accentMain(), i == 1 ? 0.34f : 0.16f), ImDrawFlags_Closed, 5.0f);
        }
        pfRim(b, 1.0f, rim);
        d->AddPolyline(rim.data(), (int)rim.size(), accentMain(), ImDrawFlags_Closed, 5.0f);
    } else if (!(g_glass && t.add)) {
        pfRim(b, 1.0f, rim);
        d->AddPolyline(rim.data(), (int)rim.size(), g_glass ? bgTint(col::WHITE, 0.42f) : bgTint(col::TEXT, 0.22f),
                       ImDrawFlags_Closed, g_glass ? 2.0f : 2.5f);
    }
}

void pfCheckBox(const ProfileTile& t, const Rect& r, bool focused) {
    ImDrawList* d = dl();
    surfaceRect(r, focused ? col::PANEL_HI : col::PANEL, 18.0f, focused ? GK::ButtonHot : GK::Button);
    if (!g_glass && focused) d->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 18.0f, 0, 2.5f);

    const float bs = 22.0f, bx = r.x + 14.0f, by = r.y + (r.h - bs) * 0.5f;
    if (t.keep) {
        d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bs, by + bs), accentMain(), 6.0f);
        const ImVec2 tick[3] = {ImVec2(bx + 5.0f, by + 11.5f), ImVec2(bx + 9.5f, by + 16.0f), ImVec2(bx + 17.0f, by + 6.5f)};
        d->AddPolyline(tick, 3, col::WHITE, 0, 3.0f);
    } else {
        if (g_glass) d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bs, by + bs), bgTint(col::WHITE, 0.10f), 6.0f);
        d->AddRect(ImVec2(bx, by), ImVec2(bx + bs, by + bs), bgTint(col::TEXT, 0.55f), 6.0f, 0, 2.0f);
    }
    const float size = TEXT_S - 1.0f;
    const std::string label = fitText(font(), size, "Auto log in", r.w - 52.0f);
    const ImVec2 ls = textSize(size, label);
    text(size, bx + bs + 10.0f, r.y + (r.h - ls.y) * 0.5f, t.keep || focused ? col::TEXT : col::MUTED, label);
}

void pfLogoutButton(const ProfileTile& t, const Rect& r, bool focused) {
    ImDrawList* d = dl();
    // Dimmed when there is nothing to log out of.
    const GK kind = t.loggedOut ? GK::ButtonDisabled : (focused ? GK::ButtonHot : GK::Button);
    surfaceRect(r, focused && !t.loggedOut ? col::PANEL_HI : col::PANEL, 18.0f, kind);
    if (!g_glass && focused) d->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 18.0f, 0, 2.5f);
    const float size = TEXT_S - 1.0f;
    const std::string label = t.loggedOut ? "Logged out" : "Log out";
    const ImVec2 ls = textSize(size, label);
    text(size, r.x + (r.w - ls.x) * 0.5f, r.y + (r.h - ls.y) * 0.5f,
         t.loggedOut ? col::MUTED : (focused ? col::TEXT : col::MUTED), label);
}

} // namespace

void drawProfiles(const ProfilesModel& m) {
    drawScreenBackdrop();
    drawLogo(30.0f, 40.0f);

    const float titleSize = TEXT_L + 16.0f;
    const ImVec2 tsz = textSize(titleSize, m.title);
    text(titleSize, (layout::SCREEN_W - tsz.x) * 0.5f, 150.0f, col::TEXT, m.title);

    const int n = (int)m.tiles.size();
    ImDrawList* d = dl();
    for (int i = 0; i < n; i++) {
        const ProfileTile& t = m.tiles[(size_t)i];
        const bool hovered = (i == m.cursor);
        const float x = pfColumnX(n, i), cx = x + PF_TILE_W * 0.5f;
        pfAvatar(d, t, cx, hovered);

        const std::string name = fitText(font(), TEXT_M, t.add ? "Add profile" : t.name, PF_TILE_W);
        const ImVec2 ns = textSize(TEXT_M, name);
        text(TEXT_M, cx - ns.x * 0.5f, PF_NAME_Y, hovered ? col::TEXT : col::MUTED, name);
        if (!t.add) {
            pfCheckBox(t, pfCheckRect(n, i), hovered && m.focusRow == 1);
            pfLogoutButton(t, pfLogoutRect(n, i), hovered && m.focusRow == 2);
        }
    }

    if (!m.message.empty()) {
        const ImVec2 ms = textSize(TEXT_S + 1, m.message);
        text(TEXT_S + 1, (layout::SCREEN_W - ms.x) * 0.5f, PF_LOGOUT_Y + PF_LOGOUT_H + 30.0f,
             m.messageIsError ? col::ERROR_RED : col::MUTED, m.message);
    }
    drawHints(m.hints, 64.0f, layout::FOOTER_Y);
}

ProfilesHit hitTestProfiles(const ProfilesModel& m, float x, float y) {
    ProfilesHit hit;
    const int n = (int)m.tiles.size();
    for (int i = 0; i < n; i++) {
        if (!m.tiles[(size_t)i].add && pfCheckRect(n, i).contains(x, y)) {
            hit.kind = ProfilesHit::Check;
            hit.index = i;
            return hit;
        }
        if (!m.tiles[(size_t)i].add && pfLogoutRect(n, i).contains(x, y)) {
            hit.kind = ProfilesHit::Logout;
            hit.index = i;
            return hit;
        }
        if (pfPictureRect(n, i).contains(x, y)) {
            hit.kind = ProfilesHit::Picture;
            hit.index = i;
            return hit;
        }
    }
    return hit;
}

} // namespace ui
