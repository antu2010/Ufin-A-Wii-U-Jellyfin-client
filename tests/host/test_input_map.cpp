// Wii Remote / Nunchuk / Classic / Pro Controller -> GamePad bits.
#include "check.h"
#include "input_map.h"

int main() {
    using namespace pad;
    // Wii Remote alone: D-pad, A/B, 1->X, 2->Y, +/-/HOME.
    CHECK_EQ(fromWiimote(wiimote::UP), UP);
    CHECK_EQ(fromWiimote(wiimote::A | wiimote::B), A | B);
    CHECK_EQ(fromWiimote(wiimote::ONE), X);
    CHECK_EQ(fromWiimote(wiimote::TWO), Y);
    CHECK_EQ(fromWiimote(wiimote::PLUS | wiimote::MINUS | wiimote::HOME), PLUS | MINUS | HOME);
    CHECK_EQ(fromWiimote(0), 0u);
    CHECK_EQ(fromKpad(EXT_CORE, wiimote::DOWN, 0xFFFFFFFF), DOWN); // extension bits ignored without one

    // Nunchuk: stick -> left stick, C/Z -> L/R, plus the Wii Remote's own.
    CHECK_EQ(fromKpad(EXT_NUNCHUK, wiimote::A, nunchuk::STICK_UP), A | STICK_L_UP);
    CHECK_EQ(fromKpad(EXT_NUNCHUK, 0, nunchuk::C | nunchuk::Z), L | R);
    CHECK_EQ(fromKpad(EXT_MPLUS_NUNCHUK, 0, nunchuk::STICK_LEFT), STICK_L_LEFT);

    // Classic and Pro: same layout as the GamePad.
    CHECK_EQ(fromKpad(EXT_CLASSIC, 0, classic::A | classic::ZR | classic::STICK_L_DOWN), A | ZR | STICK_L_DOWN);
    CHECK_EQ(fromKpad(EXT_MPLUS_CLASSIC, 0, classic::L | classic::R), L | R);
    CHECK_EQ(fromKpad(EXT_PRO, 0, pro::B | pro::Y | pro::STICK_L_UP | pro::STICK_R_LEFT), B | Y | STICK_L_UP | STICK_R_LEFT);
    CHECK_EQ(fromKpad(EXT_PRO, wiimote::A, 0), 0u); // a Pro Controller has no Wii Remote buttons

    // Every Classic / Pro button maps to exactly one GamePad bit, no overlaps.
    const uint32_t proBits[] = {pro::UP, pro::DOWN, pro::LEFT, pro::RIGHT, pro::A, pro::B, pro::X, pro::Y,
                                pro::L, pro::R, pro::ZL, pro::ZR, pro::PLUS, pro::MINUS, pro::HOME,
                                pro::STICK_L_UP, pro::STICK_L_DOWN, pro::STICK_L_LEFT, pro::STICK_L_RIGHT};
    uint32_t seen = 0;
    bool unique = true;
    for (uint32_t b : proBits) {
        uint32_t m = fromPro(b);
        if (m == 0 || (m & (m - 1)) || (seen & m)) unique = false;
        seen |= m;
    }
    CHECK(unique);
    CHECK_EQ(fromClassic(classic::STICK_R_UP), STICK_R_UP);
    return check::finish("test_input_map");
}
