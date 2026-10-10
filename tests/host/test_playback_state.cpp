// The STARTUP_BUFFERING / PLAYING / BUFFERING decision logic on its own
// (src/media/playback_state.h): where BUFFERING starts, where it ends, that
// the two differ, and that end of stream / seeks / pauses are never confused
// with starvation. Player-level behaviour (real threads, audio, video) is
// covered by test_player_rebuffer.cpp.
#include "check.h"
#include "media/playback_state.h"

static BufferSample sample(double playable, double now, bool more = true, bool full = false, bool consuming = true) {
    BufferSample s;
    s.playableSeconds = playable;
    s.nowSeconds = now;
    s.moreMediaMayArrive = more;
    s.readAheadFull = full;
    s.consuming = consuming;
    return s;
}

static PlaybackStateMachine playing() {
    PlaybackStateMachine m; // 2 s low water, 12 s recovery, 0.5 s minimum hold
    CHECK(m.startupComplete());
    CHECK(m.state() == PlaybackState::Playing);
    return m;
}

int main() {
    // The thresholds the feature ships with.
    {
        RebufferThresholds t;
        CHECK_NEAR(t.lowWaterSeconds, 2.0, 1e-9);
        CHECK_NEAR(t.recoverySeconds, 12.0, 1e-9);
        CHECK(t.recoverySeconds >= 10.0 && t.recoverySeconds <= 15.0);
        CHECK(t.recoverySeconds > t.lowWaterSeconds); // hysteresis, not one shared threshold
    }

    // Startup: nothing but startupComplete() leaves STARTUP_BUFFERING, however
    // empty the buffer looks -- the prebuffer wait owns that phase.
    {
        PlaybackStateMachine m;
        CHECK(m.state() == PlaybackState::StartupBuffering);
        CHECK(!m.update(sample(0.0, 0.0)));
        CHECK(!m.update(sample(100.0, 1.0)));
        CHECK(m.state() == PlaybackState::StartupBuffering);
        CHECK(m.startupComplete());
        CHECK(!m.startupComplete()); // only once
        CHECK(m.state() == PlaybackState::Playing);
    }

    // PLAYING -> BUFFERING happens at ~2 s remaining, i.e. BEFORE the buffer is
    // empty. Drain 20 s -> 0 s in 50 ms steps: the switch must come at the first
    // sample at or below the low-water mark, with essentially all 2 s still left.
    {
        PlaybackStateMachine m = playing();
        double enteredAt = -1.0;
        double now = 0.0;
        for (int i = 400; i >= 0; i--) { // 20.00 s down to 0.00 s
            const double playable = i * 0.05;
            now += 0.05;
            if (m.update(sample(playable, now))) {
                CHECK(m.state() == PlaybackState::Buffering);
                enteredAt = playable;
                break;
            }
        }
        CHECK(enteredAt > 0.0);              // never waited for zero
        CHECK(enteredAt <= 2.0 + 1e-9);      // at the low-water mark...
        CHECK(enteredAt >= 2.0 - 0.05 - 1e-9); // ...not a step later
        CHECK(enteredAt >= 1.0);             // "1-3 seconds of playable video remain"
    }
    // Just above the mark is still PLAYING; exactly at it is BUFFERING.
    {
        PlaybackStateMachine m = playing();
        CHECK(!m.update(sample(2.05, 1.0)));
        CHECK(m.state() == PlaybackState::Playing);
        CHECK(m.update(sample(2.0, 1.1)));
        CHECK(m.state() == PlaybackState::Buffering);
        CHECK(m.buffering());
    }

    // Hysteresis: once buffering, 2 s does NOT resume it, nor 11.9 s; 12 s does.
    // And after resuming, a dip back to 3 s (above low water) does not re-enter.
    {
        PlaybackStateMachine m = playing();
        CHECK(m.update(sample(1.9, 10.0)));
        double now = 10.0;
        for (double p : {1.9, 2.0, 2.5, 5.0, 8.0, 11.9}) {
            now += 1.0;
            CHECK(!m.update(sample(p, now)));
            CHECK(m.state() == PlaybackState::Buffering);
        }
        now += 1.0;
        CHECK(m.update(sample(12.0, now)));
        CHECK(m.state() == PlaybackState::Playing);
        CHECK(!m.update(sample(3.0, now + 1.0)));
        CHECK(m.state() == PlaybackState::Playing);
        CHECK(m.update(sample(2.0, now + 2.0))); // ...and it can enter again later
        CHECK(m.state() == PlaybackState::Buffering);
    }

    // A hiccup that refills instantly still holds for the minimum time, so the
    // wheel is never a one-frame flash; end of stream is exempt from that.
    {
        PlaybackStateMachine m = playing();
        CHECK(m.update(sample(1.5, 100.0)));
        CHECK(!m.update(sample(30.0, 100.2))); // recovered, but only 0.2 s in
        CHECK(m.state() == PlaybackState::Buffering);
        CHECK(m.update(sample(30.0, 100.5)));
        CHECK(m.state() == PlaybackState::Playing);

        PlaybackStateMachine e = playing();
        CHECK(e.update(sample(1.5, 100.0)));
        CHECK(e.update(sample(30.0, 100.1, /*more=*/false))); // nothing left to wait for
        CHECK(e.state() == PlaybackState::Playing);
    }

    // Actual end of stream is not starvation: with nothing more to arrive a low
    // buffer is just the tail of the video, so BUFFERING is never entered...
    {
        PlaybackStateMachine m = playing();
        for (double p : {1.9, 0.5, 0.0}) CHECK(!m.update(sample(p, 1.0, /*more=*/false)));
        CHECK(m.state() == PlaybackState::Playing);
    }
    // ...and if the stream ends while buffering (the last data arrived, or the
    // connection ended), playback resumes at once to play out what is left
    // instead of waiting for a recovery level that can no longer be reached.
    {
        PlaybackStateMachine m = playing();
        CHECK(m.update(sample(1.5, 10.0)));
        CHECK(!m.update(sample(4.0, 11.0, true)));
        CHECK(m.update(sample(4.0, 12.0, /*more=*/false)));
        CHECK(m.state() == PlaybackState::Playing);
    }

    // The read-ahead at its byte cap can't grow, so waiting would hang.
    {
        PlaybackStateMachine m = playing();
        CHECK(!m.update(sample(1.0, 1.0, true, /*full=*/true))); // not entered
        CHECK(m.update(sample(1.0, 2.0)));                        // entered normally
        CHECK(m.update(sample(4.0, 3.0, true, /*full=*/true)));   // cap hit -> resume
        CHECK(m.state() == PlaybackState::Playing);
    }

    // User pause: nothing is consumed, so nothing starves -- never enter. A
    // buffering wait already under way may still finish while paused.
    {
        PlaybackStateMachine m = playing();
        CHECK(!m.update(sample(0.5, 1.0, true, false, /*consuming=*/false)));
        CHECK(m.state() == PlaybackState::Playing);
        CHECK(m.update(sample(0.5, 2.0))); // unpaused: now it enters
        CHECK(!m.update(sample(5.0, 3.0, true, false, /*consuming=*/false)));
        CHECK(m.update(sample(13.0, 4.0, true, false, /*consuming=*/false)));
        CHECK(m.state() == PlaybackState::Playing);
    }

    // An unmeasurable timeline is never guessed at: no entry on NAN, and a
    // wait already under way ends rather than hanging on a value that is gone.
    {
        PlaybackStateMachine m = playing();
        CHECK(!m.update(sample(NAN, 1.0)));
        CHECK(m.state() == PlaybackState::Playing);
        CHECK(m.update(sample(1.0, 2.0)));
        CHECK(m.update(sample(NAN, 3.0)));
        CHECK(m.state() == PlaybackState::Playing);
    }

    // Seek: Player::play() ends and a new play() builds a new machine, so old
    // buffering state cannot leak into the new position. A machine that was
    // mid-BUFFERING and its fresh replacement:
    {
        PlaybackStateMachine old = playing();
        CHECK(old.update(sample(1.0, 5.0)));
        CHECK(old.state() == PlaybackState::Buffering);

        PlaybackStateMachine fresh; // what play() constructs after the seek
        CHECK(fresh.state() == PlaybackState::StartupBuffering);
        CHECK(!fresh.update(sample(0.0, 6.0))); // new stream, empty buffer: prebuffer, not BUFFERING
        CHECK(fresh.startupComplete());
        CHECK(fresh.state() == PlaybackState::Playing);
        CHECK(!fresh.update(sample(15.0, 30.0)));
        CHECK(fresh.state() == PlaybackState::Playing);
    }

    // Threshold sanitizing keeps the gap that makes it hysteresis.
    {
        RebufferThresholds t;
        t.lowWaterSeconds = 5.0;
        t.recoverySeconds = 3.0; // nonsense: below the entry point
        RebufferThresholds s = PlaybackStateMachine::sanitized(t);
        CHECK(s.recoverySeconds >= s.lowWaterSeconds + 1.0);
        t.lowWaterSeconds = -4.0;
        t.recoverySeconds = NAN;
        t.minBufferingSeconds = -1.0;
        s = PlaybackStateMachine::sanitized(t);
        CHECK_NEAR(s.lowWaterSeconds, 0.0, 1e-12);
        CHECK(s.recoverySeconds > s.lowWaterSeconds);
        CHECK_NEAR(s.minBufferingSeconds, 0.0, 1e-12);
    }

    CHECK_STR(playbackStateName(PlaybackState::StartupBuffering), "STARTUP_BUFFERING");
    CHECK_STR(playbackStateName(PlaybackState::Playing), "PLAYING");
    CHECK_STR(playbackStateName(PlaybackState::Buffering), "BUFFERING");

    return check::finish("test_playback_state");
}
