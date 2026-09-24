#include "os_screen_display.h"
#include "grid_probe.h"
#include "../ufin_log.h"

#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>

#include <cstring>

void OSScreenSurface::clear(uint32_t rgba) {
    OSScreenClearBufferEx(id_, rgba);
}

void OSScreenSurface::fillRect(int x, int y, int w, int h, uint32_t rgba) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > width_ ? width_ : x + w;
    int y1 = y + h > height_ ? height_ : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            OSScreenPutPixelEx(id_, (uint32_t)px, (uint32_t)py, rgba);
        }
    }
}

void OSScreenSurface::drawText(int column, int row, const std::string& text) {
    if (column < 0 || row < 0 || text.empty()) return;
    OSScreenPutFontEx(id_, (uint32_t)column, (uint32_t)row, text.c_str());
}

// OSScreen's TV framebuffer is 1280x720 on a 720p (or 480p) TV. The
// buffer size is the one thing the API tells us about the mode, so use
// it to spot a 1080p framebuffer rather than assuming.
static void tvDimensionsFromBufferSize(uint32_t bytes, int& width, int& height) {
    // Sizes are for two buffers of 4 bytes per pixel.
    if (bytes >= 1920u * 1080u * 4u * 2u) {
        width = 1920;
        height = 1080;
    } else {
        width = 1280;
        height = 720;
    }
}

// --- grid measurement ---
//
// OSScreen draws into memory we own, so instead of guessing where its
// text grid is, draw a test string and look at which pixels changed.
// Real hardware and Cemu disagree on both the glyph size and where
// column 0 starts; a layout built for one draws the selection band a row
// off and wraps long lines back over themselves on the other.

namespace {

void applyMeasuredGrid(OSScreenSurface& surface, OSScreenID id, void* buffer, uint32_t size,
                       const char* name) {
    ui::GridProbeOps ops;
    ops.clear = [id] { OSScreenClearBufferEx(id, 0x00000000); };
    ops.putPixel = [id](int x, int y) { OSScreenPutPixelEx(id, (uint32_t)x, (uint32_t)y, 0xFFFFFFFF); };
    ops.putText = [id](int col, int row, const char* text) {
        OSScreenPutFontEx(id, (uint32_t)col, (uint32_t)row, text);
    };
    ui::GridMetrics g;
    if (ui::measureGrid(buffer, size, surface.width(), ops, g)) {
        OSReport("Ufin: %s text grid measured: origin %d,%d cell %dx%d -> %d cols x %d rows\n", name,
                 g.originX, g.originY, g.cellW, g.cellH,
                 (surface.width() - 2 * g.originX) / g.cellW, (surface.height() - g.originY) / g.cellH);
    } else {
        g = ui::GridMetrics();
        OSReport("Ufin: %s text grid measurement failed -- using defaults origin %d,%d cell %dx%d\n", name,
                 g.originX, g.originY, g.cellW, g.cellH);
    }
    surface.setGrid(g);
}

} // namespace

bool OSScreenDisplay::init() {
    if (active_) return true;

    OSScreenInit();

    tvSize_ = OSScreenGetBufferSizeEx(SCREEN_TV);
    drcSize_ = OSScreenGetBufferSizeEx(SCREEN_DRC);

    tvBuffer_ = MEMAllocFromDefaultHeapEx(tvSize_, 0x100);
    drcBuffer_ = MEMAllocFromDefaultHeapEx(drcSize_, 0x100);
    if (!tvBuffer_ || !drcBuffer_) {
        if (tvBuffer_) MEMFreeToDefaultHeap(tvBuffer_);
        if (drcBuffer_) MEMFreeToDefaultHeap(drcBuffer_);
        tvBuffer_ = drcBuffer_ = nullptr;
        OSScreenShutdown();
        return false;
    }

    // Zeroed so grid measurement (and the first frame) start from a
    // known state -- the heap hands back whatever was there before.
    memset(tvBuffer_, 0, tvSize_);
    memset(drcBuffer_, 0, drcSize_);
    DCFlushRange(tvBuffer_, tvSize_);
    DCFlushRange(drcBuffer_, drcSize_);

    OSScreenSetBufferEx(SCREEN_TV, tvBuffer_);
    OSScreenSetBufferEx(SCREEN_DRC, drcBuffer_);

    OSScreenEnableEx(SCREEN_TV, TRUE);
    OSScreenEnableEx(SCREEN_DRC, TRUE);

    int tvW = 1280, tvH = 720;
    tvDimensionsFromBufferSize(tvSize_, tvW, tvH);
    tv_ = OSScreenSurface(SCREEN_TV, tvW, tvH);
    drc_ = OSScreenSurface(SCREEN_DRC, 854, 480);

    // The grid doesn't change while running; measure once and reuse it
    // after every GX2 hand-off.
    if (!gridMeasured_) {
        applyMeasuredGrid(tv_, SCREEN_TV, tvBuffer_, tvSize_, "TV");
        applyMeasuredGrid(drc_, SCREEN_DRC, drcBuffer_, drcSize_, "GamePad");
        tvGrid_ = tv_.grid();
        drcGrid_ = drc_.grid();
        gridMeasured_ = true;
    } else {
        tv_.setGrid(tvGrid_);
        drc_.setGrid(drcGrid_);
    }

    active_ = true;
    return true;
}

void OSScreenDisplay::flip() {
    DCFlushRange(tvBuffer_, tvSize_);
    DCFlushRange(drcBuffer_, drcSize_);
    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);
}

void OSScreenDisplay::hide() {
    if (!active_) return;
    OSScreenEnableEx(SCREEN_TV, FALSE);
    OSScreenEnableEx(SCREEN_DRC, FALSE);
}

void OSScreenDisplay::show() {
    if (!active_) return;
    OSScreenEnableEx(SCREEN_TV, TRUE);
    OSScreenEnableEx(SCREEN_DRC, TRUE);
}

void OSScreenDisplay::shutdown() {
    if (!active_) return;
    OSScreenEnableEx(SCREEN_TV, FALSE);
    OSScreenEnableEx(SCREEN_DRC, FALSE);
    // Fully release OSScreen before GX2 takes over the display -- both
    // drive the same hardware, and leaving OSScreen half-alive under
    // WHBGfxInit caused hard hangs. init() re-creates it afterwards.
    OSScreenShutdown();
    if (tvBuffer_) MEMFreeToDefaultHeap(tvBuffer_);
    if (drcBuffer_) MEMFreeToDefaultHeap(drcBuffer_);
    tvBuffer_ = nullptr;
    drcBuffer_ = nullptr;
    active_ = false;
}
