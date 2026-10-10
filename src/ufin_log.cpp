#include "ufin_log.h" // pulls in the OSReport -> UfinLog #define; harmless here,
                      // nothing in this file calls OSReport itself.

#include <cstdio>
#include <cstdarg>
#include <cstring>

// Same SD mount convention already used by config_loader.cpp.
#ifndef UFIN_LOG_PATH // overridden by the host test
#define UFIN_LOG_PATH "/vol/external01/wiiu/apps/ufin/ufin_log.txt"
#endif
static const char* LOG_PATH = UFIN_LOG_PATH;

static FILE* g_logFile = nullptr;

// Before the config is read nobody knows whether logging is wanted: keep the early lines here.
enum LogState { LOG_PENDING, LOG_ON, LOG_OFF };
static LogState g_state = LOG_PENDING;
static char g_pending[8192];
static size_t g_pendingLen = 0;

void UfinLogSetEnabled(int on) {
    if (on) {
        if (!g_logFile) {
            // "w" truncates -- each run starts a fresh file rather than growing
            // forever across runs. If the directory doesn't exist (e.g. config.json
            // was never placed there either) this just silently stays file-less
            // and UfinLog() below falls back to OSReport only.
            g_logFile = fopen(LOG_PATH, "w");
            if (g_logFile) {
                fputs("Ufin log start\n", g_logFile);
                if (g_pendingLen > 0) fwrite(g_pending, 1, g_pendingLen, g_logFile);
                fflush(g_logFile);
            }
        }
        g_state = LOG_ON;
    } else {
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
        g_state = LOG_OFF;
    }
    g_pendingLen = 0;
}

void UfinLogOpen() { UfinLogSetEnabled(1); }

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
    } else if (g_state == LOG_PENDING) {
        // Startup only (single-threaded): remember the line until the config says yes or no.
        size_t n = strlen(buf);
        if (g_pendingLen + n < sizeof(g_pending)) {
            memcpy(g_pending + g_pendingLen, buf, n);
            g_pendingLen += n;
        }
    }
}
