#include "gfx.h"
#include "app_ui.h"
#include "layout.h"

#include <coreinit/debug.h>
#include <coreinit/memory.h>
#include <coreinit/time.h>
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

} // namespace ui
