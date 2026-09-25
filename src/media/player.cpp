#include "player.h"
#include "../seek.h"
#include "http_stream_io.h"
#include "decoder.h"
#include "video_output.h"
#include "audio_output.h"
#include "frame_queue.h"

#include <SDL2/SDL.h>
#include <coreinit/debug.h>
#include <thread>
#include <atomic>
#include <cmath>
#include <algorithm>

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

    HttpStreamIO io(host, port, path);
    if (!io.open()) {
        last_error_ = "HttpStreamIO::open failed: " + std::string(io.lastError());
        return PlayResult::Error;
    }

    Decoder decoder;
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

    OSReport("Ufin: Player -- video=%d (%s %dx%d) audio=%d (%d Hz, %d ch)\n", hasVideo,
             decoder.videoCodecName(), decoder.videoWidth(), decoder.videoHeight(), hasAudio,
             decoder.audioSampleRate(), decoder.audioChannels());

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
    if (hasAudio) {
        if (!audio.init(decoder.audioSampleRate(), decoder.audioChannels(),
                         decoder.audioSampleFormat())) {
            last_error_ = "AudioOutput::init failed: " + std::string(audio.lastError());
            if (hasVideo) video.shutdown();
            decoder.close();
            return PlayResult::Error;
        }
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
    FrameQueue videoQueue;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> decodeDone{false};

    std::thread decodeThread([&]() {
        // Demuxing and decoding are separate steps (see the per-stream
        // API in decoder.h): Jellyfin's fragmented MP4 arrives as seconds
        // of video followed by the same seconds of audio, so each stream
        // is decoded from its own packet queue, only when it needs a
        // frame, and the network is read whenever the stream that needs
        // one has no packets buffered.
        while (!stopRequested) {
            bool progressed = false;
            AVFrame* frame = nullptr;

            const bool audioWanted = hasAudio &&
                (!hasVideo || audio.queuedSeconds() < MAX_AUDIO_AHEAD_SECONDS);
            const bool videoWanted = hasVideo && videoQueue.size() < FrameQueue::MAX_SIZE;

            if (audioWanted && decoder.decodeAudioFrame(&frame) == DecodedFrameType::AUDIO) {
                audio.queueFrame(frame, decoder.frameTimeSeconds(DecodedFrameType::AUDIO, frame));
                progressed = true;
            }
            if (videoWanted && decoder.decodeVideoFrame(&frame) == DecodedFrameType::VIDEO) {
                double pts = decoder.frameTimeSeconds(DecodedFrameType::VIDEO, frame);
                AVFrame* clone = av_frame_clone(frame);
                // Only this thread pushes and there was room, so this
                // never blocks.
                if (clone) videoQueue.push(clone, pts);
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
                decoder.queuedPacketBytes() < MAX_BUFFERED_PACKET_BYTES) {
                decoder.readPacket(); // blocks on the network
            } else {
                // Outputs full (wait for playback to catch up), or the
                // read-ahead cap was hit, or at end of input waiting for
                // room to hand out the last frames.
                SDL_Delay(5);
            }
        }
        decodeDone = true;
        videoQueue.finish();
    });

    PlayResult result = PlayResult::Completed;

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
        while (true) {
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

            const bool videoReady = !hasVideo || decoder.queuedVideoSeconds() >= PREBUFFER_TARGET_SECONDS;
            const bool audioReady = !hasAudio || decoder.queuedAudioSeconds() >= PREBUFFER_TARGET_SECONDS;
            if (videoReady && audioReady) break;
            // Don't wait forever on a short item or a server that will
            // never deliver PREBUFFER_TARGET_SECONDS worth (e.g. Live TV
            // right after tuning, or anything shorter than the target).
            if (decoder.demuxFinished() || decodeDone) break;
            if (nowSeconds() - prebufferStart > PREBUFFER_MAX_WAIT_SECONDS) {
                OSReport("Ufin: prebuffer target not reached after %.0fs -- starting anyway\n",
                         PREBUFFER_MAX_WAIT_SECONDS);
                break;
            }

            if (options.onBuffering) {
                double buffered = hasVideo ? decoder.queuedVideoSeconds() : decoder.queuedAudioSeconds();
                if (hasVideo && hasAudio) buffered = std::min(buffered, decoder.queuedAudioSeconds());
                options.onBuffering(buffered, PREBUFFER_TARGET_SECONDS);
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
    int framesDropped = 0;
    const double loopStartTime = nowSeconds();
    double lastTickTime = loopStartTime;

    double firstStreamTime = NAN;  // first clock value seen, for itemPositionFromStreamTime
    double pauseStartedAt = NAN;   // wall time the current pause began (video-only clock)
    double lastPausedRedraw = 0.0;

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
        if (cmd.kind == PlayerCommand::Kind::TogglePause) {
            paused_ = !paused_;
            OSReport("Ufin: %s at %.1fs\n", paused_ ? "paused" : "resumed", last_position_);
            if (hasAudio) audio.setPaused(paused_);
            if (paused_) {
                pauseStartedAt = nowSeconds();
            } else if (!std::isnan(pauseStartedAt)) {
                // Video-only wall clock: don't count the pause as played time.
                if (!std::isnan(wallClockBase)) wallClockBase += nowSeconds() - pauseStartedAt;
                pauseStartedAt = NAN;
            }
        }

        double now = nowSeconds();
        if (!paused_) {
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
                OSReport("Ufin: pos=%.1fs rendered=%d dropped=%d queued=%u audio=%.2fs%s\n",
                         last_position_, framesRendered, framesDropped,
                         (unsigned)videoQueue.size(), hasAudio ? audio.queuedSeconds() : 0.0,
                         paused_ ? " (paused)" : "");
            }
        }

        if (paused_) {
            // Hold the current picture; the decode thread fills its
            // buffers and then stops reading on its own. Keep redrawing
            // it (with the app's HUD on top, which shows the pause).
            if (hasVideo && options.presentVideo) {
                if (now - lastPausedRedraw >= 0.05) {
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

        if (!audioNotStartedYet && !std::isnan(clock)) {
            double delay = pts - clock;
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

        video.renderFrame(held.frame);
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

    if (hasVideo) video.shutdown();
    if (hasAudio) audio.shutdown();
    decoder.close();

    return result;
}
