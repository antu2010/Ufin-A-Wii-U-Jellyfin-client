// h264_geometry.h -- the framebuffer geometry the patched h264_wiiu decoder
// (patches/ffmpeg-wiiu-fixes.patch) allocates and reads, as pure functions.
//
// NOTHING here drives the decoder. It mirrors, in one testable place, the
// arithmetic that lives in the FFmpeg-wiiu patch so that Ufin can
//   * log what it *expects* the hardware to have written (diagnostics), and
//   * have host tests pin the numbers for the resolutions Ufin requests.
// tests/host/test_h264_geometry.cpp also checks that the patch file still
// contains these exact formulas, so the two cannot drift apart silently.
//
// The rules (from the patch, not from Wii U documentation):
//   pitch        = width  rounded up to a multiple of 256
//   aligned rows = height rounded up to a multiple of 16
//   Y plane      = pitch * aligned rows
//   UV plane     = pitch * aligned rows / 2      (interleaved NV12)
//   chroma start = pitch * aligned rows          (bytes from buffer start)
//   allocation   = Y + UV + 0x10000 bytes of slack
#pragma once
#include <cstddef>
#include <cstdint>

namespace h264geom {

constexpr int kPitchAlign = 256;
constexpr int kHeightAlign = 16;
constexpr size_t kFramebufferSlack = 0x10000; // 64 KiB, same as the patch

constexpr int alignUp(int v, int a) { return (v + a - 1) & ~(a - 1); }

constexpr int pitchFor(int width) { return alignUp(width, kPitchAlign); }
constexpr int alignedHeightFor(int height) { return alignUp(height, kHeightAlign); }

constexpr size_t yPlaneBytes(int width, int height) {
    return (size_t)pitchFor(width) * (size_t)alignedHeightFor(height);
}
constexpr size_t uvPlaneBytes(int width, int height) { return yPlaneBytes(width, height) / 2; }
constexpr size_t chromaOffset(int width, int height) { return yPlaneBytes(width, height); }
// What the hardware is expected to write for one picture.
constexpr size_t hardwareWriteBytes(int width, int height) {
    return yPlaneBytes(width, height) + uvPlaneBytes(width, height);
}
// What the patched h264_wiiu_decode_init allocates.
constexpr size_t framebufferAllocBytes(int width, int height) {
    return hardwareWriteBytes(width, height) + kFramebufferSlack;
}
// The pre-patch (buggy) size: tightly packed W*H*1.5. Kept only so tests can
// show how large the original overflow was.
constexpr size_t legacyUnpatchedBytes(int width, int height) {
    return ((size_t)width * (size_t)height * 3) >> 1;
}
// True when the request is aligned on both axes, i.e. the geometry on which
// the original (unpatched) allocation happened to be correct.
constexpr bool isAligned(int width, int height) {
    return width % kPitchAlign == 0 && height % kHeightAlign == 0;
}

// Size of one decoded NV12 AVFrame if its planes are tightly packed.
constexpr size_t tightNv12Bytes(int width, int height) {
    return (size_t)width * (size_t)height + (size_t)width * (size_t)((height + 1) / 2);
}

// Logs the first decoded frame's geometry and again only when it changes
// (never once per frame). Pure state machine; the caller does the printing.
class GeometryChangeTracker {
public:
    // Returns true if (w,h,linesizeY) differs from the last reported values
    // (always true for the first call).
    bool changed(int w, int h, int linesizeY) {
        if (seen_ && w == w_ && h == h_ && linesizeY == ls_) return false;
        seen_ = true; w_ = w; h_ = h; ls_ = linesizeY;
        return true;
    }
private:
    bool seen_ = false;
    int w_ = 0, h_ = 0, ls_ = 0;
};

} // namespace h264geom
