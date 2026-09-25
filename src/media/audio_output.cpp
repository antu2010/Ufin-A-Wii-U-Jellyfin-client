#include "audio_output.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

AudioOutput::AudioOutput() {}

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::init(int sourceSampleRate, int sourceChannels, AVSampleFormat sourceFormat) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        snprintf(last_error_, sizeof(last_error_), "SDL_InitSubSystem(AUDIO) failed: %s",
                 SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = out_sample_rate_;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)out_channels_;
    want.samples = 4096; // buffer size in samples per channel; on the
                         // smaller side balances latency vs. underrun risk

    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0 /* no format changes allowed */);
    if (device_ == 0) {
        snprintf(last_error_, sizeof(last_error_), "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    device_buffer_samples_ = have.samples > 0 ? have.samples : want.samples;

    // Using the classic swr_alloc_set_opts (channel layout as a bitmask
    // via av_get_default_channel_layout) rather than the newer
    // swr_alloc_set_opts2/AVChannelLayout API -- the FFmpeg-wiiu fork is
    // FFmpeg 4.3, which predates AVChannelLayout. If a future
    // FFmpeg-wiiu update changes this, this call is the first thing to
    // fix.
    if (!setSourceFormat(sourceSampleRate, sourceChannels, sourceFormat)) return false;

    started_ = false;
    clock_valid_ = false;
    return true;
}

bool AudioOutput::setSourceFormat(int sourceSampleRate, int sourceChannels, AVSampleFormat sourceFormat) {
    if (swr_ctx_) swr_free(&swr_ctx_);
    int64_t inLayout = av_get_default_channel_layout(sourceChannels);
    int64_t outLayout = av_get_default_channel_layout(out_channels_);

    swr_ctx_ = swr_alloc_set_opts(nullptr,
        outLayout, AV_SAMPLE_FMT_S16, out_sample_rate_,
        inLayout, sourceFormat, sourceSampleRate,
        0, nullptr);

    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
        if (swr_ctx_) swr_free(&swr_ctx_);
        snprintf(last_error_, sizeof(last_error_), "swr_init failed");
        return false;
    }
    src_rate_ = sourceSampleRate;
    src_channels_ = sourceChannels;
    src_format_ = sourceFormat;
    return true;
}

void AudioOutput::start() {
    if (device_ == 0 || started_) return;
    std::lock_guard<std::mutex> lock(clock_mtx_);
    started_ = true;
    clock_wall_ms_ = SDL_GetTicks();
    if (!paused_) SDL_PauseAudioDevice(device_, 0); // unpause
}

void AudioOutput::setPaused(bool paused) {
    if (device_ == 0) return;
    std::lock_guard<std::mutex> lock(clock_mtx_);
    if (paused == paused_) return;
    uint32_t nowMs = SDL_GetTicks();
    if (paused) {
        // Freeze the clock where playback is right now.
        if (started_ && clock_valid_) clock_pts_ += (nowMs - clock_wall_ms_) / 1000.0;
        paused_ = true;
        SDL_PauseAudioDevice(device_, 1);
    } else {
        paused_ = false;
        clock_wall_ms_ = nowMs; // extrapolate from here, not from before the pause
        if (started_) SDL_PauseAudioDevice(device_, 0);
    }
}

bool AudioOutput::paused() const {
    std::lock_guard<std::mutex> lock(clock_mtx_);
    return paused_;
}

void AudioOutput::queueFrame(AVFrame* frame, double ptsSeconds) {
    if (!swr_ctx_ || device_ == 0) return;

    // Worst case output sample count (resampling can change the count,
    // e.g. going from 44.1kHz source to 48kHz output).
    int maxOutSamples = (int)av_rescale_rnd(
        swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
        out_sample_rate_, frame->sample_rate, AV_ROUND_UP);

    if (maxOutSamples > convert_buffer_capacity_samples_) {
        av_freep(&convert_buffer_);
        int lineSize = 0;
        av_samples_alloc(&convert_buffer_, &lineSize, out_channels_, maxOutSamples,
                          AV_SAMPLE_FMT_S16, 0);
        convert_buffer_capacity_samples_ = maxOutSamples;
    }

    int convertedSamples = swr_convert(swr_ctx_, &convert_buffer_, maxOutSamples,
                                        (const uint8_t**)frame->data, frame->nb_samples);
    if (convertedSamples <= 0) return;

    int bytesPerSample = out_channels_ * (int)sizeof(int16_t);
    if (volume_ < 1.0f) {
        int16_t* samples = (int16_t*)convert_buffer_;
        const int count = convertedSamples * out_channels_;
        const int gain = (int)(volume_ * 65536.0f);
        for (int i = 0; i < count; i++) samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 16);
    }
    SDL_QueueAudio(device_, convert_buffer_, convertedSamples * bytesPerSample);

    if (std::isnan(ptsSeconds)) return;

    // Where is playback right now, in stream time? Everything we've
    // queued up to and including this frame ends at ptsEnd; SDL still
    // holds `queued` seconds of that, plus roughly one device buffer
    // that it has already pulled from the queue but not finished
    // playing. So the speakers are at ptsEnd - queued - deviceLatency.
    double ptsEnd = ptsSeconds + (double)convertedSamples / out_sample_rate_;
    double queued = SDL_GetQueuedAudioSize(device_) / bytesPerSecond();
    double deviceLatency = (double)device_buffer_samples_ / out_sample_rate_;
    double estimate = ptsEnd - queued - deviceLatency;

    uint32_t nowMs = SDL_GetTicks();
    std::lock_guard<std::mutex> lock(clock_mtx_);
    if (paused_) {
        // Nothing is being played, so the queue says nothing new about
        // where playback is; keep the frozen clock.
        if (!clock_valid_) { clock_pts_ = ptsSeconds; clock_valid_ = true; }
        return;
    }
    if (!started_ || !clock_valid_) {
        // Not playing yet: the clock sits at the first frame's start
        // time until start() lets playback begin.
        if (!clock_valid_) clock_pts_ = ptsSeconds;
        clock_valid_ = true;
        clock_wall_ms_ = nowMs;
        return;
    }

    // SDL's queued-size only changes in whole device buffers (~85 ms at
    // 4096 samples / 48 kHz), so `estimate` is jittery even though
    // playback itself is smooth. Extrapolate our own clock with wall time
    // and only nudge it toward the estimate, snapping outright when the
    // two genuinely disagree (a stall, or the very first update).
    double extrapolated = clock_pts_ + (nowMs - clock_wall_ms_) / 1000.0;
    double diff = estimate - extrapolated;
    if (std::fabs(diff) > 0.25) {
        clock_pts_ = estimate;
    } else {
        clock_pts_ = extrapolated + diff * 0.1;
    }
    clock_wall_ms_ = nowMs;
}

uint32_t AudioOutput::queuedBytes() const {
    if (device_ == 0) return 0;
    return SDL_GetQueuedAudioSize(device_);
}

double AudioOutput::queuedSeconds() const {
    return queuedBytes() / bytesPerSecond();
}

bool AudioOutput::hasClock() const {
    std::lock_guard<std::mutex> lock(clock_mtx_);
    return clock_valid_;
}

double AudioOutput::clockSeconds() const {
    std::lock_guard<std::mutex> lock(clock_mtx_);
    if (!clock_valid_) return NAN;
    if (!started_ || paused_) return clock_pts_;
    return clock_pts_ + (SDL_GetTicks() - clock_wall_ms_) / 1000.0;
}

void AudioOutput::shutdown() {
    if (swr_ctx_) { swr_free(&swr_ctx_); }
    if (convert_buffer_) { av_freep(&convert_buffer_); convert_buffer_capacity_samples_ = 0; }
    if (device_ != 0) { SDL_CloseAudioDevice(device_); device_ = 0; }
    started_ = false;
    clock_valid_ = false;
    paused_ = false;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
