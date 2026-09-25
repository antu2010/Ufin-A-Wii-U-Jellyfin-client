#include "SDL2/SDL.h"

#include <chrono>
#include <thread>
#include <vector>

static Uint32 g_ticks = 0;
static Uint32 g_queued = 0;
static Uint32 g_totalQueued = 0;
static bool g_paused = true;
static bool g_open = false;
static std::vector<unsigned char> g_last;
static std::thread::id g_openThread, g_closeThread;
static int g_openDevices = 0;

// "Real playback" mode (fake_sdl_set_realtime): ticks follow the wall
// clock, SDL_Delay really sleeps, and an unpaused device drains its queue
// at 48 kHz stereo S16 -- so the real Player loop can run against it.
static bool g_realtime = false;
static std::chrono::steady_clock::time_point g_rtStart, g_rtLastDrain;
static void drain() {
    if (!g_realtime) return;
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - g_rtLastDrain).count();
    g_rtLastDrain = now;
    if (g_open && !g_paused) {
        Uint32 bytes = (Uint32)(ms * 192.0) & ~3u;
        g_queued = bytes >= g_queued ? 0 : g_queued - bytes;
    }
}

extern "C" {
int SDL_InitSubSystem(Uint32) { return 0; }
void SDL_QuitSubSystem(Uint32) {}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained,
                                      int) {
    if (obtained) *obtained = *desired;
    g_openThread = std::this_thread::get_id();
    g_openDevices++;
    g_open = true;
    g_paused = true;
    g_queued = 0;
    return 2;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID) {
    g_closeThread = std::this_thread::get_id();
    g_openDevices--;
    g_open = false;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on) { g_paused = pause_on != 0; }

int SDL_QueueAudio(SDL_AudioDeviceID, const void* data, Uint32 len) {
    drain();
    g_last.assign((const unsigned char*)data, (const unsigned char*)data + len);
    g_queued += len;
    g_totalQueued += len;
    return 0;
}

Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID) { drain(); return g_queued; }
const char* SDL_GetError(void) { return "fake sdl"; }
Uint32 SDL_GetTicks(void) {
    if (g_realtime) {
        drain();
        return (Uint32)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_rtStart).count();
    }
    return g_ticks;
}
void SDL_Delay(Uint32 ms) {
    if (g_realtime) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    else g_ticks += ms;
}
}

void fake_sdl_advance_ticks(Uint32 ms) { g_ticks += ms; }
void fake_sdl_consume_audio(Uint32 bytes) { g_queued = bytes >= g_queued ? 0 : g_queued - bytes; }
bool fake_sdl_device_paused() { return g_paused; }
Uint32 fake_sdl_total_queued() { return g_totalQueued; }
const unsigned char* fake_sdl_last_queued(Uint32* len) {
    if (len) *len = (Uint32)g_last.size();
    return g_last.data();
}
std::thread::id fake_sdl_open_thread() { return g_openThread; }
std::thread::id fake_sdl_close_thread() { return g_closeThread; }
int fake_sdl_open_devices() { return g_openDevices; }
void fake_sdl_set_realtime(bool on) {
    g_realtime = on;
    g_rtStart = g_rtLastDrain = std::chrono::steady_clock::now();
}
