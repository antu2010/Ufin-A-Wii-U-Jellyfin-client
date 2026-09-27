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
