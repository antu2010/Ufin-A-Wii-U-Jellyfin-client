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

    // Decoded frames are a poor use of RAM: one second of 1080p30 NV12 is
    // ~89 MB, while the same second of compressed 6 Mbps video is 0.75 MB
    // (see Player's packet buffer). So pictures larger than 720p get a
    // fixed, moderate decoded-frame budget instead of MAX_SIZE slots of
    // the bigger frame (24 x 3 MB = ~71 MB). 720p and smaller keep exactly
    // MAX_SIZE, so the stable path is unchanged.
    static const size_t BIG_PICTURE_BUDGET_BYTES = 48u * 1024u * 1024u;
    static const size_t MIN_CAPACITY = 6; // never fewer than the old pre-tuning depth

    // Slots to use for frames of `frameBytes` each. frameBytes == 0
    // (unknown / no video) and anything at or below the 720p frame size
    // keep MAX_SIZE.
    static size_t capacityForFrameBytes(size_t frameBytes) {
        if (frameBytes <= 1280u * 720u * 3u / 2u) return MAX_SIZE;
        size_t n = BIG_PICTURE_BUDGET_BYTES / frameBytes;
        if (n < MIN_CAPACITY) n = MIN_CAPACITY;
        if (n > MAX_SIZE) n = MAX_SIZE;
        return n;
    }

    FrameQueue() = default;
    explicit FrameQueue(size_t capacity) : capacity_(capacity < 1 ? 1 : capacity) {}

    size_t capacity() const { return capacity_; }

    // Blocks while the queue is full, which naturally paces the decode
    // thread to roughly the render thread's consumption rate. Takes
    // ownership of `frame` (frees it if the queue has been stopped).
    void push(AVFrame* frame, double pts) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_not_full_.wait(lock, [this] { return queue_.size() < capacity_ || stopped_; });
        if (stopped_ || finished_) {
            av_frame_free(&frame);
            return;
        }
        queue_.push(QueuedVideoFrame{frame, pts});
        last_pushed_pts_ = pts;
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

    // Timestamp (stream seconds) of the newest frame ever pushed, whether or
    // not it has been popped since; NAN before the first push or if that
    // frame had no timestamp. Player uses it to measure decoded video ahead
    // of the playback clock on the timeline, instead of guessing from the
    // number of queued frames.
    double lastPushedPts() {
        std::lock_guard<std::mutex> lock(mtx_);
        return last_pushed_pts_;
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
    size_t capacity_ = MAX_SIZE;
    bool stopped_ = false;
    bool finished_ = false;
    double last_pushed_pts_ = NAN; // guarded by mtx_
};
