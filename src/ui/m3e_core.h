// Material 3 Expressive core -- everything about the M3E theme that needs neither ImGui nor GX2, so it is
// host-testable: the accent-derived tonal palette, the shape generators (scalloped "cookie", wavy
// "squiggle"), the spring used for shape morphs, and the small table that remembers which surface was
// focused recently so its corners can spring into a pill.
//
// Nothing here allocates per call (the generators write into caller-provided arrays) and nothing talks to
// the GPU. app_ui.cpp / m3e_draw.inc turn these numbers into ImGui geometry.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui {
namespace m3e {

static const float PI = 3.14159265f;
static const float TAU = 6.2831853f;

// ---- colour ------------------------------------------------------------------------------------------

struct Rgb8 {
    uint8_t r = 0, g = 0, b = 0;
};

struct Hsl {
    float h = 0, s = 0, l = 0; // h in turns (0..1), s and l in 0..1
};

inline Hsl toHsl(Rgb8 c) {
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    Hsl o;
    o.l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d < 1e-6f) return o; // grey: hue and saturation stay 0
    o.s = o.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float h;
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    o.h = h / 6.0f;
    return o;
}

inline Rgb8 fromHsl(float h, float s, float l) {
    h = h - std::floor(h);
    s = std::min(1.0f, std::max(0.0f, s));
    l = std::min(1.0f, std::max(0.0f, l));
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = l - c * 0.5f;
    auto q = [&](float v) { return (uint8_t)std::min(255.0f, std::max(0.0f, (v + m) * 255.0f + 0.5f)); };
    Rgb8 o;
    o.r = q(r);
    o.g = q(g);
    o.b = q(b);
    return o;
}

// Linear blend of two sRGB colours (t = 0 -> a, 1 -> b).
inline Rgb8 mix(Rgb8 a, Rgb8 b, float t) {
    Rgb8 o;
    o.r = (uint8_t)(a.r + (b.r - a.r) * t + 0.5f);
    o.g = (uint8_t)(a.g + (b.g - a.g) * t + 0.5f);
    o.b = (uint8_t)(a.b + (b.b - a.b) * t + 0.5f);
    return o;
}

// WCAG relative luminance, 0..1. The sRGB -> linear step is a 256-entry table built once (no pow per call).
inline float lin8(uint8_t v) {
    static float table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; i++) {
            const float c = i / 255.0f;
            table[i] = c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        ready = true;
    }
    return table[v];
}
inline float luminance(Rgb8 c) { return 0.2126f * lin8(c.r) + 0.7152f * lin8(c.g) + 0.0722f * lin8(c.b); }
inline float contrast(Rgb8 a, Rgb8 b) {
    const float la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

// A tone is Material's 0..100 lightness scale (CIE L*): tone 0 is black, 100 white, and the same tone has the
// same luminance for every hue. That is what makes "tone 80 on tone 20" readable for yellow as well as blue.
// HSL lightness is not that scale, so the HSL lightness is searched (12 bisection steps, a few hundred flops)
// until the colour's luminance equals the tone's.
inline float toneLuminance(float t) {
    t = std::min(100.0f, std::max(0.0f, t));
    if (t <= 8.0f) return t / 903.3f;
    const float f = (t + 16.0f) / 116.0f;
    return f * f * f;
}
inline Rgb8 tone(float hue, float sat, float t) {
    const float target = toneLuminance(t);
    float lo = 0.0f, hi = 1.0f;
    Rgb8 c = fromHsl(hue, sat, 0.5f);
    for (int i = 0; i < 12; i++) {
        const float mid = (lo + hi) * 0.5f;
        c = fromHsl(hue, sat, mid);
        if (luminance(c) < target) lo = mid; else hi = mid;
    }
    return fromHsl(hue, sat, (lo + hi) * 0.5f);
}

// The colour roles M3E uses, all derived from the user's accent colour. The neutral roles (surface...)
// carry a whisper of the accent hue, like Material's neutral tonal palette does.
struct Palette {
    Rgb8 primary, onPrimary;
    Rgb8 primaryContainer, onPrimaryContainer;
    Rgb8 secondaryContainer, onSecondaryContainer;
    Rgb8 tertiary, tertiaryContainer, onTertiaryContainer;
    Rgb8 surface;          // screen background
    Rgb8 surfaceLow;       // sidebar / navigation rail
    Rgb8 surfaceContainer; // cards and list rows
    Rgb8 surfaceHigh;      // chips, fields, raised controls
    Rgb8 surfaceHighest;   // inactive tracks
    Rgb8 onSurface, onSurfaceVariant, outline;
    Rgb8 errorContainer, onErrorContainer;
};

// `tertiarySeed` (optional): a colour whose hue and saturation set the tertiary role -- a custom palette's
// second colour. Without it the tertiary is the accent's hue turned 60 degrees, as it always was.
inline Palette derive(Rgb8 accent, bool light, const Rgb8* tertiarySeed = nullptr) {
    const Hsl a = toHsl(accent);
    // A grey or near-grey accent would give a lifeless palette: keep a floor of saturation.
    const float sat = std::min(0.92f, std::max(0.40f, a.s));
    float h3 = a.h + 1.0f / 6.0f; // tertiary: 60 degrees round the wheel
    float satT = sat;
    if (tertiarySeed) {
        const Hsl t = toHsl(*tertiarySeed);
        h3 = t.h;
        satT = std::min(0.92f, std::max(0.40f, t.s));
    }
    const float h = a.h;
    const float sec = sat * 0.55f, neu = std::min(0.11f, sat * 0.13f), tert = std::min(0.80f, satT * 0.9f);
    Palette p;
    if (!light) {
        p.primary = tone(h, sat, 80);
        p.onPrimary = tone(h, sat, 18);
        p.primaryContainer = tone(h, sat * 0.85f, 30);
        p.onPrimaryContainer = tone(h, sat, 91);
        p.secondaryContainer = tone(h, sec, 27);
        p.onSecondaryContainer = tone(h, sec, 90);
        p.tertiary = tone(h3, tert, 80);
        p.tertiaryContainer = tone(h3, tert * 0.8f, 30);
        p.onTertiaryContainer = tone(h3, tert, 90);
        p.surface = tone(h, neu, 4);
        p.surfaceLow = tone(h, neu, 7);
        p.surfaceContainer = tone(h, neu, 11);
        p.surfaceHigh = tone(h, neu, 16);
        p.surfaceHighest = tone(h, neu, 22);
        p.onSurface = tone(h, neu * 0.6f, 91);
        p.onSurfaceVariant = tone(h, neu * 0.7f, 74);
        p.outline = tone(h, neu * 0.7f, 54);
        p.errorContainer = tone(0.0f, 0.62f, 30);
        p.onErrorContainer = tone(0.0f, 0.9f, 90);
    } else {
        p.primary = tone(h, sat, 38);
        p.onPrimary = tone(h, sat, 100);
        p.primaryContainer = tone(h, std::min(1.0f, sat * 1.0f), 88);
        p.onPrimaryContainer = tone(h, sat, 12);
        p.secondaryContainer = tone(h, sec * 1.2f, 88);
        p.onSecondaryContainer = tone(h, sec * 1.6f, 14);
        p.tertiary = tone(h3, tert, 36);
        p.tertiaryContainer = tone(h3, std::min(1.0f, tert * 1.2f), 88);
        p.onTertiaryContainer = tone(h3, tert, 14);
        p.surface = tone(h, neu * 1.2f, 99);
        p.surfaceLow = tone(h, neu * 1.3f, 97);
        p.surfaceContainer = tone(h, neu * 1.5f, 94);
        p.surfaceHigh = tone(h, neu * 1.6f, 91);
        p.surfaceHighest = tone(h, neu * 1.6f, 87);
        p.onSurface = tone(h, neu, 10);
        p.onSurfaceVariant = tone(h, neu, 30);
        p.outline = tone(h, neu, 50);
        p.errorContainer = tone(0.0f, 0.9f, 90);
        p.onErrorContainer = tone(0.0f, 0.8f, 16);
    }
    return p;
}

// ---- shapes ------------------------------------------------------------------------------------------

// Radius of a scalloped "cookie" at angle `a`, in unit-circle terms: the outer edge is 1, the valleys are
// 1 - amp. `lobes` bumps round the shape (Material's cookies have 4, 6, 7, 9 or 12).
inline float cookieRadius(float a, int lobes, float amp, float phase) {
    return 1.0f - amp * 0.5f + amp * 0.5f * std::cos((float)lobes * a + phase);
}

// Fills out[2*i], out[2*i+1] with `segs` points of a unit cookie, clockwise on a y-down screen, rotated by
// `rot` radians. Multiply by the real radius and add the centre. No allocation.
inline void cookiePoints(int lobes, float amp, float phase, float rot, int segs, float* out) {
    for (int i = 0; i < segs; i++) {
        const float a = TAU * (float)i / (float)segs;
        const float r = cookieRadius(a, lobes, amp, phase);
        out[2 * i] = std::cos(a + rot) * r;
        out[2 * i + 1] = std::sin(a + rot) * r;
    }
}

// The wavy line of the M3E progress indicator from x0 to x1 around y centre `yc`: a sine of amplitude
// `amp` and wavelength `len`, travelling with `phase`. The wave grows in over the first wavelength so the
// start of the line is flat. Returns the number of points written (never more than maxPts); step is
// roughly `len/8` so the curve is smooth without being expensive.
inline int squigglePoints(float x0, float x1, float yc, float amp, float len, float phase, float* out, int maxPts,
                          float anchor = 6.0f, float ramp = 18.0f) {
    if (x1 <= x0 || maxPts < 2 || len <= 1.0f) return 0;
    // ~20 points per wavelength: fine enough that thick strokes show no facets at the crests and troughs.
    int n = (int)std::ceil((x1 - x0) / (len / 20.0f)) + 1;
    n = std::max(2, std::min(maxPts, n));
    // Both ends are anchored to the middle of the track: the last `anchor` px sit exactly on the centre line, and
    // the wave then grows to full height over the next `ramp` px along a C2-smooth curve (smootherstep). An abrupt
    // envelope would turn every crest or trough travelling through the ends into a spike.
    const float rampLen = std::max(1.0f, std::min(ramp, ((x1 - x0) * 0.5f) - anchor));
    for (int i = 0; i < n; i++) {
        const float x = x0 + (x1 - x0) * (float)i / (float)(n - 1);
        const float d = std::min(x - x0, x1 - x);                 // distance from the nearer end
        float t = (d - anchor) / rampLen;
        t = std::max(0.0f, std::min(1.0f, t));
        const float e = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        out[2 * i] = x;
        out[2 * i + 1] = yc + amp * e * std::sin((x - x0) / len * TAU - phase);
    }
    return n;
}

// ---- motion ------------------------------------------------------------------------------------------

// A damped spring for shape morphs. Slightly under-damped (about 0.6) so a morph overshoots a little and
// settles: Material's "expressive spatial" feel. Stepped in fixed 1/120 s slices so it is stable at any
// frame rate.
struct Spring {
    float x = 0.0f, v = 0.0f;
    void step(float dt, float target, float stiffness = 520.0f, float damping = 27.0f) {
        dt = std::max(0.0f, std::min(dt, 0.1f));
        acc_ += dt;
        const float h = 1.0f / 120.0f;
        while (acc_ >= h) {
            acc_ -= h;
            v += (-stiffness * (x - target) - damping * v) * h;
            x += v * h;
        }
    }
    bool settled(float target) const { return std::fabs(x - target) < 0.002f && std::fabs(v) < 0.02f; }
    void reset(float value = 0.0f) { x = value; v = 0.0f; acc_ = 0.0f; }

private:
    float acc_ = 0.0f;
};

// Remembers the surfaces that are focused right now. A focused surface asks for its morph amount by key
// (any stable hash of where it sits); a key not seen last frame starts again from 0, so every time focus
// lands on a surface its corners spring from the resting radius towards a pill. Fixed size, no heap.
class FocusMorph {
public:
    static const int SLOTS = 8;

    // Returns the spring value (0 = resting shape, 1 = full pill, may overshoot) for `key` this frame.
    // Call at most once per surface per frame; `frame` must increase between frames.
    float get(uint32_t key, uint64_t frame, float dt) {
        Slot* hit = nullptr;
        Slot* oldest = &slots_[0];
        for (Slot& s : slots_) {
            if (s.used && s.key == key) hit = &s;
            if (!s.used || s.lastFrame < oldest->lastFrame) oldest = &s;
        }
        if (hit && frame > hit->lastFrame + 1) hit = nullptr; // was not focused last frame: start over
        if (!hit) {
            hit = oldest;
            hit->used = true;
            hit->key = key;
            hit->spring.reset(0.0f);
        }
        hit->lastFrame = frame;
        hit->spring.step(dt, 1.0f);
        return hit->spring.x;
    }

    void clear() {
        for (Slot& s : slots_) s.used = false;
    }

private:
    struct Slot {
        bool used = false;
        uint32_t key = 0;
        uint64_t lastFrame = 0;
        Spring spring;
    };
    Slot slots_[SLOTS];
};

inline uint32_t hashRect(float x0, float y0, float x1, float y1) {
    uint32_t h = 2166136261u;
    for (float v : {x0, y0, x1, y1}) {
        h ^= (uint32_t)(int)std::lround(v);
        h *= 16777619u;
    }
    return h;
}

// Corner radius of a surface that morphs from `rest` to a full pill with the spring value `m` (clamped so
// an overshoot never exceeds a pill and never goes negative).
inline float morphRadius(float rest, float w, float h, float m) {
    const float pill = std::min(w, h) * 0.5f;
    const float r = rest + (pill - rest) * m;
    return std::max(0.0f, std::min(pill, r));
}

// ---- loading indicator: the expressive shape sequence -------------------------------------------------------
// Material's LoadingIndicator morphs through SoftBurst, Cookie9Sided, Pentagon, Pill, Sunny, Cookie4Sided and
// Oval while it turns. Each shape is a polar radius function (max radius ~1) so two shapes can be blended by
// mixing their radii at the same angle: one loop of ~64 points per frame, no allocation.

static const int LOADER_SHAPES = 7;

inline float loaderRadius(int shape, float a) {
    switch (((shape % LOADER_SHAPES) + LOADER_SHAPES) % LOADER_SHAPES) {
        case 0: return (1.0f + 0.15f * std::cos(10.0f * a)) / 1.15f;                                  // SoftBurst
        case 1: return (1.0f + 0.09f * std::cos(9.0f * a)) / 1.09f;                                   // Cookie9Sided
        case 2: {                                                                                     // Pentagon (rounded)
            const float sector = TAU / 5.0f;
            const float phi = std::fmod(a + sector * 8.0f, sector) - sector * 0.5f;
            const float poly = std::cos(PI / 5.0f) / std::cos(phi);
            return (0.55f * poly + 0.45f * std::cos(PI / 5.0f)) * 1.094f;
        }
        case 3: {                                                                                     // Pill (superellipse 1 x 0.56)
            const float c = std::fabs(std::cos(a)), sn = std::fabs(std::sin(a)) / 0.56f;
            return std::pow(std::pow(c, 3.6f) + std::pow(sn, 3.6f), -1.0f / 3.6f) * 0.984f;
        }
        case 4: return (1.0f + 0.17f * std::cos(8.0f * a) + 0.045f * std::cos(16.0f * a)) / 1.215f;   // Sunny
        case 5: return (1.0f + 0.13f * std::cos(4.0f * a)) / 1.13f;                                   // Cookie4Sided
        default: {                                                                                    // Oval, tilted
            const float c = std::cos(a - 0.7f), sn = std::sin(a - 0.7f), b = 0.72f;
            return b / std::sqrt(b * b * c * c + sn * sn);
        }
    }
}

// Where the loader is at `t` seconds: shape `a` morphing into shape `b` by `blend` (0..1, springy overshoot
// allowed), and the whole thing turned by `rot` radians. Each shape holds ~0.38 s then takes ~0.42 s to turn
// into the next one.
struct LoaderPose {
    int a = 0, b = 1;
    float blend = 0.0f, rot = 0.0f;
};
inline LoaderPose loaderPose(double t) {
    const double hold = 0.38, morph = 0.42, period = hold + morph;
    const double u = t / period;
    const double fl = std::floor(u);
    const double within = (u - fl) * period;
    LoaderPose p;
    p.a = (int)std::fmod(fl, (double)LOADER_SHAPES);
    p.b = (p.a + 1) % LOADER_SHAPES;
    if (within > hold) {
        const float x = (float)((within - hold) / morph);   // 0..1
        const float c1 = 1.70158f, c3 = c1 + 1.0f;          // ease-out-back: a little overshoot, like a spring
        const float y = x - 1.0f;
        p.blend = 1.0f + c3 * y * y * y + c1 * y * y;
    }
    p.rot = (float)(t * 2.4);
    return p;
}

// `segs` unit-radius points of the pose, written like cookiePoints.
inline void loaderPoints(const LoaderPose& p, int segs, float* out) {
    for (int i = 0; i < segs; i++) {
        const float a = TAU * (float)i / (float)segs;
        float r = loaderRadius(p.a, a);
        if (p.blend != 0.0f) r += (loaderRadius(p.b, a) - r) * p.blend;
        out[2 * i] = std::cos(a + p.rot) * r;
        out[2 * i + 1] = std::sin(a + p.rot) * r;
    }
}

// ---- animated icons ------------------------------------------------------------------------------------------
// Android's icons play a short animation when their state changes (a pop, a wiggle, a spin). IconFxTable keeps
// one little spring pair per icon that is "on" or just was: `kick` fires on the rising edge of `active` (and
// when an icon first appears), `hold` eases the icon a little larger while it stays active.

struct IconFxStyle {
    float scale;  // extra scale at the peak of the pop
    float rot;    // radians of wiggle at the peak
    float lift;   // pixels (at 26 px icon size) the icon hops up
    float spin;   // radians added to a monotone turn on every kick (+-TAU = one full turn each time; 0 = none)
    float flip;   // radians of "turn over" (vertical flip) at the peak: sy = cos(flip * k)
    bool holdless; // true: no extra size while active (toggles / event icons that pop but then return to normal)
};
struct IconFx {
    float scale = 1.0f, rot = 0.0f, dy = 0.0f, sy = 1.0f;
};

class IconFxTable {
public:
    static const int SLOTS = 16;

    // Returns the transform for the icon `key` this frame, or an identity when it is idle and has no slot.
    // The icon pops (a kick into an under-damped spring) when it becomes `active`, and again every time `tag`
    // changes while it is active (the caller bumps the tag on every event: a press, a seek, a mode change).
    IconFx get(uint32_t key, uint64_t frame, float dt, bool active, const IconFxStyle& st, float size, uint32_t tag = 0) {
        Slot* hit = nullptr;
        Slot* free_ = nullptr;
        for (Slot& s : slots_) {
            if (s.used && s.key == key) hit = &s;
            else if (!free_ && (!s.used || s.lastFrame + 120 < frame)) free_ = &s;
        }
        if (hit && frame > hit->lastFrame + 1) { hit->active = false; hit->tag = tag; } // was not drawn: a fresh appearance
        if (!hit) {
            if (!active || !free_) return IconFx();
            hit = free_;
            *hit = Slot();
            hit->used = true;
            hit->key = key;
            hit->tag = tag;
        }
        if (active && (!hit->active || tag != hit->tag)) {
            hit->kick.v += 14.0f; // the pop: an impulse into an under-damped spring
            hit->turnTarget += st.spin;
        }
        hit->tag = tag;
        hit->active = active;
        hit->lastFrame = frame;
        hit->kick.step(dt, 0.0f, 300.0f, 14.0f);
        hit->turn.step(dt, hit->turnTarget, 170.0f, 17.0f);
        hit->hold.step(dt, active ? 1.0f : 0.0f);
        if (hit->turn.settled(hit->turnTarget) && std::fabs(hit->turnTarget) >= TAU) { // keep the numbers small
            const float w = std::floor(hit->turnTarget / TAU) * TAU;
            hit->turnTarget -= w;
            hit->turn.x -= w;
        }
        IconFx fx;
        const float k = hit->kick.x;
        fx.scale = 1.0f + (st.holdless ? 0.0f : 0.10f * hit->hold.x) + st.scale * k;
        fx.rot = st.rot * k + hit->turn.x;
        fx.dy = -st.lift * (size / 26.0f) * k;
        fx.sy = st.flip != 0.0f ? std::cos(st.flip * k) : 1.0f;
        if (!active && hit->kick.settled(0.0f) && hit->hold.settled(0.0f) && hit->turn.settled(hit->turnTarget)) hit->used = false;
        return fx;
    }

    void clear() {
        for (Slot& s : slots_) s.used = false;
    }

private:
    struct Slot {
        bool used = false, active = false;
        uint32_t key = 0, tag = 0;
        uint64_t lastFrame = 0;
        float turnTarget = 0.0f;
        Spring kick, hold, turn;
    };
    Slot slots_[SLOTS];
};

// ---- button groups: neighbours react to a press ---------------------------------------------------------------
// Material's connected button groups let the pressed button grow while its neighbours give way. `rank` is a
// button's place in the row, `pressedRank` the pressed one's (or -1). Returns how far (in px, signed) the
// button should be pushed along the row: away from the press, strongest next door.
inline float neighbourPush(int rank, int pressedRank) {
    if (pressedRank < 0 || rank == pressedRank) return 0.0f;
    const int d = rank - pressedRank;
    const int ad = d < 0 ? -d : d;
    const float mag = ad == 1 ? 11.0f : (ad == 2 ? 5.0f : (ad == 3 ? 2.0f : 0.0f));
    return d < 0 ? -mag : mag;
}

// The play / pause icon as two quads that morph into each other (m = 0 play, 1 pause), in units of `s`.
// out[0..7] is the left quad (TL, TR, BR, BL), out[8..15] the right one.
inline void playPausePoints(float m, float* out) {
    static const float A[16] = {-0.55f, -1.0f, 0.15f, -0.5f, 0.15f, 0.5f, -0.55f, 1.0f,
                                0.15f, -0.5f, 0.85f, 0.0f, 0.85f, 0.0f, 0.15f, 0.5f};
    static const float B[16] = {-0.70f, -0.85f, -0.15f, -0.85f, -0.15f, 0.85f, -0.70f, 0.85f,
                                0.15f, -0.85f, 0.70f, -0.85f, 0.70f, 0.85f, 0.15f, 0.85f};
    for (int i = 0; i < 16; i++) out[i] = A[i] + (B[i] - A[i]) * m;
}

// ---- popup haze: a cheap CPU "blur" of the screen behind a dialog --------------------------------------------
// The screen's own draw list is rasterised onto a coarse 64 x 36 grid (20 virtual px per cell; a few thousand
// tiny triangles, no GPU), blurred, and drawn back as a 34 x 20 vertex-coloured mesh: smooth Gouraud gradients
// that read as the same screen out of focus, like the shade behind Android's quick settings. No texture, no
// snapshot, no extra GPU pass. Cells nothing opaque was drawn on keep alpha 0 (video shows through).
class Haze {
public:
    static const int W = 64, H = 36;
    static constexpr float CELL = 20.0f;

    void clear() {
        for (int i = 0; i < W * H * 4; i++) px_[i] = 0.0f;
    }

    // Composite one triangle (virtual px; colours non-premultiplied 0..1 per vertex) over the grid. `scale`
    // multiplies its alpha (used for glyph quads whose real coverage is only a fraction of the quad).
    void triangle(const float* x, const float* y, const float (*c)[4], float scale) {
        const float area2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        const float area = std::fabs(area2) * 0.5f;
        if (area < 0.05f) return;
        const float minx = std::min(x[0], std::min(x[1], x[2])), maxx = std::max(x[0], std::max(x[1], x[2]));
        const float miny = std::min(y[0], std::min(y[1], y[2])), maxy = std::max(y[0], std::max(y[1], y[2]));
        int i0 = (int)std::floor(minx / CELL), i1 = (int)std::floor(maxx / CELL);
        int j0 = (int)std::floor(miny / CELL), j1 = (int)std::floor(maxy / CELL);
        if (i1 < 0 || j1 < 0 || i0 >= W || j0 >= H) return;
        if (i0 == i1 && j0 == j1) { // smaller than a cell: its colour weighted by the share of the cell it covers
            float col[4];
            for (int k = 0; k < 4; k++) col[k] = (c[0][k] + c[1][k] + c[2][k]) / 3.0f;
            put(i0, j0, col, std::min(1.0f, area / (CELL * CELL)) * col[3] * scale, 1.0f);
            return;
        }
        i0 = std::max(0, i0); j0 = std::max(0, j0); i1 = std::min(W - 1, i1); j1 = std::min(H - 1, j1);
        const float inv = 1.0f / area2;
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                const float px = ((float)i + 0.5f) * CELL, py = ((float)j + 0.5f) * CELL;
                const float w1 = ((px - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (py - y[0])) * inv;
                const float w2 = ((x[1] - x[0]) * (py - y[0]) - (px - x[0]) * (y[1] - y[0])) * inv;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                float col[4];
                for (int k = 0; k < 4; k++) col[k] = c[0][k] * w0 + c[1][k] * w1 + c[2][k] * w2;
                put(i, j, col, col[3] * scale, 1.0f);
            }
    }

    // Separable box blur of `radius` cells, `passes` times (premultiplied, edges clamped).
    void blur(int radius, int passes) {
        float tmp[W * H * 4];
        for (int p = 0; p < passes; p++) {
            for (int j = 0; j < H; j++)
                for (int i = 0; i < W; i++)
                    for (int k = 0; k < 4; k++) {
                        float sum = 0.0f;
                        for (int d = -radius; d <= radius; d++) sum += px_[(j * W + clampi(i + d, 0, W - 1)) * 4 + k];
                        tmp[(j * W + i) * 4 + k] = sum / (float)(2 * radius + 1);
                    }
            for (int j = 0; j < H; j++)
                for (int i = 0; i < W; i++)
                    for (int k = 0; k < 4; k++) {
                        float sum = 0.0f;
                        for (int d = -radius; d <= radius; d++) sum += tmp[(clampi(j + d, 0, H - 1) * W + i) * 4 + k];
                        px_[(j * W + i) * 4 + k] = sum / (float)(2 * radius + 1);
                    }
        }
    }

    // The blurred colour at cell (i, j), un-premultiplied; out[3] is the coverage (0 = nothing was drawn there).
    void sample(int i, int j, float* out) const {
        i = clampi(i, 0, W - 1);
        j = clampi(j, 0, H - 1);
        const float* p = &px_[(j * W + i) * 4];
        const float a = p[3];
        out[0] = a > 1e-4f ? std::min(1.0f, p[0] / a) : 0.0f;
        out[1] = a > 1e-4f ? std::min(1.0f, p[1] / a) : 0.0f;
        out[2] = a > 1e-4f ? std::min(1.0f, p[2] / a) : 0.0f;
        out[3] = std::min(1.0f, a);
    }

private:
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    void put(int i, int j, const float* col, float w, float) {
        if (i < 0 || j < 0 || i >= W || j >= H) return;
        w = std::max(0.0f, std::min(1.0f, w));
        float* p = &px_[(j * W + i) * 4];
        for (int k = 0; k < 3; k++) p[k] = p[k] * (1.0f - w) + col[k] * w; // premultiplied "over"
        p[3] = p[3] * (1.0f - w) + w;
    }
    float px_[W * H * 4];
};

// Entrance of a dialog: scale and opacity from a spring value 0..1 (it may overshoot 1 slightly).
inline float entranceScale(float s) { return 0.90f + 0.10f * s; }
inline float entranceAlpha(float s) { return std::min(1.0f, std::max(0.0f, s * 1.6f)); }

} // namespace m3e
} // namespace ui
