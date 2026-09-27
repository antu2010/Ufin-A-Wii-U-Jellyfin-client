#include "gfx.h"
#include "app_ui.h"
#include "layout.h"

#include <coreinit/debug.h>
#include <coreinit/memory.h>
#include <coreinit/time.h>
#include <gx2/event.h>
#include <gx2/swap.h>
#include <whb/gfx.h>

#include <imgui.h>
#include <backends/imgui_impl_gx2.h>

namespace ui {

Gfx& gfx() {
    static Gfx instance;
    return instance;
}

bool Gfx::init() {
    if (ready_) return true;
    if (!WHBGfxInit()) {
        OSReport("Ufin: WHBGfxInit failed\n");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // The console's own UI font, straight from system memory -- nothing
    // to ship, and it has every accented letter. ImGui 1.92 rasterises
    // glyphs on demand at whatever size each draw call asks for.
    void* fontData = nullptr;
    uint32_t fontSize = 0;
    ImFont* font = nullptr;
    if (OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &fontData, &fontSize) && fontData && fontSize) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false; // system memory, not ours to free
        font = io.Fonts->AddFontFromMemoryTTF(fontData, (int)fontSize, 24.0f, &cfg);
    }
    if (!font) {
        OSReport("Ufin: system font unavailable, using ImGui's built-in font\n");
        io.Fonts->AddFontDefault();
    }

    applyTheme();

    if (!ImGui_ImplGX2_Init()) {
        OSReport("Ufin: ImGui_ImplGX2_Init failed\n");
        ImGui::DestroyContext();
        WHBGfxShutdown();
        return false;
    }
    lastTime_ = (uint64_t)OSGetSystemTime();
    ready_ = true;
    OSReport("Ufin: GX2 + ImGui %s ready\n", ImGui::GetVersion());
    return true;
}

void Gfx::shutdown() {
    if (!ready_) return;
    ImGui_ImplGX2_Shutdown();
    ImGui::DestroyContext();
    WHBGfxShutdown();
    ready_ = false;
}

void Gfx::frame(const std::function<void()>& build,
                const std::function<void(uint32_t, uint32_t)>& underlay) {
    if (!ready_) return;

    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    const uint32_t tvW = tv ? tv->surface.width : 1280;
    const uint32_t tvH = tv ? tv->surface.height : 720;

    ImGuiIO& io = ImGui::GetIO();
    // Lay out in 1280x720 whatever the TV mode; the renderer scales.
    io.DisplaySize = ImVec2(layout::SCREEN_W, layout::SCREEN_H);
    io.DisplayFramebufferScale = ImVec2(tvW / layout::SCREEN_W, tvH / layout::SCREEN_H);
    uint64_t now = (uint64_t)OSGetSystemTime();
    float dt = (float)(now - lastTime_) / (float)OSTimerClockSpeed;
    io.DeltaTime = (dt > 0.0f && dt < 1.0f) ? dt : 1.0f / 60.0f;
    lastTime_ = now;

    ImGui_ImplGX2_NewFrame();
    ImGui::NewFrame();
    if (build) build();
    if (crt_) drawCrtOverlay(ImGui::GetTime());
    ImGui::Render();

    WHBGfxBeginRender();
    WHBGfxBeginRenderTV();
    if (underlay) {
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        underlay(tvW, tvH);
    } else {
        WHBGfxClearColor(0.078f, 0.086f, 0.102f, 1.0f); // theme background
    }
    ImGui_ImplGX2_RenderDrawData(ImGui::GetDrawData());
    WHBGfxFinishRenderTV();
    // The GamePad shows the same picture, scaled by the copy.
    GX2CopyColorBufferToScanBuffer(tv, GX2_SCAN_TARGET_DRC);
    WHBGfxFinishRender();
}

void Gfx::frameTvDrc(const std::function<void()>& tvBuild,
                     const std::function<void()>& drcBuild,
                     const std::function<void(uint32_t, uint32_t)>& tvUnderlay) {
    if (!ready_) return;

    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    const uint32_t tvW = tv ? tv->surface.width : 1280;
    const uint32_t tvH = tv ? tv->surface.height : 720;
    const uint32_t drcW = drc ? drc->surface.width : 854;
    const uint32_t drcH = drc ? drc->surface.height : 480;

    ImGuiIO& io = ImGui::GetIO();
    uint64_t now = (uint64_t)OSGetSystemTime();
    float dt = (float)(now - lastTime_) / (float)OSTimerClockSpeed;
    dt = (dt > 0.0f && dt < 1.0f) ? dt : 1.0f / 60.0f;
    lastTime_ = now;

    WHBGfxBeginRender();

    // First render: TV gets the visualizer.
    io.DisplaySize = ImVec2(layout::SCREEN_W, layout::SCREEN_H);
    io.DisplayFramebufferScale = ImVec2(tvW / layout::SCREEN_W, tvH / layout::SCREEN_H);
    io.DeltaTime = dt;
    ImGui_ImplGX2_NewFrame();
    ImGui::NewFrame();
    if (tvBuild) tvBuild();
    if (crt_) drawCrtOverlay(ImGui::GetTime());
    ImGui::Render();

    WHBGfxBeginRenderTV();
    if (tvUnderlay) {
        // Movie TV-only mode: the video picture, with (normally) an empty
        // tvBuild -- so the TV shows a clean frame with no ImGui draw
        // calls layered on top, exactly like frame()'s underlay path.
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        tvUnderlay(tvW, tvH);
    } else {
        WHBGfxClearColor(0.078f, 0.086f, 0.102f, 1.0f);
    }
    ImGui_ImplGX2_RenderDrawData(ImGui::GetDrawData());
    WHBGfxFinishRenderTV();

    // Real-hardware-only bug fix: the ImGui GX2 backend keeps ONE scratch
    // vertex/index buffer per ImGui context (see ImGui_ImplGX2_Data in
    // imgui_impl_gx2.cpp), sized and refilled by memcpy on every
    // RenderDrawData() call, with no double-buffering. That's safe with a
    // single ImGui pass per real GPU frame (ui::Gfx::frame()), because
    // WHBGfxFinishRender()'s GX2DrawDone() guarantees the GPU is done
    // reading it before the next frame's CPU writes land. Here there are
    // TWO RenderDrawData() calls before that same GX2DrawDone() -- so
    // without a sync in between, the CPU can start memcpy-ing the
    // GamePad's vertex/index data over the buffer while the GPU's command
    // queue is still (asynchronously) reading the TV pass's geometry out
    // of it. On real hardware this raced and won often enough to corrupt
    // the GamePad's draw calls with stale/foreign geometry -- e.g. an
    // artwork quad drawn with a font-atlas text draw's leftover vertices,
    // which is what made cover art look like garbled text. Cemu's GX2
    // model doesn't reproduce that timing, so it never showed there
    // (same class of Cemu-vs-real-hardware gap as the alpha-test/GX2
    // context notes in the README). A GX2DrawDone() here is a real stall,
    // but it only runs for these two-pass frames (visualizer, movie
    // TV-only mode), never for the normal single-pass HUD/menu path.
    GX2DrawDone();

    // Second render: GamePad gets the regular player interface. We build a
    // fresh ImGui frame so the TV and GamePad can contain different UIs.
    io.DisplaySize = ImVec2(layout::SCREEN_W, layout::SCREEN_H);
    io.DisplayFramebufferScale = ImVec2(drcW / layout::SCREEN_W, drcH / layout::SCREEN_H);
    io.DeltaTime = dt;
    ImGui_ImplGX2_NewFrame();
    ImGui::NewFrame();
    if (drcBuild) drcBuild();
    if (crt_) drawCrtOverlay(ImGui::GetTime());
    ImGui::Render();

    WHBGfxBeginRenderDRC();
    WHBGfxClearColor(0.078f, 0.086f, 0.102f, 1.0f);
    ImGui_ImplGX2_RenderDrawData(ImGui::GetDrawData());
    WHBGfxFinishRenderDRC();

    // WHBGfxFinishRender swaps both scan buffers after both targets have been
    // rendered independently. No TV->DRC copy is performed here.
    WHBGfxFinishRender();
}


} // namespace ui
