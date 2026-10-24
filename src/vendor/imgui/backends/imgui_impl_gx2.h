// dear imgui: Renderer Backend for the Nintendo Wii U using GX2
// Copyright (c) 2023 GaryOderNichts

#pragma once
#include "imgui.h"      // IMGUI_IMPL_API

// Backend API
IMGUI_IMPL_API bool     ImGui_ImplGX2_Init();
IMGUI_IMPL_API void     ImGui_ImplGX2_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplGX2_NewFrame();
IMGUI_IMPL_API void     ImGui_ImplGX2_RenderDrawData(ImDrawData* draw_data);

// Ufin: draws `tex` stretched over the whole of `dst` (a colour buffer that is also a texture) with the backend's shader,
// blending off. Used in the middle of RenderDrawData (from a draw-list callback) to downsample a snapshot for the Liquid
// Glass frost. It has its own small vertex/index ring, so it never touches the buffers the outer RenderDrawData is reading.
// Leaves `dst` bound as the render target; the caller binds the real target again.
struct GX2Texture;
struct GX2Sampler;
struct GX2ColorBuffer;
// `uShift`/`vShift` move the sampled picture (in UV units, so a texel is 1/width); `alpha` < 1 blends the draw over what is
// already in `dst` with that weight (a running average, for the blur's taps), 1 replaces it.
IMGUI_IMPL_API void     ImGui_ImplGX2_DrawTextureToTarget(const GX2Texture* tex, const GX2Sampler* sampler, GX2ColorBuffer* dst,
                                                          float uShift = 0.0f, float vShift = 0.0f, float alpha = 1.0f);

// (Optional) Called by Init/NewFrame/Shutdown
IMGUI_IMPL_API void     ImGui_ImplGX2_HandleTexture(ImTextureData* tex);
IMGUI_IMPL_API bool     ImGui_ImplGX2_CreateDeviceObjects();
IMGUI_IMPL_API void     ImGui_ImplGX2_DestroyDeviceObjects();
