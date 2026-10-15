#include "player.h"
#include "../seek.h"
#include "http_stream_io.h"
#include "decoder.h"
#include "video_output.h"
#include "audio_output.h"
#include "frame_queue.h"
#include "frame_reorder.h"
#include "h264_geometry.h"

#include <SDL2/SDL.h>
#include <coreinit/debug.h>
#include "../ufin_log.h" // OSReport -> also the SD-card log file (see ufin_log.h)
#include <thread>
#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <chrono>

// A/V sync tuning. A video frame is shown when the master clock reaches
// its timestamp; if it's already this far behind the clock and more
// frames are waiting, it's dropped instead of shown late. 200 ms is
// deliberately lenient -- the Wii U CPU has little headroom, so the
// priority is smooth-enough motion over strict sync.
static const double LATE_FRAME_DROP_SECONDS = 0.20;
// How far the decode thread may run ahead of audio playback before it
// pauses. Bounds memory and, since the container interleaves audio and
// video, indirectly bounds how far ahead video decoding gets too.
// Raised from 1.5s: decoded audio is cheap (~192 KB/s for 48kHz stereo
// PCM), and 1.5s left almost no cushion against a real Wi-Fi hiccup
// before playback audibly stalled.
static const double MAX_AUDIO_AHEAD_SECONDS = 4.0;

// How long to wait, buffering, before the very first frame is shown (and
// before audio starts). Measured in stream time sitting in Decoder's
// compressed packet queues (see queuedVideoSeconds/queuedAudioSeconds) --
// NOT decoded frames, which are far too large to hold minutes of (see
// FrameQueue::MAX_SIZE). This is what actually protects against a Wi-Fi
// hiccup: once playback starts, there's this many seconds of
// already-downloaded (if not yet decoded) data to draw from before the
// decode thread has to touch the network again.
static const double PREBUFFER_TARGET_SECONDS = 15.0;

// Safety valve: if PREBUFFER_TARGET_SECONDS hasn't been reached after this
// long, start anyway with whatever's buffered. Covers two cases: a
// connection so slow it will never sustain real-time playback anyway (no
// point waiting forever when the user can already tell something's
// wrong), and a stream whose packets don't carry usable duration
// metadata, which would otherwise leave queuedVideoSeconds()/
// queuedAudioSeconds() stuck at 0 and the wait unable to ever end on its
// own. The user's own Stop button is the other way out, at any time.
static const double PREBUFFER_MAX_WAIT_SECONDS = 30.0;

// Once buffered-ahead reaches this much, stop reading further ahead of
// the current playback position -- no point holding more than this in
// memory. This is a *target*; MAX_BUFFERED_PACKET_BYTES below is the
// hard ceiling that actually gets hit first on anything but a low
// bitrate stream (see the comment there).
static const double MAX_BUFFER_AHEAD_SECONDS = 180.0;

// Cap on compressed packets read ahead of decoding (both streams
// combined). This is the real memory limit -- MAX_BUFFER_AHEAD_SECONDS
// above is a time-based target, but a high-bitrate stream will hit this
// byte ceiling well before 180 seconds of it fits. At 2.5 Mbit/s, 96 MB
// is a bit over 5 minutes of stream; at 8 Mbit/s it's under 100 seconds.
// This number trades Wii U MEM2 budget against how deep the buffer can
// get on higher-bitrate streams -- if this ever needs to come down
// because MEM2 is tight alongside everything else the app allocates
// (image cache, ImGui, audio buffers), the prebuffer/look-ahead targets
// above will simply be satisfied by whichever limit is hit first.
static const size_t MAX_BUFFERED_PACKET_BYTES = 96 * 1024 * 1024;

// Buffering profile for pictures larger than 720p (the experimental
// 1080p30 mode). Same mechanism as above, deeper: a 6 Mbps stream is only
// ~0.75 MB/s, so 45 s of prebuffer is ~34 MB, and the byte cap gets more
// room for the same reason. A real-hardware freeze pattern (video stops
// first, audio ~seconds later) is what a drained read-ahead looks like,
// because decoded video holds well under a second while audio holds up to
// MAX_AUDIO_AHEAD_SECONDS.
static const double BIG_PREBUFFER_TARGET_SECONDS = 45.0;
static const double BIG_PREBUFFER_MAX_WAIT_SECONDS = 90.0;
static const size_t BIG_MAX_BUFFERED_PACKET_BYTES = 160u * 1024u * 1024u;

// Graceful rebuffering (video only). Enter BUFFERING while this much playable
// media is still left -- early enough that audio and video can stop cleanly
// with data to spare, instead of running to zero and visibly stuttering while
// the network catches up. "Playable" is measured on the timeline (decoded
// video ahead of the clock + compressed video read-ahead, and the audio
// equivalent; the smaller of the two), never from packet or frame counts.
static const double REBUFFER_LOW_WATER_SECONDS = 2.0;
// Resume once this much is playable again. Deliberately far above the entry
// point (hysteresis) so a marginal connection does not flap in and out.
static const double REBUFFER_RECOVERY_SECONDS = 12.0;
// Redraw cadence for the buffering wheel while playback is held.
static const double BUFFERING_REDRAW_SECONDS = 1.0 / 30.0;

// Diagnostics only (nothing here changes what is shown or played). A video
// frame normally sits well under a second from the audio clock. When a picture
// has to be held for longer than this because its timestamp is that far ahead
// of the clock, the video and audio timelines of the stream disagree -- the
// picture then looks frozen while the sound plays on -- and the log says so,
// at most once per HOLD_WARN_INTERVAL_SECONDS. The same threshold flags a
// first frame that starts this far from the audio.
static const double TIMELINE_SKEW_WARN_SECONDS = 2.0;
static const double HOLD_WARN_INTERVAL_SECONDS = 5.0;

static double nowSeconds() {
    return SDL_GetTicks() / 1000.0;
}

PlayResult Player::play(const std::string& host, int port, const std::string& path,
                         const std::function<PlayerCommand()>& poll,
                         const std::function<void(double)>& onTick,
                         const PlayOptions& options) {
    last_position_ = options.startOffsetSeconds;
    paused_ = false;
    seek_target_ = 0.0;
    volume_ = options.initialVolume < 0.0 ? 0.0 : (options.initialVolume > 1.0 ? 1.0 : options.initialVolume);

    HttpStreamIO io(host, port, path);
    if (options.startupTimeoutSeconds > 0) {
        // A slow-starting stream (see PlayOptions::startupTimeoutSeconds): wait
        // longer, but keep B working while we do. Only Stop is acted on here;
        // any other command in this window is dropped, as there is nothing
        // playing yet for it to apply to.
        io.setStartupTimeoutSeconds(options.startupTimeoutSeconds);
        if (poll) io.setAbortCheck([&poll]() { return poll().kind == PlayerCommand::Kind::Stop; });
    }
    if (!io.open()) {
        if (io.cancelled()) {
            OSReport("Ufin: stream start cancelled by the user\n");
            return PlayResult::Stopped;
        }
        last_error_ = "HttpStreamIO::open failed: " + std::string(io.lastError());
        return PlayResult::Error;
    }

    Decoder decoder;
    decoder.setAllowUnalignedGeometry(options.allowUnalignedVideoGeometry);
    if (!decoder.open(io.avioContext())) {
        last_error_ = "Decoder::open failed: " + std::string(decoder.lastError());
        return PlayResult::Error;
    }

    const bool hasVideo = decoder.hasVideo();
    const bool hasAudio = decoder.hasAudio();

    if (!hasVideo && !hasAudio) {
        last_error_ = "stream has neither video nor audio we can decode";
        decoder.close();
        return PlayResult::Error;
    }

    OSReport("Ufin: player.cpp built %s %s (H.264 reorder + diagnostics)\n", __DATE__, __TIME__);
    OSReport("Ufin: Player -- video=%d (%s %dx%d) audio=%d (%d Hz, %d ch)\n", hasVideo,
             decoder.videoCodecName(), decoder.videoWidth(), decoder.videoHeight(), hasAudio,
             decoder.audioSampleRate(), decoder.audioChannels());

    // Frame bytes decide which buffering profile applies: 720p and below
    // keep the original constants exactly.
    const size_t frameBytes =
        hasVideo ? h264geom::tightNv12Bytes(decoder.videoWidth(), decoder.videoHeight()) : 0;
    const bool bigPicture = frameBytes > (size_t)1280 * 720 * 3 / 2;
    const double prebufferTargetSeconds = options.prebufferSeconds > 0.0
        ? options.prebufferSeconds
        : (bigPicture ? BIG_PREBUFFER_TARGET_SECONDS : PREBUFFER_TARGET_SECONDS);
    const double prebufferMaxWaitSeconds = bigPicture ? BIG_PREBUFFER_MAX_WAIT_SECONDS : PREBUFFER_MAX_WAIT_SECONDS;
    const size_t maxBufferedPacketBytes = options.maxBufferedPacketBytes
        ? options.maxBufferedPacketBytes
        : (bigPicture ? BIG_MAX_BUFFERED_PACKET_BYTES : MAX_BUFFERED_PACKET_BYTES);

    RebufferThresholds rebufferThresholds;
    rebufferThresholds.lowWaterSeconds =
        options.rebufferLowWaterSeconds > 0.0 ? options.rebufferLowWaterSeconds : REBUFFER_LOW_WATER_SECONDS;
    rebufferThresholds.recoverySeconds =
        options.rebufferRecoverySeconds > 0.0 ? options.rebufferRecoverySeconds : REBUFFER_RECOVERY_SECONDS;
    PlaybackStateMachine stateMachine(rebufferThresholds); // sanitizes the thresholds
    buffering_ = false;
    buffered_seconds_ = 0.0;
    buffering_target_seconds_ = stateMachine.thresholds().recoverySeconds;

    VideoOutput video;
    if (hasVideo) {
        if (options.presentVideo) video.setPresenter(options.presentVideo);
        if (!video.init(decoder.videoWidth(), decoder.videoHeight(), options.displayAspect)) {
            last_error_ = "VideoOutput::init failed: " + std::string(video.lastError());
            decoder.close();
            return PlayResult::Error;
        }
    }

    AudioOutput audio;
    OSReport("Ufin: DBG before audio.init\n");
    if (hasAudio) {
        if (!audio.init(decoder.audioSampleRate(), decoder.audioChannels(),
                         decoder.audioSampleFormat())) {
            last_error_ = "AudioOutput::init failed: " + std::string(audio.lastError());
            if (hasVideo) video.shutdown();
            decoder.close();
            return PlayResult::Error;
        }
        OSReport("Ufin: DBG audio.init ok\n");
        audio.setVolume((float)volume_);
        active_audio_ = &audio;
        // Playback starts once the prebuffer phase below is satisfied,
        // whether or not there's video -- audio.start() is called there.
    }

    // Decode runs on its own thread, separate from rendering: the
    // hardware H.264 decoder call (inside h264_wiiu) and the network
    // reads both block, and neither should stall presentation. Decoded
    // video frames cross to this thread through a bounded FrameQueue;
    // audio is converted and handed to SDL directly from the decode
    // thread (SDL_QueueAudio is documented thread-safe for exactly
    // this). Same split CafeMP uses.
    // Sized from the real decoded picture: exactly FrameQueue::MAX_SIZE for
    // 720p and below (unchanged), fewer slots for bigger pictures so the
    // decoded-frame memory stays at the 720p level (see frame_queue.h).
    const size_t videoQueueCapacity = options.videoQueueFrames
        ? options.videoQueueFrames
        : FrameQueue::capacityForFrameBytes(frameBytes);
    FrameQueue videoQueue(videoQueueCapacity);
    if (hasVideo) {
        OSReport("Ufin: video queue capacity=%u frames for %dx%d (%.2f MiB per tightly packed frame)\n",
                 (unsigned)videoQueueCapacity, decoder.videoWidth(), decoder.videoHeight(),
                 frameBytes / 1048576.0);
        OSReport("Ufin: buffering profile=%s prebuffer=%.0fs (max wait %.0fs) packet cap=%.0f MiB look-ahead target=%.0fs\n",
                 bigPicture ? "large-picture" : "standard", prebufferTargetSeconds, prebufferMaxWaitSeconds,
                 maxBufferedPacketBytes / 1048576.0, MAX_BUFFER_AHEAD_SECONDS);
    }
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> decodeDone{false};
    // Set by the decode thread the moment it observes
    // decoder.fatalDecodeError() (see decoder.h); the render loop below
    // polls this and turns it into a clean PlayResult::Error instead of
    // the decode thread continuing to hammer a wedged hardware decoder.
    std::atomic<bool> fatalDecodeDetected{false};

    h264geom::GeometryChangeTracker geometryTracker; // decode thread only

    // Where does the time go? Written by the decode thread, read+reset by
    // the once-a-second log below. Microseconds, since the last log line.
    // 32-bit on purpose: the Espresso (32-bit PowerPC) has no lock-free
    // 8-byte atomics, and std::atomic<uint64_t> fails to link there
    // (__atomic_*_8 needs libatomic). A one-second interval fits easily.
    // Tells "the hardware decode/copy can't keep up" (vdec high) apart
    // from "the network can't keep up" (read high, pkt buffer draining).
    struct DecodeStats {
        std::atomic<uint32_t> vdecUs{0}, vdecMaxUs{0}, vdecCount{0};
        std::atomic<uint32_t> readUs{0}, readMaxUs{0}, readCount{0};
    } stats;
    auto usSince = [](std::chrono::steady_clock::time_point t) {
        const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t).count();
        return (uint32_t)(us < 0 ? 0 : (us > 0x7fffffff ? 0x7fffffff : us)); // clamp ~35 min
    };
    auto bumpMax = [](std::atomic<uint32_t>& m, uint32_t v) {
        uint32_t cur = m.load();
        while (v > cur && !m.compare_exchange_weak(cur, v)) {}
    };

    // Display-order buffer for B-frame video (see frame_reorder.h). Depth 2 covers x264's
    // B-pyramid; it grows by itself if a late frame shows the server reordered deeper.
    FrameReorder<AVFrame*> reorder(hasVideo && decoder.videoNeedsReorder() ? 2 : 0);
    std::vector<FrameReorder<AVFrame*>::Entry> reorderReady;
    int ptsDumpCount = 0;
    std::string ptsDump;
    std::atomic<unsigned> reorderLate{0};   // shown in the once-a-second log line
    auto pushReordered = [&]() {
        for (auto& e : reorderReady) {
            if (e.late) { reorderLate++; av_frame_free(&e.item); continue; } // older than a frame already shown
            videoQueue.push(e.item, e.pts);                   // blocks while full; frees if stopped
        }
        reorderReady.clear();
    };
    if (hasVideo) OSReport("Ufin: video reorder depth %u\n", (unsigned)reorder.depth());

    OSReport("Ufin: DBG starting decode thread\n");
    std::thread decodeThread([&]() {
        OSReport("Ufin: DBG decode thread running\n");
        // Demuxing and decoding are separate steps (see the per-stream
        // API in decoder.h): Jellyfin's fragmented MP4 arrives as seconds
        // of video followed by the same seconds of audio, so each stream
        // is decoded from its own packet queue, only when it needs a
        // frame, and the network is read whenever the stream that needs
        // one has no packets buffered.
        while (!stopRequested) {
            if (decoder.fatalDecodeError()) {
                // The circuit breaker has already tripped inside
                // decodeVideoFrame()/decodeFrom() -- decodeFrom() itself
                // stops feeding the decoder once this is set, so there's
                // nothing left to do here but stop the thread and let
                // the render loop notice fatalDecodeDetected.
                fatalDecodeDetected = true;
                break;
            }
            bool progressed = false;
            AVFrame* frame = nullptr;

            // Keep decoded audio close to the actual playback position even
            // for audio-only streams. Without this cap, an audio-only track
            // can be decoded and queued to completion almost instantly; the
            // visualizer would then only ever see the final decoded samples
            // while the user is listening to the middle of the song.
            const bool audioWanted = hasAudio &&
                audio.queuedSeconds() < MAX_AUDIO_AHEAD_SECONDS;
            const bool videoWanted = hasVideo && videoQueue.size() < videoQueue.capacity();

            if (audioWanted && decoder.decodeAudioFrame(&frame) == DecodedFrameType::AUDIO) {
                audio.queueFrame(frame, decoder.frameTimeSeconds(DecodedFrameType::AUDIO, frame));
                progressed = true;
            }
            const auto vdecStart = std::chrono::steady_clock::now();
            const bool gotVideo = videoWanted && decoder.decodeVideoFrame(&frame) == DecodedFrameType::VIDEO;
            if (videoWanted) {
                const uint32_t us = usSince(vdecStart);
                stats.vdecUs += us; stats.vdecCount++; bumpMax(stats.vdecMaxUs, us);
            }
            if (gotVideo) {
                double pts = decoder.frameTimeSeconds(DecodedFrameType::VIDEO, frame);
                // First frame, then only when the geometry changes -- never
                // per frame. NOTE this is the AVFrame that h264_wiiu copied
                // *out* of the hardware buffer (FFmpeg's own linesize), not
                // the hardware's pitch; the "expected" line is what the
                // patched allocator assumes the hardware wrote.
                if (geometryTracker.changed(frame->width, frame->height, frame->linesize[0])) {
                    OSReport("Ufin: H264 output visible=%dx%d avframe linesize=[%d,%d] fmt=%d\n",
                             frame->width, frame->height, frame->linesize[0], frame->linesize[1],
                             frame->format);
                    OSReport("Ufin: H264 expected hw layout coded=%dx%d pitch=%d Y=%u UV=%u write=%u alloc=%u aligned=%d\n",
                             frame->width,
                             decoder.videoHardwareHeight() > 0 ? decoder.videoHardwareHeight()
                                                                : h264geom::alignedHeightFor(frame->height),
                             h264geom::pitchFor(frame->width),
                             (unsigned)h264geom::yPlaneBytes(frame->width, frame->height),
                             (unsigned)h264geom::uvPlaneBytes(frame->width, frame->height),
                             (unsigned)h264geom::hardwareWriteBytes(frame->width, frame->height),
                             (unsigned)h264geom::framebufferAllocBytes(frame->width, frame->height),
                             (int)h264geom::isAligned(frame->width, frame->height));
                }
                // Diagnostic: the pts of the first 24 pictures, in the order the hardware
                // decoder returned them -- shows at a glance whether that is decode order
                // (pts jumping around, e.g. 0 3 1 2) or already display order.
                if (ptsDumpCount < 24) {
                    char b[24];
                    snprintf(b, sizeof(b), " %.3f", pts);
                    ptsDump += b;
                    if (++ptsDumpCount == 24) OSReport("Ufin: decoder output pts order:%s\n", ptsDump.c_str());
                }
                AVFrame* clone = av_frame_clone(frame);
                // Pictures come out of the hardware decoder in decode order; with B-frames
                // (Main/High) that is not display order, so they pass through the reorder
                // buffer first (depth 0 -- Baseline -- hands each one straight back).
                // Normally room for one frame; after a reorder step several can be ready.
                if (clone) {
                    reorderReady.clear();
                    reorder.push(clone, pts, reorderReady);
                    pushReordered();
                }
                progressed = true;
            }
            if (progressed) continue;

            if (decoder.finished()) break;

            const bool starved = (audioWanted && !decoder.hasQueuedAudioPackets()) ||
                                 (videoWanted && !decoder.hasQueuedVideoPackets());
            // Below the look-ahead target: keep reading even if nothing is
            // currently starved for packets, so a backlog actually builds
            // up instead of the network only ever being touched on demand
            // (which left effectively no cushion against a hiccup -- see
            // PREBUFFER_TARGET_SECONDS above).
            const bool belowLookAheadTarget =
                (hasVideo && decoder.queuedVideoSeconds() < MAX_BUFFER_AHEAD_SECONDS) ||
                (hasAudio && decoder.queuedAudioSeconds() < MAX_BUFFER_AHEAD_SECONDS);
            if ((starved || belowLookAheadTarget) && !decoder.demuxFinished() &&
                decoder.queuedPacketBytes() < maxBufferedPacketBytes) {
                const auto readStart = std::chrono::steady_clock::now();
                decoder.readPacket(); // blocks on the network
                const uint32_t us = usSince(readStart);
                stats.readUs += us; stats.readCount++; bumpMax(stats.readMaxUs, us);
            } else {
                // Outputs full (wait for playback to catch up), or the
                // read-ahead cap was hit, or at end of input waiting for
                // room to hand out the last frames.
                SDL_Delay(5);
            }
        }
        if (!stopRequested) {   // end of stream: the last pictures are still held back
            reorderReady.clear();
            reorder.flush(reorderReady);
            pushReordered();
        } else {
            reorderReady.clear();
            reorder.flush(reorderReady);
            for (auto& e : reorderReady) av_frame_free(&e.item);
            reorderReady.clear();
        }
        if (reorder.lateCount() > 0) {
            OSReport("Ufin: reorder dropped %u late frame(s), final depth %u\n",
                     (unsigned)reorder.lateCount(), (unsigned)reorder.depth());
        }
        decodeDone = true;
        videoQueue.finish();
    });

    OSReport("Ufin: DBG decode thread started, entering prebuffer\n");
    PlayResult result = PlayResult::Completed;

    // Rebuffering only makes sense with a picture to hold; music keeps its own
    // behaviour untouched.
    bool rebufferEnabled = hasVideo;

    // --- Prebuffer: don't show a frame or start audio until both streams
    // have a real cushion of already-downloaded data, so a playback start
    // isn't immediately followed by running dry on the first hiccup. The
    // decode thread above is already running and filling its (small,
    // bounded) decoded-frame buffers concurrently with this wait, so
    // there's no extra latency once the target is reached -- frames are
    // ready to show immediately.
    //
    // Short items that never reach the target (a clip shorter than
    // PREBUFFER_TARGET_SECONDS, or a slow server that would otherwise
    // make the user wait forever) fall through as soon as the demuxer
    // reports end of stream or the decode thread finishes.
    {
        const double prebufferStart = nowSeconds();
        bool stoppedDuringPrebuffer = false;
        double dbgLast = -10.0;
        while (true) {
            if (nowSeconds() - dbgLast > 1.0) {
                dbgLast = nowSeconds();
                OSReport("Ufin: DBG prebuffer tick audioQ=%.1fs pktBytes=%u demuxFin=%d decodeDone=%d\n",
                         hasAudio ? decoder.queuedAudioSeconds() : 0.0, (unsigned)decoder.queuedPacketBytes(),
                         (int)decoder.demuxFinished(), (int)decodeDone.load());
            }
            PlayerCommand cmd = poll();
            if (cmd.kind == PlayerCommand::Kind::Stop) {
                stopRequested = true;
                videoQueue.stop();
                result = PlayResult::Stopped;
                stoppedDuringPrebuffer = true;
                break;
            }
            if (cmd.kind == PlayerCommand::Kind::SeekTo) {
                seek_target_ = cmd.seconds < 0.0 ? 0.0 : cmd.seconds;
                stopRequested = true;
                videoQueue.stop();
                result = PlayResult::SeekRequested;
                stoppedDuringPrebuffer = true;
                break;
            }

            if (cmd.kind == PlayerCommand::Kind::SetVolume) {
                volume_ = cmd.volume < 0.0 ? 0.0 : (cmd.volume > 1.0 ? 1.0 : cmd.volume);
                if (hasAudio) audio.setVolume((float)volume_);
            }
            if (fatalDecodeDetected) {
                // Tripped before we ever got to show a frame -- don't
                // fall through into "start anyway with what's buffered"
                // below, there's no working video decoder to play from.
                stopRequested = true;
                videoQueue.stop();
                last_error_ = "persistent hardware video decode failure (circuit breaker)";
                result = PlayResult::Error;
                stoppedDuringPrebuffer = true;
                break;
            }

            const bool videoReady = !hasVideo || decoder.queuedVideoSeconds() >= prebufferTargetSeconds;
            const bool audioReady = !hasAudio || decoder.queuedAudioSeconds() >= prebufferTargetSeconds;
            if (videoReady && audioReady) break;
            // Don't wait forever on a short item or a server that will
            // never deliver PREBUFFER_TARGET_SECONDS worth (e.g. Live TV
            // right after tuning, or anything shorter than the target).
            if (decoder.demuxFinished() || decodeDone) break;
            if (nowSeconds() - prebufferStart > prebufferMaxWaitSeconds) {
                OSReport("Ufin: prebuffer target not reached after %.0fs (video=%.1fs audio=%.1fs of %.0fs) -- starting anyway\n",
                         prebufferMaxWaitSeconds, decoder.queuedVideoSeconds(),
                         hasAudio ? decoder.queuedAudioSeconds() : 0.0, prebufferTargetSeconds);
                break;
            }

            if (options.onBuffering) {
                double buffered = hasVideo ? decoder.queuedVideoSeconds() : decoder.queuedAudioSeconds();
                if (hasVideo && hasAudio) buffered = std::min(buffered, decoder.queuedAudioSeconds());
                options.onBuffering(buffered, prebufferTargetSeconds);
            }
            SDL_Delay(50);
        }
        if (!stoppedDuringPrebuffer) {
            OSReport("Ufin: prebuffer done after %.1fs (video=%.1fs audio=%.1fs)\n",
                     nowSeconds() - prebufferStart, decoder.queuedVideoSeconds(),
                     hasAudio ? decoder.queuedAudioSeconds() : 0.0);
            // Audio-only: nothing else to wait for -- start now. With
            // video, start() happens when the first frame is shown so the
            // two begin together (see audioNotStartedYet below).
            if (!hasVideo && hasAudio) audio.start();

            stateMachine.startupComplete();
            OSReport("Ufin: playback state %s -> %s\n", playbackStateName(PlaybackState::StartupBuffering),
                     playbackStateName(PlaybackState::Playing));
            if (options.onPlaybackState) {
                options.onPlaybackState(PlaybackState::StartupBuffering, PlaybackState::Playing, NAN);
            }
            // Rebuffering rests on the packets' timeline durations. If plenty
            // of video packets were read and none carried a duration, the
            // playable-seconds figure would read ~0 forever and we would hold
            // playback for no reason -- so leave the feature off rather than
            // guess.
            if (hasVideo && !decoder.videoTimelineUsable()) {
                rebufferEnabled = false;
                OSReport("Ufin: rebuffering disabled -- no measurable packet durations in this stream\n");
            }
        }
    }

    double nominalFrameDuration = decoder.videoFrameDuration();
    if (!(nominalFrameDuration > 0.0)) nominalFrameDuration = 1.0 / 30.0;

    // Master clock: audio when we have it, otherwise wall time anchored
    // to the first video frame's timestamp.
    double wallClockBase = NAN;
    auto masterClock = [&]() -> double {
        if (hasAudio && audio.hasClock()) return audio.clockSeconds();
        if (!std::isnan(wallClockBase)) return nowSeconds() - wallClockBase;
        return NAN;
    };

    QueuedVideoFrame held{};
    bool haveHeld = false;
    double lastRenderedPts = NAN;
    int framesRendered = 0;
    uint64_t renderUs = 0, renderCount = 0, renderMaxUs = 0; // render thread only, reset each log line
    int framesDropped = 0;
    const double loopStartTime = nowSeconds();
    double lastTickTime = loopStartTime;

    bool loggedFirstFrameSkew = false;     // diagnostics, see TIMELINE_SKEW_WARN_SECONDS
    double lastHoldWarnAt = -HOLD_WARN_INTERVAL_SECONDS;
    double firstStreamTime = NAN;  // first clock value seen, for itemPositionFromStreamTime
    double pauseStartedAt = NAN;   // wall time the current hold began (video-only clock)
    double lastPausedRedraw = 0.0;
    double bufferingStartedAt = 0.0;

    // Playback is *held* -- audio device paused, clock frozen, no new frame
    // shown -- for either reason: the user's pause, or rebuffering. Both use
    // the one mechanism the user pause already proved (AudioOutput::setPaused
    // freezes the clock exactly where playback is, and extrapolates from
    // there on resume), so entering/leaving BUFFERING cannot reset the clock
    // or create a timestamp jump, and A/V stays where it was. The two reasons
    // are tracked separately (paused_ vs the state machine); the hold is
    // simply their OR, applied here in exactly one place.
    bool clockHeld = false;
    auto syncClockHold = [&](double at) {
        const bool hold = paused_ || stateMachine.buffering();
        if (hold == clockHeld) return;
        clockHeld = hold;
        if (hasAudio) audio.setPaused(hold);
        if (hold) {
            pauseStartedAt = at;
        } else if (!std::isnan(pauseStartedAt)) {
            // Video-only wall clock: don't count the hold as played time.
            if (!std::isnan(wallClockBase)) wallClockBase += at - pauseStartedAt;
            pauseStartedAt = NAN;
        }
    };

    // Playable media ahead of the playback clock, in timeline seconds. Video:
    // decoded frames ahead of the clock (newest queued frame's end time minus
    // the clock; the queue is only ~0.5 s deep at 1080p, the rest of the
    // buffer is compressed) plus the compressed read-ahead. Audio: what SDL
    // still holds plus the compressed read-ahead. The result is the smaller
    // of the two, because playback needs both. Every input is a timeline
    // duration, never a packet or frame count (except the fallback for frames
    // that carry no timestamp at all).
    double lastVideoPlayable = NAN, lastAudioPlayable = NAN;
    auto measurePlayable = [&]() -> double {
        double videoSec = NAN, audioSec = NAN;
        const double clock = masterClock();
        if (hasVideo) {
            double ahead;
            const double lastPts = videoQueue.lastPushedPts();
            if (!std::isnan(lastPts) && !std::isnan(clock)) {
                ahead = (lastPts + nominalFrameDuration) - clock;
            } else {
                ahead = (double)(videoQueue.size() + (haveHeld ? 1 : 0)) * nominalFrameDuration;
            }
            if (ahead < 0.0) ahead = 0.0;
            videoSec = ahead + decoder.queuedVideoSeconds();
        }
        if (hasAudio) audioSec = audio.queuedSeconds() + decoder.queuedAudioSeconds();
        lastVideoPlayable = videoSec;
        lastAudioPlayable = audioSec;
        if (hasVideo && hasAudio) return std::min(videoSec, audioSec);
        return hasVideo ? videoSec : audioSec;
    };

    while (!stopRequested) {
        PlayerCommand cmd = poll();
        if (cmd.kind == PlayerCommand::Kind::Stop) {
            stopRequested = true;
            videoQueue.stop();
            result = PlayResult::Stopped;
            break;
        }
        if (cmd.kind == PlayerCommand::Kind::SeekTo) {
            seek_target_ = cmd.seconds < 0.0 ? 0.0 : cmd.seconds;
            OSReport("Ufin: seek requested to %.1fs\n", seek_target_);
            stopRequested = true;
            videoQueue.stop();
            result = PlayResult::SeekRequested;
            break;
        }
        if (fatalDecodeDetected) {
            // Circuit breaker tripped (see decoder.h/decoder.cpp):
            // persistent hardware video decode failures, not a one-off
            // glitch. Stop cleanly through the normal error path rather
            // than let the decode thread (already stopped itself) or
            // this loop keep going with a wedged decoder.
            OSReport("Ufin: persistent decode failure detected -- ending playback (position %.1fs)\n",
                     last_position_);
            stopRequested = true;
            videoQueue.stop();
            last_error_ = "persistent hardware video decode failure (circuit breaker)";
            result = PlayResult::Error;
            break;
        }
        if (cmd.kind == PlayerCommand::Kind::SetVolume) {
            volume_ = cmd.volume < 0.0 ? 0.0 : (cmd.volume > 1.0 ? 1.0 : cmd.volume);
            if (hasAudio) audio.setVolume((float)volume_);
        }
        if (cmd.kind == PlayerCommand::Kind::TogglePause) {
            paused_ = !paused_;
            OSReport("Ufin: %s at %.1fs\n", paused_ ? "paused" : "resumed", last_position_);
            syncClockHold(nowSeconds());
        }

        double now = nowSeconds();

        // Rebuffering: decide PLAYING <-> BUFFERING from what is playable
        // *now*, before deciding whether to show or play anything this
        // iteration. Not before the first picture is up (nothing to hold
        // yet), and not for streams whose durations cannot be measured.
        if (rebufferEnabled && framesRendered > 0) {
            const double playable = measurePlayable();
            BufferSample sample;
            sample.playableSeconds = playable;
            sample.nowSeconds = now;
            // End of stream is the demuxer's word, never a momentary lack of
            // packets: a stalled network just leaves this false.
            sample.moreMediaMayArrive = !decoder.demuxFinished();
            sample.readAheadFull = decoder.queuedPacketBytes() >= maxBufferedPacketBytes;
            sample.consuming = !paused_;
            const PlaybackState before = stateMachine.state();
            if (stateMachine.update(sample)) {
                const PlaybackState after = stateMachine.state();
                buffering_ = stateMachine.buffering();
                OSReport("Ufin: playback state %s -> %s\n", playbackStateName(before), playbackStateName(after));
                if (after == PlaybackState::Buffering) {
                    bufferingStartedAt = now;
                    OSReport("Ufin: BUFFERING entered: video=%.1fs audio=%.1fs (pos %.1fs, low-water %.1fs, resume at %.1fs)\n",
                             lastVideoPlayable, hasAudio ? lastAudioPlayable : 0.0, last_position_,
                             stateMachine.thresholds().lowWaterSeconds, stateMachine.thresholds().recoverySeconds);
                } else {
                    OSReport("Ufin: BUFFERING recovery: video=%.1fs audio=%.1fs after %.1fs%s\n",
                             lastVideoPlayable, hasAudio ? lastAudioPlayable : 0.0, now - bufferingStartedAt,
                             decoder.demuxFinished() ? " (end of stream)" : "");
                }
                syncClockHold(now);
                if (options.onPlaybackState) options.onPlaybackState(before, after, playable);
            }
            if (stateMachine.buffering()) buffered_seconds_ = playable;
        }

        if (!paused_ && !stateMachine.buffering()) {
            double clock = masterClock();
            if (!std::isnan(clock)) {
                if (std::isnan(firstStreamTime)) {
                    firstStreamTime = clock;
                    OSReport("Ufin: first stream timestamp %.2fs (requested start %.1fs) -> playing from %.1fs\n",
                             clock, options.startOffsetSeconds,
                             itemPositionFromStreamTime(clock, options.startOffsetSeconds, clock));
                }
                last_position_ = itemPositionFromStreamTime(clock, options.startOffsetSeconds, firstStreamTime);
            }
        }
        if (onTick && now - lastTickTime >= 1.0) {
            onTick(last_position_);
            lastTickTime = now;
            if (hasVideo) {
                OSReport("Ufin: pos=%.1fs rendered=%d dropped=%d reorder_late=%u queued=%u audio=%.2fs%s\n",
                         last_position_, framesRendered, framesDropped, reorderLate.load(),
                         (unsigned)videoQueue.size(), hasAudio ? audio.queuedSeconds() : 0.0,
                         paused_ ? " (paused)" : (stateMachine.buffering() ? " (buffering)" : ""));
                // Packet read-ahead (compressed, in stream seconds) plus where the
                // decode thread and render thread spent the last interval.
                const uint32_t vc = stats.vdecCount.exchange(0), vu = stats.vdecUs.exchange(0), vm = stats.vdecMaxUs.exchange(0);
                const uint32_t rc = stats.readCount.exchange(0), ru = stats.readUs.exchange(0), rm = stats.readMaxUs.exchange(0);
                const double playableNow = rebufferEnabled ? measurePlayable() : NAN;
                OSReport("Ufin:   buffer video=%.1fs audio=%.1fs (%.1f MiB) state=%s playable=%.1fs | vdec %llu calls avg %.1fms max %.1fms | "
                         "net read %llu calls total %.0fms max %.0fms | render avg %.1fms max %.1fms\n",
                         decoder.queuedVideoSeconds(), hasAudio ? decoder.queuedAudioSeconds() : 0.0,
                         decoder.queuedPacketBytes() / 1048576.0, playbackStateName(stateMachine.state()),
                         std::isnan(playableNow) ? -1.0 : playableNow,
                         (unsigned long long)vc, vc ? vu / 1000.0 / vc : 0.0, vm / 1000.0,
                         (unsigned long long)rc, ru / 1000.0, rm / 1000.0,
                         renderCount ? renderUs / 1000.0 / renderCount : 0.0, renderMaxUs / 1000.0);
                renderUs = 0; renderCount = 0; renderMaxUs = 0;
            }
        }

        if (paused_ || stateMachine.buffering()) {
            // Hold the current picture (the user's pause, or rebuffering).
            // The decode thread keeps working: it fills its decoded
            // buffers, then -- because it reads whenever nothing else is
            // ready to decode -- keeps pulling packets off the network into
            // the compressed read-ahead, which is exactly what BUFFERING is
            // waiting for. Keep redrawing the last picture with the app's HUD
            // on top (which shows the pause icon or the buffering wheel);
            // the wheel animates, so it redraws a little more often.
            const double redrawInterval = (stateMachine.buffering() && !paused_) ? BUFFERING_REDRAW_SECONDS : 0.05;
            if (hasVideo && options.presentVideo) {
                if (now - lastPausedRedraw >= redrawInterval) {
                    video.presentLast();
                    lastPausedRedraw = now;
                } else {
                    SDL_Delay(10);
                }
            } else if (!hasVideo && options.onIdleFrame) {
                options.onIdleFrame();
            } else {
                SDL_Delay(15);
            }
            continue;
        }

        if (!hasVideo) {
            // Audio-only stream: the decode thread has no pacing other
            // than MAX_AUDIO_AHEAD (which only applies with video), so it
            // can finish decoding long before SDL has played everything.
            // Don't declare completion -- and don't let audio.shutdown()
            // cut the device off -- until the queue has actually drained.
            if (decodeDone && audio.queuedBytes() == 0) {
                result = PlayResult::Completed;
                break;
            }
            if (options.onIdleFrame) options.onIdleFrame(); // draws + waits for vsync
            else SDL_Delay(16);
            continue;
        }

        if (!haveHeld) {
            if (!videoQueue.pop(held, 50)) {
                if (hasAudio && !audio.started() && audio.hasClock() &&
                    now - loopStartTime > 3.0) {
                    // Audio is buffered but no picture has arrived for
                    // seconds (hardware decoder rejecting every frame?).
                    // Don't sit silent forever: let the sound play, and
                    // the decode thread's audio throttle keep flowing.
                    OSReport("Ufin: no video frames after 3s -- starting audio anyway\n");
                    audio.start();
                }
                if (videoQueue.isDrained()) {
                    // No more video will ever arrive. Let any remaining
                    // audio finish, then we're done.
                    if (!hasAudio || audio.queuedBytes() == 0) {
                        result = PlayResult::Completed;
                        break;
                    }
                    SDL_Delay(16);
                }
                continue; // timeout: loop back to poll()
            }
            haveHeld = true;
        }

        double pts = held.pts;
        if (std::isnan(pts)) {
            // No timestamps at all -- fall back to pacing at the nominal
            // frame rate from whatever we showed last.
            double clock = masterClock();
            pts = std::isnan(lastRenderedPts) ? (std::isnan(clock) ? 0.0 : clock)
                                              : lastRenderedPts + nominalFrameDuration;
        }

        if (!hasAudio && std::isnan(wallClockBase)) {
            wallClockBase = now - pts; // first frame defines t=0 of the wall clock
        }

        bool audioNotStartedYet = hasAudio && !audio.started();
        double clock = masterClock();

        if (!loggedFirstFrameSkew && hasAudio && !std::isnan(clock)) {
            // Once per stream start: where the first picture sits against the
            // first audio. Close together is normal; seconds apart means the
            // server sent video and audio on different timelines.
            loggedFirstFrameSkew = true;
            OSReport("Ufin: first video pts=%.3fs, audio clock=%.3fs, video leads audio by %.3fs\n", pts, clock, pts - clock);
            if (std::fabs(pts - clock) > TIMELINE_SKEW_WARN_SECONDS) {
                OSReport("Ufin: WARNING video and audio timelines start %.1fs apart -- the picture will be held until the audio clock reaches it\n",
                         pts - clock);
            }
        }

        if (!audioNotStartedYet && !std::isnan(clock)) {
            double delay = pts - clock;
            if (delay > TIMELINE_SKEW_WARN_SECONDS && now - lastHoldWarnAt >= HOLD_WARN_INTERVAL_SECONDS) {
                lastHoldWarnAt = now;
                OSReport("Ufin: holding the picture: next frame pts=%.2fs is %.1fs ahead of the playback clock (%.2fs); video looks frozen until then\n",
                         pts, delay, clock);
            }
            if (delay > 0.005) {
                // Not time yet. Sleep in short slices so poll()
                // stays responsive even if the timestamps jump.
                SDL_Delay((Uint32)std::min(delay * 1000.0, 15.0));
                continue;
            }
            if (delay < -LATE_FRAME_DROP_SECONDS && videoQueue.size() > 0) {
                // Hopelessly late and there's a newer frame waiting --
                // skip this one to catch up.
                av_frame_free(&held.frame);
                haveHeld = false;
                framesDropped++;
                continue;
            }
        }

        const auto renderStart = std::chrono::steady_clock::now();
        video.renderFrame(held.frame);
        {
            const uint32_t us = usSince(renderStart);
            renderUs += us; renderCount++; if (us > renderMaxUs) renderMaxUs = us;
        }
        av_frame_free(&held.frame);
        haveHeld = false;
        lastRenderedPts = pts;
        framesRendered++;

        if (audioNotStartedYet) {
            // First picture is up -- let the sound begin, and let
            // subsequent frames sync to it.
            audio.start();
        }
    }

    // Make sure the decode thread actually exits before we tear down
    // decoder/audio/video out from under it. Worst case this can take up
    // to HttpStreamReader's own read timeout (15s) if it's mid-blocking-
    // read on the network when stop is requested -- acceptable for now,
    // a more aggressive cancellation would need HttpStreamReader itself
    // to check a shared stop flag inside its own wait loop.
    stopRequested = true;
    videoQueue.stop();
    decodeThread.join();

    if (haveHeld && held.frame) {
        av_frame_free(&held.frame);
    }

    OSReport("Ufin: Player done -- result=%d rendered=%d dropped=%d position=%.1fs\n",
             (int)result, framesRendered, framesDropped, last_position_);

    buffering_ = false;
    active_audio_ = nullptr; // about to shut down / go out of scope
    if (hasVideo) video.shutdown();
    if (hasAudio) audio.shutdown();
    decoder.close();

    return result;
}

int Player::fetchVisualizerSamples(int16_t* out, int capacity, uint32_t* lastSeq) const {
    if (!active_audio_) return 0;
    return active_audio_->fetchVisualizerSamples(out, capacity, lastSeq);
}

int Player::visualizerSampleRate() const {
    return active_audio_ ? active_audio_->outputSampleRate() : 0;
}
