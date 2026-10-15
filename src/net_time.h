// Clock and sleep for the network code. On the Wii U these go through coreinit (the same OSGetTime the rest
// of the app uses) instead of std::chrono / std::this_thread, whose newlib-backed versions are not trustworthy
// under Cemu (std::thread already froze it); on a PC (the host tests) they are the standard ones.
#pragma once
#include <cstdint>

#if defined(__WIIU__)
#include <coreinit/thread.h>
#include <coreinit/time.h>
namespace nettime {
inline int64_t nowMs() { return (int64_t)OSTicksToMilliseconds(OSGetTime()); }
inline void sleepMs(int ms) { OSSleepTicks(OSMillisecondsToTicks(ms)); }
} // namespace nettime
#else
#include <chrono>
#include <thread>
namespace nettime {
inline int64_t nowMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
} // namespace nettime
#endif
