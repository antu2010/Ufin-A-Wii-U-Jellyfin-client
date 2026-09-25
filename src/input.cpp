#include "input.h"
#include "input_map.h"

#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>

#include <cstring>

// The tables in input_map.h are copies of wut's values: check them.
static_assert(pad::A == VPAD_BUTTON_A && pad::B == VPAD_BUTTON_B && pad::X == VPAD_BUTTON_X &&
              pad::Y == VPAD_BUTTON_Y && pad::UP == VPAD_BUTTON_UP && pad::DOWN == VPAD_BUTTON_DOWN &&
              pad::LEFT == VPAD_BUTTON_LEFT && pad::RIGHT == VPAD_BUTTON_RIGHT && pad::L == VPAD_BUTTON_L &&
              pad::R == VPAD_BUTTON_R && pad::ZL == VPAD_BUTTON_ZL && pad::ZR == VPAD_BUTTON_ZR &&
              pad::PLUS == VPAD_BUTTON_PLUS && pad::MINUS == VPAD_BUTTON_MINUS && pad::HOME == VPAD_BUTTON_HOME &&
              pad::STICK_L_UP == VPAD_STICK_L_EMULATION_UP && pad::STICK_L_DOWN == VPAD_STICK_L_EMULATION_DOWN &&
              pad::STICK_L_LEFT == VPAD_STICK_L_EMULATION_LEFT && pad::STICK_L_RIGHT == VPAD_STICK_L_EMULATION_RIGHT &&
              pad::STICK_R_UP == VPAD_STICK_R_EMULATION_UP && pad::STICK_R_DOWN == VPAD_STICK_R_EMULATION_DOWN,
              "GamePad bits");
static_assert(pad::wiimote::A == WPAD_BUTTON_A && pad::wiimote::B == WPAD_BUTTON_B &&
              pad::wiimote::ONE == WPAD_BUTTON_1 && pad::wiimote::TWO == WPAD_BUTTON_2 &&
              pad::wiimote::UP == WPAD_BUTTON_UP && pad::wiimote::HOME == WPAD_BUTTON_HOME &&
              pad::wiimote::PLUS == WPAD_BUTTON_PLUS && pad::wiimote::MINUS == WPAD_BUTTON_MINUS,
              "Wii Remote bits");
static_assert(pad::nunchuk::Z == WPAD_NUNCHUK_BUTTON_Z && pad::nunchuk::C == WPAD_NUNCHUK_BUTTON_C &&
              pad::nunchuk::STICK_UP == WPAD_NUNCHUK_STICK_EMULATION_UP, "Nunchuk bits");
static_assert(pad::classic::A == WPAD_CLASSIC_BUTTON_A && pad::classic::ZR == WPAD_CLASSIC_BUTTON_ZR &&
              pad::classic::STICK_L_UP == WPAD_CLASSIC_STICK_L_EMULATION_UP &&
              pad::classic::RIGHT == WPAD_CLASSIC_BUTTON_RIGHT, "Classic Controller bits");
static_assert(pad::pro::A == WPAD_PRO_BUTTON_A && pad::pro::ZL == WPAD_PRO_BUTTON_ZL &&
              pad::pro::STICK_L_UP == WPAD_PRO_STICK_L_EMULATION_UP &&
              pad::pro::STICK_L_LEFT == WPAD_PRO_STICK_L_EMULATION_LEFT && pad::pro::L == WPAD_PRO_BUTTON_L,
              "Pro Controller bits");
static_assert(pad::EXT_PRO == WPAD_EXT_PRO_CONTROLLER && pad::EXT_CLASSIC == WPAD_EXT_CLASSIC &&
              pad::EXT_NUNCHUK == WPAD_EXT_NUNCHUK && pad::EXT_MPLUS_CLASSIC == WPAD_EXT_MPLUS_CLASSIC &&
              pad::EXT_MPLUS_NUNCHUK == WPAD_EXT_MPLUS_NUNCHUK, "extension types");

namespace input {

static KPADStatus g_kpad[4];
static bool g_connected[4] = {false, false, false, false};
static uint32_t g_kpadHold[4] = {0, 0, 0, 0};
static int g_last = 4;
static bool g_inited = false;

void init() {
    if (g_inited) return;
    KPADInit();
    WPADEnableURCC(TRUE); // Pro Controllers
    memset(g_kpad, 0, sizeof(g_kpad));
    g_inited = true;
}

void shutdown() {
    if (!g_inited) return;
    KPADShutdown();
    g_inited = false;
}

static uint32_t extButtons(const KPADStatus& s, bool trigger) {
    switch (s.extensionType) {
        case WPAD_EXT_PRO_CONTROLLER: return trigger ? s.pro.trigger : s.pro.hold;
        case WPAD_EXT_CLASSIC:
        case WPAD_EXT_MPLUS_CLASSIC:  return trigger ? s.classic.trigger : s.classic.hold;
        case WPAD_EXT_NUNCHUK:
        case WPAD_EXT_MPLUS_NUNCHUK:  return trigger ? s.nunchuk.trigger : s.nunchuk.hold;
        default:                      return 0;
    }
}

State read() {
    State st;

    VPADStatus vpad;
    VPADReadError vpadErr;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadErr);
    if (vpadErr == VPAD_READ_SUCCESS) {
        st.hold |= vpad.hold;
        st.trigger |= vpad.trigger;
        if (vpad.trigger) g_last = 4;
        VPADTouchData touch;
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &touch, &vpad.tpNormal);
        if (vpad.tpNormal.touched && touch.validity == VPAD_VALID) {
            st.touched = true;
            st.touchX = (float)touch.x;
            st.touchY = (float)touch.y;
            g_last = 4;
        }
    }

    if (g_inited) {
        for (int c = 0; c < 4; c++) {
            KPADStatus s;
            KPADError err = KPAD_ERROR_OK;
            uint32_t n = KPADReadEx((KPADChan)c, &s, 1, &err);
            if (n > 0 && err == KPAD_ERROR_OK) {
                g_kpad[c] = s;
                g_connected[c] = true;
                uint32_t coreHold = s.extensionType == WPAD_EXT_PRO_CONTROLLER ? 0 : s.hold;
                uint32_t coreTrig = s.extensionType == WPAD_EXT_PRO_CONTROLLER ? 0 : s.trigger;
                g_kpadHold[c] = pad::fromKpad(s.extensionType, coreHold, extButtons(s, false));
                uint32_t trig = pad::fromKpad(s.extensionType, coreTrig, extButtons(s, true));
                st.trigger |= trig;
                if (trig) g_last = c;
            } else if (err != KPAD_ERROR_OK && err != KPAD_ERROR_NO_SAMPLES) {
                g_connected[c] = false; // gone (disconnected / not paired)
                g_kpadHold[c] = 0;
            }
            // No new sample this frame: buttons still held as last seen.
            st.hold |= g_kpadHold[c];
        }
    }
    return st;
}

int lastController() { return g_last; }

const KPADStatus* kpad(int channel) {
    if (channel < 0 || channel > 3 || !g_connected[channel]) return nullptr;
    return &g_kpad[channel];
}

} // namespace input
