// End-to-end skip test: Ufin's real Player (decoder, audio clock,
// position tracking, seek handling) against a fake Jellyfin that, like
// the real one, answers every request by transcoding with
// `ffmpeg -ss <StartTimeTicks>` into a fragmented MP4. Drives Player the
// way playItem() does: play, skip +30 s, check where playback resumed,
// skip -10 s, check again.
#include "check.h"
#include "fake_http_server.h"
#include "media/player.h"
#include "seek.h"
#include "video_output.h" // tests/host/fake_media

#include <SDL2/SDL.h>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>

static std::string g_source;

// ffmpeg -ss T -i source ... -> fragmented MP4 bytes, like Jellyfin's
// progressive transcode (see EncodingHelper.GetProgressiveVideoFullCommandLine).
static std::string transcodeFrom(double seconds) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error %s -i '%s' -map_metadata -1 -codec:v:0 libx264 -preset ultrafast "
             "-force_key_frames 'expr:gte(t,n_forced*5)' -profile:v baseline -pix_fmt yuv420p "
             "-codec:a:0 aac -ac 2 -f mp4 -movflags frag_keyframe+empty_moov+delay_moov pipe:1",
             seconds > 0 ? ("-ss " + std::to_string(seconds)).c_str() : "", g_source.c_str());
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

static long long queryTicks(const std::string& path) {
    size_t k = path.find("StartTimeTicks=");
    return k == std::string::npos ? 0 : atoll(path.c_str() + k + 15);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_player_seek <60s-source.mp4>\n");
        return 2;
    }
    g_source = argv[1];
    fake_sdl_set_realtime(true);

    FakeHttpServer server;
    CHECK(server.start());
    std::mutex cacheMutex;
    std::map<long long, std::string> cache;
    server.setHandler([&](const FakeRequest& req, FakeRoute& out) {
        if (req.path.rfind("/Videos/v1/stream.mp4", 0) != 0) return false;
        long long ticks = queryTicks(req.path);
        std::string body;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = cache.find(ticks);
            if (it != cache.end()) body = it->second;
        }
        if (body.empty()) {
            body = transcodeFrom(ticks / 10000000.0);
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache[ticks] = body;
        }
        out.contentType = "video/mp4";
        out.body = body;
        out.chunked = true;
        out.chunkSize = 16000;
        return !body.empty();
    });

    const double duration = 60.0;
    Player player;
    int phase = 0;
    double startAt = 0.0;
    double resumedAt1 = -1, resumedAt2 = -1;
    int framesAtRestart = 0, framesAfter1 = 0;
    auto wallStart = std::chrono::steady_clock::now();

    // The GamePad, scripted: skip +30 s at 3 s in, then -10 s once the
    // new stream has played 2 s, then stop 2 s after that.
    auto poll = [&]() -> PlayerCommand {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
        if (elapsed > 90.0) return PlayerCommand::stop(); // safety net
        double pos = player.positionSeconds();
        if (phase == 0 && pos >= 3.0) {
            phase = 1;
            return PlayerCommand::seekTo(clampSeek(pos, 30, duration));
        }
        if (phase == 1 && pos >= startAt + 2.0) {
            resumedAt1 = pos;
            framesAfter1 = VideoOutput::framesShown - framesAtRestart;
            phase = 2;
            return PlayerCommand::seekTo(clampSeek(pos, -10, duration));
        }
        if (phase == 2 && pos >= startAt + 2.0) {
            resumedAt2 = pos;
            phase = 3;
            return PlayerCommand::stop();
        }
        return PlayerCommand::none();
    };

    PlayOptions options;
    options.displayAspect = 16.0 / 9.0;
    std::vector<double> starts;
    uint32_t session = 0;
    for (int round = 0; round < 5; round++) {
        std::string path = "/Videos/v1/stream.mp4?static=false&PlaySessionId=" +
                           makePlaySessionId(1234, ++session);
        if (startAt > 0) path += "&StartTimeTicks=" + std::to_string((long long)(startAt * 10000000.0));
        options.startOffsetSeconds = startAt;
        starts.push_back(startAt);
        framesAtRestart = VideoOutput::framesShown;
        PlayResult r = player.play("127.0.0.1", server.port(), path, poll, {}, options);
        if (r != PlayResult::SeekRequested) {
            CHECK(r == PlayResult::Stopped);
            break;
        }
        startAt = player.seekTarget();
    }

    printf("  stream starts: ");
    for (double s : starts) printf("%.1f ", s);
    printf("\n  resumed at %.1f s after +30, at %.1f s after -10\n", resumedAt1, resumedAt2);

    CHECK_EQ(phase, 3);
    CHECK_EQ((int)starts.size(), 3);
    // +30 from ~3 s: the second stream was asked to start around 33 s...
    CHECK(starts.size() > 1 && starts[1] > 31.0 && starts[1] < 36.0);
    // ...and playback really continued from there, not from 0:00.
    CHECK(resumedAt1 >= starts[1] + 1.5 && resumedAt1 < starts[1] + 4.0);
    CHECK(framesAfter1 > 20);
    // -10 from there lands ~25 s, and plays on from that point.
    CHECK(starts.size() > 2 && starts[2] > starts[1] - 10.0 - 0.5 && starts[2] < starts[1] - 10.0 + 4.5);
    CHECK(resumedAt2 >= starts[2] + 1.5 && resumedAt2 < starts[2] + 4.0);

    // Every request carried its own session; seeks carried StartTimeTicks.
    auto reqs = server.requests();
    std::vector<std::string> videoReqs;
    for (auto& r : reqs) if (r.path.rfind("/Videos/v1/", 0) == 0) videoReqs.push_back(r.path);
    CHECK_EQ((int)videoReqs.size(), 3);
    if (videoReqs.size() == 3) {
        CHECK(videoReqs[0].find("StartTimeTicks") == std::string::npos);
        CHECK(queryTicks(videoReqs[1]) > 300000000LL);
        CHECK(queryTicks(videoReqs[2]) > 200000000LL);
        CHECK(videoReqs[0].substr(videoReqs[0].find("PlaySessionId")) !=
              videoReqs[1].substr(videoReqs[1].find("PlaySessionId")));
    }

    server.stop();
    return check::finish("test_player_seek");
}
