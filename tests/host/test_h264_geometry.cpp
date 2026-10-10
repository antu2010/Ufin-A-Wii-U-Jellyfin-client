// Pins the framebuffer geometry the patched h264_wiiu allocates (see
// src/media/h264_geometry.h) for the resolutions Ufin requests or can be
// handed by Jellyfin, and checks patches/ffmpeg-wiiu-fixes.patch still
// contains the formulas the helper mirrors. Host-only: this proves the
// arithmetic, NOT the behaviour of the Wii U hardware decoder.
#include "check.h"
#include "media/h264_geometry.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace h264geom;

static void expectGeometry(int w, int h, int pitch, int alignedH, size_t y, size_t uv) {
    CHECK_EQ(pitchFor(w), pitch);
    CHECK_EQ(alignedHeightFor(h), alignedH);
    CHECK_EQ((long long)yPlaneBytes(w, h), (long long)y);
    CHECK_EQ((long long)uvPlaneBytes(w, h), (long long)uv);
    CHECK_EQ((long long)chromaOffset(w, h), (long long)y);
    CHECK_EQ((long long)hardwareWriteBytes(w, h), (long long)(y + uv));
    CHECK_EQ((long long)framebufferAllocBytes(w, h), (long long)(y + uv + 0x10000));
    // The allocation must always cover the hardware write, with the slack.
    CHECK(framebufferAllocBytes(w, h) >= hardwareWriteBytes(w, h) + 0x10000);
    // Pitch/height are always at least the visible size.
    CHECK(pitchFor(w) >= w);
    CHECK(alignedHeightFor(h) >= h);
    CHECK(pitchFor(w) % 256 == 0);
    CHECK(alignedHeightFor(h) % 16 == 0);
}

int main(int argc, char** argv) {
    // 1280x720: aligned on both axes -- pre-patch formula was exact.
    expectGeometry(1280, 720, 1280, 720, 921600, 460800);
    CHECK(isAligned(1280, 720));
    CHECK_EQ((long long)hardwareWriteBytes(1280, 720), 1382400LL);
    CHECK_EQ((long long)legacyUnpatchedBytes(1280, 720), 1382400LL);
    CHECK_EQ((long long)framebufferAllocBytes(1280, 720), 1447936LL);

    // 768x480: the 480p low-bandwidth target. Aligned on both axes (3 x 256,
    // 30 x 16), so the pre-patch allocation was already exact -- no unaligned
    // framebuffer risk. (The textbook 854x480 is NOT aligned: pitch 1024.)
    expectGeometry(768, 480, 768, 480, 768u * 480u, 768u * 240u);
    CHECK(isAligned(768, 480));
    CHECK_EQ((long long)hardwareWriteBytes(768, 480), 552960LL);
    CHECK_EQ((long long)legacyUnpatchedBytes(768, 480), 552960LL);
    CHECK_EQ((long long)framebufferAllocBytes(768, 480), 618496LL);
    CHECK_EQ((long long)tightNv12Bytes(768, 480), 552960LL);
    CHECK(tightNv12Bytes(768, 480) < tightNv12Bytes(1280, 720)); // standard (not large-picture) buffering profile
    expectGeometry(854, 480, 1024, 480, 1024u * 480u, 1024u * 240u);
    CHECK(!isAligned(854, 480));
    CHECK_EQ((long long)hardwareWriteBytes(854, 480) - (long long)legacyUnpatchedBytes(854, 480), 122400LL); // the old formula would under-allocate

    // 1280x692 (real Jellyfin output for some sources): the old formula
    // under-allocated by 23,040 bytes.
    expectGeometry(1280, 692, 1280, 704, 901120, 450560);
    CHECK(!isAligned(1280, 692));
    CHECK_EQ((long long)legacyUnpatchedBytes(1280, 692), 1328640LL);
    CHECK_EQ((long long)hardwareWriteBytes(1280, 692) - (long long)legacyUnpatchedBytes(1280, 692), 23040LL);

    // 1920x1080: the experimental target.
    expectGeometry(1920, 1080, 2048, 1088, 2048u * 1088u, 2048u * 544u);
    CHECK_EQ((long long)yPlaneBytes(1920, 1080), 2228224LL);
    CHECK_EQ((long long)uvPlaneBytes(1920, 1080), 1114112LL);
    CHECK_EQ((long long)hardwareWriteBytes(1920, 1080), 3342336LL);
    CHECK_EQ((long long)framebufferAllocBytes(1920, 1080), 3407872LL);
    CHECK_EQ((long long)chromaOffset(1920, 1080), 2228224LL);
    CHECK(!isAligned(1920, 1080));
    CHECK_EQ((long long)hardwareWriteBytes(1920, 1080) - (long long)legacyUnpatchedBytes(1920, 1080), 231936LL);

    // 1920x1088 (already 16-row aligned, width still not 256-aligned).
    expectGeometry(1920, 1088, 2048, 1088, 2048u * 1088u, 2048u * 544u);
    CHECK(!isAligned(1920, 1088));

    // Wide movies scaled by Jellyfin keep a 1920 width, odd heights.
    expectGeometry(1920, 804, 2048, 816, 2048u * 816u, 2048u * 408u);
    expectGeometry(1920, 800, 2048, 800, 2048u * 800u, 2048u * 400u);
    CHECK(!isAligned(1920, 800)); // 1920 % 256 != 0 even though 800 % 16 == 0

    // Tightly packed NV12 AVFrame sizes (used for queue sizing).
    CHECK_EQ((long long)tightNv12Bytes(1280, 720), 1382400LL);
    CHECK_EQ((long long)tightNv12Bytes(1920, 1080), 3110400LL);

    // Change tracker: first frame reports, repeats are silent, changes report.
    {
        GeometryChangeTracker t;
        CHECK(t.changed(1920, 1080, 1920));
        CHECK(!t.changed(1920, 1080, 1920));
        CHECK(!t.changed(1920, 1080, 1920));
        CHECK(t.changed(1920, 800, 1920));
        CHECK(!t.changed(1920, 800, 1920));
        CHECK(t.changed(1920, 800, 1984));
    }

    // The FFmpeg-wiiu patch must still contain the formulas mirrored above.
    {
        std::string text;
        if (argc > 1) {
            std::ifstream f(argv[1]);
            if (f) { std::stringstream ss; ss << f.rdbuf(); text = ss.str(); }
        }
        CHECK(!text.empty());
        CHECK(text.find("(avctx->width + 255) & ~255") != std::string::npos);
        CHECK(text.find("(avctx->height + 15) & ~15") != std::string::npos);
        CHECK(text.find("pitch_a * height_a * 3 / 2 + 0x10000") != std::string::npos);
        CHECK(text.find("(((avctx->height + 15) & ~15) * pitch)") != std::string::npos);
        CHECK(text.find("DCInvalidateRange(framebuffer, ctx->framebuffer_size)") != std::string::npos);
    }

    return check::finish("test_h264_geometry");
}
