// VideoOutput -- takes decoded NV12 video AVFrames and puts them on both
// the TV and the GamePad screen via raw GX2, using WHBGfx (the helper
// library bundled with wut) for display/context setup.
//
// Design, deliberately mirroring CafeMP's working Wii U renderer:
//
//  * Two textures per frame, not one: an R8 texture for the luma (Y)
//    plane at full resolution and an RG8 texture for the interleaved
//    chroma (UV) plane at half resolution -- exactly the memory layout
//    NV12 already has, so uploading a frame is two straight memcpys of
//    the decoder's planes. The pixel shader (shaders/nv12_video.frag)
//    does the YUV->RGB conversion on the GPU. The earlier design here
//    converted to RGBA on the CPU with libswscale first; on the Wii U's
//    CPU that alone costs more than a 30 fps frame budget at 720p, so
//    even had it displayed it could never have kept up.
//
//  * Each plane is double-buffered (write into one texture while the
//    GPU may still be sampling the other), matching CafeMP's
//    VideoPlane::tex[2].
//
//  * The video is drawn aspect-fitted (letterboxed / pillarboxed) into
//    whatever resolution WHBGfx actually gave us for each target, rather
//    than assuming 1280x720 -- the TV colour buffer is 1920x1080 on a
//    1080p TV and 854x480 or 640x480 on a 480p one, and the GamePad is
//    always 854x480.
//
// SDL2 is deliberately not involved in drawing at all (only audio uses
// it): SDL2's texture rendering path was confirmed to produce no output
// on this platform despite every call succeeding.

#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
}

#include <whb/gfx.h>
#include <gx2/enum.h>
#include <gx2/texture.h>
#include <gx2/sampler.h>
#include <gx2/surface.h>

#include <cstdint>

class VideoOutput {
public:
    VideoOutput();
    ~VideoOutput();

    // Brings up the GX2 context for the whole app's lifetime. Call this
    // exactly once, near the top of main(), BEFORE OSScreenDisplay::init()
    // ever runs -- and shutdownGX2Context() exactly once, at the very end,
    // AFTER the final OSScreenDisplay::shutdown(). Leaves TV/DRC output
    // disabled (GX2SetTVEnable/DRCEnable FALSE) so OSScreen alone drives
    // the display until a VideoOutput instance's init() below re-enables
    // it. See the big comment in os_screen_display.h for why this exists:
    // tearing the whole GX2 context down and rebuilding it per playback
    // session (the original design) left GX2 computing correct frames
    // that real hardware never actually scanned out.
    static bool initGX2Context();
    static void shutdownGX2Context();

    // width/height must match the decoded frames (Decoder::videoWidth()/
    // videoHeight()). displayAspect is the aspect ratio the picture
    // should be *shown* at -- normally width/height, but Jellyfin is
    // asked to encode everything at exactly 1280x720 (see
    // JellyfinClient::buildVideoStreamUrl for why), so a 2.39:1 movie
    // arrives squeezed into 16:9 and this is how it gets un-squeezed.
    // Pass <= 0 to use width/height.
    bool init(int width, int height, double displayAspect);

    // Uploads one NV12 frame to the GPU and draws + presents it on both
    // screens. Frames of any other pixel format are ignored (logged once).
    void renderFrame(AVFrame* frame);

    // Diagnostic: fills the planes with a flat colour and draws +
    // presents through the exact same shader/draw/present path as
    // renderFrame(), with no decoder involved. Lets the GX2 pipeline be
    // checked on its own (hold ZR in the menu -- see main.cpp).
    void renderTestPattern();

    void shutdown();

    const char* lastError() const { return last_error_; }

private:
    struct Plane {
        GX2Texture tex[2]{};
        GX2Sampler sampler{};
        uint32_t bytesPerTexel = 1;
        bool valid = false;
    };

    int width_ = 0;
    int height_ = 0;
    char last_error_[256] = {0};

    bool gfx_initialized_ = false;
    WHBGfxShaderGroup shader_{};
    uint32_t y_sampler_location_ = 0;
    uint32_t uv_sampler_location_ = 1;

    Plane y_plane_;   // R8, width_ x height_
    Plane uv_plane_;  // RG8, width_/2 x height_/2
    int write_index_ = 0;

    // One aspect-fitted quad per target, since their resolutions (and
    // on a 4:3 TV, aspect) differ. 4 vertices of {x, y, u, v}.
    void* tv_quad_ = nullptr;
    void* drc_quad_ = nullptr;
    uint32_t tv_width_ = 0, tv_height_ = 0;
    uint32_t drc_width_ = 0, drc_height_ = 0;

    int unsupported_format_logged_ = 0;

    bool loadShader();
    bool createPlane(Plane& plane, GX2SurfaceFormat format, uint32_t compMap,
                     int width, int height, const char* name);
    void destroyPlane(Plane& plane);
    void fillPlane(Plane& plane, int index, uint8_t byte0, uint8_t byte1);
    void uploadPlane(Plane& plane, int index, const uint8_t* src, int srcLinesize,
                     int bytesPerRow, int rows);
    void* buildQuad(uint32_t targetWidth, uint32_t targetHeight, double displayAspect);
    void drawQuad(int readIndex, const void* quad, uint32_t targetWidth, uint32_t targetHeight);
    void present(int readIndex);
};
