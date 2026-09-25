// Where a skip lands: position + delta, kept inside the item. The upper
// limit stays a few seconds short of the end, so skipping forward near
// the end still shows the last moments instead of a stream that ends
// before its first frame.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

inline double clampSeek(double position, double delta, double duration) {
    double target = position + delta;
    if (target < 0.0) target = 0.0;
    if (duration > 0.0) {
        double latest = duration - 5.0;
        if (latest < 0.0) latest = 0.0;
        if (target > latest) target = latest;
    }
    return target;
}

// Converts a stream-clock time to a position in the item. A transcode
// started at StartTimeTicks usually has timestamps starting at 0 (so the
// offset has to be added), but some FFmpeg setups keep the original
// timestamps; the first timestamp seen tells them apart.
inline double itemPositionFromStreamTime(double streamTime, double startOffset, double firstStreamTime) {
    if (std::isnan(streamTime)) return NAN;
    if (startOffset <= 0.0) return streamTime;
    if (!std::isnan(firstStreamTime) && firstStreamTime >= startOffset - 2.0) return streamTime;
    return startOffset + streamTime;
}

// A new PlaySessionId for every stream start, including every seek.
// Jellyfin names a transcode's output file after media + user agent +
// DeviceId + PlaySessionId only -- not the start time -- and reuses an
// existing file from its beginning. Without a fresh id, the request that
// restarts the transcode at the seek position gets the old transcode
// instead, played from 0:00. `seed` should differ per call (time); the
// counter makes back-to-back calls distinct regardless.
inline std::string makePlaySessionId(uint64_t seed, uint32_t counter) {
    // splitmix64 so ids look unrelated even for nearby seeds.
    auto mix = [](uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    uint64_t a = mix(seed ^ ((uint64_t)counter << 32)), b = mix(a ^ counter);
    char buf[40];
    snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
    return buf;
}
