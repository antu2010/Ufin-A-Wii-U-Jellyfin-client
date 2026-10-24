// help_content.h -- what the Help window (sidebar > Help) shows: one page per controller, each a
// list of buttons grouped by the screen they work on. Pure data, no UI or Wii U includes, so the
// host tests can check it against the real mappings (input_map.h) and the README.
//
// The 3D models are not built in: they are .glb files read from the SD card (see modelFile()).
// The `highlight` mask names the button(s) a row is about (Btn bits). The loaded models are plain
// meshes without named buttons, so nothing lights up on them for now; the masks stay as data.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace help {

// Parts of a controller model, one bit each.
enum Btn : uint32_t {
    A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3,
    DPAD = 1u << 4, STICK_L = 1u << 5, STICK_R = 1u << 6,
    L = 1u << 7, R = 1u << 8, ZL = 1u << 9, ZR = 1u << 10,
    PLUS = 1u << 11, MINUS = 1u << 12, HOME = 1u << 13,
    ONE = 1u << 14, TWO = 1u << 15,
    NUN_C = 1u << 16, NUN_Z = 1u << 17, NUN_STICK = 1u << 18,
    TOUCH = 1u << 19,
};

enum class Controller { GamePad, Pro, Classic, WiiRemote };
static const int kControllerCount = 4;

struct Row {
    bool header = false;   // a group title ("Video playback"); `action` holds it
    std::string button;    // "A", "L / R", "D-pad / left stick" ...
    std::string action;
    uint32_t highlight = 0;
};

inline Controller controllerAt(int page) {
    switch (page) {
        case 1: return Controller::Pro;
        case 2: return Controller::Classic;
        case 3: return Controller::WiiRemote;
        default: return Controller::GamePad;
    }
}

inline const char* controllerName(Controller c) {
    switch (c) {
        case Controller::Pro: return "Pro Controller";
        case Controller::Classic: return "Classic Controller";
        case Controller::WiiRemote: return "Wii Remote";
        default: return "GamePad";
    }
}

// Where the models live. Cemu maps /vol/external01 to its sdcard folder, like the other assets.
constexpr const char* kModelDir = "/vol/external01/wiiu/apps/ufin/assets/models/";
constexpr const char* kModelDirShown = "assets/models/"; // what the Help window shows (inside sd:/wiiu/apps/ufin/)

// File name (inside kModelDir) of a controller's model.
inline const char* modelFile(Controller c) {
    switch (c) {
        case Controller::Pro: return "wiiu_pro.glb";
        case Controller::Classic: return "wii_classic.glb";
        case Controller::WiiRemote: return "wiimote.glb";
        default: return "gamepad.glb";
    }
}

// A short line under the model.
inline const char* controllerNote(Controller c) {
    switch (c) {
        case Controller::Pro: return "Wireless; works like the GamePad without the touch screen";
        case Controller::Classic: return "Classic Controller / Pro; same buttons as the Pro Controller";
        case Controller::WiiRemote: return "Held upright, with a Nunchuk for the stick and page buttons";
        default: return "The built-in controls, plus the touch screen";
    }
}

inline std::vector<Row> rowsFor(Controller c) {
    std::vector<Row> r;
    auto head = [&](const char* title) { Row x; x.header = true; x.action = title; r.push_back(x); };
    auto add = [&](const char* b, const char* a, uint32_t hl) { Row x; x.button = b; x.action = a; x.highlight = hl; r.push_back(x); };

    const bool remote = (c == Controller::WiiRemote);
    const bool gamepad = (c == Controller::GamePad);

    head("Browsing");
    if (remote) {
        add("D-pad / Nunchuk stick", "Move; hold to scroll fast", DPAD | NUN_STICK);
        add("Left / Right", "Sidebar / list; Right in a list reaches Sort and New playlist", DPAD);
        add("Nunchuk C / Z", "Page up / down; in a music library: switch view", NUN_C | NUN_Z);
        add("A", "Open / play", A);
        add("B", "Back; from the top level, to the sidebar", B);
        add("1", "Search", ONE);
        add("2", "Favourite / unfavourite", TWO);
        add("1 + 2 together", "Add a song to a playlist (or remove it there); songs, movies, albums, shows: add to Up Next", ONE | TWO);
        add("+", "Shuffle the selected album or playlist", PLUS);
        add("-", "Use the selected song as menu music", MINUS);
    } else {
        add("D-pad / left stick", "Move; hold to scroll fast", DPAD | STICK_L);
        add("Left / Right", "Sidebar / list; Right in a list reaches Sort and New playlist", DPAD);
        add("L / R", "Page up / down; in a music library: switch view", L | R);
        add("A", "Open / play", A);
        add("B", "Back; from the top level, to the sidebar", B);
        add("X", "Search", X);
        add("Y", "Favourite / unfavourite", Y);
        add("ZL", "Mark watched / unwatched", ZL);
        add("ZR", "Refresh the list", ZR);
        add("Right stick up", "Add a song to a playlist (or remove it there); songs, movies, albums, shows: add to Up Next", STICK_R);
        add("+", "Shuffle the selected album or playlist", PLUS);
        add("-", "Use the selected song as menu music", MINUS);
    }
    if (gamepad) add("Touch", "Tap a sidebar entry, row, tab, + or round button", TOUCH);

    head("Sort menu (opened from the Sort button)");
    add("Up / Down", "Pick a sort; last row flips the direction", DPAD);
    add("A", "Apply", A);
    add("B", "Close", B);

    head("Settings");
    add("A", "Change the selected setting (\"Restore default menu music\" goes back to the built-in music)", A);
    add("B", "Back", B);

    head("Video playback");
    add("A", "Pause / resume", A);
    add("Left", "Back 10 s (presses add up)", DPAD);
    add("Right", "Forward 30 s (presses add up)", DPAD);
    if (remote) add("2", "Audio and subtitles", TWO);
    else add("Y", "Audio and subtitles", Y);
    add("+", "TV mode: the GamePad becomes a remote", PLUS);
    if (gamepad) add("Touch", "In TV mode: touch the progress bar to seek (release to go), the gear for quality / audio / subtitles, Queue to browse", TOUCH);
    add("Up / Y", remote ? "In TV mode: open the Queue / the playback settings (2)" : "In TV mode: open the Queue / the playback settings", DPAD);
    add("-", "GamePad screen on / off", MINUS);
    add("B", "Stop", B);

    head("Music");
    if (remote) add("Nunchuk C / Z", "Previous / next track (C restarts a song after 3 s)", NUN_C | NUN_Z);
    else add("L / R", "Previous / next track (L restarts a song after 3 s)", L | R);
    add("A", "Pause / resume", A);
    add("Left / Right", "Back 10 s / forward 30 s", DPAD);
    if (remote) {
        add("1", "Shuffle on / off", ONE);
        add("2", "Repeat: off, then all, then one song (press again to change)", TWO);
    } else {
        add("X", "Shuffle on / off", X);
        add("Y", "Repeat: off, then all, then one song (press again to change)", Y);
    }
    add("-", "Visualizer: on, next style (5), then off", MINUS);
    add("Up", "Open the Up Next queue (the Queue button under the cover)", DPAD);
    add("B", "Stop", B);
    if (gamepad) add("Touch", "Transport buttons, Shuffle / Repeat, Queue, the volume slider, and the progress bar (touch to seek)", TOUCH);

    head("Up Next queue (open over the player; the music keeps playing)");
    add("Up / Down", "Choose Add to Queue, Clear, or a queued item", DPAD);
    add("A", "Play the item now / open Add to Queue / clear (press twice)", A);
    if (remote) { add("1", "Remove the item", ONE); add("2", "Clear the queue (press twice)", TWO); }
    else { add("X", "Remove the item", X); add("Y", "Clear the queue (press twice)", Y); }
    add("Left / Right", "Move the item up / down (on the buttons: switch between them)", DPAD);
    if (remote) add("Nunchuk C / Z", "Move the item up / down", NUN_C | NUN_Z);
    else add("L / R", "Move the item up / down", L | R);
    add("+", "Add to Queue", PLUS);
    add("B", "Close the queue (does not stop the music)", B);
    if (gamepad) add("Touch", "Tap an item to play it; the arrows move it, the cross removes it", TOUCH);

    head("Add to Queue (the pop-up browser)");
    add("Up / Down", "Move in the list", DPAD);
    add("A", "Open an album / show / folder; on a song, movie or episode: add it", A);
    if (remote) { add("1", "Add the selected item (an album or show adds all its songs / episodes)", ONE); add("2", "Add everything listed", TWO); }
    else { add("X", "Add the selected item (an album or show adds all its songs / episodes)", X); add("Y", "Add everything listed", Y); }
    add(remote ? "Left / Right or Nunchuk C / Z" : "Left / Right or L / R", "Switch view in a music library",
        remote ? (DPAD | NUN_C | NUN_Z) : (DPAD | L | R));
    add("B", "Back; from the first page, return to the queue", B);
    if (gamepad) add("Touch", "Tap a row to select, tap it again to open; the buttons add", TOUCH);

    head("Live TV");
    add("B", "Stop", B);
    add("-", "GamePad screen on / off", MINUS);

    head("Anywhere");
    add("HOME", "Back to the Wii U menu (Ufin keeps its place)", HOME);
    return r;
}

// Index of the first row that is not a group title (the cursor's starting place), -1 if none.
inline int firstEntry(const std::vector<Row>& rows) {
    for (size_t i = 0; i < rows.size(); i++)
        if (!rows[i].header) return (int)i;
    return -1;
}

// Moves the cursor by `dir` (+1 / -1) to the next non-header row, wrapping around.
inline int stepEntry(const std::vector<Row>& rows, int cursor, int dir) {
    const int n = (int)rows.size();
    if (n == 0) return -1;
    int i = cursor;
    for (int k = 0; k < n; k++) {
        i = ((i + dir) % n + n) % n;
        if (!rows[(size_t)i].header) return i;
    }
    return cursor;
}

} // namespace help
