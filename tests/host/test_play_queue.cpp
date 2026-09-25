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
