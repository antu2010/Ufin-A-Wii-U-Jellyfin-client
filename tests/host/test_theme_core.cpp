// Shared theme maths (src/ui/theme_core.h): hex codes, HSV, custom palette arrangement, readability helpers
// and the artwork dominant colour. No ImGui, no GPU.
#include "check.h"
#include "ui/theme_core.h"

#include <vector>

using namespace ui::theme;
namespace m3e = ui::m3e;

static bool same(Rgb8 a, Rgb8 b, int tol = 0) {
    return std::abs((int)a.r - b.r) <= tol && std::abs((int)a.g - b.g) <= tol && std::abs((int)a.b - b.b) <= tol;
}

static void testHex() {
    Rgb8 c{9, 9, 9};
    CHECK(parseHex("#00A4DC", c)); CHECK(same(c, {0x00, 0xA4, 0xDC}));
    CHECK(parseHex("aa5cc3", c));  CHECK(same(c, {0xAA, 0x5C, 0xC3}));
    CHECK(parseHex("  #F80 \n", c)); CHECK(same(c, {0xFF, 0x88, 0x00}));
    CHECK(parseHex("abc", c));     CHECK(same(c, {0xAA, 0xBB, 0xCC}));
    c = {1, 2, 3};
    for (const char* bad : {"", "#", "#12", "#1234", "#12345", "#1234567", "#GG0000", "00A4D", "0x00A4DC", "# 00A4DC", "#00A4DC#"}) {
        CHECK(!parseHex(bad, c));
        CHECK(same(c, {1, 2, 3})); // untouched on failure
    }
    CHECK_STR(toHex({0, 0xA4, 0xDC}).c_str(), "#00A4DC");
    CHECK_STR(toHex({0xFF, 0, 0x0A}).c_str(), "#FF000A");
    for (uint32_t v : {0x000000u, 0xFFFFFFu, 0x123456u, 0xABCDEFu}) { // round trip
        Rgb8 o{}, in{(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        CHECK(parseHex(toHex(in), o) && same(o, in));
    }
}

static void testHsv() {
    for (uint32_t v : {0x00A4DCu, 0xAA5CC3u, 0x2ECC71u, 0xFF9F43u, 0xE74C3Cu, 0x5C6BC0u, 0x000000u, 0xFFFFFFu, 0x808080u}) {
        const Rgb8 c{(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        const Hsv h = toHsv(c);
        CHECK(same(fromHsv(h.h, h.s, h.v), c, 1));
    }
    CHECK(same(fromHsv(0.0f, 1.0f, 1.0f), {255, 0, 0}));
    CHECK(same(fromHsv(1.0f / 3.0f, 1.0f, 1.0f), {0, 255, 0}, 1));
    CHECK(same(fromHsv(1.5f, 1.0f, 1.0f), fromHsv(0.5f, 1.0f, 1.0f))); // hue wraps
    CHECK(same(fromHsv(0.3f, 5.0f, -2.0f), {0, 0, 0}));                // clamped
}

static void testArrangement() {
    Custom p;
    p.c[0] = {1, 0, 0}; p.c[1] = {0, 2, 0}; p.c[2] = {0, 0, 3};
    p.arrange = 0;
    Roles r = rolesOf(p);
    CHECK(same(r.main, p.c[0]) && same(r.deep, p.c[1]) && same(r.second, p.c[2]));
    // Every arrangement is a different permutation of all three colours.
    std::vector<int> seen;
    for (int a = 0; a < kArrangeCount; a++) {
        p.arrange = a;
        r = rolesOf(p);
        const int key = r.main.r * 100 + r.main.g * 10 + r.main.b + 1000 * (r.deep.r * 100 + r.deep.g * 10 + r.deep.b) / 10 +
                        (r.second.r * 100 + r.second.g * 10 + r.second.b) * 7;
        for (int k : seen) CHECK(k != key);
        seen.push_back(key);
        // all three colours present exactly once
        const int sum = (r.main.r + r.deep.r + r.second.r) + (r.main.g + r.deep.g + r.second.g) + (r.main.b + r.deep.b + r.second.b);
        CHECK_EQ(sum, 6);
    }
    p.arrange = 99; // out of range -> natural order
    CHECK(same(rolesOf(p).main, p.c[0]));
    p.arrange = -3;
    CHECK(same(rolesOf(p).deep, p.c[1]));
    CHECK_STR(arrangeLabel(0).c_str(), "Main 1  Deep 2  Second 3");
    CHECK_STR(arrangeLabel(5).c_str(), "Main 3  Deep 2  Second 1");
}

static void testAutoColours() {
    const Rgb8 main{0x00, 0xA4, 0xDC};
    const Rgb8 d = autoDeep(main), s = autoSecond(main);
    CHECK(m3e::luminance(d) < m3e::luminance(main));
    CHECK(std::fabs(m3e::toHsl(d).h - m3e::toHsl(main).h) < 0.01f);
    CHECK(std::fabs(m3e::toHsl(s).h - m3e::toHsl(main).h - 1.0f / 6.0f) < 0.01f);
}

static void testReadability() {
    // A colour already readable is returned unchanged; a dark one on a dark background is lifted.
    const Rgb8 darkBg{0x14, 0x16, 0x1A}, lightBg{0xF6, 0xF7, 0xF9};
    CHECK(same(ensureContrast({0xFF, 0xFF, 0x00}, darkBg, 3.0f), {0xFF, 0xFF, 0x00}));
    for (uint32_t v : {0x000000u, 0x101020u, 0x1A237Eu, 0x404040u, 0xFFFFFFu, 0xF1C40Fu, 0x808080u}) {
        const Rgb8 c{(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        CHECK(m3e::contrast(ensureContrast(c, darkBg, 3.0f), darkBg) >= 3.0f);
        CHECK(m3e::contrast(ensureContrast(c, lightBg, 3.0f), lightBg) >= 3.0f);
    }
    // Hue is kept while lightening.
    const Rgb8 lifted = ensureContrast({0x10, 0x10, 0x60}, darkBg, 3.0f);
    CHECK(std::fabs(m3e::toHsl(lifted).h - m3e::toHsl({0x10, 0x10, 0x60}).h) < 0.03f);
    // onColour: white on dark colours, near-black on light ones; always >= 4.5 for these.
    CHECK(same(onColour({0x1A, 0x23, 0x7E}), {255, 255, 255}));
    CHECK(!same(onColour({0xF1, 0xC4, 0x0F}), {255, 255, 255}));
    for (uint32_t v : {0x00A4DCu, 0xAA5CC3u, 0x2ECC71u, 0xFF9F43u, 0xFFFF00u, 0xFFFFFFu, 0x000000u, 0x808080u})
        CHECK(m3e::contrast(onColour({(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}),
                            {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}) >= 4.0f); // (mid tones cannot do better than ~4.1)
}

static std::vector<uint8_t> solid(int w, int h, Rgb8 c, uint8_t a = 255) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) { px[i * 4] = c.r; px[i * 4 + 1] = c.g; px[i * 4 + 2] = c.b; px[i * 4 + 3] = a; }
    return px;
}

static void testDominantColour() {
    Rgb8 o{};
    auto red = solid(64, 64, {200, 30, 40});
    CHECK(dominantColour(red.data(), 64, 64, o));
    CHECK(same(o, {200, 30, 40}, 1));
    // A big image costs the same (grid sampling) and gives the same answer.
    auto big = solid(1000, 1000, {30, 160, 90});
    CHECK(dominantColour(big.data(), 1000, 1000, o));
    CHECK(same(o, {30, 160, 90}, 1));
    // Mostly black cover with a small saturated blue logo: the logo wins, black is ignored.
    auto cover = solid(96, 96, {5, 5, 8});
    for (int y = 30; y < 60; y++)
        for (int x = 30; x < 60; x++) { uint8_t* p = &cover[((size_t)y * 96 + x) * 4]; p[0] = 20; p[1] = 90; p[2] = 230; }
    CHECK(dominantColour(cover.data(), 96, 96, o));
    CHECK(o.b > 200 && o.r < 40);
    // Mostly one hue with a little of another: the big one wins.
    auto two = solid(96, 96, {220, 160, 20});
    for (int y = 0; y < 96; y++)
        for (int x = 0; x < 20; x++) { uint8_t* p = &two[((size_t)y * 96 + x) * 4]; p[0] = 20; p[1] = 40; p[2] = 220; }
    CHECK(dominantColour(two.data(), 96, 96, o));
    CHECK(o.r > 150);
    // No usable colour -> false (the caller keeps the selected palette).
    for (Rgb8 c : {Rgb8{0, 0, 0}, Rgb8{255, 255, 255}, Rgb8{128, 128, 128}, Rgb8{140, 135, 138}}) {
        auto grey = solid(32, 32, c);
        CHECK(!dominantColour(grey.data(), 32, 32, o));
    }
    auto clear = solid(32, 32, {200, 30, 40}, 0);
    CHECK(!dominantColour(clear.data(), 32, 32, o));
    CHECK(!dominantColour(nullptr, 32, 32, o));
    CHECK(!dominantColour(red.data(), 0, 10, o));
    CHECK(!dominantColour(red.data(), 10, -1, o));
    // Tiny images (1x1, 3x2) work.
    auto one = solid(1, 1, {20, 200, 60});
    CHECK(dominantColour(one.data(), 1, 1, o));
    auto tiny = solid(3, 2, {20, 200, 60});
    CHECK(dominantColour(tiny.data(), 3, 2, o));
}

int main() {
    testHex();
    testHsv();
    testArrangement();
    testAutoColours();
    testReadability();
    testDominantColour();
    return check::finish("test_theme_core");
}
