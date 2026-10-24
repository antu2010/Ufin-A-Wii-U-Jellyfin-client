// Settings > Customization logic (src/customize_logic.h): page switching and the colour editor's sliders.
#include "check.h"
#include "customize_logic.h"

using namespace customize;

static bool near(Rgb8 a, Rgb8 b, int tol) {
    return std::abs((int)a.r - b.r) <= tol && std::abs((int)a.g - b.g) <= tol && std::abs((int)a.b - b.b) <= tol;
}

int main() {
    // Pages wrap both ways; nonsense counts are safe.
    CHECK_EQ(nextPage(0, 1, 2), 1);
    CHECK_EQ(nextPage(1, 1, 2), 0);
    CHECK_EQ(nextPage(0, -1, 2), 1);
    CHECK_EQ(nextPage(1, -1, 2), 0);
    CHECK_EQ(nextPage(0, 1, 0), 0);
    CHECK_EQ(nextPage(5, 1, 2), 0);

    // RGB channels read and write exactly; the others stay put.
    HueMemory mem;
    const Rgb8 base{0x12, 0x80, 0xF0};
    CHECK_NEAR(channelValue(base, Channel::Red, mem), 0x12 / 255.0, 1e-6);
    CHECK_NEAR(channelValue(base, Channel::Blue, mem), 0xF0 / 255.0, 1e-6);
    Rgb8 c = setChannel(base, Channel::Green, 1.0f, mem);
    CHECK(c.r == 0x12 && c.g == 0xFF && c.b == 0xF0);
    c = setChannel(base, Channel::Red, -4.0f, mem); // clamped
    CHECK(c.r == 0 && c.g == 0x80);
    c = setChannel(base, Channel::Blue, 9.0f, mem);
    CHECK(c.b == 255);

    // HSV: changing one component changes only it (within rounding).
    const ui::theme::Hsv h0 = ui::theme::toHsv(base);
    c = setChannel(base, Channel::Saturation, 0.5f, mem);
    ui::theme::Hsv h1 = ui::theme::toHsv(c);
    CHECK_NEAR(h1.s, 0.5, 0.01); CHECK_NEAR(h1.h, h0.h, 0.01); CHECK_NEAR(h1.v, h0.v, 0.01);
    c = setChannel(base, Channel::Brightness, 0.4f, mem);
    h1 = ui::theme::toHsv(c);
    CHECK_NEAR(h1.v, 0.4, 0.01); CHECK_NEAR(h1.h, h0.h, 0.01); CHECK_NEAR(h1.s, h0.s, 0.02);
    c = setChannel(base, Channel::Hue, 0.0f, mem);
    h1 = ui::theme::toHsv(c);
    CHECK_NEAR(h1.h, 0.0, 0.01); CHECK_NEAR(h1.s, h0.s, 0.01);
    c = setChannel(base, Channel::Hue, 1.0f, mem); // the top of the slider is not a different colour than the bottom + 1 step
    CHECK(near(c, setChannel(base, Channel::Hue, 0.9999f, mem), 1));

    // A grey remembers its hue: saturation back up returns to the colour, not red.
    HueMemory m2;
    Rgb8 col{0x20, 0x90, 0x40};
    channelValue(col, Channel::Hue, m2);
    Rgb8 grey = setChannel(col, Channel::Saturation, 0.0f, m2);
    CHECK(grey.r == grey.g && grey.g == grey.b);
    Rgb8 back = setChannel(grey, Channel::Saturation, 1.0f, m2);
    CHECK(back.g > back.r && back.g > back.b);
    // ... and black stays addressable by brightness.
    Rgb8 black = setChannel(col, Channel::Brightness, 0.0f, m2);
    CHECK(black.r == 0 && black.g == 0 && black.b == 0);
    Rgb8 lit = setChannel(black, Channel::Brightness, 0.8f, m2);
    CHECK(lit.g > lit.r);

    // Steps: size per channel, direction, coarse = double, clamping, hue wraps.
    HueMemory m3;
    Rgb8 s1 = stepChannel({100, 100, 100}, Channel::Red, 1, false, m3);
    CHECK_EQ((int)s1.r, 105);
    s1 = stepChannel({100, 100, 100}, Channel::Red, -1, true, m3);
    CHECK_EQ((int)s1.r, 90);
    s1 = stepChannel({252, 0, 0}, Channel::Red, 1, true, m3);
    CHECK_EQ((int)s1.r, 255);
    s1 = stepChannel({1, 0, 0}, Channel::Red, -1, true, m3);
    CHECK_EQ((int)s1.r, 0);
    Rgb8 red{255, 0, 0};
    Rgb8 wrapped = stepChannel(red, Channel::Hue, -1, false, m3); // below 0 -> top of the wheel (pinkish red)
    CHECK(wrapped.r == 255 && wrapped.b > 0 && wrapped.g == 0);
    HueMemory m4;
    Rgb8 sat = stepChannel({100, 150, 200}, Channel::Saturation, 1, false, m4);
    CHECK(ui::theme::toHsv(sat).s > ui::theme::toHsv({100, 150, 200}).s);
    // Walking the whole hue wheel in steps comes back round (72 steps of 5 degrees).
    Rgb8 w{200, 40, 40};
    HueMemory m5;
    for (int i = 0; i < 72; i++) w = stepChannel(w, Channel::Hue, 1, false, m5);
    CHECK(near(w, {200, 40, 40}, 25)); // (8-bit rounding drifts a little per step)

    CHECK_EQ(pack({1, 2, 3}), 0x010203u);
    CHECK(near(unpack(0xAABBCC), {0xAA, 0xBB, 0xCC}, 0));
    return check::finish("test_customize_logic");
}
