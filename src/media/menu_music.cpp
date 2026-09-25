#include "menu_music.h"

#include "audio_output.h"
#include "decoder.h"
#include "http_stream_io.h"

#include <SDL2/SDL.h>
#include <chrono>

MenuMusic::MenuMusic() = default;

MenuMusic::~MenuMusic() { stop(); }

bool MenuMusic::start(const std::string& host, int port, std::function<std::string()> nextPath, float volume) {
    stop();
    // Opened here, on the caller's thread (see the header). Jellyfin's AAC
    // transcode decodes to 48 kHz stereo float planar; the worker adjusts
    // the resampler if a stream turns out different.
    audio_.reset(new AudioOutput());
    if (!audio_->init(48000, 2, AV_SAMPLE_FMT_FLTP)) {
        audio_->shutdown();
        audio_.reset();
        return false;
    }
    audio_->setVolume(volume);
    stop_ = false;
    loops_ = 0;
    thread_ = std::thread([this, host, port, nextPath] { run(host, port, nextPath); });
    return true;
}

void MenuMusic::stop() {
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    if (audio_) {
        audio_->shutdown(); // closes the device on this (main) thread
        audio_.reset();
    }
}

bool MenuMusic::waitFor(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stop_.load(); });
    return !stop_;
}

void MenuMusic::run(std::string host, int port, std::function<std::string()> nextPath) {
    AudioOutput& audio = *audio_;
    int failures = 0;
    bool started = false;
    while (!stop_) {
        loops_++;
        HttpStreamIO io(host, port, nextPath ? nextPath() : std::string());
        Decoder decoder;
        bool ok = io.open() && decoder.open(io.avioContext()) && decoder.hasAudio();
        if (ok && !audio.matchesSource(decoder.audioSampleRate(), decoder.audioChannels(), decoder.audioSampleFormat())) {
            ok = audio.setSourceFormat(decoder.audioSampleRate(), decoder.audioChannels(), decoder.audioSampleFormat());
        }
        if (!ok) {
            // Server down, song deleted...: retry, but back off.
            failures++;
            if (!waitFor(failures > 3 ? 30000 : 5000)) break;
            continue;
        }
        failures = 0;

        while (!stop_) {
            if (audio.queuedSeconds() < 1.0) {
                AVFrame* frame = nullptr;
                if (decoder.decodeAudioFrame(&frame) == DecodedFrameType::AUDIO) {
                    audio.queueFrame(frame, decoder.frameTimeSeconds(DecodedFrameType::AUDIO, frame));
                    if (!started && audio.queuedSeconds() > 0.3) {
                        audio.start();
                        started = true;
                    }
                    continue;
                }
                if (decoder.finished()) {
                    // Song over: let the tail play, then loop (the device
                    // stays open between loops).
                    if (!started) {
                        audio.start();
                        started = true;
                    }
                    while (!stop_ && audio.queuedBytes() > 0) SDL_Delay(40);
                    break;
                }
                if (!decoder.hasQueuedAudioPackets() && !decoder.demuxFinished()) {
                    decoder.readPacket();
                    continue;
                }
            }
            SDL_Delay(20);
        }
    }
    // No SDL device calls here: stop() closes the device on the main thread.
}
