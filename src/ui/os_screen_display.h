// OSScreenDisplay -- owns the OSScreen framebuffers for the TV and the
// GamePad and exposes each as a ui::Surface, so the menu screens can be
// drawn identically to both. This is the only file in ui/ that touches
// wut.
//
// OSScreen is used for menus because it needs no shader pipeline and
// coexists with plain text output. It must never be *enabled* at the
// same time as GX2 video output -- both drive the same display hardware,
// and having both active at once hangs hard (OSFatal, on Cemu and real
// hardware alike). But a FULL OSScreenShutdown()/OSScreenInit() cycle
// around every GX2 session (the original approach here) turned out to
// itself be broken on real hardware: it left GX2 computing correct
// frames that never actually reached the physical screen for the rest
// of that GX2 session, only becoming visible (briefly, and stale) once
// GX2 tore back down -- reproducible with a completely bare GX2SetTVEnable/
// GX2SetDRCEnable+WHBGfxClearColor loop with zero OSScreen calls anywhere
// in that process showed a picture immediately, on the same console.
// Whatever OSScreenInit()/OSScreenShutdown() do to claim/release the
// display, real hardware doesn't hand it back to GX2 the way Cemu's GX2
// model (or a process that never touches OSScreen at all) does.
//
// The fix: only ever call OSScreenInit()/OSScreenShutdown() once each,
// at app start/exit (see init()/shutdown() below). For every GX2
// hand-off in between, use hide()/show() instead -- they only toggle
// OSScreenEnableEx, never touching OSScreenInit/Shutdown or the
// framebuffer allocations, so there's nothing left for real hardware to
// mishandle. GX2's own context is kept alive the same way, for the same
// reason -- see VideoOutput::initGX2Context()/shutdownGX2Context().

#pragma once
#include "surface.h"

#include <coreinit/screen.h>
#include <cstdint>

class OSScreenSurface : public ui::Surface {
public:
    OSScreenSurface(OSScreenID id, int width, int height)
        : id_(id), width_(width), height_(height) {}

    int width() const override { return width_; }
    int height() const override { return height_; }
    void clear(uint32_t rgba) override;
    void fillRect(int x, int y, int w, int h, uint32_t rgba) override;
    void drawText(int column, int row, const std::string& text) override;

private:
    OSScreenID id_;
    int width_;
    int height_;
};

class OSScreenDisplay {
public:
    // Initialises OSScreen and allocates both framebuffers. Safe to call
    // again after shutdown().
    bool init();
    void shutdown();
    bool isActive() const { return active_; }

    // Lighter-weight than shutdown()/init(): just toggles the physical
    // output on/off without touching OSScreenInit/Shutdown or the
    // framebuffer allocations. Use these (not shutdown()/init()) for
    // handing the display to GX2 and back -- see the big comment at the
    // top of this file about why a full teardown/recreate cycle here was
    // the actual bug behind Ufin's black-screen-on-real-hardware issue.
    // OSScreen and GX2 output must still never both be *enabled* at the
    // same time (that combination hangs, on Cemu and real hardware
    // alike) -- hide() before enabling GX2 output, show() only after
    // disabling it.
    void hide();
    void show();

    ui::Surface& tv() { return tv_; }
    ui::Surface& drc() { return drc_; }

    // Runs `draw` once per screen, then flushes and flips both. `draw`
    // gets a ui::Surface&.
    template <typename DrawFn>
    void render(DrawFn draw) {
        if (!active_) return;
        draw(tv_);
        draw(drc_);
        flip();
    }

private:
    bool active_ = false;
    void* tvBuffer_ = nullptr;
    void* drcBuffer_ = nullptr;
    uint32_t tvSize_ = 0;
    uint32_t drcSize_ = 0;
    OSScreenSurface tv_{SCREEN_TV, 1280, 720};
    OSScreenSurface drc_{SCREEN_DRC, 854, 480};
    bool gridMeasured_ = false;
    ui::GridMetrics tvGrid_;
    ui::GridMetrics drcGrid_;

    void flip();
};
