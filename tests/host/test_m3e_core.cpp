// Material 3 Expressive core (src/ui/m3e_core.h): the accent-derived palette (hue, contrast, light/dark), the
// cookie and squiggle generators, the morph spring and the focus table. No ImGui, no GPU.
#include "check.h"
#include "ui/m3e_core.h"

using namespace ui::m3e;

static void testColourRoundTrip() {
    for (uint32_t v : {0x00A4DCu, 0xAA5CC3u, 0x2ECC71u, 0xFF9F43u, 0xE74C3Cu, 0x5C6BC0u}) {
        const Rgb8 c{(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        const Hsl h = toHsl(c);
        const Rgb8 b = fromHsl(h.h, h.s, h.l);
        CHECK(std::abs((int)b.r - c.r) <= 1 && std::abs((int)b.g - c.g) <= 1 && std::abs((int)b.b - c.b) <= 1);
    }
    CHECK_NEAR(contrast({0, 0, 0}, {255, 255, 255}), 21.0, 0.01);
}

// Every accent Ufin ships (plus greys and extremes) must give readable text on every role it is used on,
// in dark and light.
static void testPaletteContrast() {
    const uint32_t accents[] = {0x00A4DC, 0xAA5CC3, 0x2ECC71, 0xFF9F43, 0xFF6BB5, 0x1ABC9C, 0xE74C3C, 0xF1C40F, 0x5C6BC0,
                                0x808080, 0x000000, 0xFFFFFF, 0xFF0000};
    for (bool light : {false, true}) {
        for (uint32_t v : accents) {
            const Palette p = derive({(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}, light);
            CHECK(contrast(p.onSurface, p.surface) >= 7.0f);
            CHECK(contrast(p.onSurface, p.surfaceContainer) >= 7.0f);
            CHECK(contrast(p.onSurface, p.surfaceHigh) >= 6.0f);
            CHECK(contrast(p.onSurfaceVariant, p.surface) >= 4.5f);
            CHECK(contrast(p.onSurfaceVariant, p.surfaceContainer) >= 4.5f);
            CHECK(contrast(p.onPrimary, p.primary) >= 4.5f);
            CHECK(contrast(p.onPrimaryContainer, p.primaryContainer) >= 4.5f);
            CHECK(contrast(p.onSecondaryContainer, p.secondaryContainer) >= 4.5f);
            CHECK(contrast(p.onTertiaryContainer, p.tertiaryContainer) >= 4.5f);
            CHECK(contrast(p.onErrorContainer, p.errorContainer) >= 4.5f);
            // Primary text on a surface (accent-coloured labels) stays legible too.
            CHECK(contrast(p.primary, p.surface) >= 3.0f);
            // Hierarchy: surfaces step away from the background.
            CHECK(luminance(p.surfaceContainer) != luminance(p.surface));
        }
        // Dark is dark, light is light.
        const Palette p = derive({0x00, 0xA4, 0xDC}, light);
        CHECK(light ? luminance(p.surface) > 0.8f : luminance(p.surface) < 0.02f);
    }
}

static void testPaletteFollowsAccent() {
    const Palette blue = derive({0x00, 0xA4, 0xDC}, false), red = derive({0xE7, 0x4C, 0x3C}, false);
    CHECK(toHsl(blue.primary).h > 0.45f && toHsl(blue.primary).h < 0.62f);  // blue-ish
    CHECK(toHsl(red.primary).h < 0.06f || toHsl(red.primary).h > 0.95f);    // red-ish
    CHECK(blue.primaryContainer.b != red.primaryContainer.b);
    // The neutrals carry only a whisper of the accent (low saturation), the containers are clearly coloured.
    CHECK(toHsl(blue.surface).s <= 0.20f);
    CHECK(toHsl(blue.primaryContainer).s >= 0.30f);
    // Tertiary sits 60 degrees round the wheel from the accent.
    CHECK_NEAR(std::fmod(toHsl(blue.tertiary).h - toHsl(blue.primary).h + 1.0f, 1.0f), 1.0f / 6.0f, 0.03);
}

static void testCookie() {
    float pts[2 * 96];
    cookiePoints(9, 0.1f, 0.0f, 0.0f, 90, pts);
    float lo = 10, hi = 0;
    for (int i = 0; i < 90; i++) {
        const float r = std::sqrt(pts[2 * i] * pts[2 * i] + pts[2 * i + 1] * pts[2 * i + 1]);
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    CHECK_NEAR(hi, 1.0, 0.002);  // outer edge is the nominal radius: it never pokes out of its box
    CHECK_NEAR(lo, 0.9, 0.002);  // valleys are amp deep
    // 9 lobes: the radius repeats every 40 degrees.
    CHECK_NEAR(cookieRadius(0.3f, 9, 0.1f, 0), cookieRadius(0.3f + TAU / 9.0f, 9, 0.1f, 0), 1e-4);
    // amp 0 is a perfect circle.
    cookiePoints(7, 0.0f, 0.0f, 0.5f, 40, pts);
    for (int i = 0; i < 40; i++) CHECK_NEAR(std::sqrt(pts[2 * i] * pts[2 * i] + pts[2 * i + 1] * pts[2 * i + 1]), 1.0, 1e-4);
}

static void testSquiggle() {
    float pts[2 * 400];
    const int n = squigglePoints(100, 600, 50, 3.0f, 26.0f, 0.0f, pts, 400);
    CHECK(n >= 100 && n <= 400);
    CHECK_NEAR(pts[0], 100.0, 1e-3);
    CHECK_NEAR(pts[2 * (n - 1)], 600.0, 1e-3);
    CHECK_NEAR(pts[1], 50.0, 1e-3); // starts on the centre line
    CHECK_NEAR(pts[2 * (n - 1) + 1], 50.0, 1e-3); // and ends on it: both ends are anchored to the middle
    float lo = 1e9, hi = -1e9;
    for (int i = 0; i < n; i++) { lo = std::min(lo, pts[2 * i + 1]); hi = std::max(hi, pts[2 * i + 1]); }
    CHECK(hi <= 53.01f && lo >= 46.99f && hi - lo > 5.0f); // stays inside +-amp, and really waves
    {   // only the first and last 6 px are locked to the centre line: a wave of wavelength 60 is at full height 15 px in
        const int m = squigglePoints(0, 600, 0, 4.5f, 60.0f, 0.0f, pts, 400, 6.0f);
        float inEnds = 0, mid = 0;
        for (int i = 0; i < m; i++) {
            const float x = pts[2 * i], dy = std::fabs(pts[2 * i + 1]);
            if (x <= 1.5f || x >= 598.5f) inEnds = std::max(inEnds, dy);
            if (x > 60.0f && x < 540.0f) mid = std::max(mid, dy);
        }
        CHECK(inEnds < 0.6f);
        CHECK(mid > 4.3f);
        // smooth: no spikes at the crests / troughs travelling through the ends, whatever the phase
        const float step = 600.0f / (float)(m - 1), freeCurv = 4.5f * (TAU * step / 60.0f) * (TAU * step / 60.0f);
        for (float ph = 0.0f; ph < TAU; ph += 0.37f) {
            const int k = squigglePoints(0, 600, 0, 4.5f, 60.0f, ph, pts, 400, 6.0f, 18.0f);
            float worst = 0.0f;
            for (int i = 1; i + 1 < k; i++) worst = std::max(worst, std::fabs(pts[2 * i + 3] - 2.0f * pts[2 * i + 1] + pts[2 * i - 1]));
            CHECK(worst < freeCurv * 6.0f);
        }
    }
    CHECK_EQ(squigglePoints(10, 10, 0, 3, 26, 0, pts, 400), 0);
    CHECK(squigglePoints(0, 1000, 0, 3, 26, 0, pts, 16) <= 16); // never overruns the caller's buffer
    CHECK_NEAR(squigglePoints(0, 100, 0, 0.0f, 26, 0, pts, 400) > 0 ? pts[1] : 99, 0.0, 1e-6); // amp 0: flat
}

static void testSpring() {
    Spring s;
    float peak = 0;
    for (int i = 0; i < 60; i++) { s.step(1.0f / 60.0f, 1.0f); peak = std::max(peak, s.x); }
    CHECK(peak > 1.0f && peak < 1.25f); // a little overshoot (expressive), not a wild one
    for (int i = 0; i < 120; i++) s.step(1.0f / 60.0f, 1.0f);
    CHECK(s.settled(1.0f));
    // Stable at any frame rate: 20 fps and 144 fps land in the same place.
    Spring a, b;
    for (int i = 0; i < 20; i++) a.step(1.0f / 20.0f, 1.0f);
    for (int i = 0; i < 144; i++) b.step(1.0f / 144.0f, 1.0f);
    CHECK_NEAR(a.x, b.x, 0.02);
    Spring huge;
    huge.step(100.0f, 1.0f); // a long stall is clamped, never explodes
    CHECK(huge.x >= 0.0f && huge.x < 2.0f);
}

static void testFocusMorph() {
    FocusMorph fm;
    float v = 0;
    for (uint64_t f = 1; f <= 8; f++) v = fm.get(42, f, 1.0f / 60.0f);
    CHECK(v > 0.2f && v < 1.3f);          // still springing
    for (uint64_t f = 9; f <= 120; f++) v = fm.get(42, f, 1.0f / 60.0f);
    CHECK_NEAR(v, 1.0, 0.01);             // settled at a full pill
    // Another surface gets focus: it starts from 0 (the old one keeps its own state).
    const float other = fm.get(7, 121, 1.0f / 60.0f);
    CHECK(other < 0.2f);
    // A surface that was not drawn focused last frame starts over.
    const float again = fm.get(42, 500, 1.0f / 60.0f);
    CHECK(again < 0.2f);
    // More surfaces than slots never crashes and always answers.
    for (uint32_t k = 0; k < 40; k++) { const float x = fm.get(1000 + k, 600 + k, 1.0f / 60.0f); CHECK(x >= 0.0f); }
    CHECK(hashRect(0, 0, 10, 10) != hashRect(0, 0, 10, 11));
}

static void testMorphRadius() {
    CHECK_NEAR(morphRadius(16, 400, 60, 0.0f), 16.0, 1e-4);
    CHECK_NEAR(morphRadius(16, 400, 60, 1.0f), 30.0, 1e-4);   // a pill: half the short side
    CHECK_NEAR(morphRadius(16, 400, 60, 1.3f), 30.0, 1e-4);   // overshoot never exceeds a pill
    CHECK_NEAR(morphRadius(16, 400, 60, -0.5f), 9.0, 1e-4);  // an undershoot is allowed (anticipation)
    CHECK(morphRadius(16, 400, 60, -5.0f) >= 0.0f);           // nor goes negative
    CHECK_NEAR(morphRadius(50, 40, 40, 0.0f), 20.0, 1e-4);    // a rest radius larger than the box is clamped
    CHECK(entranceScale(0.0f) < entranceScale(1.0f) && entranceScale(1.0f) == 1.0f);
    CHECK_NEAR(entranceAlpha(0.0f), 0.0, 1e-6);
    CHECK_NEAR(entranceAlpha(1.0f), 1.0, 1e-6);
}


static void testLoaderShapes() {
    // Every shape in the sequence stays inside the unit circle, never collapses, and the morph is continuous.
    for (int sh = 0; sh < LOADER_SHAPES; sh++)
        for (int i = 0; i < 360; i++) {
            const float r = loaderRadius(sh, TAU * i / 360.0f);
            CHECK(r > 0.3f && r < 1.0001f);
        }
    float pts[96];
    LoaderPose a = loaderPose(0.10), b = loaderPose(0.80 * 3 + 0.05);
    CHECK(a.a == 0 && a.b == 1 && a.blend == 0.0f);          // holding the first shape
    CHECK(b.a == 3 && b.b == 4);                              // the sequence advances shape by shape
    const double period = 0.80;
    LoaderPose wrap = loaderPose(period * LOADER_SHAPES + 0.1);
    CHECK(wrap.a == 0);                                       // and loops
    // the blend runs 0 -> 1 across the morph and ends exactly on the next shape
    CHECK(loaderPose(0.38 + 0.001).blend < 0.2f);
    CHECK_NEAR(loaderPose(0.80 - 1e-4).blend, 1.0, 0.01);
    loaderPoints(loaderPose(0.55), 48, pts);
    for (int i = 0; i < 96; i++) CHECK(std::fabs(pts[i]) < 1.5f);
}

static void testIconFxAndGroup() {
    IconFxTable t;
    const IconFxStyle st{0.3f, 0.3f, 4.0f, 0.0f};
    // An idle icon with no slot is untouched and costs nothing.
    IconFx idle = t.get(1, 1, 1.0f / 60.0f, false, st, 26.0f);
    CHECK(idle.scale == 1.0f && idle.rot == 0.0f && idle.dy == 0.0f);
    // Coming on pops: the scale rises above its resting value and then settles near the "active" size.
    float peak = 1.0f, last = 1.0f;
    for (int f = 2; f < 200; f++) { IconFx fx = t.get(1, f, 1.0f / 60.0f, true, st, 26.0f); peak = std::max(peak, fx.scale); last = fx.scale; }
    CHECK(peak > 1.15f);
    CHECK_NEAR(last, 1.10, 0.01);
    // Going off returns to 1 and frees the slot.
    IconFx fx;
    for (int f = 200; f < 500; f++) fx = t.get(1, f, 1.0f / 60.0f, false, st, 26.0f);
    CHECK(fx.scale == 1.0f);
    // More icons than slots never crashes.
    for (uint32_t k = 0; k < 50; k++) t.get(100 + k, 600, 1.0f / 60.0f, true, st, 26.0f);

    // Button group: neighbours are pushed away from the pressed one, next door the most; the pressed one stays put.
    CHECK(neighbourPush(2, 2) == 0.0f);
    CHECK(neighbourPush(1, 2) < 0.0f && neighbourPush(3, 2) > 0.0f);
    CHECK(std::fabs(neighbourPush(3, 2)) > std::fabs(neighbourPush(4, 2)));
    CHECK(neighbourPush(0, 5) == 0.0f && neighbourPush(3, -1) == 0.0f);

    // Play/pause morph: endpoints are the triangle and the two bars, the middle is in between.
    float q0[16], q1[16], qh[16];
    playPausePoints(0.0f, q0);
    playPausePoints(1.0f, q1);
    playPausePoints(0.5f, qh);
    CHECK_NEAR(q0[10], 0.85, 1e-5);      // the play tip
    CHECK_NEAR(q1[10], 0.70, 1e-5);      // the right bar's edge
    for (int i = 0; i < 16; i++) CHECK_NEAR(qh[i], (q0[i] + q1[i]) * 0.5, 1e-5);
}

static void testIconTagAndTurn() {
    IconFxTable t;
    const IconFxStyle turn{0.1f, 0.0f, 0.0f, TAU, 0.0f, true};
    uint64_t f = 1;
    auto run = [&](int frames, uint32_t tag, float* maxScale, float* maxRot) {
        IconFx fx;
        for (int i = 0; i < frames; i++) {
            fx = t.get(7, f++, 1.0f / 60.0f, true, turn, 26.0f, tag);
            if (maxScale) *maxScale = std::max(*maxScale, fx.scale);
            if (maxRot) *maxRot = std::max(*maxRot, std::fabs(fx.rot));
        }
        return fx;
    };
    float sc = 1, ro = 0;
    run(240, 0, &sc, &ro);               // appears: pops once and settles
    float s2 = 1, r2 = 0;
    IconFx rest = run(10, 0, &s2, &r2);
    CHECK(s2 < 1.02f && r2 < 0.2f);      // idle again
    // every change of the tag plays the animation again, with a full lap and no unwinding
    for (uint32_t tag = 1; tag <= 4; tag++) {
        float ms = 1, mr = 0;
        run(8, tag, &ms, &mr);
        CHECK(ms > 1.03f);               // popped again
        float mr2 = mr;
        IconFx end = run(240, tag, nullptr, &mr2);
        CHECK(mr2 > 3.0f);               // it really turned (about a lap), ...
        CHECK_NEAR(std::fmod(std::fabs(end.rot), TAU) < 0.05f || std::fmod(std::fabs(end.rot), TAU) > TAU - 0.05f ? 0.0 : 1.0, 0.0, 1e-6); // ... and rests upright
        (void)rest;
    }
    // a flip style turns the icon over and back
    IconFxTable t2;
    const IconFxStyle flip{0.0f, 0.0f, 0.0f, 0.0f, 5.2f, true};
    float minSy = 1.0f, sy = 1.0f;
    for (int i = 0; i < 90; i++) { IconFx fx = t2.get(9, 1 + i, 1.0f / 60.0f, true, flip, 26.0f, 0); minSy = std::min(minSy, fx.sy); sy = fx.sy; }
    CHECK(minSy < -0.5f);
    CHECK_NEAR(sy, 1.0, 0.01);
}

int main() {
    testColourRoundTrip();
    testPaletteContrast();
    testPaletteFollowsAccent();
    testCookie();
    testSquiggle();
    testSpring();
    testFocusMorph();
    testMorphRadius();
    testLoaderShapes();
    testIconFxAndGroup();
    testIconTagAndTurn();
    return check::finish("test_m3e_core");
}
