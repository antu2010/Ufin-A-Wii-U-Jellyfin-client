// profile_anim.h -- the bounce of a picture on the Who's watching screen. Pure maths (no ImGui), so
// the host tests can step it.
//
// One Bounce per picture. While its picture is the hovered one (the cursor is on it) it jumps again
// and again: it squashes down (anticipation), springs up stretched tall and thin, floats, falls,
// splats wide and flat on landing and wobbles back to round -- squash and stretch driven by a damped
// spring, so the shape really distorts rather than just moving. Every other picture sits perfectly
// still (a picture that loses the hover drops quickly and settles; it does not keep bouncing).
//
// Units: `lift` is in picture diameters (1.0 = one picture high above the ground line).
#pragma once

#include <algorithm>
#include <cmath>

namespace ui {

// What the renderer needs for one picture.
struct BounceShape {
    float lift = 0;   // how far the picture's bottom is above the ground line (diameters, >= 0)
    float sx = 1;     // width scale
    float sy = 1;     // height scale (the bottom stays on the ground / lifted line)
    float shear = 0;  // sideways lean of the top, -1..1 (jelly wobble)
    bool still() const { return lift == 0.0f && sx == 1.0f && sy == 1.0f && shear == 0.0f; }
};

class Bounce {
public:
    enum Phase { Rest, Anticipate, Air, Land };

    static constexpr float kJumpHeight = 0.30f;   // diameters
    static constexpr float kAirTime = 0.50f;      // seconds for a full jump
    static constexpr float kAnticipate = 0.14f;   // seconds of squashing down before take-off
    static constexpr float kLandMin = 0.22f;      // seconds on the ground after a landing before the next jump
    static constexpr float kSquashMax = 0.30f;    // how flat the landing makes it (fraction of the height)
    static constexpr float kStretchMax = 0.24f;   // how tall the take-off makes it

    // Advances by dt seconds. `hovered`: the cursor is on this picture.
    void step(float dt, bool hovered) {
        dt = std::max(0.0f, std::min(dt, 0.25f));
        const float h = 1.0f / 120.0f; // fixed sub-steps: the spring stays stable at any frame rate
        timeLeft_ += dt;
        while (timeLeft_ >= h) {
            timeLeft_ -= h;
            substep(h, hovered);
        }
    }

    BounceShape shape() const {
        BounceShape s;
        if (phase_ == Rest && sq_ == 0.0f && sqv_ == 0.0f && lift_ == 0.0f) return s;
        s.lift = lift_ > 0 ? lift_ : 0.0f;
        s.sy = 1.0f + sq_;
        s.sx = 1.0f - 0.65f * sq_;
        s.shear = std::max(-0.12f, std::min(0.12f, -sqv_ * 0.012f));
        return s;
    }

    Phase phase() const { return phase_; }
    bool resting() const { return phase_ == Rest; }

private:
    static constexpr float kV0 = 4.0f * kJumpHeight / kAirTime;                // launch speed
    static constexpr float kG = 8.0f * kJumpHeight / (kAirTime * kAirTime);    // gravity

    void substep(float h, bool hovered) {
        float target = 0.0f; // where the squash/stretch spring is pulled to
        switch (phase_) {
            case Rest:
                if (hovered) { phase_ = Anticipate; t_ = 0.0f; }
                break;
            case Anticipate:
                t_ += h;
                if (!hovered) { phase_ = Land; t_ = kLandMin; break; }
                target = -kSquashMax * 0.9f * std::min(1.0f, t_ / kAnticipate);
                if (t_ >= kAnticipate) { // take off
                    phase_ = Air;
                    lift_ = 0.0f;
                    vy_ = kV0;
                }
                break;
            case Air:
                if (!hovered && vy_ > -2.4f) vy_ = -2.4f; // the cursor moved away: drop now, no more hops
                vy_ -= kG * h;
                lift_ += vy_ * h;
                target = kStretchMax * std::min(1.0f, std::fabs(vy_) / kV0);
                if (lift_ <= 0.0f && vy_ < 0.0f) { // landed: splat, speed in proportion to the fall
                    const float impact = std::min(1.0f, std::fabs(vy_) / kV0);
                    lift_ = 0.0f;
                    vy_ = 0.0f;
                    sq_ = std::min(sq_, 0.0f) * 0.5f;
                    sqv_ = -impact * 11.0f;
                    phase_ = Land;
                    t_ = 0.0f;
                }
                break;
            case Land:
                t_ += h;
                if (t_ >= kLandMin && std::fabs(sq_) < 0.012f && std::fabs(sqv_) < 0.15f) {
                    sq_ = 0.0f;
                    sqv_ = 0.0f;
                    phase_ = hovered ? Anticipate : Rest;
                    t_ = 0.0f;
                }
                break;
        }
        // Damped spring (under-damped: it overshoots and wobbles) toward the target shape.
        const float k = 380.0f, c = 15.0f;
        sqv_ += (-k * (sq_ - target) - c * sqv_) * h;
        sq_ += sqv_ * h;
    }

    Phase phase_ = Rest;
    float t_ = 0.0f;
    float lift_ = 0.0f, vy_ = 0.0f;  // height above the ground (diameters) and its speed
    float sq_ = 0.0f, sqv_ = 0.0f;   // squash (<0) / stretch (>0) of the height, and its speed
    float timeLeft_ = 0.0f;
};

} // namespace ui
