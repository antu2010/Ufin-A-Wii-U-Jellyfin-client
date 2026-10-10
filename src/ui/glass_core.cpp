#include "glass_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace ui {
namespace glass {

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

Dims blurDims(int targetW, int targetH, int divisor) {
    if (targetW <= 0 || targetH <= 0 || divisor <= 0) return Dims();
    Dims d;
    d.w = std::max(MIN_DIM, (targetW + divisor - 1) / divisor);
    d.h = std::max(MIN_DIM, (targetH + divisor - 1) / divisor);
    return d;
}

Dims sharedDims(int tvW, int tvH, int drcW, int drcH, int divisor) {
    Dims a = blurDims(tvW, tvH, divisor), b = blurDims(drcW, drcH, divisor);
    if (!a.valid()) return b;
    if (!b.valid()) return a;
    Dims d;
    d.w = std::min(a.w, b.w);
    d.h = std::min(a.h, b.h);
    return d;
}

Dims levelDims(Dims base, int level) {
    Dims d = base;
    for (int i = 0; i < level; i++) {
        d.w = std::max(1, (d.w + 1) / 2);
        d.h = std::max(1, (d.h + 1) / 2);
    }
    return d;
}

int kawaseLevels(Dims base, int requested) {
    if (!base.valid()) return 0;
    int n = std::min(requested, MAX_LEVELS);
    while (n > 0) {
        Dims l = levelDims(base, n);
        if (l.w >= 4 && l.h >= 4) break;
        n--;
    }
    return std::max(n, 0);
}

Uv uvForRect(float x, float y, float w, float h, float screenW, float screenH) {
    Uv uv;
    if (screenW <= 0.0f || screenH <= 0.0f) return Uv{0, 0, 1, 1};
    uv.u0 = clamp01(x / screenW);
    uv.v0 = clamp01(y / screenH);
    uv.u1 = clamp01((x + w) / screenW);
    uv.v1 = clamp01((y + h) / screenH);
    return uv;
}

void texelForPoint(float x, float y, float screenW, float screenH, Dims d, int& tx, int& ty) {
    tx = ty = 0;
    if (!d.valid() || screenW <= 0.0f || screenH <= 0.0f) return;
    tx = clampInt((int)std::floor(x / screenW * (float)d.w), 0, d.w - 1);
    ty = clampInt((int)std::floor(y / screenH * (float)d.h), 0, d.h - 1);
}

Scale displayScale(int pw, int ph, float screenW, float screenH) {
    Scale s{1.0f, 1.0f};
    if (pw > 0 && screenW > 0.0f) s.x = (float)pw / screenW;
    if (ph > 0 && screenH > 0.0f) s.y = (float)ph / screenH;
    return s;
}

// ---------------------------------------------------------------- blur chain

// Bilinear sample of `s` at continuous position (fx, fy) in texel-edge
// coordinates (texel i spans [i, i+1)), clamped at the borders -- what a GPU
// sampler with linear filtering and clamp-to-edge would return.
static void sampleBilinear(const std::vector<float>& px, Dims d, float fx, float fy, float out[3]) {
    const float x = fx - 0.5f, y = fy - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float tx = x - (float)x0, ty = y - (float)y0;
    const int xa = clampInt(x0, 0, d.w - 1), xb = clampInt(x0 + 1, 0, d.w - 1);
    const int ya = clampInt(y0, 0, d.h - 1), yb = clampInt(y0 + 1, 0, d.h - 1);
    const float* p00 = &px[((size_t)ya * d.w + xa) * 3];
    const float* p10 = &px[((size_t)ya * d.w + xb) * 3];
    const float* p01 = &px[((size_t)yb * d.w + xa) * 3];
    const float* p11 = &px[((size_t)yb * d.w + xb) * 3];
    for (int c = 0; c < 3; c++) {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bot = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

// Dual-Kawase down pass: centre tap x4 plus the four diagonal taps, /8, with
// the taps half a *destination* texel away.
static void kawaseDown(const std::vector<float>& src, Dims sd, std::vector<float>& dst, Dims dd, int y0, int y1) {
    const float sx = (float)sd.w / (float)dd.w, sy = (float)sd.h / (float)dd.h;
    const float hx = 0.5f * sx, hy = 0.5f * sy;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < dd.w; x++) {
            const float cx = ((float)x + 0.5f) * sx, cy = ((float)y + 0.5f) * sy;
            float c[3], a[3], b[3], d[3], e[3];
            sampleBilinear(src, sd, cx, cy, c);
            sampleBilinear(src, sd, cx - hx, cy - hy, a);
            sampleBilinear(src, sd, cx + hx, cy + hy, b);
            sampleBilinear(src, sd, cx + hx, cy - hy, d);
            sampleBilinear(src, sd, cx - hx, cy + hy, e);
            float* o = &dst[((size_t)y * dd.w + x) * 3];
            for (int k = 0; k < 3; k++) o[k] = (c[k] * 4.0f + a[k] + b[k] + d[k] + e[k]) * 0.125f;
        }
    }
}

// Dual-Kawase up pass: four axis taps (a full source texel away) plus four
// diagonal taps (half a texel, weight 2), /12.
static void kawaseUp(const std::vector<float>& src, Dims sd, std::vector<float>& dst, Dims dd, int y0, int y1) {
    const float sx = (float)sd.w / (float)dd.w, sy = (float)sd.h / (float)dd.h;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < dd.w; x++) {
            const float cx = ((float)x + 0.5f) * sx, cy = ((float)y + 0.5f) * sy;
            float t[8][3];
            sampleBilinear(src, sd, cx - 1.0f, cy, t[0]);
            sampleBilinear(src, sd, cx + 1.0f, cy, t[1]);
            sampleBilinear(src, sd, cx, cy + 1.0f, t[2]);
            sampleBilinear(src, sd, cx, cy - 1.0f, t[3]);
            sampleBilinear(src, sd, cx - 0.5f, cy + 0.5f, t[4]);
            sampleBilinear(src, sd, cx + 0.5f, cy + 0.5f, t[5]);
            sampleBilinear(src, sd, cx + 0.5f, cy - 0.5f, t[6]);
            sampleBilinear(src, sd, cx - 0.5f, cy - 0.5f, t[7]);
            float* o = &dst[((size_t)y * dd.w + x) * 3];
            for (int k = 0; k < 3; k++) {
                o[k] = (t[0][k] + t[1][k] + t[2][k] + t[3][k] + (t[4][k] + t[5][k] + t[6][k] + t[7][k]) * 2.0f) * (1.0f / 12.0f);
            }
        }
    }
}

bool BlurChain::init(Dims base, int maxLevels) {
    ready_ = false;
    down_.clear();
    up_.clear();
    synth_ = Img();
    if (!base.valid() || base.w > 1024 || base.h > 1024) return false;
    const int levels = kawaseLevels(base, maxLevels);
    if (levels < 1) return false;
    base_ = base;
    maxLevels_ = levels;
    levels_ = levels;
    synth_.d = base;
    synth_.px.assign((size_t)base.w * base.h * 3, 0.0f);
    down_.resize((size_t)levels);
    for (int i = 1; i <= levels; i++) {
        down_[(size_t)i - 1].d = levelDims(base, i);
        down_[(size_t)i - 1].px.assign((size_t)down_[(size_t)i - 1].d.w * down_[(size_t)i - 1].d.h * 3, 0.0f);
    }
    up_.resize((size_t)levels);
    for (int i = 0; i < levels; i++) {
        up_[(size_t)i].d = levelDims(base, i);
        up_[(size_t)i].px.assign((size_t)up_[(size_t)i].d.w * up_[(size_t)i].d.h * 3, 0.0f);
    }
    ready_ = true;
    return true;
}

void BlurChain::setLevels(int n) { levels_ = clampInt(n, 1, std::max(1, maxLevels_)); }

size_t BlurChain::memoryBytes() const {
    size_t n = synth_.px.size();
    for (const Img& i : down_) n += i.px.size();
    for (const Img& i : up_) n += i.px.size();
    return n * sizeof(float);
}

// The synthesized "scene": a vertical gradient of the base colour with three
// soft blobs (accent, second accent, artwork colour) drifting on slow
// Lissajous paths. Cheap: no transcendental per pixel, just a polynomial
// falloff.
static void synthesize(const BackdropParams& p, BlurChain::Img& img, int y0, int y1);

void BlurChain::begin(const BackdropParams& p) {
    if (!ready_) return;
    params_ = p;
    nStages_ = 0;
    stages_[nStages_++] = Stage{0, 0, base_.h};
    for (int i = 1; i <= levels_; i++) stages_[nStages_++] = Stage{1, i, down_[(size_t)i - 1].d.h};
    for (int i = levels_ - 1; i >= 0; i--) stages_[nStages_++] = Stage{2, i, up_[(size_t)i].d.h};
    stage_ = 0;
    row_ = 0;
}

int BlurChain::totalRows() const {
    if (!ready_) return 0;
    int n = base_.h;
    for (int i = 1; i <= levels_; i++) n += down_[(size_t)i - 1].d.h;
    for (int i = levels_ - 1; i >= 0; i--) n += up_[(size_t)i].d.h;
    return n;
}

bool BlurChain::step(int rowBudget) {
    if (!ready_) return true;
    while (rowBudget > 0 && stage_ < nStages_) {
        const Stage& st = stages_[stage_];
        const int n = std::min(rowBudget, st.rows - row_);
        const int y0 = row_, y1 = row_ + n;
        if (st.kind == 0) {
            synthesize(params_, synth_, y0, y1);
        } else if (st.kind == 1) {
            const Img& s = (st.level == 1) ? synth_ : down_[(size_t)st.level - 2];
            Img& d = down_[(size_t)st.level - 1];
            kawaseDown(s.px, s.d, d.px, d.d, y0, y1);
        } else {
            const Img& s = (st.level == levels_ - 1) ? down_[(size_t)levels_ - 1] : up_[(size_t)st.level + 1];
            Img& d = up_[(size_t)st.level];
            kawaseUp(s.px, s.d, d.px, d.d, y0, y1);
        }
        row_ += n;
        rowBudget -= n;
        if (row_ >= st.rows) {
            stage_++;
            row_ = 0;
        }
    }
    return stage_ >= nStages_;
}

void BlurChain::finish(uint8_t* rgbaOut) {
    if (!ready_ || !rgbaOut) return;
    const Img& out = up_[0];
    for (int y = 0; y < base_.h; y++) {
        for (int x = 0; x < base_.w; x++) {
            // Tiny ordered dither so the smooth gradient does not band in 8 bits.
            const float dither = (float)(((x * 7 + y * 13) & 3) - 1) * (0.5f / 255.0f);
            const float* s = &out.px[((size_t)y * base_.w + x) * 3];
            uint8_t* o = rgbaOut + ((size_t)y * base_.w + x) * 4;
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(clamp01(s[c] + dither) * 255.0f + 0.5f);
            o[3] = 255;
        }
    }
}

void BlurChain::render(const BackdropParams& p, uint8_t* rgbaOut) {
    if (!ready_ || !rgbaOut) return;
    begin(p);
    step(1 << 30);
    finish(rgbaOut);
}

static void synthesize(const BackdropParams& p, BlurChain::Img& img, int y0, int y1) {
    const double t = p.animated ? p.time : 0.0;
    struct Blob {
        float cx, cy, r, strength;
        Rgb col;
    };
    const float artS = p.artStrength < 0.0f ? 0.0f : (p.artStrength > 1.0f ? 1.0f : p.artStrength);
    Blob blobs[3] = {
        {0.22f + 0.16f * (float)std::sin(t * 0.21), 0.30f + 0.14f * (float)std::cos(t * 0.17 + 1.0), 0.55f, p.light ? 0.22f : 0.30f, p.accent},
        {0.78f + 0.14f * (float)std::cos(t * 0.15 + 2.0), 0.70f + 0.16f * (float)std::sin(t * 0.19 + 0.5), 0.55f, p.light ? 0.16f : 0.24f, p.accent2},
        {0.55f + 0.20f * (float)std::sin(t * 0.11 + 4.0), 0.15f + 0.10f * (float)std::cos(t * 0.13), 0.45f,
         artS > 0.0f ? 0.45f * artS : 0.10f, artS > 0.0f ? p.art : p.accent},
    };
    const float aspect = 16.0f / 9.0f;
    const float topGain = p.light ? 1.0f : 1.10f, botGain = p.light ? 0.94f : 0.90f;
    const int w = img.d.w, h = img.d.h;
    for (int y = y0; y < y1; y++) {
        const float v = ((float)y + 0.5f) / (float)h;
        const float gain = topGain + (botGain - topGain) * v;
        for (int x = 0; x < w; x++) {
            const float u = ((float)x + 0.5f) / (float)w;
            float col[3] = {p.base.r * gain, p.base.g * gain, p.base.b * gain};
            for (const Blob& b : blobs) {
                const float dx = (u - b.cx) * aspect, dy = v - b.cy;
                float f = 1.0f - (dx * dx + dy * dy) / (b.r * b.r);
                if (f <= 0.0f) continue;
                f = f * f * b.strength;
                col[0] += (b.col.r - col[0]) * f;
                col[1] += (b.col.g - col[1]) * f;
                col[2] += (b.col.b - col[2]) * f;
            }
            float* o = &img.px[((size_t)y * w + x) * 3];
            o[0] = col[0];
            o[1] = col[1];
            o[2] = col[2];
        }
    }
}

// ------------------------------------------------------------ backdrop manager

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Off: return "off";
        case Mode::Full: return "full";
        case Mode::Low: return "low";
        case Mode::Flat: return "flat";
    }
    return "?";
}

uint32_t BackdropManager::signature(const BackdropParams& p) {
    uint32_t h = 2166136261u;
    auto mix = [&](int v) {
        h ^= (uint32_t)v;
        h *= 16777619u;
    };
    auto q = [](float v) { return (int)(clamp01(v) * 255.0f + 0.5f); };
    mix(q(p.base.r)); mix(q(p.base.g)); mix(q(p.base.b));
    mix(q(p.accent.r)); mix(q(p.accent.g)); mix(q(p.accent.b));
    mix(q(p.accent2.r)); mix(q(p.accent2.g)); mix(q(p.accent2.b));
    mix(q(p.art.r)); mix(q(p.art.g)); mix(q(p.art.b));
    mix((int)(clamp01(p.artStrength) * 64.0f + 0.5f));
    mix(p.light ? 1 : 0);
    mix(p.animated ? 1 : 0);
    return h;
}

bool BackdropManager::enable(Dims dims, const TextureSink& sink, int levels) {
    disable();
    sink_ = sink;
    char msg[160];
    if (!sink.create || !sink.update || !sink.release) {
        toFlat("no texture sink");
        return false;
    }
    if (!chain_.init(dims, levels)) {
        snprintf(msg, sizeof(msg), "invalid blur dimensions %dx%d", dims.w, dims.h);
        toFlat(msg);
        return false;
    }
    rgba_.assign((size_t)dims.w * dims.h * 4, 0);
    chain_.render(BackdropParams(), rgba_.data()); // real content from the first frame, no black flash
    for (int i = 0; i < TEXTURES; i++) {
        tex_[i] = sink_.create(rgba_.data(), dims.w, dims.h);
        stats_.texturesCreated++;
        if (!tex_[i]) {
            toFlat("could not allocate the blur texture");
            return false;
        }
    }
    from_ = to_ = 0;
    swapTime_ = 0.0;
    fadeDur_ = 1.0;
    haveFrame_ = false;
    mode_ = Mode::Full;
    snprintf(msg, sizeof(msg), "Ufin: Glass blur initialized: %dx%d", dims.w, dims.h);
    say(msg);
    snprintf(msg, sizeof(msg), "Ufin: Glass blur passes: %d down + %d up", chain_.levels(), chain_.levels());
    say(msg);
    say("Ufin: Glass mode: enabled");
    return true;
}

void BackdropManager::toFlat(const char* why) {
    for (int i = 0; i < TEXTURES; i++) {
        if (tex_[i] && sink_.release) sink_.release(tex_[i]);
        tex_[i] = 0;
    }
    chain_ = BlurChain();
    rgba_.clear();
    rgba_.shrink_to_fit();
    mode_ = Mode::Flat;
    haveFrame_ = false;
    char msg[192];
    snprintf(msg, sizeof(msg), "Ufin: Glass blur unavailable (%s) -- using flat glass", why);
    say(msg);
}

void BackdropManager::disable() {
    for (int i = 0; i < TEXTURES; i++) {
        if (tex_[i] && sink_.release) sink_.release(tex_[i]);
        tex_[i] = 0;
    }
    chain_ = BlurChain();
    rgba_.clear();
    rgba_.shrink_to_fit();
    mode_ = Mode::Off;
    haveFrame_ = false;
    from_ = to_ = 0;
    lastSig_ = 0;
    lastUpdate_ = 0.0;
}

void BackdropManager::update(double now, const BackdropParams& params, bool videoActive) {
    if (!active()) return;
    if (videoActive) { // playback wins: no work, no texture writes while video is up
        chain_.cancel(); // a half-done regeneration is simply dropped
        stats_.skipped++;
        return;
    }
    if (!chain_.busy()) {
        const uint32_t sig = signature(params);
        const bool due = !haveFrame_ || sig != lastSig_ || now < lastUpdate_ ||
                         (params.animated && now - lastUpdate_ >= ANIMATED_INTERVAL);
        if (!due) {
            stats_.skipped++;
            return;
        }
        chain_.begin(params);
        jobSig_ = sig;
        lastUpdate_ = now;
    }

    // One bounded slice of the regeneration this frame.
    const auto t0 = std::chrono::steady_clock::now();
    const bool done = chain_.step(rowsPerUpdate(chain_.dims().w));
    bool ok = true;
    if (done) {
        chain_.finish(rgba_.data());
        // Never a texture the GPU may still sample: neither the fade source nor target.
        int writeIdx = 0;
        while (writeIdx == from_ || writeIdx == to_) writeIdx++;
        const Dims d = chain_.dims();
        ok = sink_.update(tex_[writeIdx], rgba_.data(), d.w, d.h);
        if (ok) {
            // Fade from what was showing to the new result, over about one regeneration interval.
            if (haveFrame_) {
                const double gap = now - swapTime_;
                const double measured = gap < 0.25 ? 0.25 : (gap > 2.0 ? 2.0 : gap);
                fadeDur_ = fadeDur_ * 0.5 + measured * 0.5;
            }
            from_ = to_;
            to_ = writeIdx;
            swapTime_ = now;
            haveFrame_ = true;
            lastSig_ = jobSig_;
            stats_.updates++;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    stats_.slices++;
    stats_.lastMs = ms;
    stats_.avgMs = stats_.slices == 1 ? ms : stats_.avgMs * 0.9 + ms * 0.1;
    if (!ok) {
        if (++stats_.updateFailures >= 3) toFlat("texture update failed");
        return;
    }

    // Cost guard: fewer passes, then no blur at all, rather than ever costing
    // the frame loop real time.
    if (stats_.slices >= 16) {
        if (stats_.avgMs > FLAT_MS) {
            toFlat("blur too slow");
            return;
        }
        if (stats_.avgMs > LOW_MS && mode_ == Mode::Full && chain_.levels() > 1) {
            chain_.cancel();
            chain_.setLevels(1);
            mode_ = Mode::Low;
            stats_.avgMs = 0.0;
            stats_.slices = 0;
            say("Ufin: Glass blur cost high -- dropping to 1 pass");
            return;
        }
    }
    if (debug_ && now - lastLog_ >= 5.0) {
        lastLog_ = now;
        char msg[160];
        snprintf(msg, sizeof(msg), "Ufin: Glass blur frame time: %.2f ms (avg %.2f ms/frame, %u updates, mode %s)", ms,
                 stats_.avgMs, (unsigned)stats_.updates, modeName(mode_));
        say(msg);
    }
}

float BackdropManager::blend(double now) const {
    if (from_ == to_ || fadeDur_ <= 0.0) return 1.0f;
    double t = (now - swapTime_) / fadeDur_;
    if (!(t > 0.0)) return 0.0f; // also covers a clock that went backwards
    if (t >= 1.0) return 1.0f;
    return (float)(t * t * (3.0 - 2.0 * t)); // smoothstep: no visible start/stop
}

size_t BackdropManager::memoryBytes() const {
    size_t n = chain_.memoryBytes() + rgba_.size();
    if (active()) n += (size_t)chain_.dims().w * chain_.dims().h * 4 * TEXTURES;
    return n;
}

BackdropManager& backdrop() {
    static BackdropManager instance;
    return instance;
}

} // namespace glass
} // namespace ui
