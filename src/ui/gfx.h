// The one owner of the display: GX2 (via WHBGfx) + Dear ImGui, set up
// once at start-up and kept for the whole run. Menus, Now Playing, the
// keyboard and video all draw through it, so there is never a hand-off
// between display systems (the old OSScreen <-> GX2 switch around every
// video is gone).
//
// Each frame draws the TV picture once -- an optional underlay (a video
// frame) plus the ImGui UI in the virtual 1280x720 space of layout.h --
// and copies it to the GamePad, which shows the same picture scaled.
#pragma once
#include <cstdint>
#include <functional>

namespace ui {

class Gfx {
public:
    bool init();
    void shutdown();
    bool ready() const { return ready_; }

    // CRT easter egg: when on, every frame (video included) gets the
    // scanline / vignette overlay.
    void setCrt(bool on) { crt_ = on; }
    bool crt() const { return crt_; }

    // Liquid Glass appearance (see glass_core.h): allocates (on) or releases
    // (off) the shared low-resolution blur textures. Call between frames only.
    // Returns false if the blur could not be set up -- glass panels then use
    // the flat (tint + gloss) style, which needs no GPU resources -- and true
    // otherwise. Off releases everything and returns true.
    //
    // `refraction`: also allocate a snapshot texture per display so glass can
    // refract the real picture behind it (see glass_core.h, "Real refraction").
    // Costs one full-screen GPU copy per glass layer (usually two per frame) and
    // about 5 MB at 720p. If the snapshot cannot be set up, glass quietly keeps
    // using the soft blurred backdrop. `refractionStage` (1..5) is a bring-up aid
    // for real hardware (1..7), see UfinConfig::glassRefractionStage.
    // Glass in the two-pass visualizer frame (UfinConfig::visualizerGlass): 0 GamePad refracts / TV flat, 1 both flat, 2 both refract.
    void setVisualizerGlass(int mode);
    // Liquid Glass frost (UfinConfig::glassBlur: 0 sharp, 1..3 = light..strong). Call before setGlass().
    void setGlassBlur(int level);
    bool setGlass(bool on, bool debugLog = false, bool refraction = true, int refractionStage = 6);

    // Draws one frame. `build` adds this frame's UI (ImGui draw calls);
    // `underlay`, if set, draws first into the TV target of the given
    // size (used by VideoOutput for the video picture).
    void frame(const std::function<void()>& build,
               const std::function<void(uint32_t width, uint32_t height)>& underlay = {});

    // Draws two independent UI frames: one to the TV and one to the GamePad.
    // This is used by the music visualizer so the TV can show the visualizer
    // while the GamePad keeps the normal Now Playing controls, and by the
    // movie "TV-only" mode, where `tvUnderlay` draws the video picture (as
    // in frame() above) and `tvBuild` is left unset so the TV shows a clean
    // picture with no ImGui HUD on top, while `drcBuild` draws the GamePad
    // movie-remote UI. Unlike frame(), the GamePad is never a scaled copy
    // of the TV here -- it always renders its own independent ImGui frame.
    void frameTvDrc(const std::function<void()>& tvBuild,
                   const std::function<void()>& drcBuild,
                   const std::function<void(uint32_t width, uint32_t height)>& tvUnderlay = {});

private:
    bool ready_ = false;
    bool crt_ = false;
    uint64_t lastTime_ = 0;
};

Gfx& gfx();

} // namespace ui
