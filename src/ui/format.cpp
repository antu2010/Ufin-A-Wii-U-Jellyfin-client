#include "layout.h"

#include <cmath>
#include <cstdio>

namespace ui {

std::string formatTime(double seconds) {
    if (!(seconds >= 0.0) || std::isinf(seconds)) seconds = 0.0;
    long long total = (long long)(seconds + 0.5);
    long long hh = total / 3600, mm = (total / 60) % 60, ss = total % 60;
    char buf[32];
    if (hh > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", hh, mm, ss);
    else snprintf(buf, sizeof(buf), "%lld:%02lld", mm, ss);
    return buf;
}

} // namespace ui
