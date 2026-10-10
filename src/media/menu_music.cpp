#include "menu_music.h"

#include "audio_output.h"
#include "decoder.h"
#include "file_stream_io.h"
#include "http_stream_io.h"
#include "../ufin_log.h"

#include <SDL2/SDL.h>
#include <chrono>

MenuMusic::MenuMusic() = default;

MenuMusic::~MenuMusic() { stop(); }

bool MenuMusic::openDevice(float volume) {
    // Opened here, on the caller's thread (see the header). Jellyfin's AAC
    // transcode decodes to 48 kHz stereo float planar; the worker adjusts
    // the resampler if a stream turns out different (an MP3 is 44.1 kHz s16).
    audio_.reset(new AudioOutput());
    if (!audio_->init(48000, 2, AV_SAMPLE_FMT_FLTP)) {
        audio_->shutdown();
        audio_.reset();
        return false;
    }
    audio_->setVolume(volume);
    stop_ = false;
    gaveUp_ = false;
    loops_ = 0;
    return true;
}

bool MenuMusic::start(const std::string& host, int port, std::function<std::string()> nextPath, float volume) {
    OSReport("Ufin: DBG MenuMusic::start (stream)\n");
    stop();
    if (!openDevice(volume)) return false;
    thread_ = std::thread([this, host, port, nextPath] { run(host, port, nextPath); });
    return true;
}

bool MenuMusic::startFile(const std::string& path, float volume) {
    OSReport("Ufin: DBG MenuMusic::startFile %s\n", path.c_str());
    stop();
    if (!openDevice(volume)) return false;
    thread_ = std::thread([this, path] { runFile(path); });
    return true;
}

void MenuMusic::stop() {
    OSReport("Ufin: DBG MenuMusic::stop thread=%d audio=%d\n", (int)thread_.joinable(), (int)(audio_ != nullptr));
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
        OSReport("Ufin: DBG MenuMusic thread joined\n");
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

bool MenuMusic::prepareAudio(const Decoder& decoder) {
    AudioOutput& audio = *audio_;
    if (!decoder.hasAudio()) return false;
    if (audio.matchesSource(decoder.audioSampleRate(), decoder.audioChannels(), decoder.audioSampleFormat())) return true;
    return audio.setSourceFormat(decoder.audioSampleRate(), decoder.audioChannels(), decoder.audioSampleFormat());
}

void MenuMusic::playSong(Decoder& decoder, bool& started) {
    AudioOutput& audio = *audio_;
    // Progress lines for the SD log: if the app ever stalls with music on, the last of these says how far
    // the music thread got (the thread's own work is bounded: it sleeps whenever a second of audio is queued).
    OSReport("Ufin: menu music: playing (%d Hz, %d ch, sample fmt %d)\n", decoder.audioSampleRate(),
             decoder.audioChannels(), (int)decoder.audioSampleFormat());
    unsigned framesQueued = 0;
    uint32_t lastReport = SDL_GetTicks();
    while (!stop_) {
        if (audio.queuedSeconds() < 1.0) {
            AVFrame* frame = nullptr;
            if (decoder.decodeAudioFrame(&frame) == DecodedFrameType::AUDIO) {
                audio.queueFrame(frame, decoder.frameTimeSeconds(DecodedFrameType::AUDIO, frame));
                if (++framesQueued == 1) OSReport("Ufin: menu music: first frame decoded (%d samples)\n", frame->nb_samples);
                const uint32_t now = SDL_GetTicks();
                if (now - lastReport > 5000) {
                    lastReport = now;
                    OSReport("Ufin: menu music: %u frames queued, %.2f s buffered\n", framesQueued, audio.queuedSeconds());
                }
                // Never run flat out: the decoder is much faster than playback, and this thread must leave
                // the CPU to the render thread (Wii U threads on one core are not time-sliced against each other).
                SDL_Delay(1);
                if (!started && audio.queuedSeconds() > 0.3) {
                    audio.start();
                    started = true;
                    OSReport("Ufin: menu music: audio device started\n");
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
                return;
            }
            if (!decoder.hasQueuedAudioPackets() && !decoder.demuxFinished()) {
                decoder.readPacket();
                SDL_Delay(1);
                continue;
            }
        }
        SDL_Delay(20);
    }
}

void MenuMusic::run(std::string host, int port, std::function<std::string()> nextPath) {
    int failures = 0;
    bool started = false;
    while (!stop_) {
        loops_++;
        HttpStreamIO io(host, port, nextPath ? nextPath() : std::string());
        Decoder decoder;
        bool ok = io.open() && decoder.open(io.avioContext()) && prepareAudio(decoder);
        if (!ok) {
            // Server down, song deleted...: retry, but back off.
            failures++;
            if (!waitFor(failures > 3 ? 30000 : 5000)) break;
            continue;
        }
        failures = 0;
        playSong(decoder, started);
    }
    // No SDL device calls here: stop() closes the device on the main thread.
}

void MenuMusic::runFile(std::string path) {
    bool started = false;
    while (!stop_) {
        loops_++;
        FileStreamIO io(path);
        Decoder decoder;
        decoder.setAudioOnly(true); // a bare audio file: ignore cover art, probe the stream parameters
        bool ok = io.open() && decoder.open(io.avioContext()) && prepareAudio(decoder);
        if (!ok) {
            // A missing or undecodable file will not fix itself: stop trying (the caller sees gaveUp()).
            OSReport("Ufin: menu music: can't play %s (%s) -- use AAC in .m4a; an .mp3 needs an FFmpeg build "
                     "with the mp3 demuxer, see docs/ffmpeg-wiiu-build.md\n", path.c_str(),
                     io.lastError().empty() ? "not a decodable audio file" : io.lastError().c_str());
            gaveUp_ = true;
            return;
        }
        playSong(decoder, started);
    }
}
