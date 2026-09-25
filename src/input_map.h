// Translating Wii Remote / Nunchuk / Classic Controller / Pro Controller
// buttons into the GamePad's button bits, so the whole app keeps
// thinking in one button set (VPAD_BUTTON_*). Pure: no wut includes, the
// values are copied from wut's vpad/input.h and padscore/wpad.h (and
// checked against them with static_asserts in input.cpp), so the host
// tests can run these tables.
#pragma once
#include <cstdint>

namespace pad {

// GamePad (VPAD) bits
static const uint32_t A = 0x8000, B = 0x4000, X = 0x2000, Y = 0x1000;
static const uint32_t LEFT = 0x0800, RIGHT = 0x0400, UP = 0x0200, DOWN = 0x0100;
static const uint32_t ZL = 0x0080, ZR = 0x0040, L = 0x0020, R = 0x0010;
static const uint32_t PLUS = 0x0008, MINUS = 0x0004, HOME = 0x0002;
static const uint32_t STICK_R_LEFT = 0x04000000, STICK_R_RIGHT = 0x02000000;
static const uint32_t STICK_R_UP = 0x01000000, STICK_R_DOWN = 0x00800000;
static const uint32_t STICK_L_LEFT = 0x40000000, STICK_L_RIGHT = 0x20000000;
static const uint32_t STICK_L_UP = 0x10000000, STICK_L_DOWN = 0x08000000;

namespace wiimote {
static const uint32_t LEFT = 0x0001, RIGHT = 0x0002, DOWN = 0x0004, UP = 0x0008, PLUS = 0x0010;
static const uint32_t TWO = 0x0100, ONE = 0x0200, B = 0x0400, A = 0x0800, MINUS = 0x1000;
static const uint32_t Z = 0x2000, C = 0x4000, HOME = 0x8000;
}
namespace nunchuk {
static const uint32_t STICK_LEFT = 0x0001, STICK_RIGHT = 0x0002, STICK_DOWN = 0x0004, STICK_UP = 0x0008;
static const uint32_t Z = 0x2000, C = 0x4000;
}
namespace classic {
static const uint32_t UP = 0x00000001, LEFT = 0x00000002, ZR = 0x00000004, X = 0x00000008, A = 0x00000010;
static const uint32_t Y = 0x00000020, B = 0x00000040, ZL = 0x00000080, R = 0x00000200, PLUS = 0x00000400;
static const uint32_t HOME = 0x00000800, MINUS = 0x00001000, L = 0x00002000, DOWN = 0x00004000;
static const uint32_t RIGHT = 0x00008000;
static const uint32_t STICK_L_LEFT = 0x00010000, STICK_L_RIGHT = 0x00020000, STICK_L_DOWN = 0x00040000;
static const uint32_t STICK_L_UP = 0x00080000, STICK_R_LEFT = 0x00100000, STICK_R_RIGHT = 0x00200000;
static const uint32_t STICK_R_DOWN = 0x00400000, STICK_R_UP = 0x00800000;
}
namespace pro {
static const uint32_t UP = 0x00000001, LEFT = 0x00000002, ZR = 0x00000004, X = 0x00000008, A = 0x00000010;
static const uint32_t Y = 0x00000020, B = 0x00000040, ZL = 0x00000080, R = 0x00000200, PLUS = 0x00000400;
static const uint32_t HOME = 0x00000800, MINUS = 0x00001000, L = 0x00002000, DOWN = 0x00004000;
static const uint32_t RIGHT = 0x00008000;
static const uint32_t STICK_L_UP = 0x00200000, STICK_L_DOWN = 0x00100000, STICK_L_LEFT = 0x00040000;
static const uint32_t STICK_L_RIGHT = 0x00080000, STICK_R_UP = 0x02000000, STICK_R_DOWN = 0x01000000;
static const uint32_t STICK_R_LEFT = 0x00400000, STICK_R_RIGHT = 0x00800000;
}

struct BitMap {
    uint32_t from, to;
};

template <int N>
inline uint32_t mapBits(uint32_t in, const BitMap (&table)[N]) {
    uint32_t out = 0;
    for (int i = 0; i < N; i++) {
        if (in & table[i].from) out |= table[i].to;
    }
    return out;
}

// Wii Remote held upright (pointing at the TV): D-pad, A, B (trigger);
// 1 and 2 stand in for X (search) and Y (favourite / tracks).
inline uint32_t fromWiimote(uint32_t b) {
    static const BitMap t[] = {
        {wiimote::UP, UP}, {wiimote::DOWN, DOWN}, {wiimote::LEFT, LEFT}, {wiimote::RIGHT, RIGHT},
        {wiimote::A, A}, {wiimote::B, B}, {wiimote::ONE, X}, {wiimote::TWO, Y},
        {wiimote::PLUS, PLUS}, {wiimote::MINUS, MINUS}, {wiimote::HOME, HOME},
    };
    return mapBits(b, t);
}

// Nunchuk: stick moves like the left stick; C and Z are L and R (page up
// / down in lists, previous / next track in music).
inline uint32_t fromNunchuk(uint32_t b) {
    static const BitMap t[] = {
        {nunchuk::STICK_UP, STICK_L_UP}, {nunchuk::STICK_DOWN, STICK_L_DOWN},
        {nunchuk::STICK_LEFT, STICK_L_LEFT}, {nunchuk::STICK_RIGHT, STICK_L_RIGHT},
        {nunchuk::C, L}, {nunchuk::Z, R},
    };
    return mapBits(b, t);
}

// Classic Controller: same layout and names as the GamePad.
inline uint32_t fromClassic(uint32_t b) {
    static const BitMap t[] = {
        {classic::UP, UP}, {classic::DOWN, DOWN}, {classic::LEFT, LEFT}, {classic::RIGHT, RIGHT},
        {classic::A, A}, {classic::B, B}, {classic::X, X}, {classic::Y, Y},
        {classic::L, L}, {classic::R, R}, {classic::ZL, ZL}, {classic::ZR, ZR},
        {classic::PLUS, PLUS}, {classic::MINUS, MINUS}, {classic::HOME, HOME},
        {classic::STICK_L_UP, STICK_L_UP}, {classic::STICK_L_DOWN, STICK_L_DOWN},
        {classic::STICK_L_LEFT, STICK_L_LEFT}, {classic::STICK_L_RIGHT, STICK_L_RIGHT},
        {classic::STICK_R_UP, STICK_R_UP}, {classic::STICK_R_DOWN, STICK_R_DOWN},
        {classic::STICK_R_LEFT, STICK_R_LEFT}, {classic::STICK_R_RIGHT, STICK_R_RIGHT},
    };
    return mapBits(b, t);
}

// Wii U Pro Controller: same layout as the GamePad (its stick bits differ).
inline uint32_t fromPro(uint32_t b) {
    static const BitMap t[] = {
        {pro::UP, UP}, {pro::DOWN, DOWN}, {pro::LEFT, LEFT}, {pro::RIGHT, RIGHT},
        {pro::A, A}, {pro::B, B}, {pro::X, X}, {pro::Y, Y},
        {pro::L, L}, {pro::R, R}, {pro::ZL, ZL}, {pro::ZR, ZR},
        {pro::PLUS, PLUS}, {pro::MINUS, MINUS}, {pro::HOME, HOME},
        {pro::STICK_L_UP, STICK_L_UP}, {pro::STICK_L_DOWN, STICK_L_DOWN},
        {pro::STICK_L_LEFT, STICK_L_LEFT}, {pro::STICK_L_RIGHT, STICK_L_RIGHT},
        {pro::STICK_R_UP, STICK_R_UP}, {pro::STICK_R_DOWN, STICK_R_DOWN},
        {pro::STICK_R_LEFT, STICK_R_LEFT}, {pro::STICK_R_RIGHT, STICK_R_RIGHT},
    };
    return mapBits(b, t);
}

// Extension types (padscore WPAD_EXT_*).
static const uint8_t EXT_CORE = 0x00, EXT_NUNCHUK = 0x01, EXT_CLASSIC = 0x02, EXT_MPLUS_NUNCHUK = 0x06,
                     EXT_MPLUS_CLASSIC = 0x07, EXT_PRO = 0x1f;

// One controller's reading -> GamePad bits. `core` is the Wii Remote's own
// buttons (unused for the Pro Controller, which has none), `ext` the
// extension's.
inline uint32_t fromKpad(uint8_t extensionType, uint32_t core, uint32_t ext) {
    switch (extensionType) {
        case EXT_PRO: return fromPro(ext);
        case EXT_CLASSIC:
        case EXT_MPLUS_CLASSIC: return fromWiimote(core) | fromClassic(ext);
        case EXT_NUNCHUK:
        case EXT_MPLUS_NUNCHUK: return fromWiimote(core) | fromNunchuk(ext);
        default: return fromWiimote(core);
    }
}

} // namespace pad
