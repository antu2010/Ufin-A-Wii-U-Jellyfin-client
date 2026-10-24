// Logic of Settings > Customization that needs no UI: which settings page is next, and how the palette editor's
// sliders (hue / saturation / brightness and red / green / blue) read and change a colour. Host-tested in
// tests/host/test_customize_logic.cpp.
#pragma once
#include "ui/theme_core.h"

#include <cstdint>

namespace customize {

using ui::theme::Rgb8;

// L / R switch pages and wrap round (two pages today: Customization, Settings).
inline int nextPage(int page, int direction, int count) {
    if (count <= 0) return 0;
    int p = (page + (direction < 0 ? -1 : 1)) % count;
    return p < 0 ? p + count : p;
}

enum class Channel { Hue, Saturation, Brightness, Red, Green, Blue };

// A grey (or black) has no hue of its own; the editor remembers the last one so dragging saturation up from zero
// brings back the colour the user was on instead of red.
struct HueMemory {
    float hue = 0.0f;
    float sat = 1.0f; // (black likewise forgets its saturation)
};

inline float channelValue(Rgb8 c, Channel ch, HueMemory& mem) {
    switch (ch) {
        case Channel::Red:   return c.r / 255.0f;
        case Channel::Green: return c.g / 255.0f;
        case Channel::Blue:  return c.b / 255.0f;
        default: break;
    }
    const ui::theme::Hsv h = ui::theme::toHsv(c);
    if (h.s > 0.001f && h.v > 0.001f) mem.hue = h.h;
    if (h.v > 0.001f) mem.sat = h.s;
    if (ch == Channel::Hue) return mem.hue;
    return ch == Channel::Saturation ? h.s : h.v;
}

inline Rgb8 setChannel(Rgb8 c, Channel ch, float value, HueMemory& mem) {
    value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    switch (ch) {
        case Channel::Red:   c.r = (uint8_t)(value * 255.0f + 0.5f); return c;
        case Channel::Green: c.g = (uint8_t)(value * 255.0f + 0.5f); return c;
        case Channel::Blue:  c.b = (uint8_t)(value * 255.0f + 0.5f); return c;
        default: break;
    }
    const ui::theme::Hsv h = ui::theme::toHsv(c);
    float hue = (h.s > 0.001f && h.v > 0.001f) ? h.h : mem.hue;
    float sat = h.v > 0.001f ? h.s : mem.sat, val = h.v;
    if (h.v > 0.001f) mem.sat = h.s;
    if (ch == Channel::Hue) { hue = value >= 1.0f ? 0.9999f : value; mem.hue = hue; }
    else if (ch == Channel::Saturation) { sat = value; mem.sat = value; }
    else val = value;
    // A grey picked by saturation 0 or brightness 0 keeps its remembered hue for later.
    return ui::theme::fromHsv(hue, sat, val);
}

// What the - and + buttons and the d-pad do: one step along the slider. Hue moves in 5 degree steps, the RGB
// channels by 5 / 255, saturation and brightness by 2 %. `coarse` (a touch on - / +) is twice as big.
inline float stepSize(Channel ch, bool coarse) {
    float s = 0.02f;
    if (ch == Channel::Hue) s = 1.0f / 72.0f;
    else if (ch == Channel::Red || ch == Channel::Green || ch == Channel::Blue) s = 5.0f / 255.0f;
    return coarse ? s * 2.0f : s;
}

inline Rgb8 stepChannel(Rgb8 c, Channel ch, int direction, bool coarse, HueMemory& mem) {
    float v = channelValue(c, ch, mem) + (direction < 0 ? -1.0f : 1.0f) * stepSize(ch, coarse);
    if (ch == Channel::Hue) v = v - std::floor(v); // the hue wheel wraps
    return setChannel(c, ch, v, mem);
}

inline uint32_t pack(Rgb8 c) { return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b; }
inline Rgb8 unpack(uint32_t v) { return {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }

} // namespace customize
