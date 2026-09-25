// Background music for the menus: one song from the user's own library,
// streamed and looped quietly while the UI keeps running.
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

class MenuMusic {
public:
    MenuMusic();
    ~MenuMusic();

    // Main thread. Opens the audio device and starts looping. `nextPath`
    // is asked for the request path before every loop (so each gets a
    // fresh play session). Restarts if already playing. False if the
    // audio device couldn't be opened.
    bool start(const std::string& host, int port, std::function<std::string()> nextPath, float volume = 0.35f);

    // Main thread. Stops the worker (normally well under a second), then
    // closes the audio device.
    void stop();

    bool running() const { return thread_.joinable(); }
    int loopsStarted() const { return loops_; }

private:
    void run(std::string host, int port, std::function<std::string()> nextPath);
    bool waitFor(int ms); // sleeps unless stopped; returns false if stopping

    std::unique_ptr<AudioOutput> audio_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> loops_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
};
