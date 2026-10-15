// End-to-end test for subtitles burned in at a start offset (seeking, changing
// the subtitle track mid-episode, resuming): Ufin's real JellyfinClient URL
// builder and real Player against a fake Jellyfin that transcodes with ffmpeg
// the way Jellyfin's progressive burn-in does.
//
// What Jellyfin does (EncodingHelper.cs, GetTextSubtitlesFilter and
// GetAudioFilterParam): for a text subtitle burned in at StartTimeTicks > 0, and
// unless the request carries CopyTimestamps=true, it appends
// "setpts=PTS-N/TB" to the video chain and "asetpts=PTS-N/TB" to the audio
// chain (N = the rounded offset in seconds) on top of an input ffmpeg has
// already moved to 0. The audio then starts at -N s while the video frames that
// land before 0 are dropped. With CopyTimestamps=true Jellyfin skips both
// filters and uses -copyts instead. The subtitles filter itself is left out
// here (it needs libass and fonts); it does not touch timestamps.
//
// Expected, and asserted:
//   * at the packet level (independent of the FFmpeg version): the model server's
//     answer to the request Ufin used to send has the audio starting at about -N s
//     and the video at 0, while the answer to the request Ufin sends now has both
//     starting at about N s. This is what makes a player that follows the audio
//     clock hold the first picture for N seconds on the Wii U's FFmpeg. (Newer
//     libavcodec, such as the 6.1 these host tests may run on, trims audio frames
//     with negative timestamps, so Player itself only shows the freeze on the
//     console's FFmpeg; that is why the legacy case is not played here.)
//   * the request Ufin builds for a text subtitle at an offset carries
//     CopyTimestamps=true, and playing it shows video promptly with the position
//     starting at the offset;
//   * offset 0 and no-subtitle requests are unchanged.
#include "check.h"
#include "fake_http_server.h"
#include "jellyfin_client.h"
#include "media/player.h"
#include "seek.h"
#include "video_output.h" // tests/host/fake_media

#include <SDL2/SDL.h>
#include <chrono>
#include <cstdio>
#include <string>

static std::string g_source;

static long long queryTicks(const std::string& path) {
    size_t k = path.find("StartTimeTicks=");
    return k == std::string::npos ? 0 : atoll(path.c_str() + k + 15);
}

// The fake server's transcode: Jellyfin's argument shape for a burned-in text
// subtitle, with or without CopyTimestamps.
static std::string transcode(long long startTicks, bool copyTimestamps) {
    const double seconds = startTicks / 10000000.0;
    const long long rounded = (long long)(seconds + 0.5);
    std::string timing;
    if (copyTimestamps) {
        timing = " -copyts -avoid_negative_ts disabled -start_at_zero";
    } else {
        timing = " -vf 'setpts=PTS-" + std::to_string(rounded) + "/TB' -af 'asetpts=PTS-" + std::to_string(rounded) + "/TB'";
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -ss %.3f -i '%s' -map_metadata -1 -codec:v:0 libx264 -preset ultrafast "
             "-force_key_frames 'expr:gte(t,n_forced*5)' -profile:v baseline -pix_fmt yuv420p "
             "-codec:a:0 aac -ac 2%s -f mp4 -movflags frag_keyframe+empty_moov+delay_moov pipe:1",
             seconds, g_source.c_str(), timing.c_str());
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

extern "C" {
#include <libavformat/avformat.h>
}
#include <cstdlib>
#include <unistd.h>

// First video and audio packet timestamps (seconds) of an MP4 held in memory.
static bool firstPacketTimes(const std::string& bytes, double& video, double& audio) {
    char name[] = "/tmp/ufin_burnin_XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    AVFormatContext* f = nullptr;
    bool ok = avformat_open_input(&f, name, nullptr, nullptr) >= 0;
    video = audio = NAN;
    if (ok) {
        AVPacket* pkt = av_packet_alloc();
        while ((std::isnan(video) || std::isnan(audio)) && av_read_frame(f, pkt) >= 0) {
            AVStream* st = f->streams[pkt->stream_index];
            int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            double t = ts == AV_NOPTS_VALUE ? NAN : ts * av_q2d(st->time_base);
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && std::isnan(video)) video = t;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && std::isnan(audio)) audio = t;
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avformat_close_input(&f);
    }
    unlink(name);
    return ok && !std::isnan(video) && !std::isnan(audio);
}

struct Outcome {
    int frames = 0;
    double position = 0.0;
};

// Plays `path` until `seconds` of wall time after its first picture (or a safety limit), then stops.
static Outcome playFor(FakeHttpServer& server, const std::string& path, double startAt, double seconds) {
    Player player;
    PlayOptions options;
    options.displayAspect = 16.0 / 9.0;
    options.startOffsetSeconds = startAt;
    const int before = VideoOutput::framesShown;
    auto t0 = std::chrono::steady_clock::now();
    auto firstFrameAt = t0;
    bool seenFrame = false;
    auto poll = [&]() -> PlayerCommand {
        auto now = std::chrono::steady_clock::now();
        if (!seenFrame && VideoOutput::framesShown > before) { seenFrame = true; firstFrameAt = now; }
        if (std::chrono::duration<double>(now - t0).count() > 60.0) return PlayerCommand::stop(); // safety net
        if (seenFrame && std::chrono::duration<double>(now - firstFrameAt).count() >= seconds) return PlayerCommand::stop();
        return PlayerCommand::none();
    };
    PlayResult r = player.play("127.0.0.1", server.port(), path, poll, nullptr, options);
    CHECK(r == PlayResult::Stopped);
    Outcome o;
    o.frames = VideoOutput::framesShown - before;
    o.position = player.positionSeconds();
    return o;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_player_burnin <60s-source.mp4>\n");
        return 2;
    }
    g_source = argv[1];
    fake_sdl_set_realtime(true);

    FakeHttpServer server;
    CHECK(server.start());
    server.setHandler([&](const FakeRequest& req, FakeRoute& out) {
        if (req.path.rfind("/Videos/v1/skew", 0) == 0) {
            // Video timeline 8 s ahead of the audio's: the shape of the reported freeze.
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -v error -i '%s' -map 0:v:0 -map 0:a:0 -vf 'setpts=PTS+8/TB' -fps_mode passthrough "
                     "-codec:v libx264 -preset ultrafast -profile:v baseline -pix_fmt yuv420p -codec:a aac -ac 2 -t 20 "
                     "-f mp4 -movflags frag_keyframe+empty_moov+delay_moov pipe:1", g_source.c_str());
            std::string body;
            FILE* p = popen(cmd, "r");
            char buf[65536];
            size_t n;
            while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0) body.append(buf, n);
            if (p) pclose(p);
            out.contentType = "video/mp4";
            out.body = body;
            out.chunked = true;
            out.chunkSize = 16000;
            return !body.empty();
        }
        if (req.path.rfind("/Videos/v1/stream.mp4", 0) != 0) return false;
        const bool copyTs = req.path.find("CopyTimestamps=true") != std::string::npos;
        std::string body = transcode(queryTicks(req.path), copyTs);
        out.contentType = "video/mp4";
        out.body = body;
        out.chunked = true;
        out.chunkSize = 16000;
        return !body.empty();
    });

    JellyfinClient client("127.0.0.1", server.port());
    const double startAt = 20.0;

    VideoStreamOptions o;
    o.startTimeTicks = (long long)(startAt * 10000000.0);
    o.subtitleStreamIndex = 2;

    // --- Before the fix: a text subtitle at an offset, no CopyTimestamps. ---
    o.subtitleIsText = false; // the request Ufin used to send for every track
    std::string legacy = client.buildVideoStreamUrl("v1", o).path;
    CHECK(legacy.find("CopyTimestamps") == std::string::npos);
    {
        double v, a;
        CHECK(firstPacketTimes(transcode(queryTicks(legacy), false), v, a));
        printf("  legacy stream  : first video %.2f s, first audio %.2f s\n", v, a);
        CHECK(v > -1.0 && v < 1.0);              // video starts at ~0...
        CHECK(a < startAt * -0.75);              // ...audio at about -N: 20 s apart
    }

    // --- With the fix: same track, request carries CopyTimestamps=true. ---
    o.subtitleIsText = true;
    std::string fixed = client.buildVideoStreamUrl("v1", o).path;
    CHECK(fixed.find("&CopyTimestamps=true") != std::string::npos);
    {
        double v, a;
        CHECK(firstPacketTimes(transcode(queryTicks(fixed), true), v, a));
        printf("  fixed stream   : first video %.2f s, first audio %.2f s\n", v, a);
        CHECK(v > startAt - 1.0 && v < startAt + 1.0);   // both at ~N, together
        CHECK(a > startAt - 1.0 && a < startAt + 1.0);
        CHECK(std::fabs(v - a) < 1.0);
    }
    Outcome good = playFor(server, fixed, startAt, 5.0);
    printf("  fixed request  : %d frames in 5 s, position %.1f s\n", good.frames, good.position);
    CHECK(good.frames > 80);                               // the picture really plays
    CHECK(good.position > startAt + 2.0 && good.position < startAt + 8.0); // from the offset, not from 0:00

    // --- Offset 0 (subtitle chosen before playback starts): untouched request, plays fine. ---
    o.startTimeTicks = 0;
    std::string atZero = client.buildVideoStreamUrl("v1", o).path;
    CHECK(atZero.find("CopyTimestamps") == std::string::npos);
    Outcome zero = playFor(server, atZero, 0.0, 4.0);
    printf("  offset 0       : %d frames in 4 s\n", zero.frames);
    CHECK(zero.frames > 60);

    // --- The reported symptom itself: video timeline ahead of the audio's. The
    // picture is held on its first frame while the audio plays (and the log says
    // so: "holding the picture ..."), then resumes when the clock reaches it.
    {
        Outcome held = playFor(server, "/Videos/v1/skew", 0.0, 5.0);
        printf("  skewed stream  : %d frames in the first 5 s (video starts 8 s after audio)\n", held.frames);
        CHECK(held.frames < 15);
    }

    server.stop();
    return check::finish("test_player_burnin");
}
