#include "ufin_log.h" // pulls in the OSReport -> UfinLog #define; harmless here,
                      // nothing in this file calls OSReport itself.

#include <cstdio>
#include <cstdarg>
#include <cstring>

// Same SD mount convention already used by config_loader.cpp.
static const char* LOG_PATH = "/vol/external01/wiiu/apps/ufin/ufin_log.txt";

static FILE* g_logFile = nullptr;

void UfinLogOpen() {
    // "w" truncates -- each run starts a fresh file rather than growing
    // forever across runs. If the directory doesn't exist (e.g. config.json
    // was never placed there either) this just silently stays file-less
    // and UfinLog() below falls back to OSReport only.
    g_logFile = fopen(LOG_PATH, "w");
    if (g_logFile) {
        fputs("Ufin log start\n", g_logFile);
        fflush(g_logFile);
    }
}

void UfinLogClose() {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void UfinLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Undo the OSReport -> UfinLog #define for this one call so we still
    // reach the real OSReport (UDP/console/serial log, when those work).
#undef OSReport
    OSReport("%s", buf);
#define OSReport UfinLog

    if (g_logFile) {
        fputs(buf, g_logFile);
        // Most OSReport call sites already end their format string with
        // "\n"; if one doesn't, the file still reads fine without a
        // guaranteed trailing newline per record -- less important than
        // keeping this file matching OSReport's own behaviour exactly.
        fflush(g_logFile); // flush every line: a hang shouldn't cost us the last lines
    }
}
