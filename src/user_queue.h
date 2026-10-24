// The "Up Next" list: items the user queued by hand. One list for the whole session (music, movies and
// episodes), played BEFORE the rest of the current music queue (play_queue.h) or the next autoplay episode.
// Pure logic (no wut), covered by tests/host/test_user_queue.cpp. Nothing here is saved to the server or to disk.
//
// Semantics:
//  - the same item may be queued many times (it is a list of plays, not a set);
//  - playing an entry removes it; the item playing now is never part of the list, so editing the list
//    never touches the current track;
//  - "play now" moves the chosen entry to the front: entries ahead of it stay queued behind it.
#pragma once
#include "jellyfin_client.h"

#include <cstddef>
#include <vector>

class UserQueue {
public:
    // Songs / movies / episodes only: containers (albums, shows ...) must be expanded to their items first.
    static bool playable(const JellyfinItem& it) {
        return it.type == "Audio" || it.type == "Movie" || it.type == "Episode" || it.type == "Video" ||
               it.type == "MusicVideo";
    }
    // Adds the playable items of `v` (in order). Returns how many were added.
    size_t add(const std::vector<JellyfinItem>& v) {
        size_t n = 0;
        for (const JellyfinItem& it : v) {
            if (!playable(it) || items_.size() >= kMax) continue;
            items_.push_back(it);
            n++;
        }
        if (n) rev_++;
        return n;
    }
    size_t add(const JellyfinItem& it) { return add(std::vector<JellyfinItem>{it}); }
    bool remove(size_t i) {
        if (i >= items_.size()) return false;
        items_.erase(items_.begin() + (long)i);
        rev_++;
        return true;
    }
    // Moves entry i by delta places (negative = earlier). Returns the new index (i if it could not move).
    size_t move(size_t i, int delta) {
        if (i >= items_.size()) return i;
        long j = (long)i + delta;
        if (j < 0) j = 0;
        if (j >= (long)items_.size()) j = (long)items_.size() - 1;
        if ((size_t)j == i) return i;
        JellyfinItem it = items_[i];
        items_.erase(items_.begin() + (long)i);
        items_.insert(items_.begin() + j, it);
        rev_++;
        return (size_t)j;
    }
    void clear() {
        if (items_.empty()) return;
        items_.clear();
        rev_++;
    }
    // Makes entry i the next one to play.
    bool playNext(size_t i) {
        if (i >= items_.size()) return false;
        move(i, -(int)i);
        return true;
    }
    bool popFront(JellyfinItem& out) {
        if (items_.empty()) return false;
        out = items_.front();
        items_.erase(items_.begin());
        rev_++;
        return true;
    }
    const JellyfinItem* front() const { return items_.empty() ? nullptr : &items_.front(); }
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    const std::vector<JellyfinItem>& items() const { return items_; }
    unsigned revision() const { return rev_; } // changes on every edit (the UI refreshes on it)

    static const size_t kMax = 500; // plenty for a session; keeps the list (and a long album add) bounded

private:
    std::vector<JellyfinItem> items_;
    unsigned rev_ = 0;
};

#include "play_queue.h"

// What happens when a music track ends (or R / Next is pressed), given the music queue and the Up Next list.
//   Replay  - the same item again (a track that ended by itself with Repeat one; Next never replays)
//   Queued  - `out` is the next Up Next entry (now removed from the list): plays before the rest of the music queue
//   Handoff - the next Up Next entry is a movie / episode: the music loop ends and the caller plays it
//   Base    - the music queue moved on (`base.current()` is the item; Repeat all wraps here, once per round)
//   Stop    - nothing left
// Up Next entries are played once each and never re-added, so Repeat all / one cannot loop or duplicate them.
enum class Advance { Replay, Queued, Handoff, Base, Stop };
inline Advance advanceMusic(PlayQueue& base, UserQueue& up, bool skipped, JellyfinItem& out) {
    if (!skipped && base.repeat() == PlayQueue::Repeat::One) return Advance::Replay;
    if (const JellyfinItem* f = up.front()) {
        if (f->type != "Audio") return Advance::Handoff;
        up.popFront(out);
        return Advance::Queued;
    }
    const bool ok = skipped ? base.next() : base.advanceAfterEnd();
    return ok ? Advance::Base : Advance::Stop;
}
