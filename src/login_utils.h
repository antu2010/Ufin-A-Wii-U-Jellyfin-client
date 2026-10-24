// Small pure helpers for the sign-in screen.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

// What people type as a server address -> host + port.
// Accepts "192.168.1.100", "192.168.1.100:8096", "http://jelly.local:8096/",
// "https://jelly.example.com", "jelly.local/web/index.html" ... Default port 8096 (Jellyfin's own http port); for an
// https:// address 443 (what a reverse proxy listens on -- Jellyfin's own https port, 8920, has to be typed:
// https://192.168.1.100:8920). `https` says which scheme was typed.
inline bool parseServerAddress(const std::string& text, std::string& host, int& port, bool& https, std::string& error) {
    std::string s = text;
    // trim
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    std::string lower;
    for (char c : s) lower += (char)tolower((unsigned char)c);
    https = false;
    if (lower.rfind("https://", 0) == 0) {
        https = true;
        s = s.substr(8);
    } else if (lower.rfind("http://", 0) == 0) {
        s = s.substr(7);
    }
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    port = https ? 443 : 8096;
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

// The address as it is shown, and as it parses back to the same server: plain http omits the default port 8096,
// https keeps its scheme and omits 443.
// The old form: plain http only (an https:// address is refused, as before).
inline bool parseServerAddress(const std::string& text, std::string& host, int& port, std::string& error) {
    bool https = false;
    if (!parseServerAddress(text, host, port, https, error)) return false;
    if (https) {
        error = "this needs the https-aware parser";
        return false;
    }
    return true;
}

inline std::string formatServerAddress(const std::string& host, int port, bool https = false) {
    if (host.empty()) return "";
    if (https) return "https://" + (port == 443 || port <= 0 ? host : host + ":" + std::to_string(port));
    return port == 8096 || port <= 0 ? host : host + ":" + std::to_string(port);
}
