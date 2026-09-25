// Test stand-in for src/media/video_output.h: same interface, no GX2.
// Counts the frames Player hands it. Used by tests/host/test_player_seek
// (player.cpp is compiled next to this file so its #include
// "video_output.h" picks this one up).
#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <cstdint>
#include <functional>

using VideoDrawFn = std::function<void(uint32_t width, uint32_t height)>;
using VideoPresenter = std::function<void(const VideoDrawFn& drawVideo)>;

class VideoOutput {
public:
    bool init(int width, int height, double) { width_ = width; height_ = height; return width > 0 && height > 0; }
    void renderFrame(AVFrame* frame) {
        if (!frame) return;
        framesShown++;
        if (presenter_) presenter_([](uint32_t, uint32_t) {});
    }
    void renderTestPattern() {}
    void shutdown() {}
    void setPresenter(VideoPresenter p) { presenter_ = std::move(p); }
    void presentLast() {}
    const char* lastError() const { return "fake video output"; }

    static inline int framesShown = 0;

private:
    int width_ = 0, height_ = 0;
    VideoPresenter presenter_;
};
