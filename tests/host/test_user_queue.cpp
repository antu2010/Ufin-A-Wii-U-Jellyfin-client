// UserQueue (the Up Next list) and advanceMusic (how it interleaves with the music queue).
#include "check.h"
#include "user_queue.h"

static JellyfinItem mk(const std::string& id, const char* type = "Audio") {
    JellyfinItem it; it.id = id; it.name = id; it.type = type; return it;
}
static std::vector<JellyfinItem> tracks(int n) {
    std::vector<JellyfinItem> v;
    for (int i = 0; i < n; i++) v.push_back(mk("t" + std::to_string(i)));
    return v;
}
static std::string ids(const UserQueue& q) {
    std::string s;
    for (auto& i : q.items()) s += i.id + ",";
    return s;
}

int main() {
    UserQueue q;
    CHECK(q.empty());
    CHECK(q.add({mk("a"), mk("b"), mk("c")}) == 3);
    CHECK(q.add(mk("a")) == 1); // duplicates are allowed
    CHECK_STR(ids(q), "a,b,c,a,");
    CHECK(q.add(mk("album", "MusicAlbum")) == 0); // containers are never queued
    CHECK(q.add(mk("m", "Movie")) == 1);
    CHECK(q.add(mk("e", "Episode")) == 1);
    CHECK_STR(ids(q), "a,b,c,a,m,e,");
    unsigned r = q.revision();
    CHECK(q.move(2, -1) == 1);
    CHECK_STR(ids(q), "a,c,b,a,m,e,");
    CHECK(q.move(0, -5) == 0); // at the edge: nothing changes
    CHECK(q.revision() == r + 1);
    CHECK(q.move(5, 3) == 5);
    CHECK(q.remove(1));
    CHECK(!q.remove(99));
    CHECK_STR(ids(q), "a,b,a,m,e,");
    CHECK(q.playNext(3)); // play now: m first, the rest keep their order
    CHECK_STR(ids(q), "m,a,b,a,e,");
    JellyfinItem out;
    CHECK(q.popFront(out)); CHECK_STR(out.id, "m");
    q.clear();
    CHECK(q.empty());
    CHECK(!q.popFront(out));

    // The cap
    UserQueue big;
    std::vector<JellyfinItem> many;
    for (int i = 0; i < 700; i++) many.push_back(mk("x" + std::to_string(i)));
    CHECK(big.add(many) == UserQueue::kMax);

    // Music: Up Next plays before the rest of the queue, once each.
    {
        PlayQueue base(tracks(4), 0, false, 1);
        UserQueue up;
        up.add({mk("u1"), mk("u2")});
        JellyfinItem o;
        CHECK(advanceMusic(base, up, false, o) == Advance::Queued); CHECK_STR(o.id, "u1");
        CHECK(advanceMusic(base, up, true, o) == Advance::Queued);  CHECK_STR(o.id, "u2");
        CHECK(advanceMusic(base, up, false, o) == Advance::Base);   CHECK_STR(base.current().id, "t1");
        CHECK(advanceMusic(base, up, true, o) == Advance::Base);    CHECK_STR(base.current().id, "t2");
    }
    // Repeat one: ends replay (even a queued item); Next always moves on and takes the queue first.
    {
        PlayQueue base(tracks(3), 0, false, 1);
        base.setRepeat(PlayQueue::Repeat::One);
        UserQueue up; up.add(mk("u1"));
        JellyfinItem o;
        CHECK(advanceMusic(base, up, false, o) == Advance::Replay);
        CHECK(up.size() == 1);
        CHECK(advanceMusic(base, up, true, o) == Advance::Queued);
        CHECK(advanceMusic(base, up, false, o) == Advance::Replay);
        CHECK(advanceMusic(base, up, true, o) == Advance::Base);
        CHECK_STR(base.current().id, "t1");
    }
    // Repeat all: wraps the base queue only; Up Next entries are not duplicated by it.
    {
        PlayQueue base(tracks(2), 1, false, 1);
        base.setRepeat(PlayQueue::Repeat::All);
        UserQueue up; up.add(mk("u1"));
        JellyfinItem o;
        CHECK(advanceMusic(base, up, false, o) == Advance::Queued);
        CHECK(advanceMusic(base, up, false, o) == Advance::Base); CHECK_STR(base.current().id, "t0");
        CHECK(up.empty());
    }
    // End of the queue, and a movie in the list.
    {
        PlayQueue base(tracks(1), 0, false, 1);
        UserQueue up; JellyfinItem o;
        CHECK(advanceMusic(base, up, false, o) == Advance::Stop);
        up.add(mk("m", "Movie"));
        CHECK(advanceMusic(base, up, false, o) == Advance::Handoff);
        CHECK(up.size() == 1); // left for the caller
    }
    return check::finish("test_user_queue");
}
