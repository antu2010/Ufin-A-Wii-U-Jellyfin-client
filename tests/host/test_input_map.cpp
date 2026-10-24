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

    // Add to playlist: right stick up on GamePad / Pro / Classic, 1 + 2 on the Wii Remote (all one bit).
    CHECK_EQ(ADD_TO_PLAYLIST, STICK_R_UP);
    CHECK_EQ(fromKpad(EXT_PRO, 0, pro::STICK_R_UP), ADD_TO_PLAYLIST);
    CHECK_EQ(fromKpad(EXT_CLASSIC, 0, classic::STICK_R_UP), ADD_TO_PLAYLIST);
    CHECK_EQ(fromWiimote(wiimote::CHORD_ONE_TWO), ADD_TO_PLAYLIST);
    // ... and no other button of any controller produces it
    const uint32_t wm[] = {wiimote::LEFT, wiimote::RIGHT, wiimote::DOWN, wiimote::UP, wiimote::PLUS, wiimote::TWO,
                           wiimote::ONE, wiimote::B, wiimote::A, wiimote::MINUS, wiimote::HOME};
    for (uint32_t b : wm) CHECK((fromWiimote(b) & ADD_TO_PLAYLIST) == 0);
    for (uint32_t b : proBits) CHECK((fromPro(b) & ADD_TO_PLAYLIST) == 0);

    {   // The 1 + 2 chord.
        ChordFilter f;
        uint32_t t, h;
        // both in one read -> one chord press, no 1 / 2
        t = wiimote::ONE | wiimote::TWO; h = t;
        f.step(t, h, 1000);
        CHECK_EQ(t, wiimote::CHORD_ONE_TWO);
        t = 0; h = wiimote::ONE | wiimote::TWO; // still down: nothing new
        f.step(t, h, 1010);
        CHECK_EQ(t, 0u);
        t = 0; h = 0; f.step(t, h, 1020); // released
        // 1 first, then 2 a frame later -> the chord, and 1 never fired
        t = wiimote::ONE; h = wiimote::ONE;
        f.step(t, h, 2000);
        CHECK_EQ(t, 0u);
        t = wiimote::TWO; h = wiimote::ONE | wiimote::TWO;
        f.step(t, h, 2016);
        CHECK_EQ(t, wiimote::CHORD_ONE_TWO);
        t = 0; h = 0; f.step(t, h, 2100);
        // 1 alone: held back, then let through after the window
        t = wiimote::ONE; h = wiimote::ONE;
        f.step(t, h, 3000);
        CHECK_EQ(t, 0u);
        t = 0; h = wiimote::ONE; f.step(t, h, 3020);
        CHECK_EQ(t, 0u);
        t = 0; h = wiimote::ONE; f.step(t, h, 3045);
        CHECK_EQ(t, wiimote::ONE);
        t = 0; h = wiimote::ONE; f.step(t, h, 3060); // not repeated
        CHECK_EQ(t, 0u);
        t = 0; h = 0; f.step(t, h, 3100);
        // a quick tap (released inside the window) still counts as a press
        t = wiimote::TWO; h = wiimote::TWO; f.step(t, h, 4000);
        CHECK_EQ(t, 0u);
        t = 0; h = 0; f.step(t, h, 4010);
        CHECK_EQ(t, wiimote::TWO);
        // 2 held long, then 1 much later: 2 already went through, so 1 is its own press
        t = wiimote::TWO; h = wiimote::TWO; f.step(t, h, 5000);
        t = 0; h = wiimote::TWO; f.step(t, h, 5050);
        CHECK_EQ(t, wiimote::TWO);
        t = wiimote::ONE; h = wiimote::ONE | wiimote::TWO; f.step(t, h, 5500);
        CHECK_EQ(t & wiimote::CHORD_ONE_TWO, 0u);
        t = 0; h = 0; f.step(t, h, 5600);
        // other buttons are never delayed
        t = wiimote::A | wiimote::PLUS; h = t; f.step(t, h, 6000);
        CHECK_EQ(t, wiimote::A | wiimote::PLUS);
    }
    return check::finish("test_input_map");
}
