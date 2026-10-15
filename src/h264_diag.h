// Diagnostics only: human-readable summaries of H.264 parameter sets and
// packets, for the log. Nothing here changes what is decoded or played.
//
// Why: a burned-in-subtitle stream closed the emulator while the same picture
// size without subtitles played. These lines show what the server's two streams
// actually differ in (SPS/PPS fields, the first packets' NAL units) without
// guessing, and are printed *before* the decoder is handed the data, so the last
// line in a log that stops tells where it stopped.
//
// Pure functions, no I/O, no FFmpeg: host-testable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace h264diag {

// Bit reader over an RBSP (emulation-prevention bytes already removed).
class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return !fail_; }
    uint32_t bit() {
        if (pos_ >= n_ * 8) { fail_ = true; return 0; }
        uint32_t b = (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        pos_++;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (!fail_ && bit() == 0) {
            if (++zeros > 31) { fail_ = true; return 0; }
        }
        if (fail_) return 0;
        return ((1u << zeros) - 1) + bits(zeros);
    }
    int32_t se() {
        uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }

private:
    const uint8_t* d_;
    size_t n_;
    size_t pos_ = 0;
    bool fail_ = false;
};

inline std::vector<uint8_t> unescape(const uint8_t* d, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && d[i] == 0x03) { zeros = 0; continue; }
        out.push_back(d[i]);
        zeros = d[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

inline std::string hex(const uint8_t* d, size_t n, size_t maxBytes = 96) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n && i < maxBytes; i++) {
        snprintf(b, sizeof(b), "%02x", d[i]);
        s += b;
    }
    if (n > maxBytes) s += "...";
    return s;
}

// One-line summary of an SPS NAL unit (starting at the NAL header byte).
inline std::string describeSps(const uint8_t* nal, size_t n) {
    if (n < 5) return "sps too short";
    std::vector<uint8_t> r = unescape(nal + 1, n - 1);
    BitReader br(r.data(), r.size());
    const uint32_t profile = br.bits(8);
    const uint32_t constraints = br.bits(8);
    const uint32_t level = br.bits(8);
    const uint32_t spsId = br.ue();
    char buf[320];
    std::string extra;
    const bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
                      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
                      profile == 139 || profile == 134 || profile == 135;
    if (high) {
        uint32_t chroma = br.ue();
        if (chroma == 3) br.bit();
        br.ue(); br.ue(); // bit depths
        br.bit();         // qpprime_y_zero_transform_bypass
        if (br.bit()) return "sps with scaling matrices (not decoded here)";
        snprintf(buf, sizeof(buf), " chroma_format=%u", chroma);
        extra = buf;
    }
    br.ue(); // log2_max_frame_num_minus4
    const uint32_t pocType = br.ue();
    if (pocType == 0) {
        br.ue();
    } else if (pocType == 1) {
        br.bit(); br.se(); br.se();
        uint32_t cycle = br.ue();
        for (uint32_t i = 0; i < cycle && br.ok() && i < 255; i++) br.se();
    }
    const uint32_t numRef = br.ue();
    br.bit(); // gaps_in_frame_num_allowed
    const uint32_t wMbs = br.ue() + 1;
    const uint32_t hMapUnits = br.ue() + 1;
    const uint32_t frameMbsOnly = br.bit();
    if (!frameMbsOnly) br.bit(); // mb_adaptive_frame_field
    const uint32_t direct8x8 = br.bit();
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    const uint32_t crop = br.bit();
    if (crop) { cl = br.ue(); cr = br.ue(); ct = br.ue(); cb = br.ue(); }
    const uint32_t vui = br.bit();
    if (!br.ok()) return "sps truncated";
    snprintf(buf, sizeof(buf),
             "profile=%u constraints=0x%02x level=%u sps_id=%u%s poc_type=%u num_ref_frames=%u coded=%ux%u mbs "
             "frame_mbs_only=%u direct_8x8=%u crop(l,r,t,b)=%u,%u,%u,%u vui=%u",
             profile, constraints, level, spsId, extra.c_str(), pocType, numRef, wMbs,
             hMapUnits * (frameMbsOnly ? 1 : 2), frameMbsOnly, direct8x8, cl, cr, ct, cb, vui);
    return buf;
}

// One-line summary of a PPS NAL unit.
inline std::string describePps(const uint8_t* nal, size_t n) {
    if (n < 3) return "pps too short";
    std::vector<uint8_t> r = unescape(nal + 1, n - 1);
    BitReader br(r.data(), r.size());
    const uint32_t ppsId = br.ue();
    const uint32_t spsId = br.ue();
    const uint32_t cabac = br.bit();
    const uint32_t bottomFieldPoc = br.bit();
    const uint32_t sliceGroups = br.ue() + 1;
    if (sliceGroups > 1) return "pps with slice groups (FMO)";
    const uint32_t l0 = br.ue() + 1;
    const uint32_t l1 = br.ue() + 1;
    const uint32_t weightedP = br.bit();
    const uint32_t weightedB = br.bits(2);
    if (!br.ok()) return "pps truncated";
    char buf[200];
    snprintf(buf, sizeof(buf), "pps_id=%u sps_id=%u entropy=%s bottom_field_poc=%u ref_idx_default=%u/%u weighted_p=%u weighted_b=%u",
             ppsId, spsId, cabac ? "CABAC" : "CAVLC", bottomFieldPoc, l0, l1, weightedP, weightedB);
    return buf;
}

// Describes an avcC record (the mp4 decoder configuration): length-field size,
// every SPS and PPS in it.
inline std::string describeAvcC(const uint8_t* d, size_t n) {
    if (n < 7 || d[0] != 1) return "not an avcC record";
    std::string s = "avcC lengthSize=" + std::to_string((d[4] & 3) + 1);
    size_t p = 5;
    const unsigned nSps = d[p++] & 0x1f;
    for (unsigned i = 0; i < nSps; i++) {
        if (p + 2 > n) return s + " (truncated)";
        size_t len = ((size_t)d[p] << 8) | d[p + 1];
        p += 2;
        if (p + len > n) return s + " (truncated)";
        s += "\n    SPS[" + std::to_string(len) + " B " + hex(d + p, len, 40) + "]: " + describeSps(d + p, len);
        p += len;
    }
    if (p >= n) return s;
    const unsigned nPps = d[p++];
    for (unsigned i = 0; i < nPps; i++) {
        if (p + 2 > n) return s + " (truncated)";
        size_t len = ((size_t)d[p] << 8) | d[p + 1];
        p += 2;
        if (p + len > n) return s + " (truncated)";
        s += "\n    PPS[" + std::to_string(len) + " B " + hex(d + p, len, 40) + "]: " + describePps(d + p, len);
        p += len;
    }
    return s;
}

// NAL unit types of a length-prefixed (mp4) packet, e.g. "6:612 5:48211" meaning
// an SEI of 612 bytes then an IDR slice of 48211. At most maxNals entries.
inline std::string describePacketNals(const uint8_t* d, size_t n, int lengthSize, int maxNals = 6) {
    if (lengthSize < 1 || lengthSize > 4) return "?";
    std::string s;
    size_t p = 0;
    int count = 0;
    while (p + (size_t)lengthSize < n) {
        size_t len = 0;
        for (int i = 0; i < lengthSize; i++) len = (len << 8) | d[p + i];
        p += (size_t)lengthSize;
        if (len == 0 || p + len > n) {
            s += s.empty() ? "bad-length" : " bad-length";
            break;
        }
        if (count++ >= maxNals) { s += " ..."; break; }
        char b[40];
        snprintf(b, sizeof(b), "%s%u:%zu", s.empty() ? "" : " ", (unsigned)(d[p] & 0x1f), len);
        s += b;
        p += len;
    }
    return s;
}


// ---------------------------------------------------------------------------
// Coded picture size from the SPS (not diagnostics: the decoder open path uses it).
//
// The container (mp4 stsd) only knows the *visible* size. The hardware decoder
// writes the *coded* picture: macroblock rows, before the SPS cropping. They are
// the same height when the encoder pads to 16 (what a plain transcode gives:
// 1920x804 coded as 816, cropped by 12), but the burned-in-subtitle transcode of
// the same file came back coded 1920x832 (52 macroblock rows), cropped by 28.
// h264_wiiu places the chroma plane after align16(visible height) rows, i.e.
// 816, so for that stream the hardware and the decoder wrapper disagree.
struct SpsGeometry {
    bool ok = false;
    int codedWidth = 0, codedHeight = 0;                 // macroblock-aligned size
    int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0; // luma samples
};

// avcC record (as in the mp4 extradata) -> geometry of its first SPS. ok=false
// for anything it does not fully understand (High-profile scaling matrices,
// interlaced, odd chroma formats, truncated data): callers then change nothing.
inline SpsGeometry spsGeometry(const uint8_t* avcC, size_t n) {
    SpsGeometry g;
    if (!avcC || n < 12 || avcC[0] != 1 || (avcC[5] & 0x1f) < 1) return g;
    const size_t len = ((size_t)avcC[6] << 8) | avcC[7];
    if (len < 5 || 8 + len > n) return g;
    const uint8_t* nal = avcC + 8;
    if ((nal[0] & 0x1f) != 7) return g;
    std::vector<uint8_t> r = unescape(nal + 1, len - 1);
    BitReader br(r.data(), r.size());
    const uint32_t profile = br.bits(8);
    br.bits(16); // constraint flags, level
    br.ue();     // sps id
    uint32_t chroma = 1;
    const bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
                      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
                      profile == 139 || profile == 134 || profile == 135;
    if (high) {
        chroma = br.ue();
        if (chroma == 3) br.bit();
        br.ue(); br.ue();
        br.bit();
        if (br.bit()) return g; // scaling matrices: not parsed
    }
    if (chroma != 1) return g;  // crop units below assume 4:2:0
    br.ue();                    // log2_max_frame_num_minus4
    const uint32_t pocType = br.ue();
    if (pocType == 0) {
        br.ue();
    } else if (pocType == 1) {
        br.bit(); br.se(); br.se();
        uint32_t cycle = br.ue();
        for (uint32_t i = 0; i < cycle && br.ok() && i < 255; i++) br.se();
    } else if (pocType != 2) {
        return g;
    }
    br.ue();  // num_ref_frames
    br.bit(); // gaps_in_frame_num_allowed
    const uint32_t wMbs = br.ue() + 1;
    const uint32_t hMapUnits = br.ue() + 1;
    if (!br.bit()) return g; // not frame_mbs_only (interlaced)
    br.bit();                // direct_8x8_inference
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br.bit()) { cl = br.ue(); cr = br.ue(); ct = br.ue(); cb = br.ue(); }
    if (!br.ok() || wMbs == 0 || wMbs > 512 || hMapUnits == 0 || hMapUnits > 512) return g;
    g.codedWidth = (int)wMbs * 16;
    g.codedHeight = (int)hMapUnits * 16;
    g.cropLeft = (int)cl * 2; g.cropRight = (int)cr * 2; g.cropTop = (int)ct * 2; g.cropBottom = (int)cb * 2;
    if (g.cropLeft + g.cropRight >= g.codedWidth || g.cropTop + g.cropBottom >= g.codedHeight) return g;
    g.ok = true;
    return g;
}

// The height to give h264_wiiu when it differs from what it would work out by
// itself (align16 of the visible height), or 0 = leave everything as it is.
// Only returned for the exact situation that is wrong today: the picture is
// cropped at the bottom only, the width agrees, and the coded height is not the
// 16-aligned visible height. Every stream that already works returns 0.
inline int hardwareHeightOverride(const SpsGeometry& g, int visibleWidth, int visibleHeight) {
    if (!g.ok || visibleWidth <= 0 || visibleHeight <= 0) return 0;
    if (g.codedWidth != visibleWidth || g.cropLeft != 0 || g.cropRight != 0 || g.cropTop != 0) return 0;
    if (g.codedHeight - g.cropBottom != visibleHeight) return 0; // the SPS and the container disagree: leave it
    const int aligned = (visibleHeight + 15) & ~15;
    if (g.codedHeight == aligned) return 0;                      // already what h264_wiiu assumes
    if (g.codedHeight < visibleHeight) return 0;
    return g.codedHeight;
}

} // namespace h264diag
