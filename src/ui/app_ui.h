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
#include "../lyrics.h"

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

// An optional control drawn inside a list row (Settings > Customization).
struct RowWidget {
    enum class Kind { None, Swatches, Slider, Preview, Shapes, ButtonPreview };
    Kind kind = Kind::None;
    std::vector<uint32_t> colours; // Swatches: chips as 0xRRGGBB (bit 24 set = the rainbow chip)
    int selected = -1;             // Swatches: the chosen chip
    bool numbered = false;         // Swatches: draw 1, 2, 3 on the chips (the palette's colours)
    float value = 0.0f;            // Slider: 0..1
    uint32_t trackLo = 0, trackHi = 0; // Slider: the track runs from one colour to the other (0xRRGGBB)
    bool hueTrack = false;         // Slider: the track is the rainbow instead
    std::vector<int> sizes;        // ButtonPreview: width, height (percent) per button kind; `shapes` holds the 4 shapes
    std::vector<int> shapes;       // Shapes: the button shapes on offer (ids as buttonShapeName)
    int count() const { return kind == Kind::Shapes ? (int)shapes.size() : (int)colours.size(); }
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
    RowWidget widget;       // swatches / slider / preview inside the row
    bool grouped = false;   // part of one long panel with the neighbouring grouped rows (the palette editor)
    bool dimmed = false;    // greyed out: shown, but not usable right now
    bool addable = false;   // a song: shows the little "+" (touch: add to a playlist)
};

struct SidebarEntry {
    std::string label;
    Icon icon = Icon::None;
};

// Floating round buttons at the bottom right of a list (circle in Plain and Liquid Glass, the cookie in M3E).
enum class FabKind { Sort, NewPlaylist };

// The sort flyout, anchored above the Sort button: the criteria, then the direction.
struct SortMenuModel {
    bool open = false;
    int anchor = 0;                    // index into BrowserModel::fabs of the Sort button
    std::string title = "Sort by";
    std::vector<std::string> options;  // criteria offered
    int current = 0;                   // the active one (index into options)
    int cursor = 0;                    // 0..options.size(); options.size() = the direction row
    bool descending = false;
    std::string directionText;         // "Newest first"
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
    std::vector<std::string> tabs; // page tabs under the title (Settings); empty = none
    int tabSelected = 0;
    std::string emptyMessage = "Nothing here yet.";
    std::vector<Hint> hints;
    std::vector<FabKind> fabs;     // floating buttons, left to right (the last is in the corner)
    int fabFocus = -1;             // the one the controller cursor is on, -1 = none
    SortMenuModel sortMenu;        // the flyout, drawn over everything while open
    int total = 0;                 // items in the whole list when only part is loaded (0 = items.size())
};

// The New playlist dialog.
struct PlaylistFormModel {
    std::string title = "New playlist";
    std::string name;
    bool showVisibility = false;   // only on servers that have playlist visibility
    bool isPublic = false;
    int focus = 0;                 // 0 name, 1 visibility, 2 Create, 3 Cancel
    std::string message;           // an error (kept with what was typed) or "Creating ..."
    bool messageIsError = false;
    bool busy = false;             // a request is running: Create is greyed
    std::vector<Hint> hints;
};

// "Add to playlist": a compact scrolling list of the user's playlists, "New playlist" first.
struct PlaylistPickerModel {
    std::string title = "Add to playlist";
    std::string subtitle;          // the song
    std::vector<std::string> rows; // row 0 is "New playlist"
    int selected = 0;
    std::string message;           // e.g. "No playlists yet" or an error
    bool busy = false;
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

// One line of the queue flyout / the Add to Queue browser.
struct QueueRow {
    std::string title;
    std::string sub;       // artist / show and episode / year ...
    Texture art = 0;
    bool container = false; // browser: opens (album, show ...) rather than being a single item
};

// The Up Next flyout of Now Playing (anchored to the Queue button).
struct QueueFlyoutModel {
    bool open = false;       // asked for; the drawing eases in / out by itself
    bool visible = false;    // rows are filled in (open, or still fading out)
    std::string title = "Up Next";
    QueueRow current;        // what plays now
    // upcoming, in play order. Only the rows near the cursor are filled in: rows[0] is entry `first` of `count`.
    std::vector<QueueRow> rows;
    int first = 0, count = 0;
    int cursor = 0;          // -2 Add to Queue, -1 Clear, 0.. a row
    bool confirmClear = false;
    std::string toast;       // short feedback line ("Added 12 songs")
    std::string empty = "Nothing queued. Add songs, movies or episodes.";
};

// The popup media browser opened by "Add to Queue".
struct QueueBrowserModel {
    bool open = false;
    bool visible = false;
    std::string title = "Add to Queue";
    std::string context;     // "Music  >  Albums  >  Abbey Road"
    std::vector<std::string> tabs; int tab = 0;
    std::vector<QueueRow> rows; // the visible window: rows[0] is entry `first` of `count`
    int first = 0, count = 0;
    int selected = 0;
    int total = 0;           // size of the whole list on the server (rows may be fewer: pages load as you scroll)
    bool busy = false;
    Texture cover = 0; float coverAspect = 1.0f;
    std::string coverTitle, coverSub;
    std::string addLabel = "Add";
    bool canAddAll = false;
    std::string toast;
    std::string empty = "Nothing here.";
    std::vector<Hint> hints;
};

// The movie remote's settings flyout (the gear at the top right): what plays now, changeable for this viewing only.
struct VideoSettingsModel {
    bool gear = false;              // show the gear button (movie remote only)
    bool open = false;
    std::vector<std::string> qualities; // the presets, in order
    int quality = -1;               // the one in use (-1 = custom)
    int cursor = 0;                 // controller cursor: 0.. quality rows, then audio, then subtitles
    std::string audioLabel, subtitleLabel;
    bool hasAudio = false, hasSubtitles = false;
    std::string toast;
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
    // Lyrics (music only). `lyrics` is null / empty when the song has none: the Lyrics button is then greyed.
    // lyricsOpen slides the player to the left and shows them; lyricsSync follows the song line by line
    // (when the lyrics carry timing) instead of just drifting through the text.
    const lyrics::Lyrics* lyrics = nullptr;
    bool lyricsOpen = false;
    bool lyricsSync = true;
    bool lyricsTouching = false;   // a finger is dragging the lyrics
    float lyricsDragDy = 0.0f;     // how far it moved down (px) since the last frame
    bool lyricsCapable = false;    // a music player (has the Lyrics button); the movie remote is not
    // Queue button (music) and its flyout. queueCount < 0 = no button (the movie remote).
    int queueCount = -1;           // upcoming entries in the Up Next list
    bool queueFocus = false;
    QueueFlyoutModel flyout;
    VideoSettingsModel settings;
    bool scrubbing = false;        // a finger is on the progress bar: it shows scrubSeconds, and lifting seeks there
    double scrubSeconds = 0.0;
    QueueBrowserModel browser;
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
    // Look: 0 Bars, 1 Terrain (3D), 2 Segments, 3 Radial, 4 Wave (the "-" button steps through them, then switches off).
    static const int STYLE_COUNT = 5;
    int style = 0;
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

// The prompt shown when a video starts: which audio track and subtitles to use. The two selector rows
// work like the Y panel during playback; OK plays, "Always use default" plays and stops asking.
struct TrackPromptModel {
    std::string title;
    std::string subtitle;
    std::string audioLabel;
    std::string subtitleLabel;
    int selected = 2;              // 0 audio, 1 subtitles, 2 OK, 3 always use default
    std::vector<Hint> hints;
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
// Button shapes (Material 3 Expressive; other styles ignore them). Kind: 0 play / pause, 1 seek & skip, 2 toggles
// (shuffle, repeat, lyrics), 3 dialog and login buttons. Shape 0 = the default look; out-of-range values fall back to it.
void setButtonShapes(const int shapes[4]);
// Width / height of each kind of button in percent of the normal size (100 = normal), {w0, h0, w1, h1, ...}. Clamped to
// buttonSizeMin()..buttonSizeMax().
void setButtonSizes(const int sizes[8]);
int buttonSizeMin();
int buttonSizeMax(int index); // index = kind * 2 + (0 width, 1 height)
int buttonShapeCount(int kind);
const char* buttonShapeName(int shape);
const char* buttonKindName(int kind);
uint32_t accentSwatchColour(Accent a); // 0xRRGGBB for the Customization chips (bit 24 = the rainbow chip)

// --- global theme customisation (shared by every renderer: Plain, Liquid Glass, Material 3 Expressive, the video
// HUD, the GamePad remote -- they all read the same three theme colours) ---
// Custom palette: three colours (0xRRGGBB) plus an arrangement 0..5 (which colour is main / deep / second).
// on = false returns to the premade accent chosen with setAccent, which is remembered meanwhile.
void setCustomPalette(bool on, const uint32_t rgbColours[3], int arrange);
bool customPaletteActive();
// Artwork-reactive accent (Material 3 Expressive only; other styles ignore it). `enabled` is the Settings switch,
// `playing` is true while a music / video player is on screen, and the colour is the playing item's dominant colour
// (valid = false when there is no usable artwork). The theme eases to it and back; menus keep the palette.
void setArtworkAccent(bool enabled);
void setArtworkContext(bool playing);
void setArtworkColour(bool valid, uint32_t rgbColour);
bool artworkAccentActive();
float artworkMix(); // 0 = selected palette .. 1 = artwork colour (for tests)

// Swaps the base neutral palette (background/panel/text/...) for a light
// set instead of the default dark one; every accent colour works with
// either. Independent of setAccent -- the two combine freely.
void setLightMode(bool on);

// UI style: exactly one of three looks is active. Plain is the original flat UI; Liquid Glass and
// Material 3 Expressive (M3E) are appearance layers on top of the same screens and layout. All three use
// the one user-selected accent colour (setAccent) and light/dark flag (setLightMode).
// M3E: tonal surfaces and a palette derived from the accent, pill / cookie / squiggle shapes, spring
// morphs on focus, faux-bold headings, and soft frosted dialogs over the calm shared blur (no refraction).
enum class Style { Plain, LiquidGlass, Expressive };
void setStyle(Style s);
Style style();
const char* styleName(Style s);
void setExpressive(bool on); // (turns Liquid Glass off when on)
bool expressiveEnabled();

// Liquid Glass UI (Settings -> Appearance; default off). A pure appearance
// layer: translucent, rounder, softly lit panels over a shared low-resolution
// blurred backdrop (glass_core.h). Off, every screen draws exactly as before.
void setGlass(bool on); // (turns M3E off when on)
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
// Liquid Glass only: the frost on the refracted picture. `level` 0 = sharp, 1/2/3 = light / medium / strong (default 1): the
// picture it bends is a shrunk, smoothed copy. The blurred texture is made by the renderer (gfx.cpp) from the snapshot
// and handed over with setGlassBlurTexture(); without one, the sharp snapshot is used.
void setGlassBlur(int level);
void setGlassBlurTexture(uint64_t texture);
int glassBlurLevel();
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
void drawPlaylistForm(const PlaylistFormModel& model);     // over whatever is drawn already
void drawPlaylistPicker(const PlaylistPickerModel& model); // over whatever is drawn already

// Touch tests for the library chrome. All return -1 for "nothing".
int hitTestFab(const BrowserModel& m, float x, float y);                  // index into m.fabs
// Sort flyout: 0..options-1 a criterion, options = the direction row, -2 = on the card but on no row.
int hitTestSortMenu(const BrowserModel& m, float x, float y);
bool hitTestRowAdd(const BrowserModel& m, int itemIndex, int slot, float x, float y); // the "+" on row `slot`
// Form: 0 name, 1 visibility (Private half = 10, Public half = 11), 2 Create, 3 Cancel.
int hitTestPlaylistForm(const PlaylistFormModel& m, float x, float y);
// Picker: row index (into m.rows, accounting for the scroll window), or -1.
int hitTestPlaylistPicker(const PlaylistPickerModel& m, float x, float y);
// The user activated something (A button, a tap). M3E bounces the rows next to the activated one and replays its
// icon's animation; the other styles ignore it. Cheap: sets two numbers.
void pulse();
// Call once after the first (TV) pass of a two-pass TV + GamePad tick, so animations count ticks, not passes.
void noteExtraPass();
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
void drawTrackPrompt(const TrackPromptModel& model);   // a whole screen (backdrop + card), like drawMessage
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
