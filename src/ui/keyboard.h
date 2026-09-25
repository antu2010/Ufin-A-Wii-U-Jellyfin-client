// Wii U software keyboard (nn::swkbd) as a single blocking call.
//
// swkbd draws itself with GX2, into the display ui::Gfx already owns;
// it runs its own frames (TV and GamePad) until the user confirms or
// cancels.
#pragma once
#include <string>

namespace ui {

// Shows the keyboard with `hint` as the placeholder text. Returns true
// and fills `out` (UTF-8, trimmed) when the user confirms a non-empty
// entry; false on cancel, empty input, or if the keyboard couldn't start
// (then `error` says why).
bool promptKeyboard(const char16_t* hint, std::string& out, std::string& error);

// Same, starting from `initial` (UTF-8). With `password`, typed text is
// hidden. With `allowEmpty`, confirming an empty field counts (e.g.
// clearing a field).
bool promptKeyboard(const char16_t* hint, const std::string& initial, bool password, bool allowEmpty,
                    std::string& out, std::string& error);

} // namespace ui
