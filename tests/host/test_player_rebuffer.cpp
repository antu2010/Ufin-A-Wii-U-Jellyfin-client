// Graceful rebuffering, end to end: the real Player (decode thread, audio
// clock, frame queue, state machine) against a fake Jellyfin whose bandwidth
// is throttled below the stream's bitrate after an initial burst -- the
// situation that used to end in a visible stutter once the read-ahead ran dry.
//
// Checked, per scenario:
//   1. throttled playback: STARTUP -> PLAYING -> BUFFERING (while ~2 s of
//      media are still left, not zero) -> PLAYING; no frame is shown and the
//      audio device is paused while BUFFERING; no stutter outside BUFFERING;
//      audio and video stay in sync afterwards; the stream is never re-requested
//   2. seek requested while BUFFERING: play() ends with SeekRequested, the next
//      play() starts fresh (startup prebuffer) at the new position
//   3. stop requested while BUFFERING: prompt, clean Stopped
//   4. genuine end of stream while BUFFERING: leaves BUFFERING, plays out the
//      rest, Completed -- a lack of packets is not mistaken for EOF, nor
//      real EOF for a lack of packets
#include "check.h"
#include "fake_http_server.h"
#include "media/player.h"
#include "seek.h"
#include "video_output.h" // tests/host/fake_media

#include <SDL2/SDL.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

using Clock = std::chrono::steady_clock;
static Clock::time_point g_t0 = Clock::now();
static double wall() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

struct Clip {
    std::string body;
    double seconds = 0.0;
    double bytesPerSecond() const { return seconds > 0 ? body.size() / seconds : 0.0; }
};

// A Jellyfin-style fragmented MP4 (2 s fragments), 1/90000 video time base so
// the test can turn frame timestamps into seconds.
static Clip makeClip(double seconds) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -f lavfi -i testsrc2=size=320x240:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 "
             "-t %.1f -c:v libx264 -preset ultrafast -profile:v baseline -pix_fmt yuv420p -g 60 "
             "-force_key_frames 'expr:gte(t,n_forced*2)' -video_track_timescale 90000 -c:a aac -b:a 128k -ac 2 "
             "-f mp4 -movflags frag_keyframe+empty_moov+delay_moov pipe:1",
             seconds);
    Clip c;
    c.seconds = seconds;
    FILE* p = popen(cmd, "r");
    if (!p) return c;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) c.body.append(buf, n);
    pclose(p);
    return c;
}

static long long queryTicks(const std::string& path) {
    size_t k = path.find("StartTimeTicks=");
    return k == std::string::npos ? 0 : atoll(path.c_str() + k + 15);
}

// What the fake server does to the first request (StartTimeTicks == 0);
// requests after a seek are served unthrottled.
struct Serve {
    const Clip* clip = nullptr;
    double burstSeconds = 0.0;   // this much media at full speed...
    double rateFraction = 1.0;   // ...then this fraction of real time
};
static Serve g_serve;

struct Event { PlaybackState from, to; double playable; double wall; };
struct FrameRec { double wall; double ptsRel; double pos; };

struct Run {
    PlayResult result = PlayResult::Error;
    std::vector<Event> events;
    std::vector<FrameRec> frames;
    double endedAt = 0.0;
};

static Player* g_player = nullptr;
static double g_firstPts = NAN;
static Run* g_run = nullptr;

static Run runPlayer(int port, double startAt, PlayOptions options,
                     const std::function<PlayerCommand(const Run&, double)>& policy) {
    Run run;
    g_run = &run;
    g_firstPts = NAN;
    VideoOutput::onFrame = [&](AVFrame* f) {
        int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        double pts = ts == AV_NOPTS_VALUE ? NAN : ts / 90000.0;
        if (std::isnan(g_firstPts)) g_firstPts = pts;
        run.frames.push_back({wall(), pts - g_firstPts, g_player->positionSeconds()});
    };
    options.onPlaybackState = [&](PlaybackState from, PlaybackState to, double playable) {
        run.events.push_back({from, to, playable, wall()});
    };
    std::string path = "/Videos/v1/stream.mp4?static=false&PlaySessionId=" + makePlaySessionId(1234, 1);
    if (startAt > 0) path += "&StartTimeTicks=" + std::to_string((long long)(startAt * 10000000.0));
    options.startOffsetSeconds = startAt;
    options.displayAspect = 16.0 / 9.0;
    auto poll = [&]() -> PlayerCommand {
        if (wall() > 200.0) return PlayerCommand::stop(); // safety net
        return policy(run, wall());
    };
    run.result = g_player->play("127.0.0.1", port, path, poll, nullptr, options);
    run.endedAt = wall();
    VideoOutput::onFrame = nullptr;
    return run;
}

// Wall-clock intervals spent in BUFFERING.
static std::vector<std::pair<double, double>> bufferingIntervals(const Run& r) {
    std::vector<std::pair<double, double>> out;
    for (size_t i = 0; i < r.events.size(); i++) {
        if (r.events[i].to != PlaybackState::Buffering) continue;
        double end = r.endedAt;
        for (size_t j = i + 1; j < r.events.size(); j++) {
            if (r.events[j].from == PlaybackState::Buffering) { end = r.events[j].wall; break; }
        }
        out.push_back({r.events[i].wall, end});
    }
    return out;
}

static bool sawBuffering(const Run& r) {
    for (auto& e : r.events) if (e.to == PlaybackState::Buffering) return true;
    return false;
}

int main() {
    fake_sdl_set_realtime(true);

    printf("  generating clips...\n");
    Clip clip40 = makeClip(40.0);
    Clip clip12 = makeClip(12.0);
    CHECK(clip40.body.size() > 100000 && clip12.body.size() > 30000);
    if (clip40.body.empty() || clip12.body.empty()) return check::finish("test_player_rebuffer");

    FakeHttpServer server;
    CHECK(server.start());
    server.setHandler([&](const FakeRequest& req, FakeRoute& out) {
        if (req.path.rfind("/Videos/v1/stream.mp4", 0) != 0 || !g_serve.clip) return false;
        out.contentType = "video/mp4";
        out.body = g_serve.clip->body;
        out.chunked = true;
        out.chunkSize = 4000;
        if (queryTicks(req.path) == 0) {
            const double r = g_serve.clip->bytesPerSecond();
            out.burstBytes = (size_t)(g_serve.burstSeconds * r);
            out.bytesPerSecond = g_serve.rateFraction * r;
        }
        return true;
    });

    Player player;
    g_player = &player;

    // Small thresholds so the test is quick; same mechanism and ratios as the
    // shipping 2 s / 12 s defaults (see test_playback_state for those).
    PlayOptions opt;
    opt.prebufferSeconds = 4.0;
    opt.rebufferLowWaterSeconds = 2.0;
    opt.rebufferRecoverySeconds = 6.0;

    // ---- 1. throttled playback: clean pause, no stutter, still in sync ----
    {
        printf("  [1] throttled stream\n");
        g_serve = {&clip40, 6.0, 0.4};
        double leftAt = -1.0;
        int bufferingSamples = 0, pausedSamples = 0, playingSamples = 0, runningSamples = 0;
        auto run = runPlayer(server.port(), 0.0, opt, [&](const Run& r, double now) -> PlayerCommand {
            for (auto& e : r.events) if (e.from == PlaybackState::Buffering) leftAt = e.wall;
            // Sample the audio device the way the speaker would see it.
            if (!r.events.empty() && now > r.events.back().wall + 0.3) {
                if (g_player->isBuffering()) { bufferingSamples++; if (fake_sdl_device_paused()) pausedSamples++; }
                else if (r.events.back().to == PlaybackState::Playing) { playingSamples++; if (!fake_sdl_device_paused()) runningSamples++; }
            }
            if (leftAt > 0 && now > leftAt + 4.0) return PlayerCommand::stop();
            return PlayerCommand::none();
        });
        CHECK(run.result == PlayResult::Stopped);
        CHECK(!player.isBuffering()); // cleared when play() ends

        CHECK(run.events.size() >= 3);
        if (run.events.size() >= 3) {
            CHECK(run.events[0].from == PlaybackState::StartupBuffering && run.events[0].to == PlaybackState::Playing);
            CHECK(run.events[1].from == PlaybackState::Playing && run.events[1].to == PlaybackState::Buffering);
            CHECK(run.events[2].from == PlaybackState::Buffering && run.events[2].to == PlaybackState::Playing);
            // Entered with playable media still left -- ~2 s, never ~0.
            printf("      entered BUFFERING with %.2f s playable, resumed at %.2f s, held %.1f s\n",
                   run.events[1].playable, run.events[2].playable, run.events[2].wall - run.events[1].wall);
            CHECK(run.events[1].playable >= 1.0 && run.events[1].playable <= 2.6);
            CHECK(run.events[2].playable >= 6.0);
            CHECK(run.events[2].wall - run.events[1].wall >= 0.5);
        }
        // Audio is held for the whole wait and running again afterwards.
        CHECK(bufferingSamples > 20);
        CHECK_EQ(pausedSamples, bufferingSamples);
        CHECK(playingSamples > 20);
        CHECK_EQ(runningSamples, playingSamples);

        // No frame is shown while BUFFERING; no gap > 0.35 s anywhere else.
        auto ivs = bufferingIntervals(run);
        int during = 0, stutters = 0;
        double worstUnexplained = 0.0;
        for (auto& f : run.frames)
            for (auto& iv : ivs)
                if (f.wall > iv.first + 0.05 && f.wall < iv.second - 0.05) during++;
        for (size_t i = 1; i < run.frames.size(); i++) {
            double a = run.frames[i - 1].wall, b = run.frames[i].wall;
            bool explained = false;
            for (auto& iv : ivs) if (a <= iv.second && b >= iv.first) explained = true;
            if (!explained && b - a > 0.35) { stutters++; worstUnexplained = std::max(worstUnexplained, b - a); }
        }
        printf("      frames=%zu shownDuringBuffering=%d unexplainedStutters=%d (worst %.2fs)\n",
               run.frames.size(), during, stutters, worstUnexplained);
        CHECK(run.frames.size() > 100);
        CHECK_EQ(during, 0);
        CHECK_EQ(stutters, 0);

        // A/V: the audio-clock position and the picture's own timestamp agree
        // for every frame, including everything after the pause.
        double worstSkew = 0.0;
        bool monotonic = true;
        for (size_t i = 0; i < run.frames.size(); i++) {
            worstSkew = std::max(worstSkew, std::fabs(run.frames[i].ptsRel - run.frames[i].pos));
            if (i && !(run.frames[i].ptsRel > run.frames[i - 1].ptsRel)) monotonic = false;
        }
        printf("      worst |video pts - audio-clock position| = %.3fs\n", worstSkew);
        CHECK(worstSkew < 0.30);
        CHECK(monotonic);

        // One stream request the whole time: no restart, no reconnect, no seek.
        int videoReqs = 0;
        for (auto& r : server.requests()) if (r.path.rfind("/Videos/v1/", 0) == 0) videoReqs++;
        CHECK_EQ(videoReqs, 1);
    }

    // ---- 2. seek while BUFFERING ----
    {
        printf("  [2] seek during BUFFERING\n");
        g_serve = {&clip40, 5.0, 0.3};
        bool seekIssuedWhileBuffering = false;
        auto run = runPlayer(server.port(), 0.0, opt, [&](const Run& r, double now) -> PlayerCommand {
            if (g_player->isBuffering() && !r.events.empty() && now > r.events.back().wall + 0.4) {
                seekIssuedWhileBuffering = true;
                return PlayerCommand::seekTo(20.0);
            }
            return PlayerCommand::none();
        });
        CHECK(run.result == PlayResult::SeekRequested);
        CHECK(seekIssuedWhileBuffering);
        CHECK_NEAR(player.seekTarget(), 20.0, 1e-9);
        CHECK(!player.isBuffering());

        // The restart begins from scratch (startup prebuffer, not BUFFERING),
        // at the new position, and plays.
        auto again = runPlayer(server.port(), player.seekTarget(), opt, [&](const Run& r, double now) -> PlayerCommand {
            return (!r.frames.empty() && r.frames.back().pos >= 22.5) ? PlayerCommand::stop() : PlayerCommand::none();
        });
        CHECK(again.result == PlayResult::Stopped);
        CHECK(again.events.size() >= 1);
        if (!again.events.empty()) {
            CHECK(again.events[0].from == PlaybackState::StartupBuffering && again.events[0].to == PlaybackState::Playing);
            CHECK(!sawBuffering(again)); // fresh, unthrottled stream: no stale BUFFERING carried over
        }
        double minPos = 1e9;
        for (auto& f : again.frames) minPos = std::min(minPos, f.pos);
        CHECK(!again.frames.empty() && minPos >= 19.9); // nothing from the old position
        int videoReqs = 0;
        long long lastTicks = 0;
        for (auto& r : server.requests())
            if (r.path.rfind("/Videos/v1/", 0) == 0) { videoReqs++; lastTicks = queryTicks(r.path); }
        CHECK_EQ(videoReqs, 3); // scenario 1's, plus the throttled one and the seek restart here
        CHECK_EQ(lastTicks, 200000000LL);
    }

    // ---- 3. stop while BUFFERING ----
    {
        printf("  [3] stop during BUFFERING\n");
        g_serve = {&clip40, 5.0, 0.3};
        double stopAt = -1.0;
        auto run = runPlayer(server.port(), 0.0, opt, [&](const Run& r, double now) -> PlayerCommand {
            if (g_player->isBuffering() && now > r.events.back().wall + 0.4) {
                if (stopAt < 0) stopAt = now;
                return PlayerCommand::stop();
            }
            return PlayerCommand::none();
        });
        CHECK(run.result == PlayResult::Stopped);
        CHECK(stopAt > 0);
        CHECK(run.endedAt - stopAt < 3.0); // returned promptly
        CHECK(!player.isBuffering());
    }

    // ---- 4. genuine end of stream while BUFFERING ----
    {
        printf("  [4] end of stream during BUFFERING\n");
        // The burst covers the prebuffer, the slow tail (0.3x) starves playback
        // after ~3 s, and the stream only ends ~20 s in.
        g_serve = {&clip12, 6.0, 0.3};
        PlayOptions o = opt;
        o.prebufferSeconds = 3.0;
        o.rebufferRecoverySeconds = 60.0; // unreachable: only real EOF can end the wait
        auto run = runPlayer(server.port(), 0.0, o, [&](const Run&, double) { return PlayerCommand::none(); });
        CHECK(run.result == PlayResult::Completed);
        bool entered = false, leftForEof = false;
        for (auto& e : run.events) {
            if (e.to == PlaybackState::Buffering) entered = true;
            if (e.from == PlaybackState::Buffering && e.to == PlaybackState::Playing) leftForEof = e.playable < 60.0;
        }
        CHECK(entered);
        CHECK(leftForEof);
        printf("      frames shown=%zu of ~360, completed after %.1fs\n", run.frames.size(), run.endedAt);
        CHECK(run.frames.size() >= 300); // the tail was played out, not dropped or hung on
        CHECK(!player.isBuffering());
    }

    server.stop();
    return check::finish("test_player_rebuffer");
}
