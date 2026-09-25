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
inline Rect listRow(int slot) {
    Rect r;
    r.x = CONTENT_X;
    r.y = LIST_TOP + slot * (ROW_H + ROW_GAP);
    r.w = CONTENT_W - SCROLLBAR_W - 10.0f;
    r.h = ROW_H;
    return r;
}

inline Rect sidebarItem(int index) {
    Rect r;
    r.x = SIDEBAR_PAD;
    r.y = SIDEBAR_ITEM_TOP + index * (SIDEBAR_ITEM_H + SIDEBAR_ITEM_GAP);
    r.w = SIDEBAR_W - 2 * SIDEBAR_PAD;
    r.h = SIDEBAR_ITEM_H;
    return r;
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

// Which login row a touch hits, -1 for none.
inline int hitTestLogin(float x, float y, int rows) {
    for (int i = 0; i < rows; i++) if (layout::loginRow(i).contains(x, y)) return i;
    return -1;
}

// "m:ss" or "h:mm:ss".
std::string formatTime(double seconds);

} // namespace ui
