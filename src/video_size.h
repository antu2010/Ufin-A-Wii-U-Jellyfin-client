// video_size.h -- the transcode sizes Ufin will ask Jellyfin for, in one place.
//
// Both the config loader (video_height -> width) and the stream URL builder
// (which exact Width/Height go on the wire) use this, so they cannot disagree.
//
// Why these three and nothing else: the Wii U hardware H.264 decoder writes
// rows at a 256-pixel-aligned pitch and a 16-row-aligned height (see
// media/h264_geometry.h and the long comment in
// JellyfinClient::buildVideoStreamUrl). Jellyfin is asked for an EXACT size
// (it squeezes any picture to fit, and VideoOutput un-squeezes it at draw time
// from the item's real aspect ratio), so the size must be chosen with that
// alignment in mind:
//
//   480p   768 x  480   aligned on both axes (3 x 256, 30 x 16). Not the
//                       textbook 854x480: 854 is not a multiple of 256, so the
//                       hardware pitch would be 1024 -- the same unaligned-
//                       framebuffer risk as 1080p, for no benefit.
//   720p  1280 x  720   aligned on both axes (5 x 256, 45 x 16). The stable one.
//   1080p 1920 x 1080   NOT aligned (7.5 x 256, 67.5 x 16): depends on the
//                       patched decoder -- which is why it is Experimental.
//
// Anything else is treated as 720p rather than producing an untested geometry.
#pragma once

namespace videosize {

struct Size {
    int width;
    int height;
};

constexpr Size k480p = {768, 480};
constexpr Size k720p = {1280, 720};
constexpr Size k1080p = {1920, 1080};

constexpr bool supportedHeight(int height) { return height == 480 || height == 720 || height == 1080; }

// The exact size for a configured height; unsupported heights give 720p.
constexpr Size forHeight(int height) {
    return height == 480 ? k480p : (height == 1080 ? k1080p : k720p);
}

// The exact size for a requested width x height pair; only the three pairs
// above are honoured, anything else gives 720p.
constexpr Size forRequest(int width, int height) {
    return (width == k480p.width && height == k480p.height)     ? k480p
           : (width == k1080p.width && height == k1080p.height) ? k1080p
                                                                : k720p;
}

// Bitrate (bits/s) used when config.json names a video_height but no
// video_bitrate. 720p keeps the long-standing 2.5 Mbps default.
constexpr int defaultBitrateFor(int height) {
    return height == 480 ? 1500000 : (height == 1080 ? 6000000 : 2500000);
}

} // namespace videosize
