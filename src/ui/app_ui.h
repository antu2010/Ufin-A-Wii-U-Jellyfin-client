// Ufin's screens, drawn with Dear ImGui draw lists: the library browser
// (sidebar + list), message cards, Now Playing, and the HUD over video.
//
// Each screen takes a plain model and draws into ImGui's background draw
// list in the virtual 1280x720 space of ui/layout.h. Nothing here knows
// about Jellyfin, VPAD or GX2, so the same code runs in the host tests.
// Look: dark theme with a left sidebar, a list browser and a player HUD
// -- the general layout CafeMP popularised on the Wii U, drawn with
// Ufin's own shapes and icons (no assets needed).
#pragma once
#include "layout.h"

#include <cstdint>
#include <string>
#include <vector>

struct ImFont;

namespace ui {

// A texture from ImageCache (an ImTextureID); 0 = no image (yet).
using Texture = uint64_t;

enum class Icon {
    None, Home, LiveTv, Search, Library, Folder, Movie, Series, Episode,
    Music, Album, Channel, Video, Collection, Settings, User, Info, Palette, Snow, Clock, Sparkle,
    Heart, Resume, Gamepad, Remote
};

struct Hint {
    std::string button; // "A", "B", "X", "Y", "+", "L R" ...
    std::string label;
};

struct ListEntry {
    std::string name;
    std::string tag;     // right-aligned, e.g. "2008  1:52:00"
    std::string detail;  // second line
    Icon icon = Icon::None;
    Texture art = 0;     // poster / cover / logo; the icon shows until it loads
    float artAspect = 0; // width / height of `art`

    bool watched = false;   // tick on the artwork
    bool favorite = false;  // heart next to the tag
    float progress = 0.0f;  // partly watched: bar under the artwork (0..1)
    int unplayed = -1;      // shows / seasons: unwatched count badge
};

struct SidebarEntry {
    std::string label;
    Icon icon = Icon::None;
};

struct BrowserModel {
    std::vector<SidebarEntry> sidebar;
    int sidebarSelected = 0;
    bool sidebarFocused = false;
    std::string footnote;          // bottom of the sidebar, e.g. "user @ server"
    std::string clock;             // e.g. "21:37", empty = no clock

    std::string title;             // current folder
    std::string path;              // breadcrumb above it
    std::vector<ListEntry> items;
    int selected = 0;
    std::string emptyMessage = "Nothing here yet.";
    std::vector<Hint> hints;
};

struct MessageModel {
    std::string title;
    std::vector<std::string> lines; // word-wrapped; "" = gap
    std::vector<Hint> hints;
    bool isError = false;
    bool busy = false;              // animated dots after the title
};

struct NowPlayingModel {
    std::string title;
    std::string subtitle;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;  // 0 = unknown
    bool paused = false;
    std::string queueText;         // "Track 3 of 12  |  Shuffle", empty = no queue
    std::string nextText;          // "Next: ..."
    std::vector<Hint> hints;
    Texture art = 0;               // album art, 0 = gradient placeholder
};

// A small dialog: title, a line of text, and a column of choices.
struct ChoiceModel {
    std::string title;
    std::string subtitle;
    std::vector<std::string> options;
    int selected = 0;
    std::vector<Hint> hints;
    Texture art = 0;       // optional artwork next to the text
    float artAspect = 0;
};

// "Up next" between episodes, with a countdown.
struct UpNextModel {
    std::string showName;
    std::string episodeTitle;  // "S2E5  The Episode"
    std::string detail;
    double secondsLeft = 10.0;
    double totalSeconds = 10.0;
    Texture art = 0;
    float artAspect = 0;
    std::vector<Hint> hints;
};

struct VideoHudModel {
    bool visible = false;          // title/progress panel
    std::string title;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    bool paused = false;
    bool live = false;
    double seekDelta = 0.0;        // pending skip, 0 = none
    double seekTarget = 0.0;
    std::vector<Hint> hints;

    // Audio & subtitles panel (Y during playback)
    bool trackMenu = false;
    int trackRow = 0;              // 0 = audio, 1 = subtitles
    std::string audioLabel;
    std::string subtitleLabel;
    std::string notice;            // short line at the top ("GamePad screen off")
};

struct LoginField {
    std::string label;     // "Server"
    std::string value;     // shown (password fields show dots)
    std::string placeholder;
    bool password = false;
    bool isButton = false; // a button row instead of a field
    bool primary = false;  // the main button (accent coloured)
};

struct LoginModel {
    std::vector<LoginField> rows;
    int focused = 0;
    std::string message;   // under the rows
    bool messageIsError = false;
    bool busy = false;     // animated dots after the message
    std::vector<Hint> hints;
};

struct QuickConnectModel {
    std::string code;      // "123456"
    std::string server;    // "192.168.1.100:8096"
    std::string message;   // status / error
    bool messageIsError = false;
    std::vector<Hint> hints;
};

// --- theme ---

enum class Accent { Blue, Purple, Green, Orange, Pink, Rainbow, COUNT };
const char* accentName(Accent a);
void setAccent(Accent a);

// Soft glowing shapes drifting slowly behind the menus.
void setAmbientBackground(bool on);

// Call once per frame (drives the Rainbow accent).
void tickTheme(double time);

// Falling snow over the menus (drawn on top; call after the screen).
void drawSnow(double time);

// ImGui style colours for anything drawn with widgets (little is).
void applyTheme();

void drawBrowser(const BrowserModel& model);
void drawMessage(const MessageModel& model);
void drawNowPlaying(const NowPlayingModel& model);
void drawVideoHud(const VideoHudModel& model);
void drawLogin(const LoginModel& model);
void drawChoice(const ChoiceModel& model);   // over whatever is drawn already
void drawUpNext(const UpNextModel& model);
void drawQuickConnect(const QuickConnectModel& model);

// A short notice at the top of the screen ("Menu music: ..."), fading
// out over its last half second. `age` is seconds since it appeared.
void drawToast(const std::string& text, double age, double lifetime = 2.5);

// The CRT easter egg: scanlines, a soft glow, a rolling bright band and
// a vignette with rounded dark corners, over everything drawn this
// frame (video included). `time` in seconds drives the animation.
void drawCrtOverlay(double time);

// Text of at most maxWidth pixels at `size`, cut with "..." at a UTF-8
// character boundary. Exposed for tests.
std::string fitText(ImFont* font, float size, const std::string& text, float maxWidth);

} // namespace ui
