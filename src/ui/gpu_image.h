// RGBA8 pixels -> a GX2 texture ImGui can draw (ImTextureID).
//
// The GX2 ImGui renderer treats an ImTextureID as a pointer to
// { GX2Texture, GX2Sampler } -- its documented way to draw app textures
// -- so that's what upload() allocates. Used as ImageCache's backend.
#pragma once
#include <cstdint>

struct GX2ColorBuffer; // (wut)

namespace ui {

uint64_t uploadTexture(const uint8_t* rgba, int width, int height);

// Overwrites the pixels of a texture from uploadTexture() with same-sized new
// ones (the Liquid Glass backdrop). Only between frames, like releaseTexture,
// and never with a size different from the one it was created with.
// Returns false (leaving the texture as it was) on any problem.
bool updateTexture(uint64_t handle, const uint8_t* rgba, int width, int height);

// Liquid Glass refraction: a texture the same size and format as `src` that the
// frame can be copied into mid-render (see copyColorBufferToSnapshot). Released
// with releaseTexture(). Returns 0 if it cannot be created. `asRenderTarget` also
// flags the surface as a colour buffer (a copy implemented as a draw may need it);
// false makes it a plain texture. Logs both surfaces' layout (size, pitch, tiling,
// swizzle...) so a mismatch between source and copy can be seen in ufin_log.txt.
uint64_t createSnapshotTexture(const GX2ColorBuffer* src, bool asRenderTarget = true);

// Copies everything drawn so far into `src` (the colour buffer currently being
// rendered to) into the snapshot texture. Called from inside the draw-list
// callback, i.e. in the middle of RenderDrawData. Returns false (doing nothing)
// if the sizes do not match.
//
// `stage` is the bring-up level from config.json (glass_refraction_stage), so a
// failure on real hardware can be pinned to one step:
//   3  the bare GX2CopySurface
//   4  + invalidate the colour-buffer and texture caches around it, and bind the
//        colour buffer as the render target again afterwards
//   5  + re-assert the GPU state a copy may leave behind: shader mode, viewport
//        and scissor (the caller still follows with ImDrawCallback_ResetRenderState)
bool copyColorBufferToSnapshot(uint64_t handle, GX2ColorBuffer* src, int stage = 5);

// Liquid Glass frost: a colour buffer + texture of `like`'s size shifted right by `shift` (1 = half, 2 = quarter...),
// released with releaseTexture(). 0 if it cannot be created.
uint64_t createBlurTarget(const GX2ColorBuffer* like, int shift);
// Blurs the snapshot: shrinks it through `chain` (`downsteps` targets, each half the size of the one before), then runs
// `passes` (even, 0..4) smoothing passes at that size, ping-ponging with `scratch` (a target of the last one's size). The
// blurred picture ends up in chain[downsteps - 1]. Then binds `display` as the render target again. Called from the
// snapshot callback, right after copyColorBufferToSnapshot().
void blurSnapshot(uint64_t snapshot, const uint64_t* chain, int downsteps, uint64_t scratch, int passes, GX2ColorBuffer* display);

// Only between frames: ui::Gfx::frame() ends with GX2DrawDone, so the
// GPU is finished with every texture once it returns.
void releaseTexture(uint64_t handle);

} // namespace ui
