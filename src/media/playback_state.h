// PlaybackStateMachine -- the STARTUP_BUFFERING / PLAYING / BUFFERING
// decision logic, kept free of FFmpeg, SDL and threads so it can be tested
// on the host. Player (player.cpp) feeds it one BufferSample per render-loop
// iteration and acts on the state it reports; the machine itself never
// touches audio, video or the network.
//
//   STARTUP_BUFFERING --startupComplete()--> PLAYING
//   PLAYING   --playable <= lowWater (and more media may still arrive)--> BUFFERING
//   BUFFERING --playable >= recovery, or nothing more can arrive-------> PLAYING
//
// The two thresholds are deliberately far apart (hysteresis): entering at
// ~2 s remaining means playback pauses *while there is still media left*,
// so the pause is clean; leaving only at ~12 s means it does not bounce
// straight back into BUFFERING on the next hiccup.
#pragma once
#include <cmath>

enum class PlaybackState {
    StartupBuffering, // initial prebuffer, before the first frame is shown
    Playing,
    Buffering,        // rebuffering: hold playback, keep reading
};

inline const char* playbackStateName(PlaybackState s) {
    switch (s) {
        case PlaybackState::StartupBuffering: return "STARTUP_BUFFERING";
        case PlaybackState::Playing: return "PLAYING";
        case PlaybackState::Buffering: return "BUFFERING";
    }
    return "?";
}

struct RebufferThresholds {
    // Enter BUFFERING at or below this much playable media (seconds of
    // timeline still to show/play, not a packet or frame count).
    double lowWaterSeconds = 2.0;
    // Leave BUFFERING at or above this much.
    double recoverySeconds = 12.0;
    // Once BUFFERING has started, stay at least this long even if the data
    // arrives at once, so the indicator never just flashes for a frame or two.
    double minBufferingSeconds = 0.5;
};

// What the caller measured this iteration.
struct BufferSample {
    // Playable timeline ahead of the playback clock, in seconds (the smaller
    // of the video and audio figures when both exist). NAN = cannot be
    // measured right now; the machine never guesses in that case.
    double playableSeconds = NAN;
    double nowSeconds = 0.0;       // any monotonic clock, seconds
    // False once the demuxer has hit the genuine end of the stream: nothing
    // more will arrive, so waiting would only be a hang, not buffering.
    bool moreMediaMayArrive = true;
    // The compressed read-ahead is at its byte cap: reading has stopped, so
    // waiting cannot add anything.
    bool readAheadFull = false;
    // False while the user has paused: nothing is being consumed, so there is
    // no starvation to get ahead of and BUFFERING must not start.
    bool consuming = true;
};

class PlaybackStateMachine {
public:
    explicit PlaybackStateMachine(RebufferThresholds t = RebufferThresholds()) : t_(sanitized(t)) {}

    // Keeps the thresholds usable: non-negative low water, and a recovery
    // point that is always strictly above it (that gap *is* the hysteresis).
    static RebufferThresholds sanitized(RebufferThresholds t) {
        if (!(t.lowWaterSeconds >= 0.0)) t.lowWaterSeconds = 0.0;
        if (!(t.recoverySeconds >= t.lowWaterSeconds + 1.0)) t.recoverySeconds = t.lowWaterSeconds + 1.0;
        if (!(t.minBufferingSeconds >= 0.0)) t.minBufferingSeconds = 0.0;
        return t;
    }

    PlaybackState state() const { return state_; }
    bool buffering() const { return state_ == PlaybackState::Buffering; }
    const RebufferThresholds& thresholds() const { return t_; }

    // The initial prebuffer is over and the first frame is about to be shown.
    // Returns true if the state changed.
    bool startupComplete() {
        if (state_ != PlaybackState::StartupBuffering) return false;
        state_ = PlaybackState::Playing;
        return true;
    }

    // Returns true if the state changed (the caller reads state()).
    bool update(const BufferSample& s) {
        switch (state_) {
            case PlaybackState::StartupBuffering:
                return false; // only startupComplete() leaves this state

            case PlaybackState::Playing:
                if (!s.consuming) return false;
                if (std::isnan(s.playableSeconds)) return false;
                if (s.playableSeconds <= t_.lowWaterSeconds && s.moreMediaMayArrive && !s.readAheadFull) {
                    state_ = PlaybackState::Buffering;
                    bufferingSince_ = s.nowSeconds;
                    return true;
                }
                return false;

            case PlaybackState::Buffering: {
                // Nothing left to wait for, or the measurement is gone:
                // play out what there is instead of hanging.
                const bool cannotImprove = !s.moreMediaMayArrive || s.readAheadFull || std::isnan(s.playableSeconds);
                const bool recovered = !std::isnan(s.playableSeconds) && s.playableSeconds >= t_.recoverySeconds;
                if (!cannotImprove && !recovered) return false;
                if (!cannotImprove && s.nowSeconds - bufferingSince_ < t_.minBufferingSeconds) return false;
                state_ = PlaybackState::Playing;
                return true;
            }
        }
        return false;
    }

private:
    RebufferThresholds t_;
    PlaybackState state_ = PlaybackState::StartupBuffering;
    double bufferingSince_ = 0.0;
};
