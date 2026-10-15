// PlayQueue: list order, shuffle (first track fixed, all tracks exactly
// once, reproducible), next/previous at the edges, and picking the audio
// tracks out of a mixed list.
#include "check.h"
#include "play_queue.h"

#include <algorithm>
#include <set>

static std::vector<JellyfinItem> tracks(int n) {
    std::vector<JellyfinItem> v;
    for (int i = 0; i < n; i++) {
        JellyfinItem it;
        it.id = "t" + std::to_string(i);
        it.name = "Track " + std::to_string(i);
        it.type = "Audio";
        v.push_back(it);
    }
    return v;
}

int main() {
    // In order, starting in the middle.
    {
        PlayQueue q(tracks(5), 2, false, 1);
        CHECK(!q.shuffled());
        CHECK_STR(q.current().id, "t2");
        CHECK(q.hasPrevious());
        CHECK_STR(q.peekNext()->id, "t3");
        CHECK(q.next()); CHECK(q.next());
        CHECK_STR(q.current().id, "t4");
        CHECK(!q.hasNext());
        CHECK(!q.next());
        CHECK_STR(q.current().id, "t4");
        CHECK(q.previous());
        CHECK_STR(q.current().id, "t3");
    }

    // Shuffle: chosen track first, every track exactly once, and the
    // order actually changes.
    {
        PlayQueue q(tracks(20), 7, true, 12345);
        CHECK(q.shuffled());
        CHECK_EQ((int)q.size(), 20);
        CHECK_STR(q.current().id, "t7");
        CHECK(!q.hasPrevious());
        std::set<std::string> seen;
        std::vector<std::string> order;
        do {
            seen.insert(q.current().id);
            order.push_back(q.current().id);
        } while (q.next());
        CHECK_EQ((int)seen.size(), 20);
        std::vector<std::string> sorted = order;
        std::sort(sorted.begin() + 1, sorted.end());
        CHECK(order != sorted);

        // Same seed, same order; different seed, different order.
        PlayQueue a(tracks(20), 7, true, 12345), b(tracks(20), 7, true, 999);
        std::vector<std::string> oa, ob;
        do oa.push_back(a.current().id); while (a.next());
        do ob.push_back(b.current().id); while (b.next());
        CHECK(oa == order);
        CHECK(ob != order);
    }

    // Edge cases.
    {
        PlayQueue empty(tracks(0), 0, true, 1);
        CHECK(empty.empty());
        PlayQueue one(tracks(1), 0, true, 1);
        CHECK_STR(one.current().id, "t0");
        CHECK(!one.hasNext());
        PlayQueue badStart(tracks(3), 9, false, 1);
        CHECK_STR(badStart.current().id, "t0");
        PlayQueue two(tracks(2), 1, true, 5);
        CHECK_STR(two.current().id, "t1");
        CHECK(two.next());
        CHECK_STR(two.current().id, "t0");
    }

    // Repeat: off ends the queue; all wraps (both ways); one replays only when a track ends by itself.
    {
        PlayQueue q(tracks(3), 1, false, 1);
        CHECK(q.repeat() == PlayQueue::Repeat::Off);
        CHECK(q.next());
        CHECK(!q.next());                       // end, repeat off
        CHECK(!q.advanceAfterEnd());
        CHECK(q.cycleRepeat() == PlayQueue::Repeat::All);
        CHECK(q.hasNext());
        CHECK_STR(q.peekNext()->id, "t0");
        CHECK(q.next());                        // wraps
        CHECK_STR(q.current().id, "t0");
        CHECK(q.hasPrevious());
        CHECK(q.previous());                    // and back the other way
        CHECK_STR(q.current().id, "t2");
        CHECK(q.cycleRepeat() == PlayQueue::Repeat::One);
        CHECK(q.advanceAfterEnd());             // a track that ended: same one again
        CHECK_STR(q.current().id, "t2");
    }
    {
        PlayQueue q(tracks(3), 2, false, 1);
        q.setRepeat(PlayQueue::Repeat::One);
        CHECK(!q.next());                       // skip at the end with repeat one: nothing after the last track
        CHECK(q.advanceAfterEnd());             // but an ended track plays again
        CHECK_STR(q.current().id, "t2");
        CHECK(q.cycleRepeat() == PlayQueue::Repeat::Off);
        CHECK(!q.advanceAfterEnd());
    }

    // Repeat all + shuffle: the next round is a fresh order of every track, not starting with the last played.
    {
        PlayQueue q(tracks(6), 0, true, 77);
        q.setRepeat(PlayQueue::Repeat::All);
        for (int i = 0; i < 5; i++) CHECK(q.next());
        const std::string last = q.current().id;
        const JellyfinItem* peek = q.peekNext();
        CHECK(peek != nullptr);
        const std::string promised = peek->id;
        CHECK(promised != last);
        CHECK(q.next());
        CHECK_STR(q.current().id, promised);    // "Next:" told the truth
        CHECK_EQ((int)q.position(), 0);
        std::set<std::string> seen;
        do seen.insert(q.current().id); while (q.next() && q.position() != 0);
        CHECK_EQ((int)seen.size(), 6);          // every track exactly once in the new round
    }
    {
        PlayQueue one(tracks(1), 0, false, 1);
        one.setRepeat(PlayQueue::Repeat::All);
        CHECK(one.next());                      // a single track repeats
        CHECK_STR(one.current().id, "t0");
    }

    // Shuffle on and off while playing.
    {
        PlayQueue q(tracks(10), 3, false, 1);
        CHECK(q.next());                        // now on t4
        q.setShuffle(true, 4242);
        CHECK(q.shuffled());
        CHECK_STR(q.current().id, "t4");        // the current track keeps playing
        CHECK_EQ((int)q.position(), 4);         // history stays behind it
        CHECK(q.hasPrevious());
        CHECK(q.previous()); CHECK_STR(q.current().id, "t3");
        CHECK(q.next());
        std::set<std::string> seen = {"t0", "t1", "t2", "t3", "t4"};
        bool inOrder = true;
        std::string prev = q.current().id;
        while (q.next()) {
            seen.insert(q.current().id);
            if (q.current().id != "t" + std::to_string(std::stoi(prev.substr(1)) + 1)) inOrder = false;
            prev = q.current().id;
        }
        CHECK_EQ((int)seen.size(), 10);         // nothing lost, nothing doubled
        CHECK(!inOrder);                        // and the rest really is mixed up
    }
    {
        PlayQueue q(tracks(10), 0, true, 99);
        CHECK(q.next()); CHECK(q.next());
        const std::string here = q.current().id;
        q.setShuffle(false, 1);
        CHECK(!q.shuffled());
        CHECK_STR(q.current().id, here);        // still on the same track
        const int n = std::stoi(here.substr(1));
        if (n < 9) { CHECK(q.next()); CHECK_STR(q.current().id, "t" + std::to_string(n + 1)); } // list order from here
        q.setShuffle(false, 1);                 // already off: nothing happens
        CHECK(!q.shuffled());
    }

    // audioTracks: only Audio items, start index follows the selection.
    {
        std::vector<JellyfinItem> list = tracks(3);
        JellyfinItem folder;
        folder.type = "MusicAlbum";
        list.insert(list.begin() + 1, folder); // t0, album, t1, t2
        size_t start = 99;
        std::vector<JellyfinItem> t = audioTracks(list, 3, start);
        CHECK_EQ((int)t.size(), 3);
        CHECK_EQ((int)start, 2);
        CHECK_STR(t[start].id, "t2");
        t = audioTracks(list, 1, start); // selection isn't a track
        CHECK_EQ((int)start, 0);
    }

    return check::finish("test_play_queue");
}
