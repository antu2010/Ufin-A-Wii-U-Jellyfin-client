#include "player.h"
#include "../seek.h"
#include "../ufin_log.h"
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

// Cap on compressed packets read ahead of decoding (see the decode
// thread). At 2.5 Mbit/s this is over a minute of stream -- far more
// than one fragment -- and still a small slice of MEM2.
static const size_t MAX_BUFFERED_PACKET_BYTES = 24 * 1024 * 1024;

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
        // Audio-only: nothing to wait for, start playing as soon as data
        // arrives. With video, start() happens when the first frame is
        // on screen so the two begin together.
        if (!hasVideo) audio.start();
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
            if (starved && !decoder.demuxFinished() &&
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

    while (true) {
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
                if (std::isnan(firstStreamTime)) firstStreamTime = clock;
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
            // buffers and then stops reading on its own.
            SDL_Delay(15);
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
            SDL_Delay(16);
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
