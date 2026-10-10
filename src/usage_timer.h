// usage_timer.h -- how long Ufin has been used, for the sidebar's "Time used" counter.
//
// Pure logic (no clock, no I/O of its own) so it can be tested on the host: the caller passes
// the current time in seconds (a monotonic clock) to every call.
//
//  * The total is what the config remembered from earlier runs plus the time this run has been
//    going, minus any time the app spent suspended in the HOME menu (excludeSeconds()).
//  * It is written back to config.json every 30 minutes (dueForSave / markSaved) and again on a
//    clean exit -- not more often, to spare the SD card.
//  * TapCounter detects the hidden "tap the counter five times" gesture.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

class UsageTimer {
public:
    static constexpr double kSaveIntervalSeconds = 30.0 * 60.0;

    // `savedSeconds`: the total from config.json; `now`: seconds on the monotonic clock.
    void start(int64_t savedSeconds, double now) {
        base_ = savedSeconds < 0 ? 0 : savedSeconds;
        startedAt_ = now;
        excluded_ = 0.0;
        nextSave_ = now + kSaveIntervalSeconds;
    }

    // Total seconds used, including earlier runs.
    int64_t totalSeconds(double now) const {
        double run = now - startedAt_ - excluded_;
        return base_ + (run > 0.0 ? (int64_t)run : 0);
    }

    // Time that should not count (the app was suspended in the HOME menu).
    void excludeSeconds(double seconds) {
        if (seconds > 0.0) excluded_ += seconds;
    }

    bool dueForSave(double now) const { return now >= nextSave_; }
    void markSaved(double now) { nextSave_ = now + kSaveIntervalSeconds; }

private:
    int64_t base_ = 0;
    double startedAt_ = 0.0, excluded_ = 0.0, nextSave_ = 0.0;
};

// "N taps in a row, each within `window` seconds of the last". tap() returns true on the Nth.
class TapCounter {
public:
    explicit TapCounter(int needed = 5, double windowSeconds = 1.5) : needed_(needed), window_(windowSeconds) {}

    bool tap(double now) {
        if (count_ > 0 && now - last_ > window_) count_ = 0; // too slow: start over
        count_++;
        last_ = now;
        if (count_ >= needed_) {
            count_ = 0;
            return true;
        }
        return false;
    }
    void reset() { count_ = 0; }
    int count() const { return count_; }

private:
    int needed_;
    double window_;
    int count_ = 0;
    double last_ = 0.0;
};

// "34m", "12h 34m", "3d 4h" -- the compact form for the sidebar.
inline std::string formatUsageShort(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const int64_t d = seconds / 86400, h = (seconds % 86400) / 3600, m = (seconds % 3600) / 60;
    char buf[48];
    if (d > 0) snprintf(buf, sizeof(buf), "%lldd %lldh", (long long)d, (long long)h);
    else if (h > 0) snprintf(buf, sizeof(buf), "%lldh %02lldm", (long long)h, (long long)m);
    else snprintf(buf, sizeof(buf), "%lldm", (long long)m);
    return buf;
}

// The big figure in the celebration bubble: "12h 34m", "5m 07s", "42s".
inline std::string formatUsageBig(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const int64_t h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    char buf[48];
    if (h > 0) snprintf(buf, sizeof(buf), "%lldh %02lldm", (long long)h, (long long)m);
    else if (m > 0) snprintf(buf, sizeof(buf), "%lldm %02llds", (long long)m, (long long)s);
    else snprintf(buf, sizeof(buf), "%llds", (long long)s);
    return buf;
}

// "45,245 seconds in total" -- the exact figure that is stored in config.json.
inline std::string formatUsageSeconds(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    std::string digits = std::to_string((long long)seconds), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out + (seconds == 1 ? " second in total" : " seconds in total");
}
