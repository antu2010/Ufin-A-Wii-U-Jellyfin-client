#include "gpu_image.h"

#include <coreinit/debug.h>
#include <gx2/mem.h>
#include <gx2/sampler.h>
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

void releaseTexture(uint64_t handle) {
    GpuImage* img = (GpuImage*)(uintptr_t)handle;
    if (!img) return;
    GX2RDestroySurfaceEx(&img->Texture.surface, GX2R_RESOURCE_BIND_NONE);
    delete img;
}

} // namespace ui
