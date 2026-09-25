// Pure pieces behind pause/skip and search: where a skip lands, how
// stream timestamps map to item positions after a seek, and the UTF-16
// -> UTF-8 conversion of keyboard input.
#include "check.h"
#include "seek.h"
#include "text_utf.h"

#include <cmath>

int main() {
    // clampSeek
    CHECK_NEAR(clampSeek(100, 30, 3600), 130, 1e-9);
    CHECK_NEAR(clampSeek(100, -10, 3600), 90, 1e-9);
    CHECK_NEAR(clampSeek(4, -10, 3600), 0, 1e-9);        // never before the start
    CHECK_NEAR(clampSeek(3580, 30, 3600), 3595, 1e-9);   // stays short of the end
    CHECK_NEAR(clampSeek(100, 90, 0), 190, 1e-9);        // unknown duration: no upper limit
    CHECK_NEAR(clampSeek(1, 30, 3), 0, 1e-9);            // tiny item

    // itemPositionFromStreamTime
    CHECK_NEAR(itemPositionFromStreamTime(12.5, 0, 0), 12.5, 1e-9);          // no seek
    CHECK_NEAR(itemPositionFromStreamTime(2.0, 600, 0.0), 602.0, 1e-9);      // stream restarts at 0
    CHECK_NEAR(itemPositionFromStreamTime(602.0, 600, 600.1), 602.0, 1e-9);  // copyts: already absolute
    CHECK_NEAR(itemPositionFromStreamTime(2.0, 600, NAN), 602.0, 1e-9);      // before the first timestamp
    CHECK(std::isnan(itemPositionFromStreamTime(NAN, 600, 0)));

    // makePlaySessionId: 32 hex chars, different for every call.
    {
        std::string a = makePlaySessionId(1000, 1), b = makePlaySessionId(1000, 2), c = makePlaySessionId(1001, 1);
        CHECK_EQ((int)a.size(), 32);
        CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(a != b);
        CHECK(a != c);
        CHECK(b != c);
        CHECK_STR(makePlaySessionId(1000, 1), a); // deterministic for the same input
    }

    // utf16ToUtf8
    CHECK_STR(utf16ToUtf8(u"matrix"), "matrix");
    CHECK_STR(utf16ToUtf8(u"citt\u00e0"), "citt\xc3\xa0");            // 2-byte
    CHECK_STR(utf16ToUtf8(u"\u266a"), "\xe2\x99\xaa");                 // 3-byte
    CHECK_STR(utf16ToUtf8(u"\U0001F600"), "\xf0\x9f\x98\x80");         // surrogate pair -> 4-byte
    const char16_t lone[] = {0xD800, u'a', 0};
    CHECK_STR(utf16ToUtf8(lone), "\xef\xbf\xbd" "a");                  // lone surrogate -> U+FFFD
    CHECK_STR(utf16ToUtf8(nullptr), "");
    CHECK(utf8ToUtf16("abc") == u"abc");
    CHECK(utf8ToUtf16("citt\xc3\xa0") == u"citt\u00e0");
    CHECK(utf8ToUtf16("\xf0\x9f\x98\x80") == u"\U0001F600");
    CHECK(utf8ToUtf16("a\xff" "b") == u"a\uFFFD" u"b");
    CHECK_STR(utf16ToUtf8(utf8ToUtf16("192.168.1.100:8096").c_str()), "192.168.1.100:8096");
    CHECK_STR(trimSpaces("  star wars  "), "star wars");
    CHECK_STR(trimSpaces("   "), "");

    return check::finish("test_playback_controls");
}
