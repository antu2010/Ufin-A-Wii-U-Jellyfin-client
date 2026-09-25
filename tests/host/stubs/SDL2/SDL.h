// Host stand-in for the parts of SDL2 that media/audio_output.cpp and
// media/player.cpp use. Backed by fake_sdl.cpp: a queue that only drains
// when the test says so, and a clock that only advances when the test
// says so, so clock behaviour can be checked deterministically.
#pragma once
#include <cstdint>

typedef uint32_t Uint32;
typedef uint16_t Uint16;
typedef uint8_t Uint8;
typedef uint32_t SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;

#define SDL_INIT_AUDIO 0x10u
#define AUDIO_S16SYS 0x8010

typedef void (*SDL_AudioCallback)(void*, Uint8*, int);
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void* userdata;
} SDL_AudioSpec;

extern "C" {
int SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                      SDL_AudioSpec* obtained, int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
int SDL_QueueAudio(SDL_AudioDeviceID dev, const void* data, Uint32 len);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
const char* SDL_GetError(void);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
}

// Test controls (not part of SDL).
void fake_sdl_advance_ticks(Uint32 ms);
void fake_sdl_consume_audio(Uint32 bytes);
bool fake_sdl_device_paused();
Uint32 fake_sdl_total_queued();
const unsigned char* fake_sdl_last_queued(Uint32* len); // bytes of the last SDL_QueueAudio
#ifdef __cplusplus
#include <thread>
std::thread::id fake_sdl_open_thread();  // thread that last opened a device
std::thread::id fake_sdl_close_thread(); // thread that last closed one
int fake_sdl_open_devices();             // currently open
void fake_sdl_set_realtime(bool on);     // wall-clock ticks, real sleeps, device drains in real time
#endif
