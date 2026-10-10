#include "gfx.h"
#include "app_ui.h"
#include "layout.h"
#include "glass_core.h"
#include "gpu_image.h"

#include <coreinit/debug.h>
#include "../ufin_log.h" // OSReport -> also the SD-card log file (see ufin_log.h)
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

// --- Liquid Glass refraction: the frame snapshot ---------------------------
//
// When glass refracts, the UI inserts a draw-list callback just before the first
// glass of the frame (and before glass that sits on top of other glass). Render
// order is what makes this work: the callback runs inside RenderDrawData, after
// everything drawn so far -- the video underlay included -- is in the colour
// buffer, and copies it into a texture the glass is then drawn from. One target
// per display, since the TV and GamePad buffers differ in size.
namespace {

struct SnapshotTarget {
    GX2ColorBuffer* buffer = nullptr; // what is being rendered to
    uint64_t texture = 0;             // where the copy goes (0 = no refraction for this display)
};
SnapshotTarget g_tvSnap, g_drcSnap;
int g_snapStage = 6; // see UfinConfig::glassRefractionStage
int g_snapLogged = 0;
int g_snapRefusedLogged = 0;

void snapshotCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const SnapshotTarget* t = (const SnapshotTarget*)cmd->UserCallbackData;
    if (!t || !t->buffer || !t->texture) return;
    // Only the first few snapshots leave a trace (this runs 2-4 times every frame, and each
    // log line is a flushed write to the SD card).
    const bool trace = g_snapLogged < 4;
    if (trace) {
        g_snapLogged++;
        OSReport("Ufin: glass snapshot #%d (stage %d)\n", g_snapLogged, g_snapStage);
    }
    if (g_snapStage >= 3) {
        const bool copied = copyColorBufferToSnapshot(t->texture, t->buffer, g_snapStage);
        if (trace) OSReport("Ufin: glass snapshot copy %s\n", copied ? "issued" : "REFUSED (size/format mismatch)");
        else if (!copied && g_snapRefusedLogged < 4) {
            g_snapRefusedLogged++;
            OSReport("Ufin: glass snapshot copy REFUSED (size/format mismatch)\n");
        }
        if (g_snapStage >= 6) {
            // Re-enter the display's render pass the way libwhb does at the start of a frame.
            // This is what makes the copy safe on real hardware: it leaves the GPU's context
            // state, render target and the state the UI's shaders rely on in a condition the
            // draws that follow cannot cope with (stages 3-5, which only restore pieces of it,
            // leave the UI blank on a console; Cemu emulates the copy and does not care).
            if (t == &g_tvSnap) WHBGfxBeginRenderTV();
            else WHBGfxBeginRenderDRC();
        }
    }
    // stage 2: the callback is in the draw list and runs, but copies nothing
}

void releaseSnapshots() {
    setGlassSnapshot(0, nullptr, nullptr); // nothing may draw from them any more
    if (g_tvSnap.texture) releaseTexture(g_tvSnap.texture);
    if (g_drcSnap.texture) releaseTexture(g_drcSnap.texture);
    g_tvSnap = SnapshotTarget();
    g_drcSnap = SnapshotTarget();
}

// Point the UI at a display's snapshot before building that display's frame (the
// texture is baked into the frame's draw lists).
void useSnapshot(SnapshotTarget& t, GX2ColorBuffer* buffer) {
    t.buffer = buffer;
    if (g_snapStage >= 2 && t.texture && t.buffer) setGlassSnapshot(t.texture, snapshotCallback, &t);
    else setGlassSnapshot(0, nullptr, nullptr);
}

} // namespace

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

bool Gfx::setGlass(bool on, bool debugLog, bool refraction, int refractionStage) {
    glass::BackdropManager& bd = glass::backdrop();
    releaseSnapshots(); // (re)built below if wanted; always gone when glass is off
    if (!on) {
        bd.disable();
        return true;
    }
    if (!ready_) return false;
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    g_snapStage = refractionStage < 1 ? 1 : (refractionStage > 7 ? 7 : refractionStage);
    g_snapLogged = 0;
    g_snapRefusedLogged = 0;
    if (refraction) {
        const bool asRenderTarget = g_snapStage < 7;
        g_tvSnap.texture = tv ? createSnapshotTexture(tv, asRenderTarget) : 0;
        g_drcSnap.texture = drc ? createSnapshotTexture(drc, asRenderTarget) : 0;
        OSReport("Ufin: glass refraction %s, stage %d (TV %s, GamePad %s)\n", (g_tvSnap.texture || g_drcSnap.texture) ? "on" : "unavailable",
                 g_snapStage, g_tvSnap.texture ? "ok" : "off", g_drcSnap.texture ? "ok" : "off");
    }
    // One texture for both displays, sized from the real render targets.
    const glass::Dims dims = glass::sharedDims(tv ? (int)tv->surface.width : 1280, tv ? (int)tv->surface.height : 720,
                                               drc ? (int)drc->surface.width : 854, drc ? (int)drc->surface.height : 480);
    glass::TextureSink sink;
    sink.create = [](const uint8_t* rgba, int w, int h) { return uploadTexture(rgba, w, h); };
    sink.update = [](uint64_t handle, const uint8_t* rgba, int w, int h) { return updateTexture(handle, rgba, w, h); };
    sink.release = [](uint64_t handle) { releaseTexture(handle); };
    bd.setLog([](const char* msg) { OSReport("%s\n", msg); });
    bd.setDebug(debugLog);
    return bd.enable(dims, sink, 3); // 3 down/up passes at 1/4 res ~ the blur radius 2 had at 1/8
}

void Gfx::shutdown() {
    if (!ready_) return;
    releaseSnapshots();
    glass::backdrop().disable(); // release the blur textures while GX2 is still up
    ImGui_ImplGX2_Shutdown();
    ImGui::DestroyContext();
    WHBGfxShutdown();
    ready_ = false;
}

void Gfx::frame(const std::function<void()>& build,
                const std::function<void(uint32_t, uint32_t)>& underlay) {
    if (!ready_) return;

    // Liquid Glass: refresh the shared blur (a small time-sliced step, and
    // nothing at all while a video frame is being presented). Before this
    // frame is built or rendered, so the GPU is done with the old texture.
    glassFrame((double)OSGetSystemTime() / (double)OSTimerClockSpeed, (bool)underlay);

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

    useSnapshot(g_tvSnap, tv); // glass refracts the TV buffer (the GamePad gets a scaled copy of it)
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

    glassFrame((double)OSGetSystemTime() / (double)OSTimerClockSpeed, (bool)tvUnderlay); // see frame()

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
    useSnapshot(g_tvSnap, tv);
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
    useSnapshot(g_drcSnap, drc); // the GamePad frame refracts its own (differently sized) buffer
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
