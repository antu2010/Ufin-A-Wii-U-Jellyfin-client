// Screen geometry for the Ufin UI, in a virtual 1280x720 space.
//
// Everything is drawn in these coordinates and scaled to the real TV
// buffer (720p or 1080p) by the renderer; the GamePad shows a scaled
// copy of the TV picture, and its touch screen is read in the same
// 1280x720 space -- so drawing and touch hit-testing both come from the
// functions here and can't disagree. Pure math, host-tested.
#pragma once
#include <string>

namespace ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

namespace layout {

static const float SCREEN_W = 1280.0f;
static const float SCREEN_H = 720.0f;

// Left sidebar: logo on top, then one button per section.
static const float SIDEBAR_W = 240.0f;
static const float SIDEBAR_ITEM_TOP = 132.0f;
static const float SIDEBAR_ITEM_H = 56.0f;
static const float SIDEBAR_ITEM_GAP = 8.0f;
static const float SIDEBAR_PAD = 16.0f;

// Content area right of the sidebar.
static const float CONTENT_X = SIDEBAR_W + 32.0f;
static const float CONTENT_RIGHT = SCREEN_W - 32.0f;
static const float CONTENT_W = CONTENT_RIGHT - CONTENT_X;

static const float HEADER_TITLE_Y = 34.0f;
static const float HEADER_PATH_Y = 82.0f;

static const float LIST_TOP = 124.0f;
static const float LIST_BOTTOM = 640.0f;
static const float ROW_H = 68.0f;
static const float ROW_GAP = 6.0f;

static const float FOOTER_Y = 664.0f;

static const float SCROLLBAR_W = 6.0f;

// Artwork slot at the left of each list row; images are fitted inside
// (posters come out 40x60, covers 60x60, channel logos wide).
static const float ROW_ART_W = 64.0f;
static const float ROW_ART_H = 60.0f;

// Now Playing artwork square.
static const float NP_ART = 340.0f;
static const float NP_ART_X = 110.0f;
static const float NP_ART_Y = 150.0f;

// Now Playing text column, and the fixed vertical rhythm within it:
// label, a title block of constant height (regardless of how many
// lines the title actually wraps to, so nothing below it ever moves),
// subtitle, progress bar, time row, transport buttons. Shared by
// drawNowPlaying (drawing) and the transport-button hit-test below, so
// touch and drawing can never disagree -- same reasoning as the file
// comment at the top for hitTestBrowser et al.
static const float NP_TEXT_X = NP_ART_X + NP_ART + 60.0f;
static const float NP_LABEL_Y = NP_ART_Y + 20.0f;
static const float NP_TITLE_BLOCK_H = 110.0f;
static const float NP_SUBTITLE_Y = NP_LABEL_Y + 36.0f + NP_TITLE_BLOCK_H;
static const float NP_PROGRESS_Y = NP_SUBTITLE_Y + 60.0f;
static const float NP_TEXT_W = SCREEN_W - 90.0f - NP_TEXT_X; // = progress bar width
static const float NP_TIME_Y = NP_PROGRESS_Y + 22.0f;
static const float NP_BUTTONS_Y = NP_TIME_Y + 40.0f;
static const float NP_BUTTON_SIZE = 56.0f;
static const float NP_BUTTON_SIZE_BIG = 78.0f; // play/pause, the primary action
static const float NP_BUTTON_GAP = 26.0f;
static const float NP_TOGGLE_SIZE = 52.0f; // Shuffle / Repeat toggles of a music queue

// Volume slider, directly under the transport buttons: a plain
// horizontal drag-track the same width as the progress bar. NP_VOLUME_H
// is the touchable height (see ui::npVolumeRect below), taller than the
// thin line actually drawn, so it's comfortable to drag on the GamePad
// touch screen.
static const float NP_VOLUME_Y = NP_BUTTONS_Y + NP_BUTTON_SIZE_BIG + 30.0f;
static const float NP_VOLUME_H = 28.0f;
static const float NP_QUEUE_Y = NP_VOLUME_Y + NP_VOLUME_H + 24.0f;

// Largest image size worth fetching for each spot (in pixels of the
// 1080p TV buffer, the sharpest case -- 1.5x the layout size).
static const int ROW_IMAGE_MAX_W = 96;
static const int ROW_IMAGE_MAX_H = 90;
static const int NP_IMAGE_MAX = 510;

// Login screen: branding on the left, a column of fields and buttons on
// the right.
static const float LOGIN_X = 540.0f;
static const float LOGIN_W = 660.0f;
static const float LOGIN_TOP = 150.0f;
static const float LOGIN_ROW_H = 72.0f;
static const float LOGIN_ROW_GAP = 12.0f;

inline Rect loginRow(int index) {
    Rect r;
    r.x = LOGIN_X;
    r.y = LOGIN_TOP + index * (LOGIN_ROW_H + LOGIN_ROW_GAP);
    r.w = LOGIN_W;
    r.h = LOGIN_ROW_H;
    return r;
}

// Where a w:h image fits inside a box, centred, keeping its shape.
struct Fit { float x, y, w, h; };
inline Fit fitInside(float boxX, float boxY, float boxW, float boxH, float aspect) {
    Fit f{boxX, boxY, boxW, boxH};
    if (aspect <= 0.0f) return f;
    if (aspect > boxW / boxH) { f.h = boxW / aspect; f.y = boxY + (boxH - f.h) * 0.5f; }
    else { f.w = boxH * aspect; f.x = boxX + (boxW - f.w) * 0.5f; }
    return f;
}

inline int visibleRows() {
    return (int)((LIST_BOTTOM - LIST_TOP + ROW_GAP) / (ROW_H + ROW_GAP));
}

// Row `slot` of the visible list window (0 = top row on screen).
// Room kept clear at the right of the rows for the floating buttons (0 = none). Set by drawBrowser from what the
// screen has, read by the touch hit tests, so the two always agree.
inline float& listInset() { static float v = 0.0f; return v; }

inline Rect listRow(int slot) {
    Rect r;
    r.x = CONTENT_X;
    r.y = LIST_TOP + slot * (ROW_H + ROW_GAP);
    r.w = CONTENT_W - SCROLLBAR_W - 10.0f - listInset();
    r.h = ROW_H;
    return r;
}

// Settings pages: two tabs under the title ("Customization", "Settings"), where the breadcrumb sits elsewhere.
static const float TAB_W = 210.0f, TAB_H = 34.0f, TAB_GAP = 10.0f, TAB_Y = 78.0f;
inline Rect settingsTab(int index) {
    Rect r;
    r.x = CONTENT_X + 34.0f + index * (TAB_W + TAB_GAP); // (34: room for the "L" key hint before the first tab)
    r.y = TAB_Y;
    r.w = TAB_W;
    r.h = TAB_H;
    return r;
}

// Widgets inside a list row (Settings > Customization). All in the row's own coordinates so touch and drawing agree.
// Swatches: n round chips, right-aligned, as large as the room allows (at most 36 px).
inline float swatchSize(const Rect& row, int n) {
    const float room = row.w - 360.0f; // the name keeps the left part
    float s = n > 0 ? room / (float)n - 6.0f : 36.0f;
    return s > 36.0f ? 36.0f : (s < 18.0f ? 18.0f : s);
}
inline Rect rowSwatch(const Rect& row, int i, int n) {
    const float s = swatchSize(row, n);
    Rect r;
    r.w = s + 6.0f; // (the touch target is the chip plus its gap)
    r.h = row.h;
    r.x = row.right() - 12.0f - (float)(n - i) * r.w;
    r.y = row.y;
    return r;
}
// Slider: [-] track [+] near the right edge, the value text after them.
inline Rect rowSliderMinus(const Rect& row) { return {row.right() - 424.0f, row.y + 8.0f, 52.0f, row.h - 16.0f}; }
inline Rect rowSliderTrack(const Rect& row) { return {row.right() - 368.0f, row.y, 196.0f, row.h}; }
inline Rect rowSliderPlus(const Rect& row) { return {row.right() - 164.0f, row.y + 8.0f, 52.0f, row.h - 16.0f}; }
// 0 = no hit, 1 = minus, 2 = plus, 3 = track (frac = 0..1 along it)
inline int sliderHit(const Rect& row, float x, float y, float& frac) {
    if (rowSliderMinus(row).contains(x, y)) return 1;
    if (rowSliderPlus(row).contains(x, y)) return 2;
    const Rect t = rowSliderTrack(row);
    if (t.contains(x, y)) {
        frac = (x - (t.x + 8.0f)) / (t.w - 16.0f);
        frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
        return 3;
    }
    return 0;
}
inline int swatchHit(const Rect& row, int n, float x, float y) {
    for (int i = 0; i < n; i++)
        if (rowSwatch(row, i, n).contains(x, y)) return i;
    return -1;
}

inline Rect sidebarItem(int index) {
    Rect r;
    r.x = SIDEBAR_PAD;
    r.y = SIDEBAR_ITEM_TOP + index * (SIDEBAR_ITEM_H + SIDEBAR_ITEM_GAP);
    r.w = SIDEBAR_W - 2 * SIDEBAR_PAD;
    r.h = SIDEBAR_ITEM_H;
    return r;
}

// The "Time used" counter at the bottom of the sidebar, above the user@server footnote. Generous
// for a finger: tapping it five times is the hidden celebration.
inline Rect usageCounter() {
    Rect r;
    r.x = SIDEBAR_PAD;
    r.y = SCREEN_H - 92.0f;
    r.w = SIDEBAR_W - 2 * SIDEBAR_PAD;
    r.h = 44.0f;
    return r;
}

// --- floating buttons (Sort; New playlist) ---
// Round, bottom-right of the content area, the same on every screen that has them. `count` buttons sit side by
// side, the last one in the corner; index 0 is the leftmost.
static const float FAB = 56.0f, FAB_RIGHT = 48.0f, FAB_GAP = 16.0f;
inline Rect fabRect(int index, int count) {
    Rect r;
    r.w = FAB;
    r.h = FAB;
    r.x = SCREEN_W - FAB_RIGHT - FAB - (count - 1 - index) * (FAB + FAB_GAP);
    r.y = LIST_BOTTOM - FAB - 4.0f;
    return r;
}
// The rows' right edge moves left by this much while `count` floating buttons are shown.
inline float fabInset(int count) { return count <= 0 ? 0.0f : 68.0f + (count - 1) * (FAB + FAB_GAP); }

// The little "+" on a song row (touch: add this song to a playlist). Right end of the row.
inline Rect rowAddButton(const Rect& row) {
    Rect r;
    r.w = 38.0f;
    r.h = 38.0f;
    r.x = row.right() - 14.0f - r.w;
    r.y = row.y + (row.h - r.h) * 0.5f;
    return r;
}

// Sort flyout: a compact card anchored above its button, right edges aligned. Rows 0..options-1 are the criteria,
// row `options` is the direction toggle.
static const float SORT_W = 360.0f, SORT_HEAD = 72.0f, SORT_ROW = 40.0f;
inline Rect sortMenuCard(int options, const Rect& anchor) {
    Rect r;
    r.w = SORT_W;
    r.h = SORT_HEAD + (options + 1) * SORT_ROW + 12.0f;
    r.x = anchor.right() - r.w;
    if (r.x < CONTENT_X) r.x = CONTENT_X;
    r.y = anchor.y - 12.0f - r.h;
    if (r.y < 8.0f) r.y = 8.0f;
    return r;
}
inline Rect sortMenuRow(int options, const Rect& anchor, int row) {
    const Rect c = sortMenuCard(options, anchor);
    return Rect{c.x + 8.0f, c.y + SORT_HEAD + row * SORT_ROW, c.w - 16.0f, SORT_ROW - 4.0f};
}

// New-playlist dialog: name field, (visibility), Create / Cancel. Slots: 0 name, 1 visibility, 2 Create, 3 Cancel.
inline Rect plFormCard(bool visibility) {
    Rect r;
    r.w = 640.0f;
    r.h = visibility ? 400.0f : 330.0f;
    r.x = (SCREEN_W - r.w) * 0.5f;
    r.y = 130.0f;
    return r;
}
inline Rect plFormSlot(int slot, bool visibility) {
    const Rect c = plFormCard(visibility);
    Rect r;
    r.x = c.x + 30.0f;
    r.w = c.w - 60.0f;
    r.h = 58.0f;
    const float top = c.y + 96.0f;
    if (slot == 0) r.y = top;
    else if (slot == 1) r.y = top + 70.0f;
    else {
        r.y = c.bottom() - 30.0f - r.h;
        r.w = (c.w - 60.0f - 14.0f) * 0.5f;
        if (slot == 3) r.x = c.x + 30.0f + r.w + 14.0f;
    }
    return r;
}
// The two halves of the visibility row: 0 = Private, 1 = Public.
inline Rect plFormVisChip(int which, bool visibility) {
    const Rect r = plFormSlot(1, visibility);
    Rect h{r.x + r.w * 0.5f * which, r.y, r.w * 0.5f, r.h};
    return h;
}
// Playlist picker (add a song to a playlist): a compact scrolling list over the screen.
static const int PICK_ROWS = 7;
static const float PICK_ROW_H = 54.0f;
inline Rect pickCard() { Rect r; r.w = 560.0f; r.h = 132.0f + PICK_ROWS * PICK_ROW_H + 16.0f; r.x = (SCREEN_W - r.w) * 0.5f; r.y = 90.0f; return r; }
inline Rect pickRow(int slot) {
    const Rect c = pickCard();
    return Rect{c.x + 20.0f, c.y + 100.0f + slot * PICK_ROW_H, c.w - 40.0f, PICK_ROW_H - 6.0f};
}

inline Rect scrollTrack() {
    Rect r;
    r.x = CONTENT_RIGHT - SCROLLBAR_W;
    r.y = LIST_TOP;
    r.w = SCROLLBAR_W;
    r.h = visibleRows() * (ROW_H + ROW_GAP) - ROW_GAP;
    return r;
}

} // namespace layout

// Which slice [start, end) of `total` items is visible when `selected`
// must be on screen and `visible` rows fit. Keeps the selection roughly
// centred and never scrolls past either end.
struct ListWindow {
    int start = 0;
    int end = 0;
};

inline ListWindow computeListWindow(int total, int selected, int visible) {
    ListWindow w;
    if (total <= 0 || visible <= 0) return w;
    if (selected < 0) selected = 0;
    if (selected >= total) selected = total - 1;
    if (total <= visible) {
        w.end = total;
        return w;
    }
    int start = selected - visible / 2;
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible;
    w.start = start;
    w.end = start + visible;
    return w;
}

// What a touch at (x, y) hits on the browser screen.
struct Hit {
    enum class Kind { None, Sidebar, Row };
    Kind kind = Kind::None;
    int index = -1; // sidebar item, or item index in the full list
};

// Settings tab under the touch point (-1 = none).
inline int hitTestSettingsTab(float x, float y, int n) {
    for (int i = 0; i < n; i++)
        if (layout::settingsTab(i).contains(x, y)) return i;
    return -1;
}

inline Hit hitTestBrowser(float x, float y, int sidebarCount, const ListWindow& window) {
    Hit h;
    for (int i = 0; i < sidebarCount; i++) {
        if (layout::sidebarItem(i).contains(x, y)) {
            h.kind = Hit::Kind::Sidebar;
            h.index = i;
            return h;
        }
    }
    for (int i = window.start; i < window.end; i++) {
        if (layout::listRow(i - window.start).contains(x, y)) {
            h.kind = Hit::Kind::Row;
            h.index = i;
            return h;
        }
    }
    return h;
}

// Choice dialog rows (centred card).
inline Rect choiceRow(int index, int count) {
    Rect r;
    r.w = 520.0f;
    r.h = 62.0f;
    r.x = (layout::SCREEN_W - r.w) * 0.5f;
    float listH = count * (r.h + 12.0f) - 12.0f;
    float top = 360.0f - listH * 0.5f + 70.0f;
    r.y = top + index * (r.h + 12.0f);
    return r;
}

inline int hitTestChoice(float x, float y, int count) {
    for (int i = 0; i < count; i++) if (choiceRow(i, count).contains(x, y)) return i;
    return -1;
}

// First-play prompt (audio & subtitles): a centred card with two selector rows, then "OK" and
// "Always use default" side by side. Slots: 0 = audio row, 1 = subtitles row, 2 = OK, 3 = always default.
inline Rect trackPromptCard() { Rect r; r.w = 680.0f; r.h = 392.0f; r.x = (layout::SCREEN_W - r.w) * 0.5f; r.y = 140.0f; return r; }
inline Rect trackPromptSlot(int slot) {
    const Rect c = trackPromptCard();
    Rect r;
    r.x = c.x + 30.0f;
    r.w = c.w - 60.0f;
    if (slot <= 1) {
        r.h = 72.0f;
        r.y = c.y + 118.0f + (float)slot * 84.0f;
    } else {
        const float gap = 14.0f;
        r.h = 64.0f;
        r.y = c.y + 118.0f + 2 * 84.0f + 8.0f;
        r.w = (r.w - gap) * 0.5f;
        if (slot == 3) r.x += r.w + gap;
    }
    return r;
}
inline int hitTestTrackPrompt(float x, float y) {
    for (int i = 0; i < 4; i++) if (trackPromptSlot(i).contains(x, y)) return i;
    return -1;
}

// Which login row a touch hits, -1 for none.
inline int hitTestLogin(float x, float y, int rows) {
    for (int i = 0; i < rows; i++) if (layout::loginRow(i).contains(x, y)) return i;
    return -1;
}


// ---- Lyrics mode ----------------------------------------------------------------------------------
// With lyrics open the whole player (artwork, title, progress, buttons, volume) slides into a compact
// column on the left and the lyrics fill the right. Every position of the player is a blend between the
// normal layout (the layout:: constants above) and the compact one below, by `t` = 0 (normal) .. 1
// (lyrics). drawNowPlaying eases t every frame and publishes it with npLyr(), so touch hit-testing
// (which uses the same functions) always agrees with what is drawn.
namespace layout {
static const float NPL_ART_X = 50.0f, NPL_ART_Y = 104.0f, NPL_ART = 150.0f;
static const float NPL_TEXT_X = 224.0f, NPL_TEXT_W = 386.0f;
static const float NPL_CTL_X = 50.0f, NPL_CTL_W = 560.0f;
static const float NPL_LABEL_Y = 112.0f, NPL_SUBTITLE_Y = 238.0f;
static const float NPL_PROGRESS_Y = 284.0f, NPL_TIME_Y = 306.0f, NPL_BUTTONS_Y = 346.0f;
static const float NPL_VOLUME_Y = 454.0f, NPL_QUEUE_Y = 558.0f;
static const float NP_LYRICS_BTN = 52.0f; // the Lyrics button, at the right end of the volume row
// Lyrics panel (right) and the sync button between the player and the lyrics.
static const float LYR_X = 716.0f, LYR_Y = 92.0f, LYR_W = 520.0f, LYR_H = 548.0f;
static const float LYR_SYNC_CX = 660.0f, LYR_SYNC_CY = 366.0f;
} // namespace layout

struct NpLayout {
    float artX, artY, art;
    float textX, textW;            // label / title / subtitle (beside the art in lyrics mode)
    float labelY, titleSize, subtitleY;
    float ctlX, ctlW;              // progress bar, transport buttons, volume
    float progressY, timeY, buttonsY, volumeY, queueY;
};

inline float& npLyr() { static float t = 0.0f; return t; } // 0 = normal player, 1 = lyrics open

inline NpLayout npLayout(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    auto L = [t](float a, float b) { return a + (b - a) * t; };
    NpLayout o;
    o.artX = L(layout::NP_ART_X, layout::NPL_ART_X);
    o.artY = L(layout::NP_ART_Y, layout::NPL_ART_Y);
    o.art = L(layout::NP_ART, layout::NPL_ART);
    o.textX = L(layout::NP_TEXT_X, layout::NPL_TEXT_X);
    o.textW = L(layout::NP_TEXT_W, layout::NPL_TEXT_W);
    o.labelY = L(layout::NP_LABEL_Y, layout::NPL_LABEL_Y);
    o.titleSize = L(40.0f, 30.0f);
    o.subtitleY = L(layout::NP_SUBTITLE_Y, layout::NPL_SUBTITLE_Y);
    o.ctlX = L(layout::NP_TEXT_X, layout::NPL_CTL_X);
    o.ctlW = L(layout::NP_TEXT_W, layout::NPL_CTL_W);
    o.progressY = L(layout::NP_PROGRESS_Y, layout::NPL_PROGRESS_Y);
    o.timeY = L(layout::NP_TIME_Y, layout::NPL_TIME_Y);
    o.buttonsY = L(layout::NP_BUTTONS_Y, layout::NPL_BUTTONS_Y);
    o.volumeY = L(layout::NP_VOLUME_Y, layout::NPL_VOLUME_Y);
    o.queueY = L(layout::NP_QUEUE_Y, layout::NPL_QUEUE_Y);
    return o;
}
inline NpLayout npLayoutNow() { return npLayout(npLyr()); }

// Now Playing transport buttons, under the progress bar: seek back,
// play/pause, seek forward, stop -- in that order, left to right, the
// row centred under the progress bar. Play/pause is drawn bigger, as
// the primary action. Index order is the contract between
// drawNowPlaying (draws them) and PlaybackControl::poll() (hit-tests
// taps against them), for both the music Now Playing screen and the
// movie remote (TV-only mode).
// Count = the four transport buttons (the movie remote shows only those). A music queue also shows two toggle
// buttons either side of them -- Shuffle on the left, Repeat on the right -- numbered after the transport ones
// so one index says which was touched (hitTestNowPlayingToggles below).
enum class NpButton { SeekBack = 0, PlayPause = 1, SeekForward = 2, Stop = 3, Count = 4, Shuffle = 4, Repeat = 5, Lyrics = 6, LyricsSync = 7 };

// Material 3 Expressive "Customize shapes" sizes: the user's width / height for the play / pause button [0], the seek
// and stop buttons [1] and the toggles (shuffle, repeat, lyrics) [2], as multipliers. `active` = the look is on, so
// the row also leaves room for what the shapes draw beyond their touch rectangles. All 1 / inactive otherwise, and
// the layout is then exactly the original one.
struct NpCustom {
    float sw[3] = {1.0f, 1.0f, 1.0f}, sh[3] = {1.0f, 1.0f, 1.0f};
    bool active = false;
};
inline NpCustom& npCustom() {
    static NpCustom c;
    return c;
}

inline float npButtonSize(int index) {
    return index == (int)NpButton::PlayPause ? layout::NP_BUTTON_SIZE_BIG : layout::NP_BUTTON_SIZE;
}
inline float npButtonW(int index) { return npButtonSize(index) * npCustom().sw[index == (int)NpButton::PlayPause ? 0 : 1]; }
inline float npButtonH(int index) { return npButtonSize(index) * npCustom().sh[index == (int)NpButton::PlayPause ? 0 : 1]; }

// Room a button needs beside its touch rectangle for what is drawn there (the wide play / pause, a cookie's bumps,
// the swell of a pressed neighbour). Only used while the custom look is on.
inline float npSpill(float width, bool wide) { return width * (wide ? 0.20f : 0.09f); }
inline float npGap(float wa, bool wideA, float wb, bool wideB) {
    if (!npCustom().active) return layout::NP_BUTTON_GAP;
    return 14.0f + npSpill(wa, wideA) + npSpill(wb, wideB);
}

inline Rect npButtonRect(int index) {
    const int P = (int)NpButton::PlayPause;
    float total = 0.0f;
    for (int i = 0; i < (int)NpButton::Count; i++) {
        total += npButtonW(i);
        if (i > 0) total += npGap(npButtonW(i - 1), i - 1 == P, npButtonW(i), i == P);
    }
    const NpLayout L = npLayoutNow();
    float x = L.ctlX + (L.ctlW - total) * 0.5f;
    for (int i = 0; i < index; i++) x += npButtonW(i) + npGap(npButtonW(i), i == P, npButtonW(i + 1), i + 1 == P);
    Rect r;
    r.w = npButtonW(index);
    r.h = npButtonH(index);
    r.x = x;
    // Vertically centred against the play/pause button's normal height, so every button's centre sits on the same
    // line whatever its size.
    r.y = L.buttonsY + (layout::NP_BUTTON_SIZE_BIG - r.h) * 0.5f;
    return r;
}

// The Shuffle / Repeat toggles of a music queue: smaller round buttons level with the transport row, Shuffle
// to the left of seek-back and Repeat to the right of stop.
inline Rect npToggleRect(NpButton which) {
    const float w = layout::NP_TOGGLE_SIZE * npCustom().sw[2], h = layout::NP_TOGGLE_SIZE * npCustom().sh[2];
    const Rect first = npButtonRect(0), last = npButtonRect((int)NpButton::Count - 1);
    Rect r;
    r.w = w;
    r.h = h;
    r.x = which == NpButton::Shuffle ? first.x - npGap(w, false, first.w, false) - w : last.x + last.w + npGap(last.w, false, w, false);
    r.y = npLayoutNow().buttonsY + (layout::NP_BUTTON_SIZE_BIG - h) * 0.5f;
    return r;
}

// NpButton::Shuffle / NpButton::Repeat (as an int) if the touch lands on one, else -1.
inline int hitTestNowPlayingToggles(float x, float y) {
    if (npToggleRect(NpButton::Shuffle).contains(x, y)) return (int)NpButton::Shuffle;
    if (npToggleRect(NpButton::Repeat).contains(x, y)) return (int)NpButton::Repeat;
    return -1;
}

// -1 if the touch doesn't land on any transport button.
inline int hitTestNowPlaying(float x, float y) {
    for (int i = 0; i < (int)NpButton::Count; i++)
        if (npButtonRect(i).contains(x, y)) return i;
    return -1;
}

// Touch rect for the volume slider, below the transport buttons -- same
// x/width as the progress bar, see NP_VOLUME_Y/NP_VOLUME_H. Kept
// separate from hitTestNowPlaying (which is button hit-testing, edge
// detected) since a slider is dragged continuously: the caller checks
// Rect::contains() itself on every touched poll, not just on the first
// touch-down.
inline Rect npVolumeRect() {
    const NpLayout L = npLayoutNow();
    // (the Lyrics button sits at the right end of this row)
    return Rect{L.ctlX, L.volumeY, L.ctlW - layout::NP_LYRICS_BTN * npCustom().sw[2] - 18.0f, layout::NP_VOLUME_H};
}

// The Lyrics button: a round button at the right end of the volume row.
inline Rect npLyricsRect() {
    const NpLayout L = npLayoutNow();
    Rect r;
    r.w = layout::NP_LYRICS_BTN * npCustom().sw[2];
    r.h = layout::NP_LYRICS_BTN * npCustom().sh[2];
    r.x = L.ctlX + L.ctlW - r.w;
    r.y = L.volumeY + layout::NP_VOLUME_H * 0.5f - r.h * 0.5f;
    return r;
}

// The synced / plain switch between the player and the lyrics. It slides in with the lyrics (`t`).
inline Rect npLyricsSyncRect() {
    Rect r;
    r.w = r.h = layout::NP_LYRICS_BTN;
    r.x = layout::LYR_SYNC_CX - r.w * 0.5f + 40.0f * (1.0f - npLyr());
    r.y = layout::LYR_SYNC_CY - r.h * 0.5f;
    return r;
}

// The lyrics panel.
inline Rect npLyricsPanel() { return Rect{layout::LYR_X, layout::LYR_Y, layout::LYR_W, layout::LYR_H}; }

// NpButton::Lyrics / NpButton::LyricsSync (as an int) if the touch lands on one, else -1. The sync switch
// only answers once the lyrics are (nearly) open.
inline int hitTestNowPlayingLyrics(float x, float y) {
    if (npLyricsRect().contains(x, y)) return (int)NpButton::Lyrics;
    if (npLyr() > 0.7f && npLyricsSyncRect().contains(x, y)) return (int)NpButton::LyricsSync;
    return -1;
}

// Volume (0..1) that a touch at x along npVolumeRect() corresponds to,
// clamped to the slider's ends. Shared by drawNowPlaying (drawing the
// knob) and PlaybackControl::poll() (turning a drag into a volume),
// same reasoning as the rest of this file.
inline float npVolumeFraction(float x) {
    Rect r = npVolumeRect();
    if (r.w <= 0.0f) return 0.0f;
    float f = (x - r.x) / r.w;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}


// ---- Queue button, flyout and the Add to Queue browser (music Now Playing) --------------------------------------
// The Queue button sits under the cover art (its 340 px width) and, with the lyrics open, under the volume bar of the
// compact column. The flyout is anchored to it and grows upward; both are the same function of npLyr() as the player
// itself, so touch and drawing agree while the layout slides.
inline Rect npQueueRect() {
    const float t = npLyr() < 0.0f ? 0.0f : (npLyr() > 1.0f ? 1.0f : npLyr());
    auto L = [t](float a, float b) { return a + (b - a) * t; };
    Rect r;
    r.x = L(layout::NP_ART_X, layout::NPL_CTL_X);
    r.y = L(layout::NP_ART_Y + layout::NP_ART + 20.0f, layout::NPL_VOLUME_Y + layout::NP_VOLUME_H + 14.0f);
    r.w = L(layout::NP_ART, 210.0f);
    r.h = L(54.0f, 46.0f);
    return r;
}
namespace layout {
static const int QF_ROWS = 5;
static const float QF_W = 440.0f, QF_H = 426.0f, QF_ROW_H = 50.0f;
} // namespace layout
inline Rect queueFlyoutCard() {
    const Rect b = npQueueRect();
    Rect c;
    c.w = layout::QF_W;
    c.h = layout::QF_H;
    c.x = b.x;
    if (c.x + c.w > layout::SCREEN_W - 24.0f) c.x = layout::SCREEN_W - 24.0f - c.w;
    if (c.x < 24.0f) c.x = 24.0f;
    c.y = b.y - 10.0f - c.h;
    if (c.y < 24.0f) c.y = 24.0f;
    return c;
}
inline Rect queueFlyoutCurrent() { const Rect c = queueFlyoutCard(); return Rect{c.x + 12.0f, c.y + 48.0f, c.w - 24.0f, 60.0f}; }
inline Rect queueFlyoutAdd() { const Rect c = queueFlyoutCard(); return Rect{c.x + 12.0f, c.y + 116.0f, (c.w - 36.0f) * 0.62f, 40.0f}; }
inline Rect queueFlyoutClear() {
    const Rect c = queueFlyoutCard(), a = queueFlyoutAdd();
    return Rect{a.right() + 12.0f, a.y, c.right() - 12.0f - (a.right() + 12.0f), a.h};
}
inline Rect queueFlyoutRow(int slot) {
    const Rect c = queueFlyoutCard();
    return Rect{c.x + 12.0f, c.y + 166.0f + slot * layout::QF_ROW_H, c.w - 24.0f, layout::QF_ROW_H - 4.0f};
}
// The small buttons at the right end of a row: 0 = move up, 1 = move down, 2 = remove.
inline Rect queueFlyoutRowButton(int slot, int k) {
    const Rect r = queueFlyoutRow(slot);
    const float s = 34.0f;
    return Rect{r.right() - 8.0f - (3 - k) * (s + 2.0f) + 2.0f, r.y + (r.h - s) * 0.5f, s, s};
}
// What a touch at (x, y) hits in the open flyout: -1 outside it, -2 on its card (nothing), 0 = Add to Queue,
// 1 = Clear, 100 + slot = a row (the whole row = play now), 200/300/400 + slot = that row's up / down / remove.
inline int hitTestQueueFlyout(float x, float y, int visibleRows) {
    if (!queueFlyoutCard().contains(x, y)) return -1;
    if (queueFlyoutAdd().contains(x, y)) return 0;
    if (queueFlyoutClear().contains(x, y)) return 1;
    for (int i = 0; i < visibleRows && i < layout::QF_ROWS; i++) {
        for (int k = 0; k < 3; k++) {
            Rect b = queueFlyoutRowButton(i, k);
            b.x -= 1; b.w += 2; b.y -= 4; b.h += 8;
            if (b.contains(x, y)) return 200 + 100 * k + i;
        }
        if (queueFlyoutRow(i).contains(x, y)) return 100 + i;
    }
    return -2;
}

namespace layout {
static const int QB_ROWS = 8;
static const float QB_W = 900.0f, QB_H = 500.0f, QB_ROW_H = 46.0f;
} // namespace layout
inline Rect queueBrowserCard() { return Rect{(layout::SCREEN_W - layout::QB_W) * 0.5f, 86.0f, layout::QB_W, layout::QB_H}; }
inline Rect queueBrowserCover() { const Rect c = queueBrowserCard(); return Rect{c.x + 24.0f, c.y + 72.0f, 250.0f, 250.0f}; }
inline Rect queueBrowserClose() { const Rect c = queueBrowserCard(); return Rect{c.right() - 56.0f, c.y + 12.0f, 40.0f, 40.0f}; }
inline Rect queueBrowserAdd() { const Rect c = queueBrowserCard(); return Rect{c.x + 24.0f, c.y + 392.0f, 250.0f, 44.0f}; }
inline Rect queueBrowserAddAll() { const Rect c = queueBrowserCard(); return Rect{c.x + 24.0f, c.y + 444.0f, 250.0f, 40.0f}; }
inline Rect queueBrowserTabs() { const Rect c = queueBrowserCard(); return Rect{c.x + 300.0f, c.y + 64.0f, 576.0f, 36.0f}; }
inline Rect queueBrowserTab(int i, int n) {
    const Rect t = queueBrowserTabs();
    const float w = t.w / (float)(n < 1 ? 1 : n);
    return Rect{t.x + w * i, t.y, w - 4.0f, t.h};
}
inline Rect queueBrowserRow(int slot, bool tabs) {
    const Rect c = queueBrowserCard();
    const float top = c.y + (tabs ? 108.0f : 72.0f);
    return Rect{c.x + 300.0f, top + slot * layout::QB_ROW_H, 576.0f, layout::QB_ROW_H - 4.0f};
}
// Rows that fit: 8 with the tab strip, 9 without (so the list ends at the same place).
inline int queueBrowserRows(bool tabs) { return tabs ? layout::QB_ROWS : layout::QB_ROWS + 1; }
// -1 outside the card, -2 on the card, 0 Add selected, 1 Add all, 2 close, 10 + i = tab i, 100 + slot = a row.
inline int hitTestQueueBrowser(float x, float y, int visibleRows, int tabCount) {
    if (!queueBrowserCard().contains(x, y)) return -1;
    if (queueBrowserAdd().contains(x, y)) return 0;
    if (queueBrowserAddAll().contains(x, y)) return 1;
    Rect cl = queueBrowserClose();
    cl.x -= 6; cl.y -= 6; cl.w += 12; cl.h += 12;
    if (cl.contains(x, y)) return 2;
    for (int i = 0; i < tabCount; i++) if (queueBrowserTab(i, tabCount).contains(x, y)) return 10 + i;
    for (int i = 0; i < visibleRows; i++) if (queueBrowserRow(i, tabCount > 0).contains(x, y)) return 100 + i;
    return -2;
}

// ---- Seeking by touch, and the movie remote's settings flyout -------------------------------------------------
// The progress bar of Now Playing (the music player and the movie remote) is a slider: a finger on it picks a time,
// lifting performs the seek. The touch target is taller than the thin bar drawn.
inline Rect npProgressRect() {
    const NpLayout L = npLayoutNow();
    return Rect{L.ctlX - 10.0f, L.progressY - 17.0f, L.ctlW + 20.0f, 42.0f};
}
// The time (0..duration) a touch at x along the bar picks.
inline double npProgressSeconds(float x, double duration) {
    const NpLayout L = npLayoutNow();
    float f = L.ctlW > 0.0f ? (x - L.ctlX) / L.ctlW : 0.0f;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    return (double)f * duration;
}

// The settings button at the top right of the movie remote, and its flyout (quality, audio, subtitles for what is
// playing now). Everything is touch-sized (34 px rows and up).
namespace layout {
static const float VS_W = 400.0f, VS_ROW = 34.0f;
} // namespace layout
inline Rect npGearRect() { return Rect{layout::SCREEN_W - 32.0f - 52.0f, 30.0f, 52.0f, 52.0f}; }
inline Rect vsCard(int qualityRows) {
    const Rect g = npGearRect();
    Rect c;
    c.w = layout::VS_W;
    c.x = layout::SCREEN_W - 24.0f - c.w;
    c.y = g.bottom() + 10.0f;
    c.h = 54.0f + 26.0f + qualityRows * layout::VS_ROW + 14.0f + 2.0f * 52.0f + 12.0f;
    return c;
}
inline Rect vsQualityRow(int i) {
    const Rect c = vsCard(9);
    return Rect{c.x + 12.0f, c.y + 80.0f + i * layout::VS_ROW, c.w - 24.0f, layout::VS_ROW - 3.0f};
}
// The Audio (0) and Subtitles (1) rows under the quality list.
inline Rect vsTrackRow(int k, int qualityRows) {
    const Rect c = vsCard(qualityRows);
    return Rect{c.x + 12.0f, c.y + 80.0f + qualityRows * layout::VS_ROW + 14.0f + k * 52.0f, c.w - 24.0f, 46.0f};
}
inline Rect vsArrow(const Rect& row, bool right) { return Rect{right ? row.right() - 46.0f : row.x, row.y, 46.0f, row.h}; }
// -1 outside the card, -2 on the card, 0.. a quality row, 100 / 101 = audio < >, 102 / 103 = subtitles < >.
inline int hitTestVideoSettings(float x, float y, int qualityRows) {
    if (!vsCard(qualityRows).contains(x, y)) return -1;
    for (int i = 0; i < qualityRows; i++) if (vsQualityRow(i).contains(x, y)) return i;
    for (int k = 0; k < 2; k++) {
        const Rect r = vsTrackRow(k, qualityRows);
        if (vsArrow(r, false).contains(x, y)) return 100 + 2 * k;
        if (vsArrow(r, true).contains(x, y)) return 101 + 2 * k;
    }
    return -2;
}

// "m:ss" or "h:mm:ss".
std::string formatTime(double seconds);

} // namespace ui
