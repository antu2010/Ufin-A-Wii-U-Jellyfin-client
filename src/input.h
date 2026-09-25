// All controllers as one: the GamePad plus up to four Wii Remotes (alone,
// with a Nunchuk, a Classic Controller) or Wii U Pro Controllers, merged
// into GamePad button bits (VPAD_BUTTON_*) -- see input_map.h. Menus and
// playback read this instead of the GamePad directly, so everything works
// with the GamePad put away.
#pragma once
#include <cstdint>

struct KPADStatus;

namespace input {

struct State {
    uint32_t hold = 0;     // buttons down now (any controller)
    uint32_t trigger = 0;  // pressed since the last read
    bool touched = false;  // GamePad touch screen, in the UI's 1280x720 space
    float touchX = 0, touchY = 0;
};

void init();      // KPAD + Pro Controller support; call once at start-up
void shutdown();

// Reads every controller once. Call once per loop iteration.
State read();

// Which controller was used last: 0-3 = Wii Remote / Pro Controller
// channel, 4 = GamePad (where the keyboard should appear).
int lastController();

// The last raw reading of a Wii Remote channel, or nullptr if nothing is
// connected there (the software keyboard wants these for pointing).
const KPADStatus* kpad(int channel);

} // namespace input
