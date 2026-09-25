// Small pure helpers for the sign-in screen.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

// What people type as a server address -> host + port.
// Accepts "192.168.1.100", "192.168.1.100:8096", "http://jelly.local:8096/",
// "jelly.local/web/index.html" ... Default port 8096 (Jellyfin's). https
// isn't supported (no TLS), so it's rejected with a clear reason.
inline bool parseServerAddress(const std::string& text, std::string& host, int& port, std::string& error) {
    std::string s = text;
    // trim
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    std::string lower;
    for (char c : s) lower += (char)tolower((unsigned char)c);
    if (lower.rfind("https://", 0) == 0) {
        error = "https isn't supported -- use the server's http address (usually port 8096)";
        return false;
    }
    if (lower.rfind("http://", 0) == 0) s = s.substr(7);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    port = 8096;
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        std::string p = s.substr(colon + 1);
        s = s.substr(0, colon);
        if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos || p.size() > 5) {
            error = "the port after ':' must be a number";
            return false;
        }
        port = atoi(p.c_str());
        if (port <= 0 || port > 65535) {
            error = "the port must be between 1 and 65535";
            return false;
        }
    }
    if (s.empty()) {
        error = "enter the server's address, e.g. 192.168.1.100";
        return false;
    }
    for (char c : s) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) {
            error = "that doesn't look like a server address";
            return false;
        }
    }
    host = s;
    return true;
}

inline std::string formatServerAddress(const std::string& host, int port) {
    if (host.empty()) return "";
    return port == 8096 || port <= 0 ? host : host + ":" + std::to_string(port);
}
