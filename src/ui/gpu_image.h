// RGBA8 pixels -> a GX2 texture ImGui can draw (ImTextureID).
//
// The GX2 ImGui renderer treats an ImTextureID as a pointer to
// { GX2Texture, GX2Sampler } -- its documented way to draw app textures
// -- so that's what upload() allocates. Used as ImageCache's backend.
#pragma once
#include <cstdint>

namespace ui {

uint64_t uploadTexture(const uint8_t* rgba, int width, int height);

// Only between frames: ui::Gfx::frame() ends with GX2DrawDone, so the
// GPU is finished with every texture once it returns.
void releaseTexture(uint64_t handle);

} // namespace ui
