// Background music for the menus: either a song from the user's own library (streamed from
// Jellyfin) or a local file on the SD card (bgm.m4a), looped quietly while the UI keeps running.
//
// Threading matters on the Wii U: SDL's audio driver sits on AX, whose
// functions must run on CPU core 1. Opening an SDL device is safe from
// any thread (the driver hops to core 1), but closing it calls AXQuit()
// on the caller's core. So the device is opened in start() and closed in
// stop() -- both on the calling (main) thread -- and the worker thread
// only decodes and queues samples, which SDL allows from any thread.
//
// There's one audio device: stop the music before real playback starts.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class AudioOutput;
class Decoder;

class MenuMusic {
public:
    MenuMusic();
    ~MenuMusic();

    // Main thread. Opens the audio device and starts looping. `nextPath`
    // is asked for the request path before every loop (so each gets a
    // fresh play session). Restarts if already playing. False if the
    // audio device couldn't be opened.
    bool start(const std::string& host, int port, std::function<std::string()> nextPath, float volume = 0.35f);

    // Main thread. Same, but loops a local audio file instead of a Jellyfin stream. .m4a (AAC)
    // works with the stock FFmpeg build; .mp3 needs the mp3 demuxer and mpegaudio parser
    // (docs/ffmpeg-wiiu-build.md).
    // If the file can't be opened or decoded the worker gives up (no retry loop): gaveUp()
    // turns true and the caller should stop() and not restart it.
    bool startFile(const std::string& path, float volume = 0.35f);

    // Main thread. Stops the worker (normally well under a second), then
    // closes the audio device.
    void stop();

    bool running() const { return thread_.joinable(); }
    int loopsStarted() const { return loops_; }
    bool gaveUp() const { return gaveUp_; } // local file only: unreadable / undecodable

private:
    bool openDevice(float volume);
    void run(std::string host, int port, std::function<std::string()> nextPath);
    void runFile(std::string path);
    // Points the audio output at this decoder's format; false if it can't be used.
    bool prepareAudio(const Decoder& decoder);
    // Decodes one open song to its end (or until stop), feeding the audio device.
    void playSong(Decoder& decoder, bool& started);
    bool waitFor(int ms); // sleeps unless stopped; returns false if stopping

    std::unique_ptr<AudioOutput> audio_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> loops_{0};
    std::atomic<bool> gaveUp_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
};
