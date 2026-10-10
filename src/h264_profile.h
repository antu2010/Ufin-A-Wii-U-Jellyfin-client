// h264_profile.h -- the H.264 profile Ufin asks Jellyfin to transcode to, in one place.
//
// The config loader, the Settings row and the stream URL builder all go through
// this header, so the three names, their descriptions, the default and the
// sanitising rule cannot drift apart.
//
// WII U CONSTRAINT: h264_wiiu hands back one picture per H264DECExecute call, in
// decode order. Baseline has no B-frames, so decode order == display order. Main/High
// may use B-frames (Jellyfin's stream URL has no switch to forbid them), so the player
// puts the pictures back in display order itself: see media/frame_reorder.h. That part
// works (reorder_late=0 on hardware); the open problem is that h264_wiiu does not return
// the non-reference B pictures at all -- see kDefault.
#pragma once

#include <string>

namespace h264 {

enum class Profile { Main, Baseline, High };

// Menu order: the Settings row cycles in this order.
constexpr Profile kOrder[] = {Profile::Main, Profile::Baseline, Profile::High};
constexpr int kCount = 3;

// Single source of truth for the default.
// Baseline stays the default: it needs no decoder patch. Main/High need the patched h264_wiiu
// (docs/ffmpeg-wiiu-build.md) -- the stock decoder drops the non-reference B-frames and plays
// them at half frame rate. Change this to Profile::Main if every build you ship has the patch.
constexpr Profile kDefault = Profile::Baseline;

inline const char* wireName(Profile p) { // lowercase: config.json value and the Jellyfin "Profile=" value
    switch (p) {
        case Profile::Main: return "main";
        case Profile::High: return "high";
        default: return "baseline";
    }
}

inline const char* displayName(Profile p) {
    switch (p) {
        case Profile::Main: return "Main";
        case Profile::High: return "High";
        default: return "Baseline";
    }
}

inline const char* description(Profile p) {
    switch (p) {
        case Profile::Main:
            return "Better quality than Baseline at the same bitrate -- a good fit for Wi-Fi";
        case Profile::High:
            return "Most efficient compression -- best for 1080p on Ethernet or strong Wi-Fi";
        default:
            return "Maximum compatibility -- needs the most bandwidth for the same quality";
    }
}

// Letters only, case-insensitive; anything unknown falls back to the default.
inline Profile parse(const std::string& s) {
    std::string t;
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) t += (char)(c | 0x20);
    }
    if (t == "main") return Profile::Main;
    if (t == "high") return Profile::High;
    if (t == "baseline") return Profile::Baseline;
    return kDefault;
}

// Next profile in menu order, wrapping.
inline Profile next(Profile p) {
    for (int i = 0; i < kCount; i++) {
        if (kOrder[i] == p) return kOrder[(i + 1) % kCount];
    }
    return kDefault;
}

// H.264 profile_idc (from the avcC box / SPS) -> name, for logging what the server
// really produced. "?" for anything else (extended, 10-bit, ...); 0 = unknown.
inline const char* nameForProfileIdc(int idc) {
    switch (idc) {
        case 66: return "baseline";
        case 77: return "main";
        case 100: return "high";
        case 0: return "unknown";
        default: return "?";
    }
}

} // namespace h264
