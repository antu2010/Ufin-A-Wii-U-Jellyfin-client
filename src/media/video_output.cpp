#include "video_output.h"
#include "shaders/nv12_video_shader.h"
#include "../ufin_log.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/utils.h>
#include <gx2r/surface.h>

static const uint32_t QUAD_VERTEX_COUNT = 4;
static const uint32_t QUAD_VERTEX_STRIDE = sizeof(float) * 4; // x, y, u, v
static const uint32_t QUAD_BYTES = QUAD_VERTEX_COUNT * QUAD_VERTEX_STRIDE;

static bool s_gx2ContextReady = false;

bool VideoOutput::initGX2Context() {
    if (s_gx2ContextReady) return true;
    if (!WHBGfxInit()) {
        OSReport("Ufin: WHBGfxInit failed\n");
        return false;
    }
    // Kept hidden until a VideoOutput session's init() below turns it on
    // -- OSScreen is what should be visible right after this call, since
    // it runs before OSScreenDisplay::init() at app start.
    GX2SetTVEnable(FALSE);
    GX2SetDRCEnable(FALSE);
    s_gx2ContextReady = true;
    OSReport("Ufin: GX2 context initialised (persistent for app lifetime)\n");
    return true;
}

void VideoOutput::shutdownGX2Context() {
    if (!s_gx2ContextReady) return;
    WHBGfxShutdown();
    s_gx2ContextReady = false;
}

VideoOutput::VideoOutput() {}

VideoOutput::~VideoOutput() {
    shutdown();
}

// Sampler binding slots come from the compiled shader; look them up by
// the uniform's name so a reordering in nv12_video.frag can't silently
// bind the chroma texture to the luma sampler.
static uint32_t findSamplerLocation(const GX2PixelShader* ps, const char* name, uint32_t fallback) {
    if (!ps) return fallback;
    for (uint32_t i = 0; i < ps->samplerVarCount; i++) {
        if (ps->samplerVars[i].name && strcmp(ps->samplerVars[i].name, name) == 0) {
            return ps->samplerVars[i].location;
        }
    }
    OSReport("Ufin: sampler '%s' not found in pixel shader, assuming slot %u\n", name, fallback);
    return fallback;
}

bool VideoOutput::loadShader() {
    // nv12_video_shader_data comes from the auto-generated header,
    // produced at build time by compiling shaders/nv12_video.{vert,frag}
    // with glslcompiler.elf (see CMakeLists.txt) and converting the
    // resulting .gsh with tools/bin2h.py.
    if (!WHBGfxLoadGFDShaderGroup(&shader_, 0, nv12_video_shader_data)) {
        snprintf(last_error_, sizeof(last_error_), "WHBGfxLoadGFDShaderGroup failed");
        OSReport("Ufin: WHBGfxLoadGFDShaderGroup failed\n");
        return false;
    }

    // Names must match the vertex shader's inputs (nv12_video.vert).
    // Both attributes live in the one vertex buffer: position at byte 0,
    // texcoord at byte 8 of each 16-byte vertex.
    if (!WHBGfxInitShaderAttribute(&shader_, "in_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32)) {
        snprintf(last_error_, sizeof(last_error_), "shader has no 'in_pos' attribute");
        OSReport("Ufin: WHBGfxInitShaderAttribute(in_pos) failed\n");
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&shader_, "in_uv", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32)) {
        snprintf(last_error_, sizeof(last_error_), "shader has no 'in_uv' attribute");
        OSReport("Ufin: WHBGfxInitShaderAttribute(in_uv) failed\n");
        return false;
    }

    if (!WHBGfxInitFetchShader(&shader_)) {
        snprintf(last_error_, sizeof(last_error_), "WHBGfxInitFetchShader failed");
        OSReport("Ufin: WHBGfxInitFetchShader failed\n");
        return false;
    }

    y_sampler_location_ = findSamplerLocation(shader_.pixelShader, "tex_y", 0);
    uv_sampler_location_ = findSamplerLocation(shader_.pixelShader, "tex_uv", 1);

    OSReport("Ufin: nv12 shader loaded ok (tex_y slot %u, tex_uv slot %u)\n",
             y_sampler_location_, uv_sampler_location_);
    return true;
}

bool VideoOutput::createPlane(Plane& plane, GX2SurfaceFormat format, uint32_t compMap,
                              int width, int height, const char* name) {
    plane.bytesPerTexel = (format == GX2_SURFACE_FORMAT_UNORM_R8_G8) ? 2 : 1;

    // GX2R-managed, CPU-writable / GPU-readable, linear (untiled) so the
    // decoder's rows can be copied straight in. Field-for-field the same
    // setup as CafeMP's alloc_plane().
    const GX2RResourceFlags flags = (GX2RResourceFlags)(
        GX2R_RESOURCE_BIND_TEXTURE |
        GX2R_RESOURCE_USAGE_CPU_WRITE |
        GX2R_RESOURCE_USAGE_GPU_READ);

    for (int b = 0; b < 2; b++) {
        memset(&plane.tex[b], 0, sizeof(plane.tex[b]));
        GX2Surface& surf = plane.tex[b].surface;
        surf.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        surf.use = GX2_SURFACE_USE_TEXTURE;
        surf.width = (uint32_t)width;
        surf.height = (uint32_t)height;
        surf.depth = 1;
        surf.mipLevels = 1;
        surf.format = format;
        surf.aa = GX2_AA_MODE1X;
        surf.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;

        plane.tex[b].viewFirstMip = 0;
        plane.tex[b].viewNumMips = 1;
        plane.tex[b].viewFirstSlice = 0;
        plane.tex[b].viewNumSlices = 1;
        plane.tex[b].compMap = compMap;

        if (!GX2RCreateSurface(&surf, flags)) {
            snprintf(last_error_, sizeof(last_error_), "GX2RCreateSurface failed (%s plane, buffer %d)",
                     name, b);
            OSReport("Ufin: GX2RCreateSurface failed (%s, buffer %d, %dx%d)\n", name, b, width, height);
            if (b == 1) GX2RDestroySurfaceEx(&plane.tex[0].surface, GX2R_RESOURCE_BIND_NONE);
            return false;
        }

        GX2InitTextureRegs(&plane.tex[b]);
    }

    GX2InitSampler(&plane.sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    plane.valid = true;

    // Start out black (Y=16, Cb=Cr=128 in limited range) rather than
    // whatever was in memory, in case a draw happens before the first
    // real frame lands.
    for (int b = 0; b < 2; b++) {
        if (plane.bytesPerTexel == 2) fillPlane(plane, b, 128, 128);
        else fillPlane(plane, b, 16, 16);
    }

    OSReport("Ufin: %s plane created %dx%d pitch=%u imageSize=%u (x2 buffers)\n", name, width,
             height, plane.tex[0].surface.pitch, plane.tex[0].surface.imageSize);
    return true;
}

void VideoOutput::destroyPlane(Plane& plane) {
    if (!plane.valid) return;
    for (int b = 0; b < 2; b++) {
        GX2RDestroySurfaceEx(&plane.tex[b].surface, GX2R_RESOURCE_BIND_NONE);
    }
    plane.valid = false;
}

void VideoOutput::fillPlane(Plane& plane, int index, uint8_t byte0, uint8_t byte1) {
    GX2Surface& surf = plane.tex[index].surface;
    uint8_t* dst = (uint8_t*)GX2RLockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
    if (!dst) return;
    if (plane.bytesPerTexel == 1 || byte0 == byte1) {
        memset(dst, byte0, surf.imageSize);
    } else {
        for (uint32_t i = 0; i + 1 < surf.imageSize; i += 2) {
            dst[i] = byte0;
            dst[i + 1] = byte1;
        }
    }
    // memset only goes through the CPU cache; make sure the GPU sees it.
    DCFlushRange(dst, surf.imageSize);
    GX2RUnlockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
}

void VideoOutput::uploadPlane(Plane& plane, int index, const uint8_t* src, int srcLinesize,
                              int bytesPerRow, int rows) {
    GX2Surface& surf = plane.tex[index].surface;
    uint8_t* dst = (uint8_t*)GX2RLockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
    if (!dst) {
        OSReport("Ufin: uploadPlane -- GX2RLockSurfaceEx returned null\n");
        return;
    }

    // GX2Surface::pitch is in texels; the decoder's linesize is in bytes.
    const uint32_t dstPitch = surf.pitch * plane.bytesPerTexel;
    if ((uint32_t)bytesPerRow > dstPitch) bytesPerRow = (int)dstPitch;
    if ((uint32_t)rows > surf.height) rows = (int)surf.height;

    // OSBlockMove with flush=TRUE rather than memcpy: memcpy only writes
    // through the CPU cache, and the GPU reads memory directly, so it
    // can see stale data unless the written range is flushed.
    if (dstPitch == (uint32_t)bytesPerRow && srcLinesize == bytesPerRow) {
        OSBlockMove(dst, src, (uint32_t)bytesPerRow * rows, TRUE);
    } else {
        for (int y = 0; y < rows; y++) {
            OSBlockMove(dst + (size_t)y * dstPitch, src + (size_t)y * srcLinesize,
                        (uint32_t)bytesPerRow, TRUE);
        }
    }

    GX2RUnlockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
}

void* VideoOutput::buildQuad(uint32_t targetWidth, uint32_t targetHeight, double displayAspect) {
    void* buf = MEMAllocFromDefaultHeapEx(QUAD_BYTES, GX2_VERTEX_BUFFER_ALIGNMENT);
    if (!buf) return nullptr;

    // Aspect-fit: shrink one axis of the fullscreen quad so the picture
    // keeps displayAspect inside the target, leaving black bars on the
    // other axis.
    double targetAspect = (double)targetWidth / (double)targetHeight;
    float sx = 1.0f, sy = 1.0f;
    if (displayAspect > targetAspect) {
        sy = (float)(targetAspect / displayAspect);
    } else if (displayAspect > 0.0) {
        sx = (float)(displayAspect / targetAspect);
    }

    // Triangle strip: top-left, bottom-left, top-right, bottom-right.
    // Clip space is y-up; texture row 0 is the top of the picture, so
    // v=0 goes with +y.
    const float verts[QUAD_VERTEX_COUNT * 4] = {
        -sx,  sy, 0.0f, 0.0f,
        -sx, -sy, 0.0f, 1.0f,
         sx,  sy, 1.0f, 0.0f,
         sx, -sy, 1.0f, 1.0f,
    };
    memcpy(buf, verts, sizeof(verts));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, buf, QUAD_BYTES);
    return buf;
}

bool VideoOutput::init(int width, int height, double displayAspect) {
    width_ = width;
    height_ = height;
    if (displayAspect <= 0.0 && height > 0) displayAspect = (double)width / (double)height;
    OSReport("Ufin: VideoOutput::init (GX2 NV12) %dx%d display aspect %.3f\n", width, height,
             displayAspect);

    if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0) {
        snprintf(last_error_, sizeof(last_error_), "unsupported video size %dx%d", width, height);
        return false;
    }

    if (!s_gx2ContextReady) {
        // Programmer error, not a runtime condition -- initGX2Context()
        // must have already been called once near the top of main().
        snprintf(last_error_, sizeof(last_error_), "GX2 context not initialised");
        OSReport("Ufin: VideoOutput::init called before initGX2Context()\n");
        return false;
    }

    // The context itself is already up (see initGX2Context()); just turn
    // its output back on for this session.
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);

    // What WHBGfx actually created for each screen -- depends on the
    // TV's scan mode (480p/720p/1080p) and aspect setting.
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    tv_width_ = tv ? tv->surface.width : 1280;
    tv_height_ = tv ? tv->surface.height : 720;
    drc_width_ = drc ? drc->surface.width : 854;
    drc_height_ = drc ? drc->surface.height : 480;
    OSReport("Ufin: render targets -- TV %ux%u, DRC %ux%u\n", tv_width_, tv_height_, drc_width_,
             drc_height_);

    if (!loadShader()) return false;
    gfx_initialized_ = true; // shader_ now needs WHBGfxFreeShaderGroup() in shutdown()

    // Single-channel textures: put the one channel in R and hard-wire
    // G/B to 0 and A to 1 (the shader reads .r / .rg only).
    const uint32_t compMapR8 = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    const uint32_t compMapRG8 = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1);

    if (!createPlane(y_plane_, GX2_SURFACE_FORMAT_UNORM_R8, compMapR8, width_, height_, "Y")) return false;
    if (!createPlane(uv_plane_, GX2_SURFACE_FORMAT_UNORM_R8_G8, compMapRG8, width_ / 2, height_ / 2, "UV")) return false;

    tv_quad_ = buildQuad(tv_width_, tv_height_, displayAspect);
    drc_quad_ = buildQuad(drc_width_, drc_height_, displayAspect);
    if (!tv_quad_ || !drc_quad_) {
        snprintf(last_error_, sizeof(last_error_), "quad vertex buffer allocation failed");
        OSReport("Ufin: quad buffer alloc failed\n");
        return false;
    }

    write_index_ = 0;
    unsupported_format_logged_ = 0;

    OSReport("Ufin: VideoOutput::init complete\n");
    return true;
}

void VideoOutput::drawQuad(int readIndex, const void* quad, uint32_t targetWidth, uint32_t targetHeight) {
    // GX2's fixed-function state (blend, culling, depth test, alpha test,
    // viewport, scissor) isn't reset to sane defaults automatically -- set
    // all of it explicitly every draw (as CafeMP does). On real hardware
    // this state is whatever the environment/loader left behind before
    // handing off to us; Cemu's GX2 model doesn't reproduce that leftover
    // state, which is why a missing reset here can look fine on Cemu and
    // render nothing (alpha-tested away) on a real console. Alpha test in
    // particular was missing -- add it alongside the rest.
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                        GX2_BLEND_COMBINE_MODE_ADD, FALSE,
                        GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetViewport(0, 0, (float)targetWidth, (float)targetHeight, 0.0f, 1.0f);
    GX2SetScissor(0, 0, targetWidth, targetHeight);

    GX2SetFetchShader(&shader_.fetchShader);
    GX2SetVertexShader(shader_.vertexShader);
    GX2SetPixelShader(shader_.pixelShader);

    GX2SetPixelTexture(&y_plane_.tex[readIndex], y_sampler_location_);
    GX2SetPixelSampler(&y_plane_.sampler, y_sampler_location_);
    GX2SetPixelTexture(&uv_plane_.tex[readIndex], uv_sampler_location_);
    GX2SetPixelSampler(&uv_plane_.sampler, uv_sampler_location_);

    GX2SetAttribBuffer(0, QUAD_BYTES, QUAD_VERTEX_STRIDE, const_cast<void*>(quad));
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, QUAD_VERTEX_COUNT, 0, 1);
}

void VideoOutput::present(int readIndex) {
    // Standard WHBGfx frame: wait for the previous swap, draw each
    // target into its colour buffer, copy to the scan buffers, swap.
    // WHBGfxClearColor also (re)binds the target's context state; the
    // clear is what paints the letterbox bars black.
    WHBGfxBeginRender();

    WHBGfxBeginRenderTV();
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    drawQuad(readIndex, tv_quad_, tv_width_, tv_height_);
    WHBGfxFinishRenderTV();

    WHBGfxBeginRenderDRC();
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    drawQuad(readIndex, drc_quad_, drc_width_, drc_height_);
    WHBGfxFinishRenderDRC();

    // Includes GX2DrawDone(), so by the time this returns the GPU is
    // finished reading this frame's textures -- which is what makes it
    // safe to overwrite the *other* buffer next frame without any
    // further synchronisation.
    WHBGfxFinishRender();
}

void VideoOutput::renderTestPattern() {
    // Flat magenta in YCbCr (Y=105, Cb=212, Cr=234) through the real
    // NV12 shader, so the diagnostic exercises the same conversion.
    fillPlane(y_plane_, write_index_, 105, 105);
    fillPlane(uv_plane_, write_index_, 212, 234);
    present(write_index_);
    write_index_ ^= 1;
}

void VideoOutput::renderFrame(AVFrame* frame) {
    if (!frame || !y_plane_.valid || !uv_plane_.valid) return;

    if (frame->format != AV_PIX_FMT_NV12) {
        if (unsupported_format_logged_++ == 0) {
            OSReport("Ufin: renderFrame -- unexpected pixel format %d (expected NV12 %d), frames skipped\n",
                     frame->format, (int)AV_PIX_FMT_NV12);
        }
        return;
    }

    static int callCount = 0;
    callCount++;
    if (callCount <= 3 || callCount % 300 == 0) {
        OSReport("Ufin: renderFrame #%d (%dx%d linesize %d/%d)\n", callCount, frame->width,
                 frame->height, frame->linesize[0], frame->linesize[1]);
    }

    int w = std::min(frame->width, width_);
    int h = std::min(frame->height, height_);

    // NV12: plane 0 is Y (w x h bytes), plane 1 is interleaved CbCr
    // (w bytes per row -- w/2 texels of 2 bytes -- for h/2 rows).
    uploadPlane(y_plane_, write_index_, frame->data[0], frame->linesize[0], w, h);
    uploadPlane(uv_plane_, write_index_, frame->data[1], frame->linesize[1], w, h / 2);

    present(write_index_);
    write_index_ ^= 1;
}

void VideoOutput::shutdown() {
    destroyPlane(y_plane_);
    destroyPlane(uv_plane_);
    if (tv_quad_) { MEMFreeToDefaultHeap(tv_quad_); tv_quad_ = nullptr; }
    if (drc_quad_) { MEMFreeToDefaultHeap(drc_quad_); drc_quad_ = nullptr; }
    if (gfx_initialized_) {
        WHBGfxFreeShaderGroup(&shader_);
        gfx_initialized_ = false;
    }
    // Hide output again (context itself stays alive for the whole app --
    // see initGX2Context()/shutdownGX2Context()) so OSScreen can safely
    // come back via OSScreenDisplay::show().
    if (s_gx2ContextReady) {
        GX2SetTVEnable(FALSE);
        GX2SetDRCEnable(FALSE);
    }
}
