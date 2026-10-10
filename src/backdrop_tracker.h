// backdrop_tracker.h -- decides which Jellyfin backdrop is behind the UI and how faded in it is.
//
// Pure logic (no graphics, no clock of its own) so it can be tested on the host. Every frame the
// app tells it which backdrop path the UI WANTS (the selected title's, else the page's, "" = none)
// and whether the texture for the one it is showing is loaded yet; it answers with up to two
// layers to draw: `prev` (fully opaque, what was up before) and `cur` (fading in over it).
//
//  * Settling: scrolling quickly through a list changes the wanted path every frame. It only
//    becomes the new target after it has stayed put for `settleSeconds`, so fast scrolling does
//    not fetch a full-screen picture per row.
//  * Cross-fade: the new backdrop fades in over the old one (never through a blank frame). If it
//    does not arrive within `giveUpSeconds` (failed download), the old one is dropped.
//  * No backdrop wanted: the current one fades out, revealing the normal background underneath.
#pragma once

#include <string>

class BackdropTracker {
public:
    BackdropTracker(double settleSeconds = 0.35, double fadeSeconds = 0.6, double giveUpSeconds = 5.0)
        : settle_(settleSeconds), fade_(fadeSeconds), giveUp_(giveUpSeconds) {}

    // Call once per frame. `curReady`: the texture for curPath() is available right now.
    void update(const std::string& wanted, bool curReady, double now) {
        const double dt = lastNow_ < 0 ? 0.0 : clamp(now - lastNow_, 0.0, 0.1);
        lastNow_ = now;

        if (wanted != wanted_) {
            wanted_ = wanted;
            wantedSince_ = now;
        }
        const bool settled = wanted_.empty() || (now - wantedSince_ >= settle_);
        bool switched = false; // the shown path changed in this call

        if (settled && wanted_ != target_) {
            target_ = wanted_;
            if (target_.empty()) {
                // Fade the current one out; nothing replaces it.
                prev_.clear();
                fadingOut_ = !cur_.empty();
            } else {
                // New backdrop: the old one (even half-faded) becomes the opaque layer underneath.
                fadingOut_ = false;
                if (!cur_.empty() && curAlpha_ > 0.0f) prev_ = cur_;
                else if (cur_.empty()) { /* keep whatever prev_ is: still on screen */ }
                cur_ = target_;
                curAlpha_ = 0.0f;
                switchedAt_ = now;
                switched = true;
            }
        }

        if (fadingOut_) {
            curAlpha_ -= (float)(dt / fade_);
            if (curAlpha_ <= 0.0f) {
                curAlpha_ = 0.0f;
                cur_.clear();
                fadingOut_ = false;
            }
        } else if (!cur_.empty()) {
            if (switched) curReady = false; // the caller's flag was about the previous path
            if (curReady) {
                curAlpha_ += (float)(dt / fade_);
                if (curAlpha_ >= 1.0f) {
                    curAlpha_ = 1.0f;
                    prev_.clear(); // fully covered
                }
            } else if (!prev_.empty() && now - switchedAt_ > giveUp_) {
                prev_.clear(); // the new one is not coming: go back to the normal background
            }
        }
    }

    // Path of the backdrop being shown / loaded ("" = none). Ask the image cache for it every
    // frame and report curReady.
    const std::string& curPath() const { return cur_; }
    float curAlpha() const { return curAlpha_; }
    // Path of the opaque layer underneath while a cross-fade runs ("" = none). Also keep asking
    // the image cache for it every frame, so it is not evicted mid-fade.
    const std::string& prevPath() const { return prev_; }

    // 0..1 how much of the normal background is covered (for skipping its drawing).
    float coverage() const { return prev_.empty() ? curAlpha_ : 1.0f; }

private:
    static double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    double settle_, fade_, giveUp_;
    std::string wanted_, target_, cur_, prev_;
    double wantedSince_ = 0.0, switchedAt_ = 0.0, lastNow_ = -1.0;
    float curAlpha_ = 0.0f;
    bool fadingOut_ = false;
};
