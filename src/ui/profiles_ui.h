// profiles_ui.h -- the "Who's watching?" screen: a row of round profile pictures, each with a
// "Auto log in" check box and a "Log out" button under its name. The picture under the cursor bounces (squash and stretch,
// see profile_anim.h); the others sit still. Drawn with ImGui like every other Ufin screen: dark or
// light palette, flat or Liquid Glass. The implementation (profiles_ui.inc) is included at the end of
// app_ui.cpp because it uses that file's private drawing helpers.
#pragma once

#include "app_ui.h"
#include "profile_anim.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

constexpr int kProfileColors = 8; // avatar colours (profiles::kColors in profiles.h is the same number)

struct ProfileTile {
    std::string name;
    bool add = false;          // the "Add profile" tile (no picture, no check box)
    bool keep = false;         // "Auto log in" ticked
    bool loggedOut = false;    // no saved sign-in (the "Log out" button is dimmed)
    uint32_t color = 0x00A4DC; // 0xRRGGBB of the letter avatar
    std::string letter;        // shown on the avatar when there is no picture
    Texture picture = 0;       // the user's Jellyfin picture (a round crop is drawn); 0 = letter avatar
    float pictureAspect = 1.0f;
    BounceShape bounce;        // this frame's squash/stretch (still unless this is the hovered tile)
};

struct ProfilesModel {
    std::string title = "Who's watching?";
    std::vector<ProfileTile> tiles;
    int cursor = 0;            // the hovered tile
    int focusRow = 0;          // 0 = the picture, 1 = its "Auto log in" box, 2 = its "Log out" button
    std::string message;       // a line under the row (e.g. the remove confirmation)
    bool messageIsError = false;
    std::vector<Hint> hints;
};

struct ProfilesHit {
    enum Kind { None, Picture, Check, Logout };
    Kind kind = None;
    int index = -1;
};

void drawProfiles(const ProfilesModel& m);

// What a tap at (x, y) hits: a picture (or the Add tile), an "Auto log in" box or a "Log out" button.
ProfilesHit hitTestProfiles(const ProfilesModel& m, float x, float y);

// The 0xRRGGBB colour for avatar colour index i (any int: it wraps).
uint32_t profileColor(int i);

} // namespace ui
