// quality_presets.h -- the "Video quality" choices in Settings, as plain data.
//
// Kept out of main.cpp so host tests can check the table (names, texts, sizes,
// bitrates) without the Wii U toolchain.
//
// A preset only sets size and bitrate. The H.264 profile is its own setting
// (Settings > H.264 profile, h264_profile.h), so picking a preset never changes it.
//
// Naming: every option is "<tier> <resolution>". The tier names (Low / Safe /
// Medium / High) rise with bitrate within a resolution; 480p only has the two
// lowest, 1080p only the three above Low. The 1080p names carry EXPERIMENTAL
// (so their detail line need not repeat it -- the value tag is wide, and the
// detail line is ellipsized when tag + detail get too long for the row).
//
// Order is ascending by resolution, then bitrate. Cycling past the last
// (1080p High) wraps to the first (480p Low).
//
// Hand-editing video_bitrate / video_height in config.json
// still works and shows as "Custom" until a preset is picked again.
#pragma once
#include "video_size.h"

#include <cstring>
#include <string>

namespace quality {

struct Preset {
    const char* name;    // shown as the setting's value
    int bitrate;         // bits/s
    int height;          // 480 / 720 / 1080; the width follows (video_size.h)
    const char* detail;  // the line under the setting
};

inline const Preset* presets(int& count) {
    static const Preset table[] = {
        // 480p: smallest picture, for really slow connections.
        {"Low 480p", 750000, 480,
         "768x480, 750 kbps -- lower quality, better for really slow internet"},
        {"Safe 480p", 1500000, 480,
         "768x480, 1.5 Mbps -- lower quality, better for really slow internet"},
        // 720p: the stable, tested resolution. Texts unchanged.
        {"Low 720p", 1000000, 720, "1 Mbps -- best for a weak or busy Wi-Fi network"},
        {"Safe 720p", 2500000, 720, "2.5 Mbps -- the tested default"},
        {"Medium 720p", 4000000, 720, "4 Mbps -- sharper picture on a solid connection"},
        {"High 720p", 8000000, 720, "8 Mbps -- best quality, needs a strong network"},
        // 1080p: experimental and last, so cycling up from "High 720p" is the
        // only way in and the stable presets keep their order.
        {"Safe 1080p EXPERIMENTAL", 4000000, 1080,
         "1920x1080, 4 Mbps -- may crash; needs a strong network"},
        {"Medium 1080p EXPERIMENTAL", 6000000, 1080,
         "1920x1080, 6 Mbps -- may crash; needs a strong network"},
        {"High 1080p EXPERIMENTAL", 8000000, 1080,
         "1920x1080, 8 Mbps -- may crash; needs a strong network"},
    };
    count = (int)(sizeof(table) / sizeof(table[0]));
    return table;
}

// The preset matching the current settings, or -1 ("Custom": hand-edited).
inline int indexFor(int bitrate, int height) {
    int count;
    const Preset* p = presets(count);
    for (int i = 0; i < count; i++) {
        if (bitrate == p[i].bitrate && height == p[i].height) return i;
    }
    return -1;
}

// The tested default (Safe 720p).
inline int defaultIndex() {
    int count;
    const Preset* p = presets(count);
    for (int i = 0; i < count; i++) {
        if (p[i].height == 720 && p[i].bitrate == videosize::defaultBitrateFor(720)) return i;
    }
    return 0;
}

// What pressing A on the row selects: the next preset, wrapping; from Custom
// it lands on the tested default rather than on the very first (480p) entry.
inline int nextIndex(int current) {
    int count;
    presets(count);
    return current < 0 ? defaultIndex() : (current + 1) % count;
}

} // namespace quality
