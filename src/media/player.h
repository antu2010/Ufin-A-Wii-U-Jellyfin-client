// Player -- the only module that knows about all the other media/*
// pieces at once. Everything else (HttpStreamIO, Decoder, VideoOutput,
// AudioOutput) only needs to know about its own concern; Player wires
// them together and runs the actual playback loop.
//
// Deliberately knows nothing about VPAD/controller input or the Jellyfin
// API -- main.cpp passes in a `shouldStop` callback that Player polls
// frequently, and an `onTick` callback it calls about once a second
// with the current playback position, so this file (and the rest of
// media/) has no dependency on how input is read or progress reported.

#pragma once
#include <string>
#include <cstdint>
#include <functional>

enum class PlayResult {
    Completed,     // reached end of stream normally
    Stopped,       // the poll callback asked to stop (user backed out)
    Error,         // something failed -- see lastError()
    SeekRequested, // the poll callback asked for a new position -- see seekTarget()
};

// What the poll callback wants Player to do.
struct PlayerCommand {
    enum class Kind { None, Stop, TogglePause, SeekTo, SetVolume };
    Kind kind = Kind::None;
    double seconds = 0.0; // SeekTo: absolute position in the item
    double volume = 0.0;  // SetVolume: 0..1

    static PlayerCommand none() { return PlayerCommand(); }
    static PlayerCommand stop() { PlayerCommand c; c.kind = Kind::Stop; return c; }
    static PlayerCommand togglePause() { PlayerCommand c; c.kind = Kind::TogglePause; return c; }
    static PlayerCommand seekTo(double s) { PlayerCommand c; c.kind = Kind::SeekTo; c.seconds = s; return c; }
    static PlayerCommand setVolume(double v) { PlayerCommand c; c.kind = Kind::SetVolume; c.volume = v; return c; }
};

// Stream time -> position in the item: see itemPositionFromStreamTime in seek.h.

struct PlayOptions {
    // Aspect ratio (width / height) the video should be displayed at.
    // <= 0 means "whatever the decoded frames are". See
    // JellyfinClient::buildVideoStreamUrl for why this can differ from
    // the decoded frame dimensions.
    double displayAspect = 0.0;

    // Position in the item (seconds) the stream was requested from, i.e.
    // StartTimeTicks. Makes positionSeconds()/onTick report positions in
    // the item rather than in the stream.
    double startOffsetSeconds = 0.0;

    // Output volume (0..1) to start playback at -- e.g. carried over
    // from a slider the user was already dragging on a previous track,
    // or from a previous play() call before a seek restarted the
    // stream (see PlayerCommand::SetVolume for changing it while this
    // call is running).
    double initialVolume = 1.0;

    // Draws each video frame as part of an app frame (see VideoPresenter
    // in video_output.h) so the app can put its HUD on top. Unset = bare
    // frames.
    std::function<void(const std::function<void(uint32_t, uint32_t)>&)> presentVideo;

    // Called periodically (a few times a second) while Player is waiting
    // for the initial buffer to fill, before anything is shown or played
    // (see PREBUFFER_TARGET_SECONDS in player.cpp). Args are seconds
    // buffered so far and the target -- e.g. for a "Buffering... 6/15s"
    // message. Never called for a stream that skips prebuffering (too
    // short to reach the target).
    std::function<void(double bufferedSeconds, double targetSeconds)> onBuffering;

    // Called about once per display frame whenever no video frame is
    // being shown -- audio-only playback -- so the app can draw its own
    // screen (Now Playing). Should render one frame; it's the loop's
    // pacing then (the frame waits for vsync).
    std::function<void()> onIdleFrame;

    // Mirrors UfinConfig::allowUnalignedVideoGeometry (see config_loader.h
    // for what it's for) -- forwarded to Decoder before it opens the
    // codec. Default false: keep the h264_wiiu geometry safety check
    // active unless the caller explicitly opted out.
    bool allowUnalignedVideoGeometry = false;
};

class Player {
public:
    // host/port/path identify the media stream (see
    // JellyfinClient::buildVideoStreamUrl / buildAudioStreamUrl for how
    // these get built).
    //
    // shouldStop is polled many times per second; return true to abort
    // playback early. onTick, if provided, is called roughly once per
    // second with the current playback position in seconds (stream
    // time, so it starts near 0 and tracks what the user is actually
    // hearing/seeing) -- intended for updating a "Now Playing" screen
    // and/or reporting progress back to Jellyfin.
    //
    // poll is called many times per second; return a PlayerCommand to
    // stop, toggle pause, or ask for a seek (which ends play() with
    // PlayResult::SeekRequested -- the caller restarts the stream at
    // seekTarget()).
    PlayResult play(const std::string& host, int port, const std::string& path,
                     const std::function<PlayerCommand()>& poll,
                     const std::function<void(double positionSeconds)>& onTick = nullptr,
                     const PlayOptions& options = PlayOptions());

    // Last known playback position (seconds) -- valid after play()
    // returns, for the final progress report.
    double positionSeconds() const { return last_position_; }

    bool isPaused() const { return paused_; }
    double seekTarget() const { return seek_target_; }

    // Current output volume (0..1) -- options.initialVolume until a
    // PlayerCommand::SetVolume changes it. Valid at any time, including
    // before/after play() so the caller can carry a user's chosen
    // volume across a seek restart or into the next queued track.
    double volume() const { return volume_; }

    // Copies up to `capacity` samples of the most recently played mono
    // PCM into `out` for a music visualizer or similar -- see
    // AudioOutput::fetchVisualizerSamples for the format and threading
    // notes. Returns how many were written; 0 if there's no active audio
    // (video hasn't started playing audio yet, or play() isn't running)
    // or nothing new since `lastSeq` was last passed in. Cheap enough to
    // call every frame from onIdleFrame/presentVideo, which run on the
    // same thread as play() itself -- no extra synchronization needed on
    // this side beyond what AudioOutput's own snapshot lock provides.
    int fetchVisualizerSamples(int16_t* out, int capacity, uint32_t* lastSeq) const;

    // Output sample rate of the currently active audio, for the caller's
    // frequency analysis; 0 if there's no active audio right now.
    int visualizerSampleRate() const;

    const std::string& lastError() const { return last_error_; }

private:
    std::string last_error_;
    double last_position_ = 0.0;
    bool paused_ = false;
    double seek_target_ = 0.0;
    double volume_ = 1.0;
    class AudioOutput* active_audio_ = nullptr; // valid only while play() is running
};
