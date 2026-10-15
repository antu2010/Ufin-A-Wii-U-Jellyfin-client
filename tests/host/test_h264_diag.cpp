// Diagnostics helpers (h264_diag.h): SPS/PPS/packet summaries for the log.
// Reference bytes come from a real x264 Main-profile 1920x804 stream
// (ffmpeg -c:v libx264 -profile:v main -level 4.1 -bf 2), mp4 avcC record.
#include "check.h"
#include "h264_diag.h"

#include <cstdlib>
#include <string>
#include <vector>

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    static const uint8_t avcC[] = {
        0x01, 0x4d, 0x40, 0x29, 0xff, 0xe1, 0x00, 0x1a, 0x67, 0x4d, 0x40, 0x29, 0xec, 0xa0, 0x3c, 0x03, 0x3f, 0xcf, 0x80,
        0x88, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0x80, 0x78, 0xc1, 0x8c, 0xb0, 0x01, 0x00, 0x05, 0x68,
        0xeb, 0xe3, 0xcb, 0x20};
    std::string d = h264diag::describeAvcC(avcC, sizeof(avcC));
    printf("%s\n", d.c_str());
    CHECK(has(d, "lengthSize=4"));
    CHECK(has(d, "profile=77"));
    CHECK(has(d, "level=41"));
    CHECK(has(d, "frame_mbs_only=1"));
    CHECK(has(d, "coded=120x51 mbs"));          // 1920x816 coded...
    CHECK(has(d, "crop(l,r,t,b)=0,0,0,6"));     // ...cropped to 804 (6 chroma rows = 12 luma rows)
    CHECK(has(d, "entropy=CABAC"));
    CHECK(has(d, "sps_id=0"));

    // Not an avcC, truncated, empty: described, never read out of bounds.
    CHECK(has(h264diag::describeAvcC(avcC, 3), "not an avcC"));
    CHECK(has(h264diag::describeAvcC(avcC, 12), "truncated"));
    CHECK(has(h264diag::describeSps(avcC + 8, 2), "too short"));

    // Packet NAL listing: SEI(6, 5 B) then IDR(5, 8 B), 4-byte lengths.
    static const uint8_t pkt[] = {0, 0, 0, 5, 0x06, 1, 2, 3, 4, 0, 0, 0, 8, 0x65, 1, 2, 3, 4, 5, 6, 7};
    CHECK_EQ(h264diag::describePacketNals(pkt, sizeof(pkt), 4), std::string("6:5 5:8"));
    CHECK(has(h264diag::describePacketNals(pkt, 7, 4), "bad-length"));   // length runs past the end
    CHECK(has(h264diag::describePacketNals(pkt, sizeof(pkt), 7), "?"));  // impossible length size
    CHECK_EQ(h264diag::hex(avcC, 4), std::string("014d4029"));
    CHECK(has(h264diag::hex(avcC, sizeof(avcC), 4), "..."));

    // --- Coded size from the SPS: the two real avcC records from the Cemu log (same 1920x804 episode). ---
    auto fromHex = [](const char* h) {
        std::vector<uint8_t> v;
        for (const char* p = h; p[0] && p[1]; p += 2) { char b[3] = {p[0], p[1], 0}; v.push_back((uint8_t)strtol(b, 0, 16)); }
        return v;
    };
    // Plain transcode: coded 1920x816, cropped to 804 -- h264_wiiu already copes, nothing may change.
    auto plain = fromHex("014d0029ffe10028274d002996c803c033fcf80a1010101400000fa40002ee03a10005b8c0000b71b0bdef83b438632401000428ee3c80");
    // Burned-in subtitles: coded 1920x832, cropped to 804 -- the stream that closed Cemu.
    auto burned = fromHex("014d0029ffe10029274d002996c803c034fc7e028404040500000303e90000bb80e840016e300002dc6c2f7be0ed0e18c901000428ee3c80");
    h264diag::SpsGeometry gp = h264diag::spsGeometry(plain.data(), plain.size());
    h264diag::SpsGeometry gb = h264diag::spsGeometry(burned.data(), burned.size());
    CHECK(gp.ok && gb.ok);
    CHECK_EQ(gp.codedWidth, 1920); CHECK_EQ(gp.codedHeight, 816); CHECK_EQ(gp.cropBottom, 12);
    CHECK_EQ(gb.codedWidth, 1920); CHECK_EQ(gb.codedHeight, 832); CHECK_EQ(gb.cropBottom, 28);
    CHECK_EQ(h264diag::hardwareHeightOverride(gp, 1920, 804), 0);    // unchanged
    CHECK_EQ(h264diag::hardwareHeightOverride(gb, 1920, 804), 832);  // told the real coded height
    CHECK_EQ(h264diag::hardwareHeightOverride(gb, 1920, 800), 0);    // container and SPS disagree: leave alone
    CHECK_EQ(h264diag::hardwareHeightOverride(gb, 1280, 804), 0);    // width disagrees: leave alone
    // Aligned pictures (1280x720 etc.) never change.
    h264diag::SpsGeometry g720; g720.ok = true; g720.codedWidth = 1280; g720.codedHeight = 720;
    CHECK_EQ(h264diag::hardwareHeightOverride(g720, 1280, 720), 0);
    // Anything unparsable changes nothing.
    CHECK(!h264diag::spsGeometry(plain.data(), 9).ok);
    CHECK(!h264diag::spsGeometry(nullptr, 0).ok);
    CHECK_EQ(h264diag::hardwareHeightOverride(h264diag::SpsGeometry(), 1920, 804), 0);
    return check::finish("test_h264_diag");
}
