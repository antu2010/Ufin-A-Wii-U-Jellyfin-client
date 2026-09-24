// Ufin file-based logging fallback.
//
// WHBLogUdpInit()'s UDP broadcast log doesn't reach every dev PC -- Windows
// Firewall, AP client isolation, VLANs and some routers all silently drop
// it, with no error on either end. This writes the exact same OSReport
// output to a plain text file on the SD card instead, which needs no
// network at all: just pull the file off the card afterwards.
//
// UfinLogOpen() truncates and opens the file; every OSReport(...) call in
// the codebase is redirected to UfinLog(...) below (same printf-style
// signature) via the macro at the bottom of this header, which both keeps
// calling the real OSReport (so UDP/console logging still works when it
// does work) and appends to the file, flushing after every line so a hang
// or crash doesn't lose the last few lines. UfinLogClose() closes it.
#pragma once

#include <coreinit/debug.h>

#ifdef __cplusplus
extern "C" {
#endif

void UfinLogOpen();
void UfinLogClose();
void UfinLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

// Redirect OSReport call sites to UfinLog after this header is included.
// This only affects source files that include ufin_log.h.
#define OSReport UfinLog
