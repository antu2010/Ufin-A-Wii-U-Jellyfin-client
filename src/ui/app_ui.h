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
#include <functional>
#include <string>
#include <vector>

struct ImDrawList; // (ImGui, forward-declared: this header stays free of it)
struct ImDrawCmd;

struct ImFont;

namespace ui {

// A texture from ImageCache (an ImTextureID); 0 = no image (yet).
using Texture = uint64_t;

enum class Icon {
    None, Home, LiveTv, Search, Library, Folder, Movie, Series, Episode,
    Music, Album, Channel, Video, Collection, Settings, User, Info, Palette, Snow, Clock, Sparkle,
    Heart, Resume, Gamepad, Remote, Help
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
    std::string usageLabel;        // "Time used" counter above it, e.g. "12h 34m" ("" = hidden)
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
    bool overlay = false;           // drawn over the screen already there (no backdrop or logo of its own)
    int firstLine = 0;              // scroll position: the first line drawn (the card keeps its size)
    mutable int linesShown = 0;     // set by drawMessage: how many lines (from firstLine) fitted on the card
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
    float volume = 1.0f;           // 0..1, drawn as a slider below the transport buttons
    int pressedButton = -1;        // NpButton being touched right now, -1 = none (Liquid Glass pressed look only)
    // Music queue controls: the Shuffle and Repeat buttons beside the transport row. Each says plainly whether
    // it is on (lit in the accent colour, with its state written under it); Repeat has three states.
    bool queueControls = false;    // draw them (a music queue; not the movie remote)
    bool shuffleOn = false;
    int repeatMode = 0;            // 0 = off, 1 = repeat all, 2 = repeat one (the button shows a 1)
};

// The music visualizer (- during audio playback). Bar levels come from
// AudioVisualizer::levels() -- this struct only carries plain numbers so
// the draw function stays host-testable like the rest of ui/.
struct VisualizerModel {
    std::string title;
    bool paused = false;
    static const int MAX_BANDS = 16;
    float bands[MAX_BANDS] = {};   // roughly 0..1, low frequency first
    int bandCount = 0;
    std::vector<Hint> hints;
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

    // Rebuffering: playback is held because the download is behind. Draws the
    // buffering wheel over the (still visible) last picture. Not set while
    // the user has paused -- the pause icon owns that case.
    bool buffering = false;
    double bufferedSeconds = 0.0;        // playable so far
    double bufferingTargetSeconds = 0.0; // where playback resumes; 0 = don't show the readout
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

enum class Accent { Blue, Purple, Green, Orange, Pink, Teal, Crimson, Amber, Indigo, Rainbow, COUNT };
const char* accentName(Accent a);
void setAccent(Accent a);

// Swaps the base neutral palette (background/panel/text/...) for a light
// set instead of the default dark one; every accent colour works with
// either. Independent of setAccent -- the two combine freely.
void setLightMode(bool on);

// Liquid Glass UI (Settings -> Appearance; default off). A pure appearance
// layer: translucent, rounder, softly lit panels over a shared low-resolution
// blurred backdrop (glass_core.h). Off, every screen draws exactly as before.
void setGlass(bool on);
bool glassEnabled();
// True while the shared blur is running (false: Off, or flat glass after a fallback).
bool glassBlurAvailable();
// Once per frame, before the frame's UI is built (Gfx does this): refreshes
// the shared blur in small time-sliced steps. Does nothing when glass is off,
// and nothing at all while a video frame is being presented (videoActive).
void glassFrame(double timeSeconds, bool videoActive);

// Real refraction of what is behind the glass. The platform gives the texture a
// snapshot of the frame is copied into, plus the draw-list callback that does the
// copy (it runs, in render order, just before the first glass of the frame and
// before any glass that sits on top of other glass; `userData` is passed to it as
// ImDrawCmd::UserCallbackData). Call before building each UI frame, since the
// texture is baked into that frame's draw lists. texture 0 / null callback (or
// glass off) = no refraction: glass uses the soft blurred backdrop instead.
using GlassSnapshotCallback = void (*)(const ImDrawList* list, const ImDrawCmd* cmd); // same type as ImDrawCallback
void setGlassSnapshot(uint64_t texture, GlassSnapshotCallback callback, void* userData);
bool glassRefractionActive();
// Snapshots requested so far in the current UI frame (for tests/diagnostics).
int glassSnapshotsThisFrame();
// The image cache's upload/release hooks report artwork here so panels can
// take a faint tint from the selected item's poster. Cheap (an 8x8 sample);
// a no-op while glass is off.
void glassNoteArtwork(uint64_t texture, const uint8_t* rgba, int width, int height);
void glassForgetArtwork(uint64_t texture);

// The animated background effect drawn behind the menus. Every value
// (bar Off) is built from the current accent colours and kept subtle
// enough to work in both light and dark mode without hurting readability.
// The first six are the originals; Grid..Waves were added as Liquid Glass showcases (crisp shapes
// that the glass refraction visibly bends). New modes go before COUNT -- saved configs store the index.
enum class Background { Off, Circles, WiiBubbles, Gradient, Polygons, Starfield,
                        Grid, Aurora, Ripples, Beams, Orbs, Waves, COUNT };
const char* backgroundEffectName(Background b);
void setBackgroundEffect(Background b);

// --- Hidden celebration: tap the sidebar's "Time used" counter five times ---
// True if (x, y) in screen coordinates is on that counter (the sidebar of the browser screens).
bool hitUsageCounter(float x, float y);
// Confetti over the browser plus a big Liquid Glass bubble in the centre showing `big` (e.g.
// "12h 34m") and `small` (e.g. "45,245 seconds in total"). Ends by itself after ~16 s.
void showUsageCelebration(const std::string& big, const std::string& small);
bool usageCelebrationActive();
// Any press or tap should call this; it is ignored for the first ~0.6 s so the fifth tap that
// opened the celebration does not close it again.
void dismissUsageCelebration();
// Jumps the running celebration forward by `frames` frames (tests: reach the bubble and the confetti
// without drawing every frame). No effect when it is not running.
void advanceUsageCelebration(int frames);

// A Jellyfin backdrop behind the whole UI (Settings > Jellyfin backdrops). `prev` is an opaque
// picture underneath (what was up before), `cur` fades in over it at `curAlpha` (0..1); 0 = none.
// Aspects are width/height. With neither set the normal background (static or animated, glass
// field included) is drawn as always; the picture is laid over it, darkened so the UI stays readable.
void setScreenBackdrop(Texture prev, float prevAspect, Texture cur, float curAspect, float curAlpha);

// Call once per frame (drives the Rainbow accent).
void tickTheme(double time);

// Falling snow over the menus (drawn on top; call after the screen).
void drawSnow(double time);

// ImGui style colours for anything drawn with widgets (little is).
void applyTheme();

void drawBrowser(const BrowserModel& model);
void drawMessage(const MessageModel& model);
void drawNowPlaying(const NowPlayingModel& model);
void drawMusicVisualizer(const VisualizerModel& model);
void drawVideoHud(const VideoHudModel& model);
// Just the buffering wheel + "Buffering 4 / 12 s" pill, centred where the
// pause icon sits. drawVideoHud() uses it; the TV-only mode calls it directly
// so the clean TV picture can still show that playback is waiting for data.
// A few vertices and one text run per frame -- no textures, no allocation --
// and the spin is a pure function of ImGui::GetTime(), so it needs no state.
void drawBufferingIndicator(double bufferedSeconds, double targetSeconds);
void drawLogin(const LoginModel& model);
void drawChoice(const ChoiceModel& model);   // over whatever is drawn already
void drawUpNext(const UpNextModel& model);
void drawQuickConnect(const QuickConnectModel& model);

// A short notice at the top of the screen ("Menu music: ..."), fading
// out over its last half second. `age` is seconds since it appeared.
void drawToast(const std::string& text, double age, double lifetime = 2.5);
// The same pill with a thin progress bar under the text (fraction 0..1), for something still running.
void drawProgressToast(const std::string& text, float fraction, float y = 24.0f);

// The CRT easter egg: scanlines, a soft glow, a rolling bright band and
// a vignette with rounded dark corners, over everything drawn this
// frame (video included). `time` in seconds drives the animation.
void drawCrtOverlay(double time);

// Text of at most maxWidth pixels at `size`, cut with "..." at a UTF-8
// character boundary. Exposed for tests.
std::string fitText(ImFont* font, float size, const std::string& text, float maxWidth);


// --- Help window (sidebar > Help) ---
// A big card over the dimmed browser: a rotating 3D controller on the left (a .glb from the SD card, see
// setModelBackend; "Missing model" when the file is not there), and on the right what
// each button does on each screen. One page (tab) per controller. Liquid Glass or flat, like every
// other card.
struct HelpRow {
    bool header = false;       // a group title; `action` is its text
    std::string button, action;
    uint32_t highlight = 0;    // buttons this row is about (help::Btn bits); not drawn on the SD-card models
};
struct HelpModel {
    std::vector<std::string> tabs; // controller names, one per page
    int tab = 0;                   // the page shown (and which model is drawn: 0 GamePad, 1 Pro, 2 Classic, 3 Wii Remote)
    std::string name, note;        // under the model
    std::vector<HelpRow> rows;
    int cursor = 0;                // selected row (never a header)
    std::vector<Hint> hints;       // button hints along the bottom of the card
    int frame = 0;                 // animation clock, 60 per second
};
void drawHelp(const HelpModel& m);

// The Help models are read from `dir` (default help::kModelDir) on a worker thread the first time a
// page is shown. `upload` / `release` turn RGBA pixels into a texture handle and back (uploadTexture /
// releaseTexture on the Wii U); without `upload` models are drawn in flat colours.
void setModelDir(const std::string& dir);
void setModelBackend(std::function<uint64_t(const uint8_t* rgba, int w, int h)> upload,
                     std::function<void(uint64_t)> release);
// Forgets every loaded model (and frees its textures) so the files are read again. Between frames only.
void resetHelpModels();

struct HelpHit {
    enum Kind { None, Close, Tab, Row };
    Kind kind = None;
    int index = -1;
};
// What a tap at (x, y) hits: a controller tab, a row, or outside the card (Close).
HelpHit hitTestHelp(const HelpModel& m, float x, float y);

} // namespace ui
