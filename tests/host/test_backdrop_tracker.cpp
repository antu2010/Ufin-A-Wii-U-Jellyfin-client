// Host test for backdrop_tracker.h: settling, cross-fade, fade-out, give-up.
#include "check.h"
#include "backdrop_tracker.h"

// Runs `seconds` of 60 fps frames with a fixed wanted path; the texture for the shown path is
// ready iff `ready`. Returns the time reached.
static double run(BackdropTracker& t, const std::string& wanted, bool ready, double from, double seconds) {
    double now = from;
    const double end = from + seconds;
    while (now < end) {
        t.update(wanted, ready, now);
        now += 1.0 / 60.0;
    }
    return now;
}

int main() {
    {   // Nothing wanted: nothing shown, nothing covered.
        BackdropTracker t;
        run(t, "", false, 0.0, 1.0);
        CHECK_STR(t.curPath(), "");
        CHECK_STR(t.prevPath(), "");
        CHECK(t.coverage() == 0.0f);
    }
    {   // A backdrop only becomes the target after it has settled; then it fades in.
        BackdropTracker t(0.35, 0.6);
        double now = run(t, "a", true, 0.0, 0.2);
        CHECK_STR(t.curPath(), "");              // still settling
        now = run(t, "a", true, now, 0.3);       // 0.5 s total
        CHECK_STR(t.curPath(), "a");
        CHECK(t.curAlpha() > 0.0f && t.curAlpha() < 1.0f);
        now = run(t, "a", true, now, 1.0);
        CHECK(t.curAlpha() == 1.0f);
        CHECK(t.coverage() == 1.0f);
    }
    {   // Fast scrolling (wanted changes every 0.1 s) never settles, so nothing is fetched.
        BackdropTracker t(0.35, 0.6);
        double now = 0.0;
        const char* seq[] = {"a", "b", "c", "d", "e", "f"};
        for (const char* w : seq) now = run(t, w, true, now, 0.1);
        CHECK_STR(t.curPath(), "");
    }
    {   // Cross-fade: the old backdrop stays opaque underneath while the new one fades in.
        BackdropTracker t(0.35, 0.6);
        double now = run(t, "a", true, 0.0, 2.0);
        CHECK(t.curAlpha() == 1.0f);
        now = run(t, "b", true, now, 0.5);       // settled, switching
        CHECK_STR(t.curPath(), "b");
        CHECK_STR(t.prevPath(), "a");
        CHECK(t.coverage() == 1.0f);             // never a gap showing the plain background
        now = run(t, "b", true, now, 1.0);
        CHECK(t.curAlpha() == 1.0f);
        CHECK_STR(t.prevPath(), "");             // finished: the old one is dropped
    }
    {   // The new one is slow to load: the old stays up; if it never loads, the old is dropped.
        BackdropTracker t(0.35, 0.6, 5.0);
        double now = run(t, "a", true, 0.0, 2.0);
        now = run(t, "b", false, now, 2.0);
        CHECK_STR(t.prevPath(), "a");
        CHECK(t.curAlpha() == 0.0f);
        now = run(t, "b", false, now, 4.0);
        CHECK_STR(t.prevPath(), "");
        CHECK(t.coverage() == 0.0f);
    }
    {   // Nothing wanted any more: the current one fades out (no abrupt pop).
        BackdropTracker t(0.35, 0.6);
        double now = run(t, "a", true, 0.0, 2.0);
        now = run(t, "", true, now, 0.2);
        CHECK(t.curAlpha() > 0.0f && t.curAlpha() < 1.0f);
        now = run(t, "", true, now, 1.0);
        CHECK_STR(t.curPath(), "");
        CHECK(t.coverage() == 0.0f);
    }
    {   // Going back to the same backdrop mid-scroll does not restart it.
        BackdropTracker t(0.35, 0.6);
        double now = run(t, "a", true, 0.0, 2.0);
        now = run(t, "b", true, now, 0.2);       // not settled
        now = run(t, "a", true, now, 0.5);       // back to a
        CHECK_STR(t.curPath(), "a");
        CHECK(t.curAlpha() == 1.0f);
        CHECK_STR(t.prevPath(), "");
    }
    return check::finish("test_backdrop_tracker");
}
