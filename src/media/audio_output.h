// AudioOutput -- takes decoded audio AVFrames (whatever internal format
// the source decoder produces -- AAC decode is typically planar float,
// FLTP) and converts + queues them for SDL2 to play. Decoder.cpp doesn't
// know or care about this conversion; it's entirely AudioOutput's job,
// keeping "decode" and "get audio out of the speakers" cleanly separate.
//
// It also acts as the playback clock for A/V sync: because SDL tells us
// how much queued audio it hasn't played yet, we always know (to within
// one device buffer) which stream timestamp is currently coming out of
// the speakers. Player paces video against that -- audio is the master
// clock, as in every practical media player, because the ear notices
// audio glitches far more than the eye notices a dropped frame.

#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}
#include <SDL2/SDL.h>
#include <mutex>

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // sourceSampleRate/sourceChannels/sourceFormat should match
    // Decoder::audioSampleRate()/audioChannels()/audioSampleFormat().
    // Output is always resampled to interleaved 16-bit stereo, which is
    // what we ask SDL to open the audio device with -- simplest common
    // format, avoids needing to handle every possible source layout
    // downstream. The device starts paused; call start() to begin
    // playback (Player does this once the first video frame is up, so
    // audio doesn't run ahead while video is still spinning up).
    bool init(int sourceSampleRate, int sourceChannels, AVSampleFormat sourceFormat);

    // Changes only the source format the resampler expects (no SDL/AX
    // calls, so safe from a worker thread): for the menu music, whose
    // device is opened on the main thread before the stream's format is
    // known. Call before queueing frames of the new format.
    bool setSourceFormat(int sourceSampleRate, int sourceChannels, AVSampleFormat sourceFormat);
    bool matchesSource(int rate, int channels, AVSampleFormat fmt) const {
        return rate == src_rate_ && channels == src_channels_ && fmt == src_format_;
    }

    // Converts one decoded frame and queues the result for playback.
    // ptsSeconds is the frame's stream time (Decoder::frameTimeSeconds),
    // or NAN if unknown; it drives clockSeconds(). Safe to call from a
    // different thread than the one reading the clock.
    void queueFrame(AVFrame* frame, double ptsSeconds);

    void start();
    bool started() const { return started_; }

    // Pauses/resumes the device. While paused the clock holds still (so
    // video holds its frame) and SDL stops consuming, which in turn
    // stops the decode thread once it's far enough ahead.
    void setPaused(bool paused);

    // Output gain, 0..1 (menu music plays quieter than real playback).
    void setVolume(float volume) { volume_ = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume); }
    bool paused() const;

    // How much queued audio SDL hasn't played yet -- in bytes at the
    // output format, or in seconds. Used by Player to keep the decode
    // thread from running unboundedly ahead of playback.
    uint32_t queuedBytes() const;
    double queuedSeconds() const;

    // True once at least one timestamped frame has been queued, i.e.
    // clockSeconds() means something.
    bool hasClock() const;

    // Estimated stream time (seconds) currently being played. Constant
    // (the first queued frame's time) until start() is called.
    double clockSeconds() const;

    void shutdown();

    const char* lastError() const { return last_error_; }

private:
    SDL_AudioDeviceID device_ = 0;
    SwrContext* swr_ctx_ = nullptr;
    int out_channels_ = 2;
    int out_sample_rate_ = 48000;
    int device_buffer_samples_ = 0; // per-channel samples per SDL callback (latency estimate)
    bool started_ = false;
    char last_error_[256] = {0};

    // Scratch buffer reused across calls to avoid reallocating on every
    // single frame.
    uint8_t* convert_buffer_ = nullptr;
    int convert_buffer_capacity_samples_ = 0;

    // Clock state: "stream time clock_pts_ was playing at wall time
    // clock_wall_ms_"; extrapolated with wall time in between updates.
    mutable std::mutex clock_mtx_;
    bool clock_valid_ = false;
    bool paused_ = false;
    float volume_ = 1.0f;
    int src_rate_ = 0;
    int src_channels_ = 0;
    AVSampleFormat src_format_ = AV_SAMPLE_FMT_NONE;
    double clock_pts_ = 0.0;
    uint32_t clock_wall_ms_ = 0;

    double bytesPerSecond() const { return (double)out_sample_rate_ * out_channels_ * (int)sizeof(int16_t); }
};
