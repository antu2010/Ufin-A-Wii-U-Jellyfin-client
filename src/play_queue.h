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
        for (size_t i = order_.size() - 1; i > 1; i--) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            size_t j = 1 + state % i; // in [1, i]
            std::swap(order_[i], order_[j]);
        }
        pos_ = 0;
    }

    bool empty() const { return order_.empty(); }
    size_t size() const { return order_.size(); }
    size_t position() const { return pos_; } // 0-based place in play order
    bool shuffled() const { return shuffled_; }

    const JellyfinItem& current() const { return items_[order_[pos_]]; }
    bool hasNext() const { return pos_ + 1 < order_.size(); }
    bool hasPrevious() const { return pos_ > 0; }
    const JellyfinItem* peekNext() const { return hasNext() ? &items_[order_[pos_ + 1]] : nullptr; }

    bool next() {
        if (!hasNext()) return false;
        pos_++;
        return true;
    }
    bool previous() {
        if (!hasPrevious()) return false;
        pos_--;
        return true;
    }

private:
    std::vector<JellyfinItem> items_;
    std::vector<size_t> order_;
    size_t pos_ = 0;
    bool shuffled_ = false;
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
