// FrameQueue -- thread-safe bounded queue of decoded video frames (plus
// their presentation timestamps), connecting the decode thread
// (producer) to the render thread (consumer) in player.cpp.
//
// Frames are independently ref-counted clones (via av_frame_clone) --
// Decoder reuses a single internal AVFrame across calls, so a frame must
// be cloned before being handed across the thread boundary, or its
// contents would be overwritten by the next decode before the render
// thread gets to it. Whoever pops a frame owns it and must
// av_frame_free() it.
//
// Two ways for the producer to end the stream:
//   finish() -- normal end of stream. Nothing more will be pushed, but
//               everything already queued is still delivered, so the
//               last frames of a video actually get shown.
//   stop()   -- abort (user pressed B). Drops everything immediately
//               and wakes both sides so they can exit.

#pragma once
extern "C" {
#include <libavutil/frame.h>
}
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>

struct QueuedVideoFrame {
    AVFrame* frame = nullptr;
    double pts = NAN; // seconds in stream time, NAN if the stream had no usable timestamp
};

class FrameQueue {
public:
    // Decoded 1280x720 NV12 frames are ~1.4 MB each. The old MAX_SIZE=6
    // (~8 MB, ~0.25s at 24-30fps) turned out to give real Wi-Fi barely any
    // slack to absorb a network hiccup before the render loop ran dry --
    // even though up to 24 MB of *compressed* packets (Decoder's own
    // MAX_BUFFERED_PACKET_BYTES, tens of seconds of data) could already be
    // sitting buffered upstream of the decoder. 24 frames (~34 MB, ~0.8-1s
    // of decoded buffer) lets a brief stall get absorbed instead of
    // draining the queue and stalling playback.
    static const size_t MAX_SIZE = 24;

    // Blocks while the queue is full, which naturally paces the decode
    // thread to roughly the render thread's consumption rate. Takes
    // ownership of `frame` (frees it if the queue has been stopped).
    void push(AVFrame* frame, double pts) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_not_full_.wait(lock, [this] { return queue_.size() < MAX_SIZE || stopped_; });
        if (stopped_ || finished_) {
            av_frame_free(&frame);
            return;
        }
        queue_.push(QueuedVideoFrame{frame, pts});
        lock.unlock();
        cv_not_empty_.notify_one();
    }

    // Waits up to timeoutMs for a frame. Returns true and fills `out` if
    // one was available; false on timeout, or if the queue is stopped /
    // finished with nothing left. Callers distinguish those cases via
    // isDrained().
    bool pop(QueuedVideoFrame& out, int timeoutMs) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_not_empty_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [this] { return !queue_.empty() || stopped_ || finished_; });
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        lock.unlock();
        cv_not_full_.notify_one();
        return true;
    }

    // True once the producer has called finish() or stop() and every
    // queued frame has been popped -- i.e. there will never be another
    // frame.
    bool isDrained() {
        std::lock_guard<std::mutex> lock(mtx_);
        return (finished_ || stopped_) && queue_.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    // Producer is done pushing (normal end of stream). Queued frames
    // remain deliverable.
    void finish() {
        std::unique_lock<std::mutex> lock(mtx_);
        finished_ = true;
        lock.unlock();
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    // Abort: wakes any threads blocked in push()/pop() so they can exit
    // cleanly, and frees anything still queued.
    void stop() {
        std::unique_lock<std::mutex> lock(mtx_);
        stopped_ = true;
        while (!queue_.empty()) {
            AVFrame* f = queue_.front().frame;
            queue_.pop();
            av_frame_free(&f);
        }
        lock.unlock();
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

private:
    std::queue<QueuedVideoFrame> queue_;
    std::mutex mtx_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    bool stopped_ = false;
    bool finished_ = false;
};
