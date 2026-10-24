// m3e_draw.inc -- Material 3 Expressive drawing (see m3e_core.h for the maths and docs/material3-expressive.md).
//
// Included in app_ui.cpp just before profiles_ui.inc, because it is built from that file's private helpers
// (dl(), GK, themedSurface, col::, g_m3c, ...). Not compiled on its own. Every function here is only reached
// when M3E is the active style; Plain and Liquid Glass never call into it.
//
// Cost model: no allocation (shape points live on the stack), cookies are one triangle fan of <= 96
// triangles, nothing runs per frame except what is on screen, and the only GPU resource is the shared
// low-resolution blur that Liquid Glass already owns (no refraction snapshot, no extra pass).
#include <cstring>

namespace ui {

static m3e::FocusMorph g_focusMorph;

// The animation tables ask "was this drawn on the previous frame?". A TV + GamePad tick (visualizer, movie
// remote) runs two ImGui frames, so the raw ImGui frame count jumps by 2 between the GamePad's draws and every
// icon looked "new" each time (the animations went wild). noteExtraPass() (called by Gfx after the TV pass of such
// a tick) lets m3eFrame() count ticks instead: it advances by 1 per tick however many passes it has.
static uint64_t m3eFrame() { return (uint64_t)ImGui::GetFrameCount() - g_extraPasses; }

static float m3eDt() {
    const float dt = ImGui::GetIO().DeltaTime;
    return dt > 0.0f && dt < 0.25f ? dt : 1.0f / 60.0f;
}
static ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)std::max(0, std::min(255, a)) << 24); }

// ---- shapes ------------------------------------------------------------------------------------------

// A filled scalloped "cookie" (one triangle fan; no allocation).
static void m3eCookie(ImDrawList* d, float cx, float cy, float r, int lobes, float amp, float rot, ImU32 col) {
    if (r < 1.0f) return;
    const int segs = std::max(32, std::min(96, lobes * 8));
    float pts[2 * 96];
    m3e::cookiePoints(lobes, amp, 0.0f, rot, segs, pts);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve(segs * 3, segs + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(ImVec2(cx, cy), uv, col);
    for (int i = 0; i < segs; i++) d->PrimWriteVtx(ImVec2(cx + pts[2 * i] * r, cy + pts[2 * i + 1] * r), uv, col);
    for (int i = 0; i < segs; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % segs));
    }
}

// M3E loading indicator: one shape that turns while it morphs through Material's sequence (SoftBurst,
// Cookie9Sided, Pentagon, Pill, Sunny, Cookie4Sided, Oval). 48 points, one triangle fan, no allocation.
static void m3eLoader(ImDrawList* d, float cx, float cy, float r, ImU32 col, double time) {
    const int segs = 48;
    float pts[2 * 48];
    m3e::loaderPoints(m3e::loaderPose(time), segs, pts);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve(segs * 3, segs + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(ImVec2(cx, cy), uv, col);
    for (int i = 0; i < segs; i++) d->PrimWriteVtx(ImVec2(cx + pts[2 * i] * r, cy + pts[2 * i + 1] * r), uv, col);
    for (int i = 0; i < segs; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % segs));
    }
}

// A cover image clipped to a cookie: the same fan as m3eCookie but every vertex carries the texture
// coordinate of its place in the (square-cropped) picture. uv0/uv1 are the picture's corners.
static void m3eCookieImage(ImDrawList* d, ImTextureID tex, float cx, float cy, float r, int lobes, float amp, float rot,
                           ImVec2 uv0, ImVec2 uv1, ImU32 tint) {
    if (r < 1.0f) return;
    const int segs = std::max(32, std::min(96, lobes * 8));
    float pts[2 * 96];
    m3e::cookiePoints(lobes, amp, 0.0f, rot, segs, pts);
    const float inv = 1.0f / (2.0f * r * (1.0f + amp));
    auto uvAt = [&](float x, float y) {
        const float u = 0.5f + (x - cx) * inv, v = 0.5f + (y - cy) * inv;
        return ImVec2(uv0.x + (uv1.x - uv0.x) * u, uv0.y + (uv1.y - uv0.y) * v);
    };
    d->PushTexture(ImTextureRef(tex));
    d->PrimReserve(segs * 3, segs + 1);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteVtx(ImVec2(cx, cy), uvAt(cx, cy), tint);
    for (int i = 0; i < segs; i++) {
        const float x = cx + pts[2 * i] * r, y = cy + pts[2 * i + 1] * r;
        d->PrimWriteVtx(ImVec2(x, y), uvAt(x, y), tint);
    }
    for (int i = 0; i < segs; i++) {
        d->PrimWriteIdx(base);
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        d->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % segs));
    }
    d->PopTexture();
}

// ---- animated icons -----------------------------------------------------------------------------------------

static m3e::IconFxTable g_iconFx;

// The icon animation is applied after the icon is drawn: its new vertices are scaled, turned and lifted about
// the icon's centre. begin() before drawing, end() after; a no-op until begin() and when the icon is idle.
void M3eFxScope::begin(uint32_t key, bool active, const m3e::IconFxStyle& st, float size, float x, float y, uint32_t tag) {
    d = dl();
    v0 = d->VtxBuffer.Size;
    cx = x;
    cy = y;
    fx = g_iconFx.get(key, m3eFrame(), m3eDt(), active, st, size, tag);
    on = true;
}
void M3eFxScope::end() {
    if (!on) return;
    on = false;
    if (fx.scale == 1.0f && fx.rot == 0.0f && fx.dy == 0.0f && fx.sy == 1.0f) return;
    const float cs = std::cos(fx.rot) * fx.scale, sn = std::sin(fx.rot) * fx.scale;
    for (int i = v0; i < d->VtxBuffer.Size; i++) {
        ImVec2& p = d->VtxBuffer.Data[i].pos;
        const float x = p.x - cx, y = (p.y - cy) * fx.sy;
        p.x = cx + x * cs - y * sn;
        p.y = cy + x * sn + y * cs + fx.dy;
    }
}

// How each icon moves when it comes on (focus lands on it, it appears, it is pressed): scale pop, wiggle,
// hop, spin -- the way Android's animated vector icons play a short flourish on a state change.
static m3e::IconFxStyle m3eIconStyle(Icon i) {
    switch (i) {
        case Icon::Home: return {0.22f, 0.0f, 4.0f, 0.0f};          // hops
        case Icon::Search: return {0.18f, 0.45f, 0.0f, 0.0f};      // wiggles
        case Icon::LiveTv:
        case Icon::Channel:
        case Icon::Remote: return {0.14f, 0.40f, 0.0f, 0.0f};
        case Icon::Library:
        case Icon::Collection:
        case Icon::Folder:
        case Icon::Album: return {0.26f, 0.0f, 0.0f, 0.0f};
        case Icon::Movie:
        case Icon::Video:
        case Icon::Series:
        case Icon::Episode: return {0.20f, -0.30f, 0.0f, 0.0f};
        case Icon::Music: return {0.18f, 0.30f, 3.0f, 0.0f};
        case Icon::Settings:
        case Icon::Snow:
        case Icon::Palette: return {0.10f, 0.0f, 0.0f, 6.2832f};      // turns
        case Icon::Clock:
        case Icon::Resume: return {0.14f, 0.0f, 0.0f, 6.2832f};
        case Icon::Sparkle: return {0.40f, 0.0f, 0.0f, 6.2832f};
        case Icon::Heart: return {0.55f, 0.0f, 0.0f, 0.0f};        // a heartbeat
        case Icon::Gamepad: return {0.16f, 0.35f, 0.0f, 0.0f};
        default: return {0.24f, 0.22f, 0.0f, 0.0f};                 // User, Info, Help ...
    }
}

static void m3eIconAnimated(Icon icon, float cx, float cy, float s, ImU32 c, bool active) {
    M3eFxScope fs;
    const uint32_t key = m3e::hashRect(cx, cy, (float)(int)icon, s);
    m3e::IconFxStyle st = m3eIconStyle(icon);
    st.holdless = g_iconHoldless;
    fs.begin(key, active, st, s, cx, cy, g_iconTag);
    drawIconRaw(icon, cx, cy, s, c);
    fs.end();
}

// ---- list rows: a press bounces the rows above and below -----------------------------------------------
// ui::pulse() is called when the user activates something (A, or a tap). The spring below rises for ~0.12 s and
// then settles back, and the rows next to the activated one are pushed away by it (and bounce back).
static float m3eRowPress() {
    static m3e::Spring sp;
    static int frame = -1;
    const int f = (int)m3eFrame();
    if (f != frame) {
        frame = f;
        sp.step(m3eDt(), (ImGui::GetTime() - g_pulseTime) < 0.12 ? 1.0f : 0.0f, 620.0f, 22.0f);
    }
    return sp.x;
}

// How far the row `rank` places from the activated one moves for press amount `p` (px, signed).
static float m3eRowPush(int rank, float p) { return m3e::neighbourPush(rank, 0) * 0.9f * p; }


// ---- popup backdrop: the screen behind a dialog goes out of focus and takes the accent wash ----------------
// Like Android's quick-settings shade: everything behind the popup softens into a smooth, tinted haze, and the
// dialog sits on it as a calm tonal sheet. The haze is built on the CPU from the screen's own draw list (see
// m3e::Haze): a few thousand tiny triangles splatted on a 64 x 36 grid, blurred, drawn as a coloured mesh --
// no snapshot, no texture, no extra GPU pass, and it works the same on the TV and the GamePad.
static m3e::Haze g_haze;
static uint64_t g_hazeFrame = 0, g_hazeLastDraw = 0;

static void m3eHazeCapture(const ImDrawList* src) {
    g_haze.clear();
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    auto isWhite = [&](const ImDrawVert& v) { return std::fabs(v.uv.x - white.x) < 1e-4f && std::fabs(v.uv.y - white.y) < 1e-4f; };
    auto rgba = [](ImU32 c, float* o) {
        o[0] = (float)(c & 0xFF) / 255.0f; o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
        o[2] = (float)((c >> 16) & 0xFF) / 255.0f; o[3] = (float)((c >> 24) & 0xFF) / 255.0f;
    };
    for (int ci = 0; ci < src->CmdBuffer.Size; ci++) {
        const ImDrawCmd& cmd = src->CmdBuffer[ci];
        if (cmd.UserCallback) continue;
        const ImDrawIdx* idx = src->IdxBuffer.Data + cmd.IdxOffset;
        const ImDrawVert* vtx = src->VtxBuffer.Data + cmd.VtxOffset;
        for (unsigned n = 0; n + 2 < cmd.ElemCount; n += 3) {
            const ImDrawVert* v[3] = {&vtx[idx[n]], &vtx[idx[n + 1]], &vtx[idx[n + 2]]};
            float x[3], y[3], c[3][4];
            for (int k = 0; k < 3; k++) { x[k] = v[k]->pos.x; y[k] = v[k]->pos.y; rgba(v[k]->col, c[k]); }
            float scale = 1.0f;
            if (!(isWhite(*v[0]) && isWhite(*v[1]) && isWhite(*v[2]))) {
                // Textured: a glyph quad (small; about a quarter of it is ink) counts, a picture (large) is left out.
                const float area = std::fabs((x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])) * 0.5f;
                if (area > 1500.0f) continue;
                scale = 0.26f;
            }
            g_haze.triangle(x, y, c, scale);
        }
    }
    g_haze.blur(2, 2);
}

// Draws the haze as a 34 x 20 mesh of vertex colours at opacity `amt` (0..1).
static void m3eHazeDraw(ImDrawList* d, float amt) {
    const int NX = 34, NY = 20; // 32 x 18 samples (2 x 2 cells each) plus the screen border
    const float W = layout::SCREEN_W, H = layout::SCREEN_H, sx = W / 32.0f, sy = H / 18.0f;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    d->PrimReserve((NX - 1) * (NY - 1) * 6, NX * NY);
    const ImDrawIdx base = (ImDrawIdx)d->_VtxCurrentIdx;
    for (int j = 0; j < NY; j++) {
        const int gj = std::max(0, std::min(17, j - 1));
        const float py = j == 0 ? 0.0f : (j == NY - 1 ? H : ((float)(j - 1) + 0.5f) * sy);
        for (int i = 0; i < NX; i++) {
            const int gi = std::max(0, std::min(31, i - 1));
            const float px = i == 0 ? 0.0f : (i == NX - 1 ? W : ((float)(i - 1) + 0.5f) * sx);
            float a[4], b[4], c2[4], e[4], o[4];
            g_haze.sample(gi * 2, gj * 2, a); g_haze.sample(gi * 2 + 1, gj * 2, b);
            g_haze.sample(gi * 2, gj * 2 + 1, c2); g_haze.sample(gi * 2 + 1, gj * 2 + 1, e);
            for (int k = 0; k < 4; k++) o[k] = (a[k] + b[k] + c2[k] + e[k]) * 0.25f;
            d->PrimWriteVtx(ImVec2(px, py), uv,
                            IM_COL32((int)(o[0] * 255.0f + 0.5f), (int)(o[1] * 255.0f + 0.5f), (int)(o[2] * 255.0f + 0.5f), (int)(o[3] * amt * 255.0f + 0.5f)));
        }
    }
    for (int j = 0; j < NY - 1; j++)
        for (int i = 0; i < NX - 1; i++) {
            const ImDrawIdx p0 = (ImDrawIdx)(base + j * NX + i), p1 = (ImDrawIdx)(p0 + 1), p2 = (ImDrawIdx)(p0 + NX), p3 = (ImDrawIdx)(p2 + 1);
            d->PrimWriteIdx(p0); d->PrimWriteIdx(p1); d->PrimWriteIdx(p3);
            d->PrimWriteIdx(p0); d->PrimWriteIdx(p3); d->PrimWriteIdx(p2);
        }
}

// `src` is the draw list that holds the screen behind the popup (the background list), `d` the one the popup is
// drawn into. `amt` (0..1) is the dialog's entrance; `vtxStart` is moved past the backdrop so the dialog's
// entrance transform does not scale it.
static void m3eDialogScrim(ImDrawList* d, float amt, int& vtxStart) {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    amt = std::max(0.0f, std::min(1.0f, amt));
    if (amt <= 0.0f) { vtxStart = d->VtxBuffer.Size; return; }
    const uint64_t frame = m3eFrame();
    // Rebuild the haze when the popup has just appeared, then about every quarter second (the screen behind is
    // nearly still; the capture costs well under a millisecond).
    if (g_hazeLastDraw + 1 < frame || frame >= g_hazeFrame + 15) {
        m3eHazeCapture(ImGui::GetBackgroundDrawList());
        g_hazeFrame = frame;
    }
    g_hazeLastDraw = frame;
    // The screen behind first dissolves to its own average (the blur), then the accent wash settles over it.
    d->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), withAlpha(col::BG, (int)(0xD0 * amt)));
    m3eHazeDraw(d, amt);
    const ImU32 wc = lerpColor(col::BG, g_m3c.primaryContainer, g_light ? 0.55f : 0.30f);
    d->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), withAlpha(wc, (int)((g_light ? 0x66 : 0x70) * amt)));
    vtxStart = d->VtxBuffer.Size;
}

// ---- button groups ------------------------------------------------------------------------------------------
// The Now Playing row behaves like a connected button group: the pressed button swells while its neighbours
// are pushed aside by a spring (they bounce a little and come back).
static void m3eGroupMotion(int idx, int rank, int pressedRank, float& push, float& grow) {
    static m3e::Spring so[6], sg[6];
    idx = std::max(0, std::min(5, idx));
    so[idx].step(m3eDt(), m3e::neighbourPush(rank, pressedRank));
    sg[idx].step(m3eDt(), (pressedRank >= 0 && rank == pressedRank) ? 1.0f : 0.0f);
    push = so[idx].x;
    grow = sg[idx].x;
}

// Two big, very faint cookies behind the menus: the decoration of the M3E backdrop. Geometry is regenerated
// from a handful of cos/sin per frame (2 x 64 points); alpha is low so rows and text always win.
static void m3eBackdropShapes() {
    const float W = layout::SCREEN_W, H = layout::SCREEN_H;
    const double t = ImGui::GetTime();
    ImDrawList* d = dl();
    m3eCookie(d, W - 110.0f, H - 70.0f, 270.0f, 9, 0.09f, (float)t * 0.03f, withAlpha(g_m3c.primaryContainer, g_light ? 0x48 : 0x2C));
    m3eCookie(d, W * 0.74f, 54.0f, 74.0f, 6, 0.13f, -(float)t * 0.05f, withAlpha(g_m3c.tertiaryContainer, g_light ? 0x38 : 0x22));
}

// Badge in the corner of a message card: the loading indicator while busy, an alert cookie for an error.
static void m3eMessageBadge(float x, float y, bool busy, bool error) {
    ImDrawList* d = dl();
    const double t = ImGui::GetTime();
    if (error) {
        m3eCookie(d, x, y, 30.0f, 6, 0.12f, 0.0f, g_m3c.errorContainer);
        const ImVec2 s = textSize(34.0f, "!");
        d->AddText(font(), 34.0f, ImVec2(x - s.x * 0.5f, y - s.y * 0.5f), g_m3c.onErrorContainer, "!");
    } else if (busy) {
        m3eLoader(d, x, y, 28.0f, accentMain(), t);
    }
}

// The progress indicator, following Material 3 Expressive's specification (wavy variant, 4 dp stroke, drawn at
// 1.5 px per dp on the 1280 x 720 canvas): stroke 4 dp for the played part and the track, a 4 dp gap between
// them (the vertical handle sits in it), wavelength 40 dp, wave amplitude 3 dp (10 dp tall in all), round caps, and a 4 dp stop dot near the
// end of the track. The played wave travels while playing and eases flat when paused; its first and last 6 px
// are locked to the centre line and the wave grows smoothly out of it.
static void m3eProgressLine(ImDrawList* d, float x, float y, float w, float h, float frac, bool moving) {
    static float amp = 0.0f; // eased: a pause settles the wave instead of snapping it flat
    const float dp = 1.5f;
    const float target = moving ? 3.0f * dp : 0.0f;
    amp += (target - amp) * std::min(1.0f, m3eDt() * 7.0f);
    if (std::fabs(target - amp) < 0.02f) amp = target;
    const float th = 4.0f * dp, gap = 4.0f * dp, dot = 4.0f * dp;
    const float yc = y + h * 0.5f, px = x + w * std::max(0.0f, std::min(1.0f, frac));
    const float hw = 3.0f * dp, hh = 17.0f * dp; // the handle: a slim vertical pill in the gap, as in the player reference
    const float clear = hw * 0.5f + gap * 0.5f;  // the gap is measured from the handle's edges
    const float endX = px - clear, trackX = px + clear;
    // Track: a calm line with a round cap, and the stop dot near its end.
    if (trackX + th * 0.5f < x + w) {
        d->AddRectFilled(ImVec2(trackX, yc - th * 0.5f), ImVec2(x + w, yc + th * 0.5f), col::TRACK, th * 0.5f);
        if (x + w - trackX > dot * 3.0f) d->AddCircleFilled(ImVec2(x + w - dot, yc), dot * 0.5f, accentMain(), 12);
    }
    // Played part: the travelling squiggle (or a straight line when paused / too short to wave).
    if (endX > x + th * 0.5f) {
        const float x0 = x + th * 0.5f, x1 = endX - th * 0.5f; // the round caps sit at the ends of the centre line
        if (amp <= 0.0f || x1 - x0 < 8.0f) {
            d->AddRectFilled(ImVec2(x, yc - th * 0.5f), ImVec2(endX, yc + th * 0.5f), accentMain(), th * 0.5f);
        } else {
            float pts[2 * 384];
            const int n = m3e::squigglePoints(x0, x1, yc, amp, 40.0f * dp, (float)ImGui::GetTime() * 5.0f, pts, 384, 6.0f, 18.0f);
            if (n >= 2) {
                d->AddPolyline(reinterpret_cast<const ImVec2*>(pts), n, accentMain(), ImDrawFlags_None, th);
                d->AddCircleFilled(ImVec2(pts[0], pts[1]), th * 0.5f, accentMain(), 12);                       // round caps
                d->AddCircleFilled(ImVec2(pts[2 * (n - 1)], pts[2 * (n - 1) + 1]), th * 0.5f, accentMain(), 12);
            }
        }
    }
    d->AddRectFilled(ImVec2(px - hw * 0.5f, yc - hh * 0.5f), ImVec2(px + hw * 0.5f, yc + hh * 0.5f), accentMain(), hw * 0.5f);
}

// ---- surfaces ----------------------------------------------------------------------------------------

// The M3E counterpart of glassSurface(): same inputs (where, how round, what kind), different look.
// Tonal fills instead of glass, no refraction and no lit rims; focus is a spring-morphed pill plus a ring.
static void m3eSurface(ImDrawList* d, float x0, float y0, float x1, float y1, float rad, GK k, bool blur, float alpha) {
    const float w = x1 - x0, h = y1 - y0;
    if (w <= 1.0f || h <= 1.0f) return;
    auto A = [&](int a) { return (int)std::max(0.0f, std::min(255.0f, (float)a * alpha)); };
    const ImVec2 p0(x0, y0), p1(x1, y1);
    float R = m3e::morphRadius(rad, w, h, 0.0f);
    switch (k) {
        case GK::Dialog: {
            // Frosted dialog: soft shadow, a still blur of the shared backdrop (when it exists), then a
            // mostly-opaque tonal tint so text never depends on what is behind.
            for (int i = 0; i < 4; i++) {
                const float e = 2.0f + 3.0f * (float)i;
                d->AddRect(ImVec2(x0 - e, y0 - e + 4.0f), ImVec2(x1 + e, y1 + e + 4.0f), IM_COL32(0, 0, 0, A(0x1C - i * 4)), R + e, 0, 3.0f);
            }
            // The screen behind is already blurred by m3eDialogScrim: the card itself is a calm, nearly opaque tonal sheet.
            d->AddRectFilled(p0, p1, withAlpha(col::PANEL_HI, A(0xF2)), R);
            d->AddRect(p0, p1, withAlpha(col::TEXT, A(0x14)), R, 0, 1.0f);
            return;
        }
        case GK::Overlay: // over video / other UI: a translucent tonal scrim, no blur of the picture
            d->AddRectFilled(p0, p1, withAlpha(col::PANEL, A(0xD2)), R);
            return;
        case GK::Panel:
            d->AddRectFilled(p0, p1, col::PANEL, R);
            return;
        case GK::Row:
            d->AddRectFilled(p0, p1, col::PANEL, m3e::morphRadius(rad * 1.1f, w, h, 0.0f));
            return;
        case GK::RowFocusDim: // selected, but the other pane has focus: a quiet tonal pill
            d->AddRectFilled(p0, p1, g_m3c.secondaryContainer, m3e::morphRadius(rad, w, h, 1.0f));
            return;
        case GK::RowFocus:
        case GK::ButtonHot: {
            // Focus: the corners spring from the resting radius to a pill (with a little overshoot), the fill
            // is the primary container and a ring in the primary colour keeps it unmistakable on a TV.
            const float m = g_focusMorph.get(m3e::hashRect(x0, y0, x1, y1), m3eFrame(), m3eDt());
            R = m3e::morphRadius(rad, w, h, m);
            d->AddRectFilled(p0, p1, withAlpha(accentDeep(), A(0xFF)), R);
            d->AddRect(ImVec2(x0 - 2.5f, y0 - 2.5f), ImVec2(x1 + 2.5f, y1 + 2.5f), withAlpha(accentMain(), A(0xFF)), R + 2.5f, 0, 2.5f);
            return;
        }
        case GK::ButtonPressed: // pressed: squarer, a touch darker
            d->AddRectFilled(p0, p1, g_m3c.secondaryContainer, m3e::morphRadius(rad, w, h, 0.0f) * 0.55f);
            d->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, A(0x22)), m3e::morphRadius(rad, w, h, 0.0f) * 0.55f);
            return;
        case GK::ButtonDisabled:
            d->AddRectFilled(p0, p1, withAlpha(col::PANEL_HI, A(0x60)), R);
            return;
        case GK::Button:
        default:
            d->AddRectFilled(p0, p1, withAlpha(col::PANEL_HI, A(0xFF)), R);
            return;
    }
}

// ---- button shapes ----------------------------------------------------------------------------------

// Settings > Customization > Customize shapes: every kind of button can take one of these (M3E only). Default is
// the look each button always had. Pill / Rounded / Square change the corner radius; Clover, Cookie and Flower are
// Material's scalloped shapes (round buttons only).
// A button's resting corner radius as a fraction of its height (0.5 = pill).
static float shapeCorner(int shape) {
    switch (shape) {
        case SH_PILL:    return 0.5f;
        case SH_ROUNDED: return 0.30f;
        case SH_SQUARE:  return 0.10f;
        default:         return 0.5f; // (scalloped shapes squeeze from the full size)
    }
}
// Fills one shape: a rounded rectangle (corner radius R) or a cookie fitted in the smaller half-extent. press = 0..1.
// For the scalloped shapes the user's width / height (sw, sh: multipliers, 1 = normal) stretch the whole cookie: it is
// drawn round, in the smaller of the normal half-extents (hw / sw, hh / sh), then widened by sw and heightened by sh.
static void m3eFillShape(ImDrawList* d, float cx, float cy, float hw, float hh, int shape, float R, float press, ImU32 fill,
                         float sw, float sh) {
    if (shape >= SH_CLOVER) {
        const int lobes = shape == SH_CLOVER ? 4 : (shape == SH_COOKIE ? 9 : 12);
        const float amp = shape == SH_CLOVER ? 0.18f : (shape == SH_COOKIE ? 0.085f : 0.11f);
        const float r0 = std::min(hw / std::max(0.01f, sw), hh / std::max(0.01f, sh)) * (1.0f - 0.07f * press);
        const int v0 = d->VtxBuffer.Size;
        m3eCookie(d, cx, cy, r0, lobes, amp, 0.0f, fill);
        if (sw != 1.0f || sh != 1.0f)
            for (int i = v0; i < d->VtxBuffer.Size; i++) {
                ImVec2& p = d->VtxBuffer[i].pos;
                p.x = cx + (p.x - cx) * sw;
                p.y = cy + (p.y - cy) * sh;
            }
        return;
    }
    d->AddRectFilled(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), fill, R);
}

// A transport button as in the Material 3 Expressive media player: a rounded rectangle (halfW x halfH around
// the centre). radFrac is the corner radius as a fraction of the height: 0.5 = pill, ~0.34 = the resting
// rounded rectangle; pressing squares the corners up. One spring per button (slot), stepped once per draw.
static void m3eTransportButton(ImDrawList* d, float cx, float cy, float halfW, float halfH, float sw, float sh, ImU32 fill, bool pressed, float radFrac, int slot, int shape) {
    static m3e::Spring springs[8];
    m3e::Spring& sp = springs[std::max(0, std::min(7, slot))];
    if (shape == SH_DEFAULT) {
        sp.step(m3eDt(), pressed ? 0.20f : radFrac);
        const float f = std::max(0.12f, std::min(0.56f, sp.x));
        const float R = std::min(halfH, f * 2.0f * halfH);
        d->AddRectFilled(ImVec2(cx - halfW, cy - halfH), ImVec2(cx + halfW, cy + halfH), fill, R);
        return;
    }
    // A shape the user picked (Settings > Customization > Customize shapes): the same spring, round the shape's
    // own resting corner; pressing tightens the corners (or squeezes a cookie).
    const float base = shapeCorner(shape);
    sp.step(m3eDt(), pressed ? base * 0.6f : base);
    const float f = std::max(0.02f, std::min(0.56f, sp.x));
    m3eFillShape(d, cx, cy, halfW, halfH, shape, std::min(halfH, f * 2.0f * halfH), std::max(0.0f, std::min(1.0f, (base - f) / (base * 0.4f))), fill, sw, sh);
}

// Login-style button (full width pill that springs from rounded rectangle on focus).
static void m3eButton(const Rect& r0, ImU32 fill, bool focus, ImDrawList* onList) {
    ImDrawList* d = onList ? onList : dl();
    // The user's width / height for dialog buttons (Customize shapes): scaled about the centre; the touch area stays.
    const float sw = g_btnSize[3][0], sh = g_btnSize[3][1];
    const Rect r{r0.x + r0.w * (1.0f - sw) * 0.5f, r0.y + r0.h * (1.0f - sh) * 0.5f, r0.w * sw, r0.h * sh};
    float R = gr(14.0f);
    const int shape = g_btnShape[3];
    if (shape == SH_PILL) R = r.h * 0.5f;
    else if (shape == SH_ROUNDED) R = r.h * 0.30f;
    else if (shape == SH_SQUARE) R = r.h * 0.10f;
    else if (focus) R = m3e::morphRadius(R, r.w, r.h, g_focusMorph.get(m3e::hashRect(r.x, r.y, r.right(), r.bottom()), m3eFrame(), m3eDt()));
    else R = m3e::morphRadius(R, r.w, r.h, 0.0f);
    d->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), fill, R);
    if (focus) d->AddRect(ImVec2(r.x - 2.5f, r.y - 2.5f), ImVec2(r.right() + 2.5f, r.bottom() + 2.5f), col::TEXT, R + 2.5f, 0, 2.5f);
}

// The picture under the cursor on "Who's watching?" springs from a circle into a cookie: returns the scallop depth.
static float m3eAvatarCookie(float cx, bool hovered) {
    static m3e::Spring springs[8];
    m3e::Spring& sp = springs[((int)(cx / 218.0f)) & 7];
    sp.step(m3eDt(), hovered ? 1.0f : 0.0f);
    return std::max(0.0f, std::min(1.2f, sp.x)) * 0.11f;
}

// ---- dialog entrance -----------------------------------------------------------------------------------
// The dialog is drawn normally; afterwards its new vertices are scaled about the card's centre and faded by
// the spring (a few hundred vertices, only for the ~0.3 s the spring takes to settle).
static m3e::Spring g_entSpring;
static uint32_t g_entKey = 0;
static uint64_t g_entFrame = 0;

static void m3eEntranceBegin(ImDrawList* d, uint32_t key, int& vtxStart, float& s) {
    const uint64_t frame = m3eFrame();
    if (key != g_entKey || frame > g_entFrame + 1) { // a dialog that was not on screen last frame: spring it in
        g_entKey = key;
        g_entSpring.reset(0.0f);
    }
    g_entFrame = frame;
    g_entSpring.step(m3eDt(), 1.0f);
    s = g_entSpring.settled(1.0f) ? 1.0f : g_entSpring.x;
    vtxStart = d->VtxBuffer.Size;
}

static void m3eEntranceEnd(ImDrawList* d, int vtxStart, float s, float cx, float cy) {
    if (s == 1.0f) return;
    const float sc = m3e::entranceScale(s), al = m3e::entranceAlpha(s);
    for (int i = vtxStart; i < d->VtxBuffer.Size; i++) {
        ImDrawVert& v = d->VtxBuffer[i];
        v.pos.x = cx + (v.pos.x - cx) * sc;
        v.pos.y = cy + (v.pos.y - cy) * sc;
        const int a = (int)(((v.col >> 24) & 0xFF) * al);
        v.col = (v.col & 0x00FFFFFFu) | ((ImU32)a << 24);
    }
}

} // namespace ui
