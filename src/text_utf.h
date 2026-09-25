// UTF-16 -> UTF-8, for text coming back from the Wii U software keyboard
// (nn::swkbd hands out char16_t strings; Jellyfin wants UTF-8).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

inline std::string utf16ToUtf8(const char16_t* in, size_t maxUnits = 4096) {
    std::string out;
    if (!in) return out;
    for (size_t i = 0; i < maxUnits && in[i]; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: needs a following low surrogate.
            if (i + 1 < maxUnits && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
                i++;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD; // stray low surrogate
        }
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// UTF-8 -> UTF-16 (for text handed to the keyboard). Invalid bytes
// become U+FFFD.
inline std::u16string utf8ToUtf16(const std::string& in) {
    std::u16string out;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { out += (char16_t)0xFFFD; i++; continue; }
        if (i + n > in.size()) { out += (char16_t)0xFFFD; break; }
        bool ok = true;
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out += (char16_t)0xFFFD; i++; continue; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

// Trims leading/trailing spaces (swkbd happily returns "  ").
inline std::string trimSpaces(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}
