// frame_reorder.h -- puts decoded pictures back into display order.
//
// WHY: h264_wiiu hands back one picture per packet, in DECODE order, and each
// picture carries its own packet's pts. With B-frames (Main/High) decode order
// differs from display order, so the pts run e.g. 0, 3, 1, 2, 6, 4, 5 ... The
// player shows frames in the order it is given them, so without this they would
// play "like jelly". Baseline has no B-frames: pts already rise, nothing waits.
//
// HOW: hold the last `depth` pictures; when one more arrives, release the one
// with the smallest pts. That is correct as long as no picture arrives more
// than `depth` places after its display slot (x264 with B-pyramid needs 2).
// A picture that still turns out to be older than one already released is
// flagged `late` (the caller drops it -- showing it would step backwards) and
// the depth grows by one, up to kMaxDepth, so a server with a deeper GOP
// structure corrects itself after a frame or two instead of looking wrong all
// film. A frame without a timestamp (NAN) flushes the buffer and passes through.
//
// Header-only and generic over the payload (AVFrame* in the player, int in the
// host test).
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

template <class T>
class FrameReorder {
public:
    static constexpr size_t kMaxDepth = 4;

    struct Entry {
        T item;
        double pts;
        bool late; // older than something already released -- do not show
    };

    // depth 0 = pass-through (Baseline).
    explicit FrameReorder(size_t depth = 0) : depth_(depth > kMaxDepth ? kMaxDepth : depth) {}

    size_t depth() const { return depth_; }
    size_t held() const { return held_.size(); }
    size_t lateCount() const { return lateCount_; }

    // Adds a picture; appends whatever is now ready, in display order, to `out`.
    void push(T item, double pts, std::vector<Entry>& out) {
        if (std::isnan(pts)) {
            flush(out);
            out.push_back(Entry{item, pts, false});
            return;
        }
        if (depth_ == 0) {
            emit(Entry{item, pts, false}, out);
            return;
        }
        // Insert keeping held_ sorted by pts (ascending); equal pts keep arrival order.
        size_t i = held_.size();
        while (i > 0 && held_[i - 1].pts > pts) i--;
        held_.insert(held_.begin() + (long)i, Entry{item, pts, false});
        while (held_.size() > depth_) {
            Entry e = held_.front();
            held_.erase(held_.begin());
            emit(e, out);
        }
    }

    // End of stream: release everything, in order.
    void flush(std::vector<Entry>& out) {
        while (!held_.empty()) {
            Entry e = held_.front();
            held_.erase(held_.begin());
            emit(e, out);
        }
    }

private:
    void emit(Entry e, std::vector<Entry>& out) {
        if (!std::isnan(lastEmitted_) && e.pts < lastEmitted_) {
            e.late = true;
            lateCount_++;
            if (depth_ > 0 && depth_ < kMaxDepth) depth_++; // deeper reordering than assumed
        } else {
            lastEmitted_ = e.pts;
        }
        out.push_back(e);
    }

    size_t depth_;
    size_t lateCount_ = 0;
    double lastEmitted_ = NAN;
    std::vector<Entry> held_;
};
