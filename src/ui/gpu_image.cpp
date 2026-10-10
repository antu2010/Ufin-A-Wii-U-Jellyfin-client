#include "gpu_image.h"

#include <coreinit/debug.h>
#include "../ufin_log.h" // OSReport -> also the SD-card log file (see ufin_log.h)
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/sampler.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/surface.h>

#include <cstring>
#include <new>

namespace ui {

// Same layout as the GX2 renderer's ImGui_ImplGX2_Texture (texture
// first, then sampler), which is how it reads any ImTextureID.
struct GpuImage {
    GX2Texture Texture;
    GX2Sampler Sampler;
};

uint64_t uploadTexture(const uint8_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return 0;
    GpuImage* img = new (std::nothrow) GpuImage();
    if (!img) return 0;
    memset(img, 0, sizeof(*img));

    GX2Surface& surf = img->Texture.surface;
    surf.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surf.use = GX2_SURFACE_USE_TEXTURE;
    surf.width = (uint32_t)width;
    surf.height = (uint32_t)height;
    surf.depth = 1;
    surf.mipLevels = 1;
    surf.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    surf.aa = GX2_AA_MODE1X;
    surf.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    img->Texture.viewNumSlices = 1;
    img->Texture.viewNumMips = 1;
    // stb_image writes bytes R,G,B,A in memory order, which is exactly
    // how R8_G8_B8_A8 reads them -- no swizzle. (ImGui's own font atlas
    // needs one because it stores native-endian 32-bit words.)
    img->Texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    if (!GX2RCreateSurface(&surf, (GX2RResourceFlags)(GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE |
                                                      GX2R_RESOURCE_USAGE_GPU_READ))) {
        OSReport("Ufin: GX2RCreateSurface failed for a %dx%d image\n", width, height);
        delete img;
        return 0;
    }
    GX2InitTextureRegs(&img->Texture);

    uint8_t* dst = (uint8_t*)GX2RLockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
    if (!dst) {
        GX2RDestroySurfaceEx(&surf, GX2R_RESOURCE_BIND_NONE);
        delete img;
        return 0;
    }
    const size_t rowBytes = (size_t)width * 4;
    for (int y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * surf.pitch * 4, rgba + (size_t)y * rowBytes, rowBytes);
    }
    GX2RUnlockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
    // Real hardware: make sure the GPU doesn't sample stale cache lines.
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surf.image, surf.imageSize);

    GX2InitSampler(&img->Sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    return (uint64_t)(uintptr_t)img;
}

uint64_t createSnapshotTexture(const GX2ColorBuffer* src, bool asRenderTarget) {
    if (!src || src->surface.width == 0 || src->surface.height == 0) return 0;
    GpuImage* img = new (std::nothrow) GpuImage();
    if (!img) return 0;
    memset(img, 0, sizeof(*img));

    // Same size, format and tiling as the colour buffer, so the GPU copy is a plain
    // like-for-like surface copy. Also flagged as a colour buffer, because a surface
    // copy is allowed to write its destination as a render target.
    GX2Surface& surf = img->Texture.surface;
    surf.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surf.use = (GX2SurfaceUse)(asRenderTarget ? (GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER) : GX2_SURFACE_USE_TEXTURE);
    surf.width = src->surface.width;
    surf.height = src->surface.height;
    surf.depth = 1;
    surf.mipLevels = 1;
    surf.format = src->surface.format;
    surf.aa = GX2_AA_MODE1X;
    surf.tileMode = src->surface.tileMode;
    img->Texture.viewNumSlices = 1;
    img->Texture.viewNumMips = 1;
    // GPU-written pixels read back in the order they were written: no swizzle.
    img->Texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    const GX2RResourceFlags bind = (GX2RResourceFlags)(asRenderTarget ? (GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_BIND_COLOR_BUFFER)
                                                                       : GX2R_RESOURCE_BIND_TEXTURE);
    if (!GX2RCreateSurface(&surf, (GX2RResourceFlags)(bind | GX2R_RESOURCE_USAGE_GPU_READ | GX2R_RESOURCE_USAGE_GPU_WRITE))) {
        OSReport("Ufin: GX2RCreateSurface failed for the %ux%u glass snapshot\n", surf.width, surf.height);
        delete img;
        return 0;
    }
    GX2InitTextureRegs(&img->Texture);
    GX2InitSampler(&img->Sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    // If a copy misbehaves on real hardware, the first thing to compare is how the two
    // surfaces are laid out; these lines end up in ufin_log.txt.
    const GX2Surface& sc = src->surface;
    OSReport("Ufin: glass snapshot src %ux%u pitch=%u fmt=0x%x tile=%d use=0x%x swizzle=0x%x align=%u size=%u\n", sc.width, sc.height,
             sc.pitch, (unsigned)sc.format, (int)sc.tileMode, (unsigned)sc.use, (unsigned)sc.swizzle, sc.alignment, sc.imageSize);
    OSReport("Ufin: glass snapshot dst %ux%u pitch=%u fmt=0x%x tile=%d use=0x%x swizzle=0x%x align=%u size=%u (%s)\n", surf.width, surf.height,
             surf.pitch, (unsigned)surf.format, (int)surf.tileMode, (unsigned)surf.use, (unsigned)surf.swizzle, surf.alignment, surf.imageSize,
             asRenderTarget ? "colour-buffer flag" : "texture only");
    return (uint64_t)(uintptr_t)img;
}

bool copyColorBufferToSnapshot(uint64_t handle, GX2ColorBuffer* src, int stage) {
    GpuImage* img = (GpuImage*)(uintptr_t)handle;
    if (!img || !src) return false;
    GX2Surface& dst = img->Texture.surface;
    if (dst.width != src->surface.width || dst.height != src->surface.height || dst.format != src->surface.format) return false;

    // Everything drawn so far must be out of the colour buffer's cache before it is read...
    if (stage >= 4) GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, src->surface.image, src->surface.imageSize);
    GX2CopySurface(&src->surface, 0, 0, &dst, 0, 0);
    if (stage >= 4) {
        // ...and the texture cache must not hold stale lines of the previous snapshot.
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, dst.image, dst.imageSize);
        // The copy is a GPU operation that may leave its own render target bound.
        GX2SetColorBuffer(src, GX2_RENDER_TARGET_0);
    }
    if (stage >= 5) {
        // On real hardware the copy runs as GPU work with its own state; Cemu emulates it
        // without touching any. Put back what the UI's shaders rely on and that libwhb set
        // up once at start-up (the rest comes from ImDrawCallback_ResetRenderState).
        GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
        GX2SetViewport(0.0f, 0.0f, (float)src->surface.width, (float)src->surface.height, 0.0f, 1.0f);
        GX2SetScissor(0, 0, src->surface.width, src->surface.height);
    }
    return true;
}

bool updateTexture(uint64_t handle, const uint8_t* rgba, int width, int height) {
    GpuImage* img = (GpuImage*)(uintptr_t)handle;
    if (!img || !rgba || width <= 0 || height <= 0) return false;
    GX2Surface& surf = img->Texture.surface;
    // Never write a differently-sized image into this surface.
    if ((uint32_t)width != surf.width || (uint32_t)height != surf.height) return false;

    // Exactly uploadTexture()'s write path: lock, row copy honouring the
    // surface pitch, unlock, invalidate the GPU's texture cache.
    uint8_t* dst = (uint8_t*)GX2RLockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
    if (!dst) return false;
    const size_t rowBytes = (size_t)width * 4;
    for (int y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * surf.pitch * 4, rgba + (size_t)y * rowBytes, rowBytes);
    }
    GX2RUnlockSurfaceEx(&surf, 0, GX2R_RESOURCE_BIND_NONE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surf.image, surf.imageSize);
    return true;
}

void releaseTexture(uint64_t handle) {
    GpuImage* img = (GpuImage*)(uintptr_t)handle;
    if (!img) return;
    GX2RDestroySurfaceEx(&img->Texture.surface, GX2R_RESOURCE_BIND_NONE);
    delete img;
}

} // namespace ui
