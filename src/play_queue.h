// The list of tracks Now Playing works through: plays in list order or
// shuffled, and moves to the next/previous track. Pure logic (no wut),
// covered by tests/host/test_play_queue.cpp.
#pragma once
#include "jellyfin_client.h"

#include <cstdint>
#include <string>
#include <vector>

class PlayQueue {
public:
    // What happens when the queue reaches its end (or a track ends): Off = stop after the last track,
    // All = start again from the top (a shuffled queue is reshuffled), One = play the same track again.
    // Skipping with L/R still moves on in One (only a track that ends by itself repeats).
    enum class Repeat { Off = 0, All = 1, One = 2 };

    // Queue of `items` starting at items[startIndex]. With shuffle, the
    // start track plays first and the rest follow in random order.
    PlayQueue(std::vector<JellyfinItem> items, size_t startIndex, bool shuffle, uint32_t seed)
        : items_(std::move(items)) {
        if (items_.empty()) return;
        if (startIndex >= items_.size()) startIndex = 0;
        if (!shuffle) {
            for (size_t i = 0; i < items_.size(); i++) order_.push_back(i);
            pos_ = startIndex;
            return;
        }
        shuffled_ = true;
        order_.push_back(startIndex);
        for (size_t i = 0; i < items_.size(); i++) {
            if (i != startIndex) order_.push_back(i);
        }
        // Fisher-Yates over everything after the first track, with a
        // small xorshift PRNG so the order is reproducible in tests.
        uint32_t state = seed ? seed : 0x9E3779B9u;
        shuffleFrom(order_, 1, state);
        wrapState_ = state;
        pos_ = 0;
    }

    bool empty() const { return order_.empty(); }
    size_t size() const { return order_.size(); }
    size_t position() const { return pos_; } // 0-based place in play order
    bool shuffled() const { return shuffled_; }

    const JellyfinItem& current() const { return items_[order_[pos_]]; }
    Repeat repeat() const { return repeat_; }
    void setRepeat(Repeat r) { repeat_ = r; wrap_.clear(); }
    // Off -> All -> One -> Off (one button: press it again for the next mode).
    Repeat cycleRepeat() {
        setRepeat(repeat_ == Repeat::Off ? Repeat::All : (repeat_ == Repeat::All ? Repeat::One : Repeat::Off));
        return repeat_;
    }

    // Turns shuffle on or off while playing. On: the tracks already played and the current one stay
    // where they are, the rest are shuffled. Off: back to list order, continuing after the current track.
    void setShuffle(bool on, uint32_t seed) {
        if (order_.empty() || on == shuffled_) return;
        wrap_.clear();
        if (on) {
            uint32_t state = seed ? seed : 0x9E3779B9u;
            shuffleFrom(order_, pos_ + 1, state);
            shuffled_ = true;
        } else {
            const size_t cur = order_[pos_];
            for (size_t i = 0; i < order_.size(); i++) order_[i] = i;
            pos_ = cur;
            shuffled_ = false;
        }
    }

    bool hasNext() const {
        if (order_.empty()) return false;
        return pos_ + 1 < order_.size() || repeat_ == Repeat::All;
    }
    bool hasPrevious() const { return pos_ > 0 || (repeat_ == Repeat::All && order_.size() > 1); }
    const JellyfinItem* peekNext() const {
        if (!hasNext()) return nullptr;
        if (pos_ + 1 < order_.size()) return &items_[order_[pos_ + 1]];
        if (shuffled_) { ensureWrap(); return &items_[wrap_[0]]; } // the next round's first track
        return &items_[order_[0]];
    }

    // Skip forward (also what a finished track does when repeat is not One). At the end: wraps with
    // Repeat::All, otherwise false (the queue is over).
    bool next() {
        if (order_.empty()) return false;
        if (pos_ + 1 < order_.size()) {
            pos_++;
            return true;
        }
        if (repeat_ != Repeat::All) return false;
        if (shuffled_) {
            ensureWrap();
            order_ = wrap_;
            wrap_.clear();
        }
        pos_ = 0;
        return true;
    }
    bool previous() {
        if (!hasPrevious()) return false;
        pos_ = pos_ > 0 ? pos_ - 1 : order_.size() - 1;
        return true;
    }
    // A track ended by itself: Repeat::One plays it again (true, same track), else the next one.
    bool advanceAfterEnd() {
        if (order_.empty()) return false;
        if (repeat_ == Repeat::One) return true;
        return next();
    }

private:
    // Fisher-Yates over v[from..]: xorshift PRNG, so the order is reproducible in tests.
    static void shuffleFrom(std::vector<size_t>& v, size_t from, uint32_t& state) {
        if (v.size() < from + 2) return;
        for (size_t i = v.size() - 1; i > from; i--) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const size_t j = from + state % (i - from + 1); // in [from, i]
            std::swap(v[i], v[j]);
        }
    }

    // The order of the next round (Repeat::All with shuffle): everything reshuffled, and never starting with
    // the track that just played (unless it is the only one). Made once, so "Next:" shows what really follows.
    void ensureWrap() const {
        if (!wrap_.empty()) return;
        wrap_.clear();
        for (size_t i = 0; i < order_.size(); i++) wrap_.push_back(i);
        wrapState_ = wrapState_ ? wrapState_ : 0x9E3779B9u;
        shuffleFrom(wrap_, 0, wrapState_);
        if (wrap_.size() > 1 && wrap_[0] == order_.back()) std::swap(wrap_[0], wrap_[1 + wrapState_ % (wrap_.size() - 1)]);
    }

    std::vector<JellyfinItem> items_;
    std::vector<size_t> order_;
    size_t pos_ = 0;
    bool shuffled_ = false;
    Repeat repeat_ = Repeat::Off;
    mutable std::vector<size_t> wrap_;        // next round's shuffled order, once asked for
    mutable uint32_t wrapState_ = 0x9E3779B9u;
};

// Tracks worth queueing from a list: the audio items, in list order.
// `selected` is the index in `list` of the track to start from (or of
// anything else, in which case the queue starts at the first track);
// returns the tracks and sets `startIndex` to that track's place.
inline std::vector<JellyfinItem> audioTracks(const std::vector<JellyfinItem>& list, size_t selected,
                                             size_t& startIndex) {
    std::vector<JellyfinItem> tracks;
    startIndex = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].type != "Audio") continue;
        if (i == selected) startIndex = tracks.size();
        tracks.push_back(list[i]);
    }
    return tracks;
}
