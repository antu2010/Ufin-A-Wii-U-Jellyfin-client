// Ufin -- browse Jellyfin libraries with the GamePad and play video,
// audio and Live TV via the media/ pipeline (HttpStreamIO -> Decoder ->
// VideoOutput/AudioOutput -> Player).
//
// The whole UI is drawn with GX2 + Dear ImGui (ui/gfx.h, ui/app_ui.h),
// set up once and kept for the whole run. The main loop is a plain
// per-frame loop: read the GamePad, update state, draw one frame.
// Blocking work (network requests, playback) draws its own frames while
// it runs -- a "loading" card before a request, Now Playing / the video
// HUD during playback.

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>
#include <vpad/input.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "version.h"
#include "update_check.h"
#include "https_client.h"
#include "net_conn.h"
#include "tls_conn.h"
#include "ufin_log.h"
#include "config_loader.h"
#include "customize_logic.h"
#include "h264_profile.h"
#include "quality_presets.h"
#include "login_utils.h"
#include "image_cache.h"
#include "http_client.h"
#include "input.h"
#include "item_labels.h"
#include "library_logic.h"
#include "jellyfin_client.h"
#include "play_queue.h"
#include "user_queue.h"
#include "playback_logic.h"
#include "seek.h"
#include "backdrop_tracker.h"
#include "help_content.h"
#include "usage_timer.h"
#include "media/file_stream_io.h"
#include "media/menu_music.h"
#include "media/player.h"
#include "media/audio_visualizer.h"
#include "media/video_output.h"
#include "ui/app_ui.h"
#include "ui/gfx.h"
#include "ui/gpu_image.h"
#include "ui/keyboard.h"
#include "ui/layout.h"
#include "ui/profiles_ui.h"

// One level of browsing. The whole path is kept on a stack with each
// level's list and selection, so B goes back instantly to exactly where
// you were instead of re-fetching (and losing your place).
struct Frame {
    enum class Kind { Views, Items, LiveTv, Search, Settings, Resume, NextUp, Favorites };
    Kind kind = Kind::Views;
    std::string id;     // parent id for Items, the search term for Search
    std::string title;  // header / breadcrumb
    std::string backdropItemId, backdropTag; // the opened item's own backdrop: the fallback behind its page
    std::vector<JellyfinItem> items;
    int selected = 0;

    // Library views, sorting and paging (library_logic.h). An Items frame is "queried": loaded a page at a time
    // through JellyfinClient::queryItems, in the order `sort` asks for, so the whole result is sorted, not just
    // what is on screen.
    int tab = -1;                 // music library top level: the view shown (library::MusicView); -1 = an ordinary list
    std::string libraryId;        // the music library this list belongs to ("" = none)
    library::Query::Source source = library::Query::Source::Children;
    std::string artistId;         // source ArtistTracks
    bool queried = false;
    bool kindFixed = false;       // sortKind was set when the frame was made (otherwise it follows the items)
    int total = 0;                // size of the whole result
    library::Kind sortKind = library::Kind::None;
    library::Sort sort;
    library::Sort tabSort[library::MusicViewCount];
    int tabSelected[library::MusicViewCount] = {0, 0, 0, 0};
    bool isPlaylist() const { return source == library::Query::Source::Playlist; }
    bool moreToLoad() const { return queried && total > (int)items.size() && (int)items.size() < library::kMaxLoaded; }
};

static library::Kind kindForTab(int tab) {
    switch (tab) {
        case library::AllSongs: return library::Kind::Songs;
        case library::Artists: return library::Kind::Artists;
        case library::Albums: return library::Kind::Albums;
        case library::Playlists: return library::Kind::Playlists;
    }
    return library::Kind::None;
}

// The query that (re)loads `f` in its current view and sort.
static library::Query frameQuery(const Frame& f) {
    library::Query q;
    q.source = f.source;
    q.parentId = f.id;
    q.artistId = f.artistId;
    if (f.tab >= 0) {
        q.parentId = f.libraryId;
        switch (f.tab) {
            case library::AllSongs: q.source = library::Query::Source::Songs; break;
            case library::Artists: q.source = library::Query::Source::Artists; break;
            case library::Albums: q.source = library::Query::Source::AllAlbums; break;
            case library::Playlists: q.source = library::Query::Source::AllPlaylists; break;
        }
    } else if (f.source == library::Query::Source::ArtistTracks) {
        q.parentId = f.libraryId;
    }
    library::applySort(q, f.sortKind, f.sort);
    return q;
}

// Buttons that count as up/down: D-pad and the left stick.
static const uint32_t BTN_UP = VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP;
static const uint32_t BTN_DOWN = VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN;
static const uint32_t BTN_LEFT = VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT;
static const uint32_t BTN_RIGHT = VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT;

static double secondsSince(OSTime t) {
    return (double)OSTicksToMilliseconds(OSGetTime() - t) / 1000.0;
}

// Seconds on the console's monotonic clock.
static double monotonicSeconds() {
    return (double)OSTicksToMilliseconds(OSGetTime()) / 1000.0;
}

// How long Ufin has been used (the sidebar's "Time used" counter). Loaded from config.json at
// startup, saved back every 30 minutes and on exit.
static UsageTimer g_usage;

// WHBProcIsRunning(), but a call that blocks for a while is the app sitting suspended in the HOME
// menu (ProcUI holds it there): that time does not count as use. Only the two loops that
// actually run for long -- the menus and the video player -- use this.
static bool procRunningTracked() {
    const double before = monotonicSeconds();
    const bool running = WHBProcIsRunning();
    const double blocked = monotonicSeconds() - before;
    if (blocked > 1.0) g_usage.excludeSeconds(blocked);
    return running;
}

// --- artwork ---

// Loads posters / covers in the background (see image_cache.h). Created
// after graphics, destroyed before them.
// What is held right now (set once per frame by the main loop): the palette editor's sliders repeat while the
// d-pad or a finger stays on them.
static uint32_t g_dpadHeld = 0;
static bool g_touchHeld = false;
static float g_touchHeldX = 0, g_touchHeldY = 0;

static ImageCache* g_images = nullptr;
static const JellyfinClient* g_imageClient = nullptr;

// Jellyfin backdrops (Settings > Jellyfin backdrops) get a cache of their own: they are big
// (full-screen) and only a few are ever alive at once -- the current one, the one fading out, the
// next -- so they must not compete with the 96 small posters for room. 1920x1080 matches the TV's
// output (8 MB per texture, decoded on the cache's worker thread); drop both numbers to 1280x720
// for a smaller download and faster decode on a slow connection. The cache's own size cap
// (BACKDROP_W) must be at least as large as the request, or the picture is shrunk after download.
static ImageCache* g_backdrops = nullptr;
static const int BACKDROP_W = 1920, BACKDROP_H = 1080;

// Texture for an item's artwork at up to maxW x maxH, 0 if it has none
// or it's still loading.
static ui::Texture artFor(const JellyfinItem& item, int maxW, int maxH, float* aspect) {
    if (!g_images || !g_imageClient || item.imageTag.empty()) return 0;
    return g_images->get(g_imageClient->buildImagePath(item.imageItemId, item.imageTag, maxW, maxH), maxW, maxH,
                         aspect);
}

// Artwork-reactive accent (Settings > Customization, Material 3 Expressive only). Called every frame a player
// is on screen. Marks the player context for the theme, and feeds it the playing item's artwork colour. The
// colour comes from the same cached picture the player already shows (decoded and measured once, on the image
// worker), so this is a lookup. While the next item's picture is still loading the previous colour is kept, so
// the accent does not dip back to the palette between songs; no usable artwork -> the selected palette.
static bool g_artworkAccentOn = false; // cfg.artworkAccent && M3E, set by applyLook()
static void updateArtworkTheme(const JellyfinItem& item) {
    ui::setArtworkContext(true);
    if (!g_artworkAccentOn || !g_images || !g_imageClient) return;
    if (item.imageTag.empty()) { ui::setArtworkColour(false, 0); return; }
    uint32_t rgb = 0;
    const int side = ui::layout::NP_IMAGE_MAX;
    switch (g_images->dominantColour(g_imageClient->buildImagePath(item.imageItemId, item.imageTag, side, side), side, side, rgb)) {
        case ImageCache::ColourState::Ready:   ui::setArtworkColour(true, rgb); break;
        case ImageCache::ColourState::None:    ui::setArtworkColour(false, 0); break;
        case ImageCache::ColourState::Pending: break;
    }
}
// Leaving a player (any way out) returns the theme to the selected palette.
struct ArtworkThemeScope {
    ~ArtworkThemeScope() {
        ui::setArtworkContext(false);
        ui::setArtworkColour(false, 0); // the next player must not start from this item's colour
    }
};

// One app frame, then let the image cache upload what finished loading.
static void drawFrame(const std::function<void()>& build,
                      const std::function<void(uint32_t, uint32_t)>& underlay = {}) {
    ui::tickTheme((double)OSTicksToMilliseconds(OSGetTime()) / 1000.0); // Rainbow accent
    ui::gfx().frame(build, underlay);
    if (g_images) g_images->pump();
    if (g_backdrops) g_backdrops->pump(1);
}

// --- message cards ---

static void showMessage(const std::string& title, const std::vector<std::string>& lines,
                        const std::vector<ui::Hint>& hints = {}, bool isError = false, bool busy = false) {
    ui::MessageModel m;
    m.title = title;
    m.lines = lines;
    m.hints = hints;
    m.isError = isError;
    m.busy = busy;
    drawFrame([&] { ui::drawMessage(m); });
}

// A "working on it" card, drawn once before a blocking request so the
// screen says what's happening while it runs.
static void showBusy(const std::string& title, const std::vector<std::string>& lines = {}) {
    showMessage(title, lines, {}, false, true);
}

// (Re)loads a frame's items from the server. Keeps the selection in range.
// The next page of a queried list, appended (the order is the server's, so pages never repeat or skip).
static bool loadMore(JellyfinClient& client, Frame& f) {
    if (!f.moreToLoad()) return true;
    library::Query q = frameQuery(f);
    q.startIndex = (int)f.items.size();
    q.limit = library::kPageSize;
    return client.queryItems(q, f.items, f.total, true);
}

static bool loadFrame(JellyfinClient& client, Frame& f) {
    bool ok = false;
    switch (f.kind) {
        case Frame::Kind::Views: {
            ok = client.getViews(f.items);
            if (ok) {
                // Home: "Continue watching", "Next up" and "Favourites" first,
                // when they have anything in them.
                std::vector<JellyfinItem> rows, tmp;
                if (client.getResume(tmp) && !tmp.empty()) rows.push_back(makeHomeRow(HOME_RESUME, (int)tmp.size()));
                if (client.getNextUp(tmp) && !tmp.empty()) rows.push_back(makeHomeRow(HOME_NEXT_UP, (int)tmp.size()));
                if (client.getFavorites(tmp) && !tmp.empty()) rows.push_back(makeHomeRow(HOME_FAVORITES, (int)tmp.size()));
                f.items.insert(f.items.begin(), rows.begin(), rows.end());
            }
            break;
        }
        case Frame::Kind::Resume:    ok = client.getResume(f.items); break;
        case Frame::Kind::NextUp:    ok = client.getNextUp(f.items); break;
        case Frame::Kind::Favorites: ok = client.getFavorites(f.items); break;
        case Frame::Kind::Items: {
            library::Query q = frameQuery(f);
            q.startIndex = 0;
            // A reload keeps as many rows as were loaded (refresh must not throw away the user's scrolling).
            q.limit = std::max(library::kPageSize, std::min(library::kMaxLoaded, (int)f.items.size()));
            ok = client.queryItems(q, f.items, f.total);
            if (ok) {
                f.queried = true;
                if (!f.kindFixed) {
                    const library::Kind k = library::kindOfItems(f.items);
                    if (k != f.sortKind) { f.sortKind = k; f.sort = library::defaultSort(k); }
                }
            }
            break;
        }
        case Frame::Kind::LiveTv: ok = client.getLiveTvChannels(f.items); break;
        case Frame::Kind::Search: ok = client.search(f.id, f.items); break;
        case Frame::Kind::Settings: ok = true; break; // built by the app, not the server
    }
    if (f.selected >= (int)f.items.size()) f.selected = (int)f.items.size() - 1;
    if (f.selected < 0) f.selected = 0;
    return ok;
}

// The frame that opening `picked` (from the list `f`) shows: a folder, a music library with its four views, an
// artist's songs, a playlist. Not loaded yet. Shared by the main browser and the Add to Queue popup.
static Frame makeChildFrame(const Frame& f, const JellyfinItem& picked) {
    Frame next;
    next.kind = isLiveTvView(picked) ? Frame::Kind::LiveTv : Frame::Kind::Items;
    next.id = picked.id;
    next.title = picked.name;
    next.backdropItemId = picked.backdropItemId;
    next.backdropTag = picked.backdropTag;
    next.libraryId = f.libraryId;
    if (next.kind == Frame::Kind::Items) {
        if (picked.collectionType == "music" && f.kind == Frame::Kind::Views) {
            // A music library: four views under tabs (All Songs, Artists, Albums, Playlists).
            next.libraryId = picked.id;
            next.tab = library::AllSongs;
            next.sortKind = kindForTab(next.tab);
            next.kindFixed = true;
        } else if (picked.type == "MusicArtist") {
            next.source = library::Query::Source::ArtistTracks;
            next.artistId = picked.id;
            next.sortKind = library::Kind::Tracks;
            next.kindFixed = true;
        } else if (picked.type == "Playlist") {
            next.source = library::Query::Source::Playlist;
            next.sortKind = library::Kind::Tracks;
            next.kindFixed = true;
        }
    }
    return next;
}

// --- playback ---

// Volume the user last set with the on-screen slider (see
// ui::npVolumeRect / PlaybackControl::poll). Session-only -- kept here
// rather than in UfinConfig -- so it carries across tracks/seeks/movies
// for the life of the app without needing a config write on every drag.
static float g_playbackVolume = 1.0f;

// The Up Next list (see user_queue.h): what the user queued by hand, for the whole session.
static UserQueue g_upNext;

// Music queue modes the user last chose with the Shuffle / Repeat buttons (X / Y, or the on-screen buttons).
// Session-only, like the volume: every queue started afterwards begins with them.
static bool g_shuffleOn = false;
// Lyrics view of the music player: open / closed and "follow the song" on / off. Session-only like the above, so
// the lyrics stay open from one track to the next.
static int g_visualizerStyle = 0; // which look the music visualizer has (see ui::VisualizerModel::style)
static bool g_lyricsOpen = false;
static bool g_lyricsSync = true;
static PlayQueue::Repeat g_repeatMode = PlayQueue::Repeat::Off;

// Up/down auto-repeat while held: first repeat after 400 ms, then every
// 70 ms -- scrolling a 500-movie library one press at a time is no fun.
struct RepeatState {
    uint32_t button = 0;
    OSTime pressedAt = 0;
    OSTime lastFire = 0;
};

static uint32_t withRepeat(const input::State& vpad, RepeatState& rs) {
    uint32_t fired = vpad.trigger;
    OSTime now = OSGetTime();
    const uint32_t repeatable = BTN_UP | BTN_DOWN;

    if (vpad.trigger & repeatable) {
        rs.button = vpad.trigger & repeatable;
        rs.pressedAt = now;
        rs.lastFire = now;
    } else if (rs.button && (vpad.hold & rs.button)) {
        if (OSTicksToMilliseconds(now - rs.pressedAt) >= 400 &&
            OSTicksToMilliseconds(now - rs.lastFire) >= 70) {
            fired |= rs.button;
            rs.lastFire = now;
        }
    } else {
        rs.button = 0;
    }
    return fired;
}

// --- the Up Next flyout and the Add to Queue browser (music Now Playing) ---------------------------------------
// State and input of the Queue button's flyout and its popup media browser. Drawn by ui::drawNowPlaying through the
// model QueueUi::fill() builds; the list itself is g_upNext (user_queue.h). One instance for the session (g_qui): the
// flyout stays open from one track to the next (Play now), and the browser keeps its place while the flyout is shown.

// A short second line for a list row: artist, show + episode, year ...
static std::string queueSub(const JellyfinItem& it) {
    if (it.type == "Audio") return it.artist.empty() ? it.album : (it.album.empty() ? it.artist : it.artist + "  -  " + it.album);
    if (it.type == "Episode") {
        std::string code = itemTag(it);
        if (!code.empty()) code = code.substr(0, code.find("  "));
        return it.seriesName.empty() ? code : (code.empty() ? it.seriesName : it.seriesName + "  " + code);
    }
    if (it.type == "Movie") return it.productionYear > 0 ? std::to_string(it.productionYear) : std::string();
    if (it.type == "MusicAlbum") return it.artist;
    if (it.type == "Season") return it.seriesName;
    if (it.type == "Series") return it.productionYear > 0 ? std::to_string(it.productionYear) : std::string();
    if (it.type == "Playlist" && it.childCount >= 0) return std::to_string(it.childCount) + " items";
    return std::string();
}

static bool queueContainerType(const JellyfinItem& it) { return !UserQueue::playable(it); }

// The songs / movies / episodes behind `it`, in the order the app itself lists them (an album's tracks in track order,
// a playlist in its own order, a show's episodes in air order ...), appended to `out` up to `room` items. A music or
// movie library as a whole is not queueable (open it and pick). False with `err` when the server said no.
static bool expandForQueue(JellyfinClient& client, const Frame& parent, const JellyfinItem& it, size_t room,
                           std::vector<JellyfinItem>& out, std::string& err) {
    if (UserQueue::playable(it)) { if (out.size() < room) out.push_back(it); return true; }
    if (isHomeRow(it) || isLiveTvView(it) || it.type == "TvChannel" || it.type == "CollectionFolder" || it.type == "UserView") {
        err = "Open it and pick what to add";
        return false;
    }
    if (it.type == "Series") {
        std::vector<JellyfinItem> eps;
        if (!client.getEpisodes(it.id, eps)) { err = client.lastError(); return false; }
        for (const JellyfinItem& e : eps) if (out.size() < room && UserQueue::playable(e)) out.push_back(e);
        return true;
    }
    Frame f = makeChildFrame(parent, it);
    if (!loadFrame(client, f)) { err = client.lastError(); return false; }
    while (f.moreToLoad() && f.items.size() < room) {
        if (!loadMore(client, f)) { err = client.lastError(); return false; }
    }
    for (const JellyfinItem& e : f.items) if (out.size() < room && UserQueue::playable(e)) out.push_back(e);
    return true;
}

struct QueueUi {
    // flyout
    bool flyoutOpen = false;
    OSTime closedAt = 0;          // for the fade-out: the rows stay filled in for a moment
    int cursor = -2;              // -2 Add to Queue, -1 Clear, 0.. an entry
    bool confirmClear = false;
    std::string toast;
    OSTime toastAt = 0;
    // browser
    bool browserOpen = false;
    std::vector<Frame> bs;        // the browsing path (bs[0] = the libraries)
    std::string browserToast;
    OSTime browserToastAt = 0;
    // input
    RepeatState rs;
    bool wasTouched = false;
    float dragY = 0.0f, dragAccum = 0.0f;
    bool dragging = false;
    JellyfinClient* client = nullptr;

    bool modal() const { return flyoutOpen || browserOpen; }
    void closeAll() { flyoutOpen = false; browserOpen = false; bs.clear(); confirmClear = false; toast.clear(); cursor = -2; }

    void say(const std::string& s) { toast = s; toastAt = OSGetTime(); }
    void sayBrowser(const std::string& s) { browserToast = s; browserToastAt = OSGetTime(); }

    void openFlyout() {
        flyoutOpen = true;
        wasTouched = true; // (the touch that opened it is not a tap inside it)
        confirmClear = false;
        cursor = g_upNext.empty() ? -2 : 0;
    }
    void closeFlyout() { flyoutOpen = false; browserOpen = false; confirmClear = false; closedAt = OSGetTime(); }

    // ---- the list ----
    void clampCursor() {
        const int n = (int)g_upNext.size();
        if (cursor >= n) cursor = n - 1;
        if (cursor < -2) cursor = -2;
    }
    void removeAt(int i) {
        if (i < 0 || i >= (int)g_upNext.size()) return;
        const std::string name = itemDisplayName(g_upNext.items()[(size_t)i]);
        g_upNext.remove((size_t)i);
        clampCursor();
        say("Removed " + name);
    }
    void moveAt(int i, int delta) {
        if (i < 0 || i >= (int)g_upNext.size()) return;
        const size_t j = g_upNext.move((size_t)i, delta);
        cursor = (int)j;
    }
    // Clear needs a second press (A / Y / tap) -- it empties the whole list.
    void clearPressed() {
        if (g_upNext.empty()) { say("The queue is already empty"); return; }
        if (!confirmClear) { confirmClear = true; say("Press again to clear the queue"); return; }
        g_upNext.clear();
        confirmClear = false;
        cursor = -2;
        say("Queue cleared");
    }

    // ---- the browser ----
    void openBrowser(JellyfinClient& c) {
        client = &c;
        browserOpen = true;
        if (bs.empty()) {
            Frame root;
            root.kind = Frame::Kind::Views;
            root.title = "Libraries";
            showBusy("Loading libraries");
            if (loadFrame(c, root)) {
                std::vector<JellyfinItem> keep;
                for (const JellyfinItem& it : root.items)
                    if (!isHomeRow(it) && !isLiveTvView(it)) keep.push_back(it);
                root.items.swap(keep);
                bs.push_back(root);
            } else {
                sayBrowser("Couldn't load: " + c.lastError());
                browserOpen = false;
                say("Couldn't load the libraries");
            }
        }
    }
    Frame& cur() { return bs.back(); }
    const JellyfinItem* selectedItem() const {
        if (bs.empty()) return nullptr;
        const Frame& f = bs.back();
        return (f.selected >= 0 && f.selected < (int)f.items.size()) ? &f.items[(size_t)f.selected] : nullptr;
    }
    void loadAhead(JellyfinClient& c) {
        Frame& f = cur();
        if (f.moreToLoad() && f.selected >= (int)f.items.size() - library::kLoadAhead) loadMore(c, f);
    }
    void moveSelection(JellyfinClient& c, int d) {
        Frame& f = cur();
        if (f.items.empty()) return;
        f.selected = std::max(0, std::min((int)f.items.size() - 1, f.selected + d));
        loadAhead(c);
    }
    void openSelected(JellyfinClient& c) {
        const JellyfinItem* it = selectedItem();
        if (!it) return;
        if (!queueContainerType(*it)) { addSelected(c); return; } // a song / movie / episode: A adds it
        if (isHomeRow(*it) || it->type == "TvChannel") return;
        if (it->type == "Series" || it->type == "Season" || it->type == "MusicAlbum" || it->type == "MusicArtist" ||
            it->type == "Playlist" || it->type == "CollectionFolder" || it->type == "UserView" || it->type == "BoxSet" ||
            it->type == "Folder" || it->type == "") {
            Frame next = makeChildFrame(cur(), *it);
            showBusy("Opening", {it->name});
            if (loadFrame(c, next)) bs.push_back(next);
            else sayBrowser("Couldn't open: " + c.lastError());
        }
    }
    void back() {
        if (bs.size() > 1) bs.pop_back();
        else browserOpen = false;
    }
    void switchTab(JellyfinClient& c, int dir) { if (!bs.empty() && cur().tab >= 0) switchTabTo(c, cur().tab + dir); }
    void switchTabTo(JellyfinClient& c, int tab) {
        Frame& f = cur();
        if (f.tab < 0 || tab == f.tab) return;
        if (tab < 0 || tab >= library::MusicViewCount) return;
        f.tabSort[f.tab] = f.sort;
        f.tabSelected[f.tab] = f.selected;
        f.tab = tab;
        f.sortKind = kindForTab(tab);
        f.sort = f.tabSort[tab];
        f.items.clear();
        f.total = 0;
        showBusy("Loading", {library::musicViewName(tab)});
        if (!loadFrame(c, f)) sayBrowser("Couldn't load: " + c.lastError());
        f.selected = std::max(0, std::min(f.tabSelected[tab], (int)f.items.size() - 1));
    }
    // Adds `items` to the list and says so. `what` names them for the message.
    void addItems(const std::vector<JellyfinItem>& items, const std::string& what) {
        const size_t added = g_upNext.add(items);
        if (added == 0) sayBrowser(g_upNext.size() >= UserQueue::kMax ? "The queue is full" : "Nothing to add");
        else if (added == 1) sayBrowser("Added " + what);
        else sayBrowser("Added " + std::to_string(added) + " items" + (added < items.size() ? " (queue full)" : ""));
    }
    void addSelected(JellyfinClient& c) {
        const JellyfinItem* it = selectedItem();
        if (!it) return;
        const size_t room = UserQueue::kMax - std::min(UserQueue::kMax, g_upNext.size());
        if (room == 0) { sayBrowser("The queue is full"); return; }
        std::vector<JellyfinItem> out;
        std::string err;
        if (!UserQueue::playable(*it)) showBusy("Adding", {it->name});
        if (!expandForQueue(c, cur(), *it, room, out, err)) { sayBrowser(err); return; }
        addItems(out, itemDisplayName(*it));
    }
    bool listHasPlayable() const {
        if (bs.empty()) return false;
        for (const JellyfinItem& it : bs.back().items) if (UserQueue::playable(it)) return true;
        return false;
    }
    void addAllListed(JellyfinClient& c) {
        if (bs.empty()) return;
        Frame& f = cur();
        if (!listHasPlayable()) { sayBrowser("Open an album, playlist or season first"); return; }
        while (f.moreToLoad() && f.items.size() < UserQueue::kMax) if (!loadMore(c, f)) break;
        std::vector<JellyfinItem> out;
        for (const JellyfinItem& it : f.items) if (UserQueue::playable(it)) out.push_back(it);
        addItems(out, f.title);
    }

    // ---- input ----
    // Returns true when the user asked to play an entry now: g_upNext already has it in front, the caller skips to it.
    bool update(const input::State& in, JellyfinClient& c) {
        client = &c;
        const OSTime now = OSGetTime();
        if (!toast.empty() && OSTicksToMilliseconds(now - toastAt) > 2600) toast.clear();
        if (!browserToast.empty() && OSTicksToMilliseconds(now - browserToastAt) > 2200) browserToast.clear();
        const uint32_t p = withRepeat(in, rs);
        const bool tap = in.touched && !wasTouched;
        wasTouched = in.touched;
        bool playNow = false;

        if (browserOpen) {
            if (bs.empty()) { browserOpen = false; return false; }
            if (p & VPAD_BUTTON_B) back();
            else if (p & BTN_UP) moveSelection(c, -1);
            else if (p & BTN_DOWN) moveSelection(c, 1);
            else if (p & (BTN_LEFT | VPAD_BUTTON_L | VPAD_BUTTON_ZL)) switchTab(c, -1);
            else if (p & (BTN_RIGHT | VPAD_BUTTON_R | VPAD_BUTTON_ZR)) switchTab(c, 1);
            else if (p & VPAD_BUTTON_A) openSelected(c);
            else if (p & VPAD_BUTTON_X) addSelected(c);
            else if (p & VPAD_BUTTON_Y) addAllListed(c);
            else if (tap) {
                const Frame& f = cur();
                const bool tabs = f.tab >= 0;
                const int tc = tabs ? library::MusicViewCount : 0;
                const int vis = ui::queueBrowserRows(tabs);
                const int hit = ui::hitTestQueueBrowser(in.touchX, in.touchY, vis, tc);
                if (hit == -1) browserOpen = false;
                else if (hit == 0) addSelected(c);
                else if (hit == 1) addAllListed(c);
                else if (hit == 2) back();
                else if (hit >= 10 && hit < 100) switchTabTo(c, hit - 10); // the tab that was touched, not a step toward it
                else if (hit >= 100) {
                    const ui::ListWindow w = ui::computeListWindow((int)f.items.size(), f.selected, vis);
                    const int idx = w.start + (hit - 100);
                    if (idx < (int)f.items.size()) {
                        if (idx == f.selected) openSelected(c);
                        else { cur().selected = idx; loadAhead(c); }
                    }
                }
            }
            dragScroll(in, true, c);
            return false;
        }

        // the flyout
        const int n = (int)g_upNext.size();
        clampCursor();
        if (p & VPAD_BUTTON_B) { if (confirmClear) { confirmClear = false; toast.clear(); } else closeFlyout(); return false; }
        if (confirmClear && (p & ~(VPAD_BUTTON_A | VPAD_BUTTON_Y))) confirmClear = false; // anything else cancels the question
        if (p & BTN_UP) { if (cursor > 0) cursor--; else if (cursor == 0) cursor = -2; }
        else if (p & BTN_DOWN) { if (cursor < 0) cursor = n > 0 ? 0 : cursor; else if (cursor < n - 1) cursor++; }
        else if (p & (BTN_LEFT | VPAD_BUTTON_L | VPAD_BUTTON_ZL)) { if (cursor >= 0) moveAt(cursor, -1); else if (cursor == -1) cursor = -2; }
        else if (p & (BTN_RIGHT | VPAD_BUTTON_R | VPAD_BUTTON_ZR)) { if (cursor >= 0) moveAt(cursor, 1); else if (cursor == -2) cursor = -1; }
        else if (p & VPAD_BUTTON_A) {
            if (cursor == -2) openBrowser(c);
            else if (cursor == -1) clearPressed();
            else if (cursor >= 0 && g_upNext.playNext((size_t)cursor)) playNow = true;
        } else if (p & VPAD_BUTTON_X) { if (cursor >= 0) removeAt(cursor); }
        else if (p & VPAD_BUTTON_Y) clearPressed();
        else if (p & VPAD_BUTTON_PLUS) openBrowser(c);
        else if (tap) {
            const ui::ListWindow w = ui::computeListWindow(n, std::max(0, cursor), ui::layout::QF_ROWS);
            const int vis = std::max(0, std::min(ui::layout::QF_ROWS, n - w.start));
            const int hit = ui::hitTestQueueFlyout(in.touchX, in.touchY, vis);
            if (hit == -1) closeFlyout(); // outside the card (a tap on the Queue button too: it toggles)
            else if (hit == 0) openBrowser(c);
            else if (hit == 1) clearPressed();
            else if (hit >= 100) {
                const int slot = hit % 100, kind = hit / 100; // 1 row, 2 up, 3 down, 4 remove
                const int idx = w.start + slot;
                if (idx < n) {
                    if (kind == 1) { cursor = idx; if (g_upNext.playNext((size_t)idx)) playNow = true; }
                    else if (kind == 2) moveAt(idx, -1);
                    else if (kind == 3) moveAt(idx, 1);
                    else if (kind == 4) removeAt(idx);
                }
            }
        }
        dragScroll(in, false, c);
        return playNow;
    }

    // A finger dragging up / down the list scrolls it, one row per 40 px.
    void dragScroll(const input::State& in, bool inBrowser, JellyfinClient& c) {
        if (!in.touched) { dragging = false; return; }
        const ui::Rect area = inBrowser ? ui::queueBrowserCard() : ui::queueFlyoutCard();
        if (!dragging) { dragging = area.contains(in.touchX, in.touchY); dragY = in.touchY; dragAccum = 0.0f; return; }
        dragAccum += dragY - in.touchY; // dragging up moves down the list
        dragY = in.touchY;
        while (dragAccum >= 40.0f) { dragAccum -= 40.0f; if (inBrowser) moveSelection(c, 1); else if (cursor < (int)g_upNext.size() - 1) cursor++; }
        while (dragAccum <= -40.0f) { dragAccum += 40.0f; if (inBrowser) moveSelection(c, -1); else if (cursor > 0) cursor--; }
    }

    // ---- the model ----
    static ui::QueueRow rowOf(const JellyfinItem& it, int art) {
        ui::QueueRow r;
        r.title = itemDisplayName(it);
        r.sub = queueSub(it);
        r.art = artFor(it, art, art, nullptr);
        r.container = queueContainerType(it);
        return r;
    }
    void fill(ui::NowPlayingModel& np, const JellyfinItem& current) {
        np.queueCount = (int)g_upNext.size();
        np.flyout.open = flyoutOpen;
        const bool fading = !flyoutOpen && closedAt && OSTicksToMilliseconds(OSGetTime() - closedAt) < 400;
        np.flyout.visible = flyoutOpen || fading;
        if (np.flyout.visible) {
            ui::QueueFlyoutModel& q = np.flyout;
            q.current = rowOf(current, 96);
            q.cursor = cursor;
            q.confirmClear = confirmClear;
            q.toast = toast;
            q.count = (int)g_upNext.size();
            const ui::ListWindow w = ui::computeListWindow(q.count, std::max(0, cursor), ui::layout::QF_ROWS);
            q.first = w.start;
            for (int i = w.start; i < w.end; i++) q.rows.push_back(rowOf(g_upNext.items()[(size_t)i], 96));
        }
        np.browser.open = browserOpen && !bs.empty();
        if (np.browser.open) {
            ui::QueueBrowserModel& b = np.browser;
            const Frame& f = bs.back();
            std::string ctx;
            for (const Frame& x : bs) { if (!ctx.empty()) ctx += "  >  "; ctx += x.title; }
            b.context = ctx;
            const bool tabs = f.tab >= 0;
            if (tabs) { for (int t = 0; t < library::MusicViewCount; t++) b.tabs.push_back(library::musicViewName(t)); b.tab = f.tab; }
            b.count = (int)f.items.size();
            b.total = std::max(f.total, b.count);
            b.selected = f.selected;
            const int vis = ui::queueBrowserRows(tabs);
            const ui::ListWindow w = ui::computeListWindow(b.count, f.selected, vis);
            b.first = w.start;
            for (int i = w.start; i < w.end; i++) b.rows.push_back(rowOf(f.items[(size_t)i], 96));
            if (const JellyfinItem* it = selectedItem()) {
                b.cover = artFor(*it, 300, 300, &b.coverAspect);
                b.coverTitle = itemDisplayName(*it);
                b.coverSub = queueSub(*it);
                if (it->type == "MusicAlbum") b.addLabel = "Add album";
                else if (it->type == "MusicArtist") b.addLabel = "Add artist";
                else if (it->type == "Playlist") b.addLabel = "Add playlist";
                else if (it->type == "Series") b.addLabel = "Add show";
                else if (it->type == "Season") b.addLabel = "Add season";
                else if (queueContainerType(*it)) b.addLabel = "Add all inside";
                else b.addLabel = "Add to Queue";
                if (it->type == "CollectionFolder" || it->type == "UserView") b.addLabel = "Open to pick";
            }
            b.canAddAll = listHasPlayable();
            b.toast = browserToast;
            b.hints = {{"A", "Open / Add"}, {"X", "Add"}, {"Y", "Add all listed"}};
            if (tabs) b.hints.push_back({"< >", "Views"});
            b.hints.push_back({"B", bs.size() > 1 ? "Back" : "Close"});
        }
    }
    static std::vector<ui::Hint> flyoutHints() {
        return {{"A", "Play now"}, {"X", "Remove"}, {"< >", "Move"}, {"Y", "Clear"}, {"+", "Add"}, {"B", "Close"}};
    }
};
static QueueUi g_qui;

// Quality chosen from the movie remote's settings flyout: for what is playing now (and the rest of the queue), never
// saved. -1 = the Settings choice. Reset when playback ends (App::finishPlayback).
static int g_sessionQuality = -1;
// Set by the remote's Queue flyout "Play now" on a video: the video was stopped on purpose so the queued item starts.
static bool g_videoSkipped = false;

static void applyQualityPreset(VideoStreamOptions& o, int index) {
    int count = 0;
    const quality::Preset* t = quality::presets(count);
    if (index < 0 || index >= count) return;
    o.videoBitrate = t[index].bitrate;
    o.height = t[index].height;
    o.width = videosize::forHeight(t[index].height).width;
}

// Reads the GamePad for Player many times a second (which also keeps
// ProcUI serviced -- pressing HOME mid-playback otherwise leaves the
// system waiting on us) and turns presses into PlayerCommands:
//   B: stop    A: pause/resume    Left: -10 s    Right: +30 s
//   L/ZL, R/ZR: previous / next track (music queues)
// Skips are collected for a moment before seeking, so pressing Right
// three times makes one +90 s restart of the transcode, not three. The
// HUD shows the pending skip while it's being collected.
struct PlaybackControl {
    static const int SKIP_BACK_SECONDS = 10;
    static const int SKIP_FORWARD_SECONDS = 30;
    static const int SEEK_COMMIT_MS = 600;

    bool canSeekAndPause = true;  // false for Live TV
    bool inQueue = false;
    bool isVideo = false;
    double durationSeconds = 0.0; // 0 = unknown (no upper clamp)
    Player* player = nullptr;
    float volume = g_playbackVolume; // 0..1, dragged via the Now Playing slider

    double pendingDelta = 0.0;
    OSTime lastSkipPress = 0;
    OSTime lastInteraction = 0;   // any press: shows the HUD for a while
    int trackStep = 0;            // set when L/R ended the track: -1 previous, +1 next
    PlayQueue* queue = nullptr;   // the music queue being played (inQueue), for the Shuffle / Repeat buttons
    JellyfinClient* client = nullptr; // for the Add to Queue browser

    // Seeking by touch: a finger on the progress bar picks a time (shown while it moves); lifting performs the seek.
    bool scrubbing = false;
    double scrubSeconds = 0.0;
    bool prevTouched = false;
    bool scrubPrevTouched = false;

    // The movie remote's settings flyout (gear at the top right): quality, audio, subtitles for this viewing.
    bool vsOpen = false;
    int vsCursor = 0;
    int qualityIndex = -1;       // the preset in use (-1 = custom)
    bool qualityChanged = false; // out: playItem applies qualityIndex to the next stream request
    std::string vsToast;
    OSTime vsToastAt = 0;
    bool gearLatch = false;
    int vsCount() const { int n = 0; quality::presets(n); return n; }
    bool remoteVideo() const { return isVideo && tvOnlyMode && canSeekAndPause; }
    bool queueTapLatch = false;   // a finger on the Queue button (one tap = one toggle)

    // Audio & subtitles panel (Y). Changing a track restarts the stream
    // at the current position, like a skip.
    const VideoInfo* tracks = nullptr;
    int audioIndex = -1, subtitleIndex = -1;   // what's playing
    int menuAudio = -1, menuSubtitle = -1;     // what the panel shows
    bool trackMenu = false;
    int trackRow = 0;
    bool tracksChanged = false;

    // GamePad screen (- during video)
    bool gamepadOff = false;
    std::string notice;
    OSTime noticeAt = 0;

    // TV-only mode (+ during seekable video: movies/episodes, not Live
    // TV). Purely a presentation switch read by playItem's presentVideo:
    // the TV keeps showing the same underlying video picture either way,
    // and every command below (pause, seek, stop, ...) still goes through
    // the exact same Player/PlaybackControl path regardless of its state.
    // Scoped to this session's PlaybackControl, so it can never leak into
    // another screen and is always false again the next time a video
    // starts (a fresh PlaybackControl per playItem() call).
    bool tvOnlyMode = false;

    // Edge-detection for the on-screen transport buttons (see
    // ui::hitTestNowPlaying): -1 when nothing is held down, else the
    // button index that was under the finger on the *previous* poll, so
    // a finger held on a button doesn't repeat its command every poll --
    // same idea as `t`/trigger below being press-edges only for real
    // buttons.
    int touchLastHit = -1;

    // The same press look for a real button: A on Play / Pause, Left / Right on the seek buttons, X / Y on
    // Shuffle / Repeat, + on Lyrics light the matching on-screen button for a moment, so M3E plays the same
    // spring / ripple as for a finger. padButton = the NpButton, padAt = when.
    int padButton = -1;
    OSTime padAt = 0;
    void padPress(ui::NpButton b) { padButton = (int)b; padAt = OSGetTime(); }
    int pressedForUi() const {
        if (touchLastHit >= 0) return touchLastHit;
        if (padButton >= 0 && OSTicksToMilliseconds(OSGetTime() - padAt) < 180) return padButton;
        return -1;
    }

    // Music visualizer (- during audio playback). Video already owns -
    // for the GamePad-screen toggle above, so this only ever applies
    // when isVideo is false -- the two can't conflict.
    bool visualizerOn = false;

    // Lyrics of the song playing (music), fetched when it starts. Null / empty: none, the Lyrics button is greyed.
    const lyrics::Lyrics* lyrics = nullptr;
    bool hasLyrics() const { return lyrics && lyrics->available(); }
    // Dragging the lyrics with a finger: the panel's touches since the last frame, taken by the UI.
    bool lyrTouching = false;
    float lyrLastY = 0.0f, lyrDragDy = 0.0f;
    float takeLyricsDrag() { const float d = lyrDragDy; lyrDragDy = 0.0f; return d; }
    void toggleLyrics() { if (hasLyrics()) g_lyricsOpen = !g_lyricsOpen; }
    void toggleLyricsSync() { if (hasLyrics() && lyrics->synced) g_lyricsSync = !g_lyricsSync; }

    bool hasTrackChoice() const {
        return tracks && (tracks->audioTracks.size() > 1 || !tracks->subtitleTracks.empty());
    }

    void setGamepadScreen(bool off) {
        gamepadOff = off;
        VPADSetLcdMode(VPAD_CHAN_0, off ? VPAD_LCD_OFF : VPAD_LCD_ON);
        notice = off ? "GamePad screen off  (- to turn it back on)" : "GamePad screen on";
        noticeAt = OSGetTime();
    }

    // Shuffle button / X: turns shuffle on or off for the rest of the queue (the current track keeps playing).
    void toggleShuffle() {
        g_shuffleOn = queue ? !queue->shuffled() : !g_shuffleOn;
        if (queue) queue->setShuffle(g_shuffleOn, (uint32_t)OSGetTime() ^ (uint32_t)((uint64_t)OSGetTime() >> 32));
    }
    // Repeat button / Y: off -> repeat all -> repeat one -> off.
    void cycleRepeat() {
        g_repeatMode = queue ? queue->cycleRepeat()
                             : (g_repeatMode == PlayQueue::Repeat::Off ? PlayQueue::Repeat::All
                                : g_repeatMode == PlayQueue::Repeat::All ? PlayQueue::Repeat::One : PlayQueue::Repeat::Off);
    }

    // The settings flyout of the movie remote. True when it handled this poll (`out` is the command to return).
    bool pollVideoSettings(const input::State& in, PlayerCommand& out) {
        const uint32_t t = in.trigger;
        const int nq = vsCount();
        const bool onGear = in.touched && ui::npGearRect().contains(in.touchX, in.touchY);
        const bool gearTap = onGear && !gearLatch;
        gearLatch = onGear;
        if (!vsToast.empty() && OSTicksToMilliseconds(OSGetTime() - vsToastAt) > 2500) vsToast.clear();
        if (!vsOpen) {
            if (gearTap || (t & VPAD_BUTTON_Y)) {
                vsOpen = true;
                vsCursor = qualityIndex >= 0 ? qualityIndex : 0;
                prevTouched = true; // (the touch that opened it is not a tap inside it)
                out = PlayerCommand::none();
                return true;
            }
            return false;
        }
        out = PlayerCommand::none();
        const bool tap = in.touched && !prevTouched;
        prevTouched = in.touched;
        auto chooseQuality = [&](int idx) {
            if (idx < 0 || idx >= nq || idx == qualityIndex) return false;
            int n = 0;
            const quality::Preset* tbl = quality::presets(n);
            qualityIndex = idx;
            qualityChanged = true;
            g_sessionQuality = idx;
            vsToast = std::string("Switching to ") + tbl[idx].name + "...";
            vsToastAt = OSGetTime();
            out = PlayerCommand::seekTo(player ? player->positionSeconds() : 0.0); // the stream restarts here, like a skip
            return true;
        };
        auto stepAudio = [&](int dir) {
            if (!tracks || tracks->audioTracks.size() < 2) return false;
            audioIndex = stepTrack(tracks->audioTracks, audioIndex, dir, false);
            tracksChanged = true;
            out = PlayerCommand::seekTo(player ? player->positionSeconds() : 0.0);
            return true;
        };
        auto stepSubs = [&](int dir) {
            if (!tracks || tracks->subtitleTracks.empty()) return false;
            subtitleIndex = stepTrack(tracks->subtitleTracks, subtitleIndex, dir, true);
            tracksChanged = true;
            out = PlayerCommand::seekTo(player ? player->positionSeconds() : 0.0);
            return true;
        };
        if ((t & (VPAD_BUTTON_B | VPAD_BUTTON_Y)) || gearTap) { vsOpen = false; return true; }
        if (t & BTN_UP) vsCursor = std::max(0, vsCursor - 1);
        else if (t & BTN_DOWN) vsCursor = std::min(nq + 1, vsCursor + 1);
        else if (t & (BTN_LEFT | BTN_RIGHT)) {
            const int dir = (t & BTN_RIGHT) ? 1 : -1;
            if (vsCursor == nq) stepAudio(dir);
            else if (vsCursor == nq + 1) stepSubs(dir);
        } else if (t & VPAD_BUTTON_A) {
            if (vsCursor < nq) chooseQuality(vsCursor);
        } else if (tap) {
            const int hit = ui::hitTestVideoSettings(in.touchX, in.touchY, nq);
            if (hit == -1) vsOpen = false;
            else if (hit >= 0 && hit < nq) { vsCursor = hit; chooseQuality(hit); }
            else if (hit == 100) { vsCursor = nq; stepAudio(-1); }
            else if (hit == 101) { vsCursor = nq; stepAudio(1); }
            else if (hit == 102) { vsCursor = nq + 1; stepSubs(-1); }
            else if (hit == 103) { vsCursor = nq + 1; stepSubs(1); }
        }
        return true;
    }

    void fillVideoSettings(ui::VideoSettingsModel& v) const {
        v.gear = true;
        v.open = vsOpen;
        int n = 0;
        const quality::Preset* tbl = quality::presets(n);
        for (int i = 0; i < n; i++) v.qualities.push_back(tbl[i].name);
        v.quality = qualityIndex;
        v.cursor = vsCursor;
        v.toast = vsToast;
        if (tracks) {
            v.hasAudio = tracks->audioTracks.size() > 1;
            v.hasSubtitles = !tracks->subtitleTracks.empty();
            v.audioLabel = trackLabel(tracks->audioTracks, audioIndex, "Default");
            v.subtitleLabel = trackLabel(tracks->subtitleTracks, subtitleIndex, "Off");
        }
    }

    PlayerCommand poll() {
        if (!procRunningTracked()) return PlayerCommand::stop();
        const input::State in = input::read();
        const uint32_t t = in.trigger;
        if (t) lastInteraction = OSGetTime();

        if (trackMenu) {
            if (t & VPAD_BUTTON_B) {
                trackMenu = false;
            } else if (t & (BTN_UP | BTN_DOWN)) {
                trackRow ^= 1;
            } else if (t & (BTN_LEFT | BTN_RIGHT)) {
                int dir = (t & BTN_RIGHT) ? 1 : -1;
                if (trackRow == 0) menuAudio = stepTrack(tracks->audioTracks, menuAudio, dir, false);
                else menuSubtitle = stepTrack(tracks->subtitleTracks, menuSubtitle, dir, true);
            } else if (t & VPAD_BUTTON_A) {
                trackMenu = false;
                if (menuAudio != audioIndex || menuSubtitle != subtitleIndex) {
                    audioIndex = menuAudio;
                    subtitleIndex = menuSubtitle;
                    tracksChanged = true;
                    return PlayerCommand::seekTo(player ? player->positionSeconds() : 0.0);
                }
            }
            return PlayerCommand::none();
        }

        // The movie remote's settings flyout (gear / Y): quality, audio and subtitles of this viewing. Touch, and the
        // D-pad for those without a touch screen.
        if (client && remoteVideo()) {
            PlayerCommand c;
            if (pollVideoSettings(in, c)) return c;
        }
        // The Queue flyout / Add to Queue browser (music Now Playing, and the movie remote) own the controls while they
        // are open (B closes them instead of stopping, the D-pad moves in them), so nothing reaches the player behind
        // them. D-pad Up (or a tap on the Queue button) opens the flyout; nothing else used Up before.
        if (client && ((inQueue && !isVideo) || remoteVideo())) {
            const bool onButton = in.touched && ui::npQueueRect().contains(in.touchX, in.touchY);
            if (g_qui.modal()) {
                queueTapLatch = onButton; // (a finger that closed it on the button must not reopen it)
                if (g_qui.update(in, *client)) { trackStep = 1; if (isVideo) g_videoSkipped = true; return PlayerCommand::stop(); } // "Play now"
                return PlayerCommand::none();
            }
            if ((t & BTN_UP) || (onButton && !queueTapLatch)) { g_qui.openFlyout(); queueTapLatch = onButton; return PlayerCommand::none(); }
            queueTapLatch = onButton;
        }
        if (t & VPAD_BUTTON_B) return PlayerCommand::stop();
        if (isVideo) {
            if (t & VPAD_BUTTON_MINUS) setGamepadScreen(!gamepadOff);
            // + toggles TV-only mode: seekable video only (movies and
            // episodes), so Live TV -- which has its own simpler HUD and
            // no seeking -- is left alone, and so is music (this whole
            // branch is `isVideo`-only, same as the GamePad-screen toggle
            // above). canSeekAndPause is exactly "not Live TV" here, so no
            // separate flag is needed for that scoping.
            if (canSeekAndPause && (t & VPAD_BUTTON_PLUS)) tvOnlyMode = !tvOnlyMode;
        } else {
            if (t & VPAD_BUTTON_MINUS) {
                // Off -> the first style -> ... -> the last style -> off. (The style is remembered for the session.)
                if (!visualizerOn) visualizerOn = true;
                else if (++g_visualizerStyle >= ui::VisualizerModel::STYLE_COUNT) { g_visualizerStyle = 0; visualizerOn = false; }
            }
            if (t & VPAD_BUTTON_PLUS) { if (hasLyrics()) padPress(ui::NpButton::Lyrics); toggleLyrics(); }
        }
        // Music queue: X = shuffle, Y = repeat (a Wii Remote's 1 / 2 are X / Y).
        if (inQueue && !isVideo) {
            if (t & VPAD_BUTTON_X) { padPress(ui::NpButton::Shuffle); toggleShuffle(); }
            if (t & VPAD_BUTTON_Y) { padPress(ui::NpButton::Repeat); cycleRepeat(); }
        }

        // Lyrics open: a finger dragging inside the panel scrolls them.
        if (!isVideo && hasLyrics() && g_lyricsOpen && in.touched) {
            if (lyrTouching || (ui::npLyr() > 0.7f && ui::npLyricsPanel().contains(in.touchX, in.touchY))) {
                if (lyrTouching) lyrDragDy += in.touchY - lyrLastY;
                lyrLastY = in.touchY;
                lyrTouching = true;
            }
        } else {
            lyrTouching = false;
        }

        // GamePad touch: the transport buttons drawn under the progress
        // bar on any Now Playing screen -- music (with or without the
        // visualizer) and the movie remote (TV-only mode). Nothing else
        // (Live TV, the normal video HUD) ever draws that screen, so
        // touches are only hit-tested here in exactly those two cases.
        // Edge-detected via touchLastHit so a held finger fires once,
        // same as a real button's press-edge `t` above.
        const bool nowPlayingShown = !isVideo || tvOnlyMode;
        // Seeking by touch: a finger that lands on the progress bar picks a time (the screen shows it while the finger
        // moves, no preview); lifting it performs the seek. Nothing else reacts to that finger meanwhile.
        if (nowPlayingShown && canSeekAndPause && durationSeconds > 0.0) {
            if (scrubbing) {
                if (in.touched) {
                    scrubSeconds = ui::npProgressSeconds(in.touchX, durationSeconds);
                    return PlayerCommand::none();
                }
                scrubbing = false;
                pendingDelta = 0.0;
                return PlayerCommand::seekTo(std::max(0.0, std::min(scrubSeconds, durationSeconds)));
            }
            if (in.touched && !scrubPrevTouched && ui::npProgressRect().contains(in.touchX, in.touchY)) {
                scrubbing = true;
                scrubSeconds = ui::npProgressSeconds(in.touchX, durationSeconds);
                scrubPrevTouched = true;
                return PlayerCommand::none();
            }
        }
        scrubPrevTouched = in.touched;
        // Volume slider: unlike the transport buttons above, this isn't
        // edge-detected -- a held/dragged finger should keep updating the
        // volume every poll, not just on touch-down, or dragging would do
        // nothing until lifted and re-pressed.
        if (nowPlayingShown && in.touched && ui::npVolumeRect().contains(in.touchX, in.touchY)) {
            volume = ui::npVolumeFraction(in.touchX);
            g_playbackVolume = volume;
            return PlayerCommand::setVolume(volume);
        }
        int touchHit = (nowPlayingShown && in.touched) ? ui::hitTestNowPlaying(in.touchX, in.touchY) : -1;
        if (touchHit < 0 && inQueue && !isVideo && in.touched) touchHit = ui::hitTestNowPlayingToggles(in.touchX, in.touchY);
        if (touchHit < 0 && !isVideo && in.touched) touchHit = ui::hitTestNowPlayingLyrics(in.touchX, in.touchY);
        if (touchHit >= 0 && touchHit != touchLastHit) {
            touchLastHit = touchHit;
            switch ((ui::NpButton)touchHit) {
                case ui::NpButton::SeekBack:
                    if (canSeekAndPause) { pendingDelta -= SKIP_BACK_SECONDS; lastSkipPress = OSGetTime(); }
                    break;
                case ui::NpButton::PlayPause:
                    if (canSeekAndPause) return PlayerCommand::togglePause();
                    break;
                case ui::NpButton::SeekForward:
                    if (canSeekAndPause) { pendingDelta += SKIP_FORWARD_SECONDS; lastSkipPress = OSGetTime(); }
                    break;
                case ui::NpButton::Stop:
                    return PlayerCommand::stop();
                case ui::NpButton::Shuffle:
                    toggleShuffle();
                    break;
                case ui::NpButton::Repeat:
                    cycleRepeat();
                    break;
                case ui::NpButton::Lyrics:
                    toggleLyrics();
                    break;
                case ui::NpButton::LyricsSync:
                    toggleLyricsSync();
                    break;
                default:
                    break;
            }
        } else if (!in.touched) {
            touchLastHit = -1;
        }
        // The audio & subtitle panel is drawn as part of the TV's video
        // HUD (see presentVideo's non-tvOnlyMode branch); the clean TV
        // picture in TV-only mode has nowhere to show it, so it's parked
        // until + returns to the normal view.
        if (canSeekAndPause && !tvOnlyMode && (t & VPAD_BUTTON_Y) && hasTrackChoice()) {
            menuAudio = audioIndex;
            menuSubtitle = subtitleIndex;
            trackRow = 0;
            trackMenu = true;
            return PlayerCommand::none();
        }
        if (inQueue && (t & (VPAD_BUTTON_R | VPAD_BUTTON_ZR))) {
            trackStep = 1;
            return PlayerCommand::stop();
        }
        if (inQueue && (t & (VPAD_BUTTON_L | VPAD_BUTTON_ZL))) {
            trackStep = -1;
            return PlayerCommand::stop();
        }
        if (canSeekAndPause) {
            if (t & VPAD_BUTTON_A) { padPress(ui::NpButton::PlayPause); return PlayerCommand::togglePause(); }
            if (t & BTN_LEFT) {
                padPress(ui::NpButton::SeekBack);
                pendingDelta -= SKIP_BACK_SECONDS;
                lastSkipPress = OSGetTime();
            } else if (t & BTN_RIGHT) {
                padPress(ui::NpButton::SeekForward);
                pendingDelta += SKIP_FORWARD_SECONDS;
                lastSkipPress = OSGetTime();
            }
        }
        if (pendingDelta != 0.0 && OSTicksToMilliseconds(OSGetTime() - lastSkipPress) >= SEEK_COMMIT_MS) {
            double target = clampSeek(player ? player->positionSeconds() : 0.0, pendingDelta, durationSeconds);
            pendingDelta = 0.0;
            return PlayerCommand::seekTo(target);
        }
        return PlayerCommand::none();
    }
};

// How long a stream request that burns in a subtitle track may take to start
// answering (see PlayOptions::startupTimeoutSeconds). Jellyfin extracts the
// track from the media file first -- a read of the whole file for an embedded
// track -- so this is generous; B cancels the wait at any point.
static const int SUBTITLE_STARTUP_TIMEOUT_SECONDS = 90;

// Audio / subtitle language carried from one episode to the next.
struct TrackPrefs {
    bool set = false;
    std::string audioLanguage;
    std::string subtitleLanguage;
    bool subtitlesOff = true;
    bool alwaysDefault = false; // out: the user pressed "Always use default" in the first-play prompt
};

enum class TrackPromptResult { Cancel, Play, PlayAlways };

// The first-play prompt: audio and subtitle selectors (the same choices as the Y panel during
// playback), OK, and "Always use default". `audio` / `subtitle` come in as the defaults and go out
// as the choice. B cancels (nothing starts).
static TrackPromptResult askTracks(const std::string& title, const std::string& subtitleLine, const VideoInfo& info,
                                   int& audio, int& subtitle) {
    ui::TrackPromptModel m;
    m.title = title;
    m.subtitle = subtitleLine;
    m.selected = 2;
    m.hints = {{"< >", "Change"}, {"A", "Select"}, {"B", "Cancel"}};
    bool wasTouched = true; // a touch still held from the previous screen must not count
    while (WHBProcIsRunning()) {
        const input::State in = input::read();
        const uint32_t t = in.trigger;
        const bool tapped = in.touched && !wasTouched;
        wasTouched = in.touched;
        auto step = [&](int slot, int dir) {
            if (slot == 0) audio = stepTrack(info.audioTracks, audio, dir, false);
            else if (slot == 1) subtitle = stepTrack(info.subtitleTracks, subtitle, dir, true);
        };
        if (tapped) {
            const int hit = ui::hitTestTrackPrompt(in.touchX, in.touchY);
            if (hit >= 2) return hit == 2 ? TrackPromptResult::Play : TrackPromptResult::PlayAlways;
            if (hit >= 0) {
                m.selected = hit;
                const ui::Rect r = ui::trackPromptSlot(hit);
                step(hit, in.touchX < r.x + r.w * 0.3f ? -1 : 1); // the left part steps back, the rest forward
            }
        }
        if (t & VPAD_BUTTON_B) return TrackPromptResult::Cancel;
        if (t & BTN_DOWN) m.selected = trackPromptMove(m.selected, 1);
        else if (t & BTN_UP) m.selected = trackPromptMove(m.selected, -1);
        else if (t & (BTN_LEFT | BTN_RIGHT)) {
            const int dir = (t & BTN_RIGHT) ? 1 : -1;
            if (m.selected >= 2) m.selected = trackPromptSide(m.selected);
            else step(m.selected, dir);
        } else if (t & VPAD_BUTTON_A) {
            if (m.selected == 2) return TrackPromptResult::Play;
            if (m.selected == 3) return TrackPromptResult::PlayAlways;
            m.selected = 2; // done with this row: on to OK
        }
        m.audioLabel = trackLabel(info.audioTracks, audio, "Default");
        m.subtitleLabel = trackLabel(info.subtitleTracks, subtitle, "Off");
        drawFrame([&] { ui::drawTrackPrompt(m); });
    }
    return TrackPromptResult::Cancel;
}

class ProgressReporter {
public:
    ProgressReporter(JellyfinClient& client, std::string itemId, PlaybackIds ids)
        : client_(client), itemId_(std::move(itemId)), ids_(std::move(ids)),
          thread_([this] { run(); }) {}

    ~ProgressReporter() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void update(double positionSeconds, bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_ = positionSeconds;
        paused_ = paused;
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            wake_.wait_for(lock, std::chrono::seconds(10), [this] { return stop_; });
            if (stop_) break;
            int64_t ticks = (int64_t)(position_ * 10000000.0);
            bool paused = paused_;
            lock.unlock();
            client_.reportPlaybackProgress(itemId_, ticks, ids_, paused);
            lock.lock();
        }
    }

    JellyfinClient& client_;
    std::string itemId_;
    PlaybackIds ids_;
    double position_ = 0.0; // guarded by mutex_ (a 64-bit atomic would need libatomic on PPC)
    bool paused_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};


// When playing from a music queue: shown on Now Playing, and L/R end
// the track early (trackStep says which way).
struct QueueInfo {
    UserQueue* upNext = nullptr; // the Up Next list (the Queue flyout edits it while a track plays)
    PlayQueue* q = nullptr;  // the queue itself: Shuffle / Repeat change it while a track plays
    std::string queueText;
    std::string nextText;
    int trackStep = 0;       // out: -1 previous, +1 next, 0 = ended/stopped normally
    double endPosition = 0;  // out: where playback was when it ended
};

// "Track 3 of 12  |  Shuffle  |  Repeat all", built fresh every frame: the buttons change it while playing.
static std::string queueLine(const PlayQueue& q) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Track %u of %u", (unsigned)(q.position() + 1), (unsigned)q.size());
    std::string s = buf;
    if (q.shuffled()) s += "  |  Shuffle";
    if (q.repeat() == PlayQueue::Repeat::All) s += "  |  Repeat all";
    else if (q.repeat() == PlayQueue::Repeat::One) s += "  |  Repeat one";
    return s;
}

// Fills a Now Playing model's queue part: the text lines and the Shuffle / Repeat button states.
static void applyQueueModel(ui::NowPlayingModel& np, const QueueInfo* info) {
    if (!info) return;
    if (info->q) {
        const PlayQueue& q = *info->q;
        np.queueText = queueLine(q);
        const JellyfinItem* next = q.repeat() == PlayQueue::Repeat::One ? &q.current()
                                   : (info->upNext && !info->upNext->empty() ? info->upNext->front() : q.peekNext());
        np.nextText = next ? "Next: " + itemDisplayName(*next) : std::string();
        np.queueControls = true;
        np.shuffleOn = q.shuffled();
        np.repeatMode = (int)q.repeat();
    } else {
        np.queueText = info->queueText;
        np.nextText = info->nextText;
    }
}

// Plays one item start to finish (or until B), handling Live TV tuning,
// seeks (each one restarts the transcode), the on-screen HUD / Now
// Playing screen, and Jellyfin progress reporting. Returns the player
// result and fills errorMessage on PlayResult::Error.
static PlayResult playItem(JellyfinClient& client, const UfinConfig& cfg, const JellyfinItem& picked,
                           std::string& errorMessage, QueueInfo* queue = nullptr, double startAtSeconds = 0.0,
                           TrackPrefs* prefs = nullptr) {
    const bool isAudio = (picked.type == "Audio");
    const bool isLive = (picked.type == "TvChannel");
    ArtworkThemeScope artworkTheme; // (the theme goes back to the selected palette however playback ends)
    const std::string title = itemDisplayName(picked);
    // Subtitle for the movie remote UI (TV-only mode, below): show name +
    // "S#E#" for episodes, else the year, matching the codes already used
    // elsewhere (see item_labels.cpp's episodeCode()/itemTag()).
    std::string movieSubtitle;
    if (!picked.seriesName.empty()) {
        movieSubtitle = picked.seriesName;
        if (picked.parentIndexNumber >= 0 && picked.indexNumber >= 0) {
            char code[32];
            snprintf(code, sizeof(code), "S%dE%d", picked.parentIndexNumber, picked.indexNumber);
            movieSubtitle += std::string("  ") + code;
        }
    } else if (picked.productionYear > 0) {
        movieSubtitle = std::to_string(picked.productionYear);
    }

    VideoStreamOptions videoOptions;
    videoOptions.videoBitrate = cfg.videoBitrate;
    videoOptions.profile = cfg.videoProfile;
    videoOptions.width = cfg.videoWidth();   // 1280x720 unless a 480p or 1080p30 Experimental preset is selected
    videoOptions.height = cfg.videoHeight;
    if (g_sessionQuality >= 0) applyQualityPreset(videoOptions, g_sessionQuality); // chosen from the movie remote earlier

    double durationSeconds = picked.runTimeTicks > 0 ? picked.runTimeTicks / 10000000.0 : 0.0;
    PlayOptions playOptions;
    playOptions.allowUnalignedVideoGeometry = cfg.allowUnalignedVideoGeometry;
    playOptions.maxBufferedPacketBytes = (size_t)cfg.videoBufferMiB * 1024u * 1024u; // 0 = automatic
    playOptions.videoQueueFrames = (size_t)cfg.videoQueueFrames;                      // 0 = automatic
    PlaybackIds ids;
    VideoInfo info; // tracks, for the Audio & subtitles panel

    showBusy(isLive ? "Tuning" : "Loading",
             {title, "",
              isLive ? "Asking the server to tune the channel and start the transcode."
                     : "Waiting for the server to start streaming."});

    if (isLive) {
        // Live TV needs the tuner opened first; the stream endpoint then
        // wants the ids that come back.
        LiveStreamSession live;
        if (!client.openLiveStream(picked.id, cfg.videoBitrate, live)) {
            errorMessage = "Could not open the channel: " + client.lastError();
            return PlayResult::Error;
        }
        videoOptions.mediaSourceId = live.mediaSourceId;
        videoOptions.liveStreamId = live.liveStreamId;
        videoOptions.playSessionId = live.playSessionId;
        playOptions.displayAspect = live.info.displayAspect > 0.0 ? live.info.displayAspect : 16.0 / 9.0;
        durationSeconds = 0.0;
    } else if (!isAudio) {
        // Jellyfin is asked to encode every video at exactly 1280x720
        // (768x480 in the 480p mode, 1920x1080 in the experimental 1080p30 mode; see
        // buildVideoStreamUrl for why), so the real shape of the
        // picture has to come from the item's metadata. If we can't get
        // it, assume 16:9.
        if (client.getVideoInfo(picked.id, info)) {
            videoOptions.mediaSourceId = info.mediaSourceId;
            if (info.runTimeTicks > 0) durationSeconds = info.runTimeTicks / 10000000.0;
        }
        playOptions.displayAspect = info.displayAspect > 0.0 ? info.displayAspect : 16.0 / 9.0;
    }

    // First start of a viewing: ask which audio and subtitles to use (unless Settings says to always use
    // the default). `chosen` carries the answer into the track set-up below.
    bool promptChosen = false;
    int promptAudio = info.defaultAudioIndex, promptSubtitle = info.defaultSubtitleIndex;
    if (!isAudio && !isLive && prefs && shouldAskTracks(cfg.alwaysDefaultTracks, prefs->set, info)) {
        const TrackPromptResult r = askTracks(title, movieSubtitle.empty() ? "Choose audio and subtitles" : movieSubtitle,
                                              info, promptAudio, promptSubtitle);
        if (r == TrackPromptResult::Cancel) return PlayResult::Stopped;
        promptChosen = true;
        if (r == TrackPromptResult::PlayAlways) {
            prefs->alwaysDefault = true;
            promptAudio = info.defaultAudioIndex; // "default" means the server's own choice
            promptSubtitle = info.defaultSubtitleIndex;
        }
        showBusy("Loading", {title, "", "Waiting for the server to start streaming."});
    }

    ids.mediaSourceId = videoOptions.mediaSourceId;
    ids.liveStreamId = videoOptions.liveStreamId;
    ids.playSessionId = videoOptions.playSessionId;

    client.reportPlaybackStart(picked.id, ids);

    Player player;
    PlaybackControl control;
    playOptions.initialVolume = control.volume; // carries the user's last-set volume in
    control.canSeekAndPause = !isLive;
    control.inQueue = (queue != nullptr);
    control.client = &client;
    control.qualityIndex = g_sessionQuality >= 0 ? g_sessionQuality : quality::indexFor(videoOptions.videoBitrate, videoOptions.height);
    control.queue = queue ? queue->q : nullptr;
    control.isVideo = !isAudio;
    lyrics::Lyrics songLyrics; // music: fetched now (one small request); none = the Lyrics button stays greyed
    if (isAudio && client.getLyrics(picked.id, songLyrics)) control.lyrics = &songLyrics;
    if (!isAudio && !isLive) {
        control.tracks = &info;
        control.audioIndex = info.defaultAudioIndex;
        control.subtitleIndex = info.defaultSubtitleIndex;
        if (promptChosen) {
            control.audioIndex = promptAudio;
            control.subtitleIndex = promptSubtitle;
            if (!prefs->alwaysDefault) {
                videoOptions.audioStreamIndex = control.audioIndex;
                videoOptions.subtitleStreamIndex = control.subtitleIndex;
                videoOptions.subtitleIsText = subtitleTrackIsText(info.subtitleTracks, control.subtitleIndex);
            }
        } else if (prefs && prefs->set) {
            // Same language as the last episode, when this one has it.
            control.audioIndex = trackForLanguage(info.audioTracks, prefs->audioLanguage, info.defaultAudioIndex);
            control.subtitleIndex = prefs->subtitlesOff ? -1
                : trackForLanguage(info.subtitleTracks, prefs->subtitleLanguage, info.defaultSubtitleIndex);
            videoOptions.audioStreamIndex = control.audioIndex;
            videoOptions.subtitleStreamIndex = control.subtitleIndex;
            videoOptions.subtitleIsText = subtitleTrackIsText(info.subtitleTracks, control.subtitleIndex);
        }
    }
    control.durationSeconds = durationSeconds;
    control.player = &player;
    control.lastInteraction = OSGetTime(); // show the HUD at the start

    // Refreshes the busy screen with progress while Player waits out its
    // initial buffer (see PREBUFFER_TARGET_SECONDS in player.cpp) -- the
    // one-shot showBusy() call above would otherwise sit frozen for
    // however long that takes.
    playOptions.onBuffering = [&](double buffered, double target) {
        char line[64];
        snprintf(line, sizeof(line), "Buffering... %.0f / %.0fs", buffered, target);
        showBusy(isLive ? "Tuning" : "Loading", {title, "", line});
    };

    // Video: every frame is drawn as the underlay of an app frame with
    // the HUD on top. The HUD shows while paused, while a skip is being
    // collected, and for a few seconds after any button press.
    playOptions.presentVideo = [&](const std::function<void(uint32_t, uint32_t)>& drawVideo) {
        updateArtworkTheme(picked);
        if (control.tvOnlyMode) {
            // TV = clean video, no HUD at all (empty tvBuild). GamePad =
            // a movie remote, built the same way Now Playing already is
            // for music (see onIdleFrame below) -- same model, same draw
            // function, same controls, just a different subtitle/hints.
            // This does not touch the decoder, the audio clock or Player
            // in any way: it only changes which of the two independent
            // ImGui frames drawn by frameTvDrc goes where, exactly like
            // the music visualizer's TV/GamePad split already does.
            ui::NowPlayingModel np;
        np.pressedButton = control.pressedForUi(); // the pressed look (touch, or a real button)
            np.title = title;
            np.subtitle = movieSubtitle;
            np.positionSeconds = player.positionSeconds();
            np.durationSeconds = durationSeconds;
            np.paused = player.isPaused();
            np.volume = control.volume;
            np.scrubbing = control.scrubbing; np.scrubSeconds = control.scrubSeconds;
            if (control.canSeekAndPause) {
                control.fillVideoSettings(np.settings);
                g_qui.fill(np, picked);
                np.queueCount = (int)g_upNext.size();
            }
            if (control.pendingDelta != 0.0) {
                // Same wording as the audio Now Playing screen's pending-skip
                // subtitle below, for consistency.
                char buf[64];
                snprintf(buf, sizeof(buf), "Skipping %+d s ...", (int)control.pendingDelta);
                np.subtitle = buf;
            }
            np.hints = {{"A", np.paused ? "Play" : "Pause"}, {"<", "-10 s"}, {">", "+30 s"}};
            if (control.canSeekAndPause) { np.hints.push_back({"Up", "Queue"}); np.hints.push_back({"Y", "Settings"}); }
            np.hints.push_back({"+", "Normal view"});
            np.hints.push_back({"B", "Stop"});
            if (g_qui.flyoutOpen) np.hints = QueueUi::flyoutHints();
            if (g_qui.browserOpen || np.settings.open) np.hints.clear();
            np.art = artFor(picked, ui::layout::NP_IMAGE_MAX, ui::layout::NP_IMAGE_MAX, nullptr);
            // Rebuffering (not the user's pause): the TV stays a clean
            // picture apart from the buffering wheel -- without it the TV
            // would just look frozen -- and the GamePad says so too.
            const bool rebuffering = player.isBuffering() && !player.isPaused();
            std::function<void()> tvBuild;
            if (rebuffering) {
                const double have = player.bufferedSeconds(), target = player.bufferingTargetSeconds();
                tvBuild = [have, target] { ui::drawBufferingIndicator(have, target); };
                if (control.pendingDelta == 0.0) np.subtitle = "Buffering...";
            }
            ui::gfx().frameTvDrc(tvBuild, [&] { ui::drawNowPlaying(np); }, drawVideo);
            // Same reason as the music visualizer's frameTvDrc call above:
            // bypasses drawFrame(), so pump the image cache by hand or the
            // poster never finishes uploading and stays a placeholder.
            if (g_images) g_images->pump();
            return;
        }
        ui::VideoHudModel hud;
        hud.title = title;
        hud.live = isLive;
        hud.positionSeconds = player.positionSeconds();
        hud.durationSeconds = durationSeconds;
        hud.paused = player.isPaused();
        hud.buffering = player.isBuffering() && !hud.paused;
        hud.bufferedSeconds = player.bufferedSeconds();
        hud.bufferingTargetSeconds = player.bufferingTargetSeconds();
        hud.seekDelta = control.pendingDelta;
        if (hud.seekDelta != 0.0) {
            hud.seekTarget = clampSeek(hud.positionSeconds, hud.seekDelta, durationSeconds);
        }
        hud.visible = hud.paused || secondsSince(control.lastInteraction) < 3.0;
        hud.trackMenu = control.trackMenu;
        hud.trackRow = control.trackRow;
        if (control.trackMenu) {
            hud.audioLabel = trackLabel(info.audioTracks, control.menuAudio, "Default");
            hud.subtitleLabel = trackLabel(info.subtitleTracks, control.menuSubtitle, "Off");
        }
        if (!control.notice.empty() && secondsSince(control.noticeAt) < 2.5) hud.notice = control.notice;
        if (control.trackMenu) {
            hud.hints = {{"< >", "Change"}, {"A", "Apply"}, {"B", "Close"}};
        } else if (isLive) {
            hud.hints = {{"B", "Stop"}, {"-", "GamePad screen"}};
        } else {
            hud.hints = {{"A", hud.paused ? "Play" : "Pause"}, {"<", "-10 s"}, {">", "+30 s"}};
            if (control.hasTrackChoice()) hud.hints.push_back({"Y", "Audio & subtitles"});
            hud.hints.push_back({"+", "TV mode"});
            hud.hints.push_back({"-", "GamePad screen"});
            hud.hints.push_back({"B", "Stop"});
        }
        drawFrame([&] { ui::drawVideoHud(hud); }, drawVideo);
    };

    // Music visualizer (- to toggle, see PlaybackControl::poll). Owned
    // here rather than inside Player so it stays purely a UI/render-
    // thread concern -- Player only ever hands out raw PCM.
    media::AudioVisualizer visualizer;
    uint32_t visualizerSeq = 0;
    OSTime lastIdleFrameTime = OSGetTime();

    // Audio: Now Playing, redrawn every frame by the player loop.
    playOptions.onIdleFrame = [&]() {
        updateArtworkTheme(picked);
        OSTime now = OSGetTime();
        double dt = OSTicksToMilliseconds(now - lastIdleFrameTime) / 1000.0;
        lastIdleFrameTime = now;

        if (control.visualizerOn) {
            // Analysis runs here, on the same thread as this callback
            // (the render loop), never on the audio/decode thread --
            // see AudioVisualizer's own comment for why that's safe to
            // do unconditionally every frame.
            int16_t pcm[1024];
            int n = player.fetchVisualizerSamples(pcm, 1024, &visualizerSeq);
            int rate = player.visualizerSampleRate();
            visualizer.update(n > 0 ? pcm : nullptr, n, rate, dt);

            ui::VisualizerModel vm;
            vm.title = title;
            vm.paused = player.isPaused();
            vm.style = g_visualizerStyle;
            vm.bandCount = media::AudioVisualizer::BANDS;
            for (int i = 0; i < vm.bandCount && i < ui::VisualizerModel::MAX_BANDS; i++) {
                vm.bands[i] = visualizer.levels()[i];
            }
            vm.hints = {{"A", vm.paused ? "Play" : "Pause"}, {"< >", "Seek"}};
            if (queue) vm.hints.push_back({"L R", "Prev / Next"});
            if (queue) { vm.hints.push_back({"X", "Shuffle"}); vm.hints.push_back({"Y", "Repeat"}); }
            vm.hints.push_back({"-", "Next style / off"});
            vm.hints.push_back({"B", "Stop"});

            // The visualizer is a TV-only presentation. Keep the normal
            // Now Playing UI on the GamePad so controls, progress, artwork
            // and the visualizer toggle remain available there.
            ui::NowPlayingModel np;
        np.pressedButton = control.pressedForUi(); // the pressed look (touch, or a real button)
            np.title = title;
            np.subtitle = picked.seriesName.empty() ? "Audio" : picked.seriesName;
            np.positionSeconds = player.positionSeconds();
            np.durationSeconds = durationSeconds;
            np.paused = player.isPaused();
            np.volume = control.volume;
            np.scrubbing = control.scrubbing; np.scrubSeconds = control.scrubSeconds;
            applyQueueModel(np, queue);
            if (queue) g_qui.fill(np, picked);
            np.lyrics = control.hasLyrics() ? control.lyrics : nullptr;
            np.lyricsOpen = g_lyricsOpen && control.hasLyrics();
            np.lyricsSync = g_lyricsSync;
            np.lyricsCapable = true;
        np.lyricsTouching = control.lyrTouching;
        np.lyricsDragDy = control.takeLyricsDrag();
            if (control.pendingDelta != 0.0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Skipping %+d s ...", (int)control.pendingDelta);
                np.subtitle = buf;
            }
            np.hints = {{"A", np.paused ? "Play" : "Pause"}, {"< >", "Seek"}};
            if (queue) np.hints.push_back({"L R", "Prev / Next"});
            if (queue) { np.hints.push_back({"X", "Shuffle"}); np.hints.push_back({"Y", "Repeat"}); np.hints.push_back({"Up", "Queue"}); }
            np.hints.push_back({"-", "Visualizer"});
            if (control.hasLyrics()) np.hints.push_back({"+", "Lyrics"});
            np.hints.push_back({"B", "Stop"});
            if (g_qui.flyoutOpen) np.hints = QueueUi::flyoutHints();
            if (g_qui.browserOpen) np.hints.clear();
            np.art = artFor(picked, ui::layout::NP_IMAGE_MAX, ui::layout::NP_IMAGE_MAX, nullptr);

            ui::gfx().frameTvDrc(
                [&] { ui::drawMusicVisualizer(vm); },
                [&] { ui::drawNowPlaying(np); });
            // frameTvDrc() doesn't go through drawFrame(), so pump the
            // image cache here too -- otherwise a not-yet-uploaded art
            // texture (e.g. right after playback starts) never gets its
            // GX2 upload while this two-pass path is active, and the
            // GamePad is stuck showing the placeholder note icon.
            if (g_images) g_images->pump();
            return;
        }
        // Visualizer just turned off (or never on) -- keep it reset so
        // turning it back on later doesn't flash stale levels from
        // before.
        visualizer.reset();

        ui::NowPlayingModel np;
        np.pressedButton = control.pressedForUi(); // the pressed look (touch, or a real button)
        np.title = title;
        np.subtitle = picked.seriesName.empty() ? "Audio" : picked.seriesName;
        np.positionSeconds = player.positionSeconds();
        np.durationSeconds = durationSeconds;
        np.paused = player.isPaused();
        np.volume = control.volume;
        np.scrubbing = control.scrubbing; np.scrubSeconds = control.scrubSeconds;
        applyQueueModel(np, queue);
        if (queue) g_qui.fill(np, picked);
        np.lyrics = control.hasLyrics() ? control.lyrics : nullptr;
        np.lyricsOpen = g_lyricsOpen && control.hasLyrics();
        np.lyricsSync = g_lyricsSync;
        np.lyricsCapable = true;
        np.lyricsTouching = control.lyrTouching;
        np.lyricsDragDy = control.takeLyricsDrag();
        if (control.pendingDelta != 0.0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Skipping %+d s ...", (int)control.pendingDelta);
            np.subtitle = buf;
        }
        np.hints = {{"A", np.paused ? "Play" : "Pause"}, {"< >", "Seek"}};
        if (queue) np.hints.push_back({"L R", "Prev / Next"});
        if (queue) { np.hints.push_back({"X", "Shuffle"}); np.hints.push_back({"Y", "Repeat"}); np.hints.push_back({"Up", "Queue"}); }
        np.hints.push_back({"-", "Visualizer"});
        if (control.hasLyrics()) np.hints.push_back({"+", "Lyrics"});
        np.hints.push_back({"B", "Stop"});
        if (g_qui.flyoutOpen) np.hints = QueueUi::flyoutHints();
        if (g_qui.browserOpen) np.hints.clear();
        np.art = artFor(picked, ui::layout::NP_IMAGE_MAX, ui::layout::NP_IMAGE_MAX, nullptr);
        drawFrame([&] { ui::drawNowPlaying(np); });
    };

    PlayResult result;
    double startAt = startAtSeconds;
    if (!isAudio && cfg.gamepadOffInVideo) control.setGamepadScreen(true);
    {
    ProgressReporter reporter(client, picked.id, ids);
    while (true) {
        // Seeking restarts the transcode at the new position: the stream
        // itself can't be seeked, but Jellyfin starts one anywhere.
        videoOptions.startTimeTicks = (int64_t)(startAt * 10000000.0);
        // A fresh play session per stream start (Live TV keeps the one its
        // tuner was opened with): Jellyfin keys transcodes by session, not
        // start time, so reusing one replays the old transcode from 0:00.
        if (!isLive) {
            static uint32_t sessionCounter = 0;
            videoOptions.playSessionId = makePlaySessionId((uint64_t)OSGetTime(), ++sessionCounter);
        }
        StreamTarget target = isAudio
            ? client.buildAudioStreamUrl(picked.id, videoOptions.startTimeTicks, videoOptions.playSessionId)
            : client.buildVideoStreamUrl(picked.id, videoOptions);
        OSReport("Ufin: stream start at %.1fs (session %s)\n", startAt, videoOptions.playSessionId.c_str());
        if (!isAudio) {
            OSReport("Ufin: transcode request h264 profile=%s %dx%d %d bps level=4.1\n",
                     h264::wireName(h264::parse(videoOptions.profile)), videoOptions.width,
                     videoOptions.height, videoOptions.videoBitrate);
        }
        // A burned-in subtitle makes Jellyfin prepare the track (extract it, and
        // its fonts, from the media file) before it answers the stream request at
        // all, which can take far longer than the usual 15 s wait for the first
        // bytes -- so that wait is longer here, and B cancels it (see
        // PlayOptions::startupTimeoutSeconds). No subtitle: unchanged.
        const bool burnedInSubtitle = !isAudio && videoOptions.subtitleStreamIndex >= 0;
        playOptions.startupTimeoutSeconds = burnedInSubtitle ? SUBTITLE_STARTUP_TIMEOUT_SECONDS : 0;
        if (burnedInSubtitle) {
            OSReport("Ufin: subtitles: track %d (%s) burned in by the server%s\n", videoOptions.subtitleStreamIndex,
                     videoOptions.subtitleIsText ? "text" : "bitmap or unknown",
                     (videoOptions.subtitleIsText && videoOptions.startTimeTicks > 0) ? ", source timestamps kept (CopyTimestamps)" : "");
            showBusy("Loading", {title, "", "Preparing the subtitles on the server (B to cancel)."});
        }
        playOptions.startOffsetSeconds = startAt;

        result = player.play(target.host, target.port, target.path,
            [&]() { return control.poll(); },
            [&](double positionSeconds) {
                // Roughly once per second, on the render loop -- so
                // nothing slow here. The reporter thread tells Jellyfin.
                reporter.update(positionSeconds, player.isPaused());
            },
            playOptions);

        if (result != PlayResult::SeekRequested) break;
        // Carry whatever volume the user left it at into the stream
        // restart below (a fresh Player::play() call otherwise starts
        // back at playOptions.initialVolume's original value).
        control.volume = (float)player.volume();
        playOptions.initialVolume = control.volume;
        startAt = player.seekTarget();
        if (control.tracksChanged) {
            // New audio / subtitle choice: same restart as a skip, with the
            // tracks in the request.
            control.tracksChanged = false;
            videoOptions.audioStreamIndex = control.audioIndex;
            videoOptions.subtitleStreamIndex = control.subtitleIndex;
            videoOptions.subtitleIsText = subtitleTrackIsText(info.subtitleTracks, control.subtitleIndex);
        }
        if (control.qualityChanged) {
            control.qualityChanged = false;
            applyQualityPreset(videoOptions, control.qualityIndex);
        }
        reporter.update(startAt, false);
        if (!isAudio) showBusy("Skipping", {title, "", "Restarting the stream at " + ui::formatTime(startAt) + "."});
    }
    } // reporter stops here, before the final report

    // Report the stop against the session actually playing, so Jellyfin
    // ends that transcode.
    ids.playSessionId = videoOptions.playSessionId;
    client.reportPlaybackStopped(picked.id, (int64_t)(player.positionSeconds() * 10000000.0), ids);
    if (isLive) client.closeLiveStream(ids.liveStreamId);

    if (queue) {
        queue->trackStep = control.trackStep;
        queue->endPosition = player.positionSeconds();
    }
    if (control.gamepadOff) VPADSetLcdMode(VPAD_CHAN_0, VPAD_LCD_ON);
    if (prefs && control.tracks) {
        prefs->set = true;
        prefs->audioLanguage = trackLanguage(info.audioTracks, control.audioIndex);
        prefs->subtitlesOff = control.subtitleIndex < 0;
        prefs->subtitleLanguage = trackLanguage(info.subtitleTracks, control.subtitleIndex);
    }

    if (result == PlayResult::Error) {
        errorMessage = "Playback error: " + player.lastError();
    }
    return result;
}

// Plays a music queue track by track. L/R skip (L restarts the track
// if it's more than 3 s in, like most players), B stops the whole queue,
// and the queue ends after the last track -- unless Repeat is on (all: start over; one: the
// same track again). Shuffle and Repeat can be changed while it plays (PlaybackControl).
static PlayResult playQueueLoop(JellyfinClient& client, const UfinConfig& cfg, PlayQueue& queue,
                                std::string& errorMessage) {
    JellyfinItem playing = queue.empty() ? JellyfinItem() : queue.current();
    bool onQueued = false; // `playing` came from the Up Next list (the music queue's own position did not move)
    while (!queue.empty() && WHBProcIsRunning()) {
        QueueInfo info;
        info.q = &queue; // Now Playing reads the track number, "Next:" and the Shuffle / Repeat state from it live
        info.queueText = queueLine(queue);
        info.upNext = &g_upNext;
        if (!g_upNext.empty()) info.nextText = "Next: " + itemDisplayName(*g_upNext.front());
        else if (const JellyfinItem* next = queue.peekNext()) info.nextText = "Next: " + itemDisplayName(*next);

        PlayResult result = playItem(client, cfg, playing, errorMessage, &info);
        if (result == PlayResult::Error) return result; // (a failed Up Next entry was already taken off the list)

        JellyfinItem out;
        if (info.trackStep < 0) {
            // L / previous: from an Up Next track back to the music queue's current track; otherwise the usual
            // "restart if more than 3 s in, else the track before".
            if (onQueued) { onQueued = false; playing = queue.current(); continue; }
            if (info.endPosition <= 3.0) queue.previous(); // else: replay this track
            playing = queue.current();
            continue;
        }
        if (info.trackStep > 0 || result == PlayResult::Completed) {
            switch (advanceMusic(queue, g_upNext, info.trackStep > 0, out)) {
                case Advance::Replay: continue;
                case Advance::Queued: playing = out; onQueued = true; continue;
                case Advance::Base: playing = queue.current(); onQueued = false; continue;
                case Advance::Handoff: return PlayResult::Completed; // the caller plays the queued movie / episode
                case Advance::Stop: return PlayResult::Completed;
            }
        }
        return result; // B
    }
    return PlayResult::Stopped;
}

static PlayResult playQueue(JellyfinClient& client, const UfinConfig& cfg, PlayQueue& queue,
                            std::string& errorMessage) {
    const PlayResult r = playQueueLoop(client, cfg, queue, errorMessage);
    g_qui.closeAll(); // the flyout and the browser go away with the player (the list itself stays)
    return r;
}

// Diagnostic (ZR in the menus): a flat magenta picture through the exact
// same shader/upload/present path as real video, with no decode or
// streaming involved. B exits.
static void runGx2TestPattern() {
    VideoOutput testOutput;
    if (testOutput.init(1280, 720, 16.0 / 9.0)) {
        while (WHBProcIsRunning()) {
            testOutput.renderTestPattern();
            if (input::read().trigger & VPAD_BUTTON_B) break;
        }
    }
    testOutput.shutdown();
}

// --- input helpers ---

// --- the app ---

// --- startup update check ---------------------------------------------------
//
// Asks GitHub (over HTTPS, see https_client.h) whether a Ufin release newer than this build exists, on
// a background thread so start-up is never held up, and says so once, as a card over whatever screen the app
// is on (never during playback). Everything that can go wrong -- offline, GitHub or TLS refusing, an odd answer --
// ends in a log line and nothing on screen. Rules and wording: update_check.h. config.json
// "check_for_updates": false turns it off.

// The check runs on its own OS thread (coreinit's OSCreateThread with a stack of ours, not std::thread:
// a std::thread started from here froze Cemu), so the UI never waits for the network. The worker only
// touches its own state and the https/dns code; the main loop polls it once a frame.
struct UpdateWorker {
    OSThread thread;
    alignas(16) uint8_t stack[192 * 1024]; // Mbed TLS's handshake wants a roomy stack
    std::atomic<int> state{0};             // 0 idle, 1 running, 2 finished (outcome is valid)
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{0};          // 0..100, a rough guess from the connection's stages (for the notification bar)
    updatecheck::Outcome outcome;
};
static UpdateWorker g_update;

static int updateWorkerMain(int, const char**) {
    https::Options options;
    options.timeoutSeconds = 8;
    options.cancelled = [] { return g_update.cancel.load(); };
    updatecheck::Fetcher fetch = [&options](const std::string& host, const std::string& path,
                                            const std::string& headers) {
        const https::Response r = https::get(host, path, headers, options);
        updatecheck::FetchResult f;
        f.ok = r.ok;
        f.status = r.status;
        f.body = r.body;
        f.error = r.error;
        return f;
    };
    OSReport("Ufin: update check: this is Ufin %s, asking GitHub\n", ufin::kVersion);
    updatecheck::Outcome outcome = updatecheck::checkForUpdate(fetch, ufin::kVersion, ufin::kGithubOwner,
                                                               ufin::kGithubRepo, ufin::kStorePackageName,
                                                               [] { return g_update.cancel.load(); });
    if (g_update.cancel.load()) outcome.kind = updatecheck::Outcome::Kind::Unknown, outcome.detail = "cancelled";
    OSReport("Ufin: update check: %s\n", outcome.detail.c_str());
    g_update.outcome = outcome;
    g_update.progress = 100;
    g_update.state = 2;
    return 0;
}

// The finished check, if there is one (and the thread is reaped).
static bool takeUpdateResult(updatecheck::Outcome& out) {
    if (g_update.state.load() != 2) return false;
    int result = 0;
    OSJoinThread(&g_update.thread, &result);
    out = g_update.outcome;
    g_update.state = 0;
    return true;
}

// Starts a check. False when one is still running (a cancelled one needs a moment to notice).
static bool startUpdateCheck() {
    updatecheck::Outcome stale;
    takeUpdateResult(stale); // a cancelled check's late answer is dropped
    if (g_update.state.load() != 0) return false;
    g_update.cancel = false;
    g_update.state = 1;
    g_update.progress = 0;
    https::setTrace([](const char* message) {
        OSReport("Ufin: https: %s\n", message);
        static const struct { const char* text; int percent; } stages[] = {
            {"DNS", 5}, {"TCP connect", 15}, {"TLS setup", 25}, {"TLS handshake", 35}, {"TLS ready", 60},
            {"sending the request", 65}, {"request sent", 75}, {"the answer is arriving", 90}};
        for (const auto& st : stages) {
            if (strncmp(message, st.text, strlen(st.text)) == 0 && st.percent > g_update.progress.load())
                g_update.progress = st.percent;
        }
    });
    OSReport("Ufin: update check: starting the worker thread\n");
    if (!OSCreateThread(&g_update.thread, updateWorkerMain, 0, nullptr, g_update.stack + sizeof(g_update.stack),
                        (uint32_t)sizeof(g_update.stack), 20, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        g_update.state = 0;
        OSReport("Ufin: update check: could not create the thread\n");
        return false;
    }
    OSResumeThread(&g_update.thread);
    return true;
}

// Settings > Check for updates: shows the "Checking" card while the worker runs; B cancels.
static updatecheck::Outcome runUpdateCheck(const std::string& title) {
    updatecheck::Outcome outcome;
    outcome.kind = updatecheck::Outcome::Kind::Unknown;
    if (!startUpdateCheck()) {
        outcome.detail = "a check is already running; try again in a moment";
        return outcome;
    }
    ui::MessageModel busy;
    busy.title = title;
    busy.lines = {std::string("Your version: ") + ufin::kVersion, "", "Asking GitHub (B to cancel)."};
    busy.hints = {{"B", "Cancel"}};
    busy.busy = true;
    RepeatState rs;
    while (WHBProcIsRunning()) {
        const input::State in = input::read();
        if (withRepeat(in, rs) & VPAD_BUTTON_B) {
            g_update.cancel = true;
            outcome.detail = "cancelled";
            return outcome;
        }
        if (takeUpdateResult(outcome)) return outcome;
        drawFrame([&] { ui::drawMessage(busy); });
    }
    g_update.cancel = true;
    outcome.detail = "cancelled";
    return outcome;
}

// Up / down scrolls a card whose text is longer than it (the changelog); `pressed` is withRepeat()'s result.
static void scrollCard(ui::MessageModel& m, uint32_t pressed) {
    const int total = (int)m.lines.size();
    if ((pressed & BTN_UP) && m.firstLine > 0) m.firstLine--;
    if ((pressed & BTN_DOWN) && m.firstLine + m.linesShown < total) m.firstLine++;
}

// A card over the whole screen until A, B or a tap.
static void showCardUntilDismissed(const std::string& title, const std::vector<std::string>& lines, bool isError) {
    ui::MessageModel m;
    m.title = title;
    m.lines = lines;
    m.isError = isError;
    m.hints = {{"A", "Continue"}, {"Up Down", "Scroll"}};
    RepeatState rs;
    bool wasTouched = true; // a touch still held from before does not count
    while (WHBProcIsRunning()) {
        const input::State in = input::read();
        const uint32_t p = withRepeat(in, rs);
        const bool tapped = in.touched && !wasTouched;
        wasTouched = in.touched;
        if (tapped || (p & (VPAD_BUTTON_A | VPAD_BUTTON_B))) return;
        scrollCard(m, p);
        drawFrame([&] { ui::drawMessage(m); });
    }
}

// A card that asks a yes / no question and waits: true for A, false for B (or a tap on nothing in particular is
// ignored: a certificate is not something to trust by accident).
static bool confirmCard(const std::string& title, const std::vector<std::string>& lines, const char* yesLabel, bool isError) {
    ui::MessageModel m;
    m.title = title;
    m.lines = lines;
    m.isError = isError;
    m.hints = {{"A", yesLabel}, {"B", "Cancel"}, {"Up Down", "Scroll"}};
    RepeatState rs;
    while (WHBProcIsRunning()) {
        const input::State in = input::read();
        const uint32_t p = withRepeat(in, rs);
        if (p & VPAD_BUTTON_A) return true;
        if (p & VPAD_BUTTON_B) return false;
        scrollCard(m, p);
        drawFrame([&] { ui::drawMessage(m); });
    }
    return false;
}

// The https servers saved in config.json (and which certificate each one is pinned to) -> the connection layer.
static void registerTlsServers(const UfinConfig& cfg) {
    for (const TlsServer& s : cfg.tlsServers) net::setServerTls(s.host, s.port, true, s.pin);
}

// "Trust this certificate?": what the server showed, its fingerprint, and why we are asking. A for yes.
static bool askToTrustCertificate(const std::string& address, const tlsconn::CertInfo& info, bool changed) {
    std::vector<std::string> lines;
    if (changed) {
        lines.push_back("This server now shows a DIFFERENT certificate than the one you trusted before.");
        lines.push_back("That is normal if you renewed or replaced it; if you did not, someone may be intercepting the connection.");
    } else {
        lines.push_back("This server's certificate was not issued by an authority this console knows (a self-signed certificate is the usual reason).");
        lines.push_back("If it is your own server, you can trust this exact certificate. Ufin then accepts only it for this address.");
    }
    lines.push_back("");
    lines.push_back("Server: " + address);
    if (!info.subject.empty()) lines.push_back("Name: " + info.subject);
    if (!info.issuer.empty() && info.issuer != info.subject) lines.push_back("Issued by: " + info.issuer);
    if (!info.validTo.empty()) lines.push_back("Valid until: " + info.validTo);
    if (!info.problem.empty()) lines.push_back("Problem: " + info.problem);
    lines.push_back("");
    lines.push_back("SHA-256 fingerprint (compare it with your server's certificate):");
    const std::string fp = tlsconn::formatFingerprint(info.fingerprint); // 32 pairs
    lines.push_back(fp.substr(0, 47));
    lines.push_back(fp.size() > 48 ? fp.substr(48) : std::string());
    return confirmCard(changed ? "Certificate changed" : "Trust this server?", lines, "Trust it", changed);
}

// Settings > Check for updates: the same check, run on demand, and always answered with a card -- what
// this build is, what GitHub has, and whether to update, you are ahead of the release, or it failed.
// B cancels while it waits.
static void checkForUpdatesNow() {
    const updatecheck::Outcome outcome = runUpdateCheck("Checking for updates");
    if (outcome.detail == "cancelled") return; // B: back to Settings, nothing to say
    const updatecheck::Card card = updatecheck::resultCard(outcome, ufin::kVersion);
    showCardUntilDismissed(card.title, card.lines, card.isError);
}

enum class Section { Home, LiveTv, Search, UpNext, Settings, Help };
enum class Mode { SignIn, QuickConnect, Browse, Profiles }; // Profiles = "Who's watching?"

struct App {
    JellyfinClient& client;
    UfinConfig cfg;
    Mode mode = Mode::SignIn;
    std::vector<Frame> stack;
    bool loggedIn = false;
    std::string errorMessage;      // non-empty = error card over everything
    bool errorOffersSignIn = false; // error card: X goes to the sign-in screen

    bool sidebarFocused = false;
    int sidebarCursor = 0;
    std::string liveTvViewName;    // set if the server has a Live TV view

    // Sign-in screen
    std::string loginServer, loginUser, loginPassword;
    int loginFocus = 0;
    std::string loginMessage;
    bool loginMessageIsError = false;
    // Quick Connect
    JellyfinClient::QuickConnectRequest quick;
    OSTime quickLastPoll = 0;
    std::string quickMessage;
    bool quickMessageIsError = false;

    // Menu music, the CRT easter egg, and short notices
    MenuMusic music;
    mutable std::string localMusicPath; // the file localMusicPresent() found
    mutable int localMusicState = -1; // -1 unknown, 0 no music file, 1 present (a cache; see localMusicPresent)
    bool localMusicFailed = false; // the music file exists but could not be decoded
    bool musicDeviceFailed = false;
    int aboutTaps = 0; // seven unlock the Rainbow accent
    std::string toast;
    OSTime toastAt = 0;

    // "Who's watching?": one picture per saved profile (cfg.profiles) plus an "Add profile" tile.
    mutable std::vector<ui::Bounce> bounces; // one per tile; only the cursor's tile bounces
    mutable OSTime bounceLast = 0;
    int profileCursor = 0;       // the hovered tile
    int profileRow = 0;          // 0 = the picture, 1 = its "Auto log in" box, 2 = its "Log out" button
    int profileRemoveAsk = -1;   // X was pressed once on this profile (a second press removes it)
    int connectingProfile = -1;  // the profile a sign-in attempt is for (Retry uses it)
    bool profilesFromApp = false; // opened from Settings: B goes back to the library

    explicit App(JellyfinClient& c) : client(c) {}

    void say(const std::string& text) {
        toast = text;
        toastAt = OSGetTime();
    }

    bool toastActive() const { return !toast.empty() && secondsSince(toastAt) < 2.5; }

    void saveConfig() {
        cfg.usageSeconds = g_usage.totalSeconds(monotonicSeconds()); // every save carries the latest total
        std::string err;
        if (!saveConfigToSD(cfg, err)) OSReport("Ufin: saving config.json failed: %s\n", err.c_str());
    }

    // --- sections ---

    std::vector<Section> sections() const {
        std::vector<Section> s = {Section::Home};
        if (!liveTvViewName.empty()) s.push_back(Section::LiveTv);
        s.push_back(Section::Search);
        s.push_back(Section::UpNext);
        s.push_back(Section::Settings);
        s.push_back(Section::Help);
        return s;
    }

    Section currentSection() const {
        if (stack.back().kind == Frame::Kind::Search) return Section::Search;
        if (stack.back().kind == Frame::Kind::Settings) return Section::Settings;
        for (const Frame& f : stack) if (f.kind == Frame::Kind::LiveTv) return Section::LiveTv;
        return Section::Home;
    }

    void findLiveTv() {
        liveTvViewName.clear();
        for (const JellyfinItem& v : stack[0].items) {
            if (isLiveTvView(v)) liveTvViewName = v.name;
        }
    }

    // --- signing in ---

    // How a server is shown (and typed back): with https:// when it is one of the https servers.
    std::string serverText(const std::string& host, int port) const {
        return formatServerAddress(host, port, findTlsServer(cfg.tlsServers, host, port) != nullptr);
    }

    // After any successful sign-in: remember the token, load the libraries.
    void enterLibrary() {
        cfg.host = client.host();
        cfg.port = client.port();
        cfg.userId = client.userId();
        cfg.accessToken = client.accessToken();
        if (!client.userName().empty()) cfg.username = client.userName();
        {   // this sign-in is a profile (a new one, or the same user's again with a fresh token)
            Profile p;
            p.name = cfg.username.empty() ? std::string("User") : cfg.username;
            p.host = cfg.host;
            p.port = cfg.port;
            p.userId = cfg.userId;
            p.accessToken = cfg.accessToken;
            const int at = profiles::upsert(cfg.profiles, p);
            if (at >= 0) cfg.lastProfile = at;
        }
        saveConfig();
        loggedIn = true;
        profilesFromApp = false;
        mode = Mode::Browse;
        stack.resize(1);
        stack[0] = Frame();
        stack[0].kind = Frame::Kind::Views;
        stack[0].title = "Libraries";
        sidebarFocused = false;
        showBusy("Loading libraries");
        if (!loadFrame(client, stack[0])) {
            errorMessage = client.lastError();
            return;
        }
        findLiveTv();
    }

    void showSignIn(const std::string& message = "", bool isError = false) {
        stopMusic();
        mode = Mode::SignIn;
        loggedIn = false;
        loginServer = serverText(cfg.host, cfg.port);
        loginUser = cfg.username;
        loginPassword.clear();
        loginFocus = loginServer.empty() ? 0 : (loginUser.empty() ? 1 : 2);
        loginMessage = message;
        loginMessageIsError = isError;
    }

    // At start-up: saved token, else a password from config.json, else
    // the sign-in screen.
    void startUp() {
        // An older Ufin kept a single sign-in: it becomes the first profile (not "kept", so
        // Who's watching shows up, as it does on every start-up without a ticked profile).
        if (cfg.profiles.empty() && !cfg.accessToken.empty() && !cfg.userId.empty() && !cfg.host.empty() && cfg.port > 0) {
            Profile p;
            p.name = cfg.username.empty() ? std::string("User") : cfg.username;
            p.host = cfg.host;
            p.port = cfg.port;
            p.userId = cfg.userId;
            p.accessToken = cfg.accessToken;
            cfg.lastProfile = profiles::upsert(cfg.profiles, p);
            saveConfig();
        }
        if (!cfg.profiles.empty()) {
            const int auto_ = profiles::autoLoginIndex(cfg.profiles, cfg.lastProfile);
            if (auto_ >= 0) selectProfile(auto_);
            else showProfiles(false);
            return;
        }
        if (!hasLogin(cfg)) {
            showSignIn();
            return;
        }
        client.setServer(cfg.host, cfg.port);
        showBusy("Connecting", {serverText(cfg.host, cfg.port)});
        bool ok = false;
        if (!cfg.accessToken.empty()) {
            client.useSavedLogin(cfg.userId, cfg.accessToken);
            ok = client.validateLogin();
            if (!ok && client.lastError().find("expired") != std::string::npos) {
                cfg.accessToken.clear();
                saveConfig();
                showSignIn("Your saved sign-in has expired. Please sign in again.", true);
                return;
            }
        } else {
            ok = client.authenticate(cfg.username, cfg.password);
        }
        if (ok) {
            enterLibrary();
            return;
        }
        errorMessage = client.lastError();
        errorOffersSignIn = true;
    }

    // --- Who's watching? ---

    int tileCount() const {
        return (int)cfg.profiles.size() + (cfg.profiles.size() < profiles::kMax ? 1 : 0);
    }
    bool isAddTile(int i) const { return i >= (int)cfg.profiles.size(); }

    void showProfiles(bool fromApp) {
        stopMusic();
        mode = Mode::Profiles;
        profilesFromApp = fromApp && loggedIn;
        profileCursor = cfg.lastProfile >= 0 && cfg.lastProfile < (int)cfg.profiles.size() ? cfg.lastProfile : 0;
        profileRow = 0;
        profileRemoveAsk = -1;
        bounces.assign((size_t)tileCount(), ui::Bounce());
        bounceLast = OSGetTime();
    }

    // The sign-in screen for one server / user (a profile whose saved sign-in ran out, or a new one).
    void showSignInFor(const std::string& host, int port, const std::string& user, const std::string& message,
                       bool isError) {
        stopMusic();
        mode = Mode::SignIn;
        loggedIn = false;
        loginServer = serverText(host, port);
        loginUser = user;
        loginPassword.clear();
        loginFocus = loginServer.empty() ? 0 : (loginUser.empty() ? 1 : 2);
        loginMessage = message;
        loginMessageIsError = isError;
    }

    // Signs in as saved profile `i`: its saved token, checked with the server. A profile whose sign-in
    // has run out goes to the sign-in screen with its server and name filled in.
    void selectProfile(int i) {
        if (i < 0 || i >= (int)cfg.profiles.size()) return;
        connectingProfile = i;
        Profile& p = cfg.profiles[(size_t)i];
        if (p.accessToken.empty() || p.host.empty() || p.port <= 0) {
            showSignInFor(p.host.empty() ? cfg.host : p.host, p.host.empty() ? cfg.port : p.port, p.name,
                          "Sign in again to use this profile.", false);
            return;
        }
        client.setServer(p.host, p.port);
        showBusy("Connecting", {p.name, serverText(p.host, p.port)});
        client.useSavedLogin(p.userId, p.accessToken);
        if (client.validateLogin()) {
            enterLibrary(); // also marks this profile as the one used last
            return;
        }
        profilesFromApp = false;
        loggedIn = false;
        if (client.lastError().find("expired") != std::string::npos) {
            p.accessToken.clear();
            saveConfig();
            showSignInFor(p.host, p.port, p.name, "This profile's saved sign-in has expired. Please sign in again.", true);
            return;
        }
        mode = Mode::Profiles;
        errorMessage = client.lastError();
        errorOffersSignIn = true;
    }

    // "Auto log in": only one profile can have it, so ticking one unticks the others.
    void toggleKeepLoggedIn(int i) {
        if (i < 0 || i >= (int)cfg.profiles.size()) return;
        profiles::setAutoLogin(cfg.profiles, i, !cfg.profiles[(size_t)i].keepLoggedIn);
        saveConfig();
    }

    // "Log out" under a profile: forgets its saved sign-in (the profile stays, and signs in again with
    // the sign-in screen). Logging out the profile you are signed in as also signs you out of the server.
    void logOutProfile(int i) {
        if (i < 0 || i >= (int)cfg.profiles.size()) return;
        Profile& p = cfg.profiles[(size_t)i];
        if (p.accessToken.empty()) return; // already logged out
        const bool isCurrent = loggedIn && !client.userId().empty() && p.userId == client.userId();
        const bool mirrored = (cfg.accessToken == p.accessToken); // config.json's single sign-in copy
        profiles::logOut(cfg.profiles, i);
        if (isCurrent) {
            client.signOut();
            loggedIn = false;
            profilesFromApp = false; // nothing to go back to
        }
        if (mirrored || isCurrent) {
            cfg.accessToken.clear();
            cfg.userId.clear();
        }
        saveConfig();
        profileRemoveAsk = -1;
    }

    void removeProfile(int i) {
        if (i < 0 || i >= (int)cfg.profiles.size()) return;
        const bool wasCurrent = (!client.userId().empty() && cfg.profiles[(size_t)i].userId == client.userId());
        cfg.lastProfile = profiles::remove(cfg.profiles, i, cfg.lastProfile);
        if (wasCurrent) { // removing the profile you are signed in as signs it out of the server too
            client.signOut();
            loggedIn = false;
            profilesFromApp = false;
            cfg.accessToken.clear();
            cfg.userId.clear();
        }
        saveConfig();
        profileRemoveAsk = -1;
        if (cfg.profiles.empty()) { showSignIn(); return; }
        profileCursor = std::min(profileCursor, tileCount() - 1);
        profileRow = 0;
        bounces.assign((size_t)tileCount(), ui::Bounce());
    }

    void profilesInput(uint32_t pressed, bool tapped, float tapX, float tapY) {
        const int n = tileCount();
        if (tapped) {
            const ui::ProfilesHit hit = ui::hitTestProfiles(profilesModel(false), tapX, tapY);
            profileRemoveAsk = -1;
            if (hit.kind == ui::ProfilesHit::Check) {
                profileCursor = hit.index;
                toggleKeepLoggedIn(hit.index);
            } else if (hit.kind == ui::ProfilesHit::Logout) {
                profileCursor = hit.index;
                profileRow = 2;
                logOutProfile(hit.index);
            } else if (hit.kind == ui::ProfilesHit::Picture) {
                profileCursor = hit.index;
                profileRow = 0;
                activateProfileTile(hit.index);
            }
            return;
        }
        auto move = [&](int by) {
            profileCursor = (profileCursor + by + n) % n;
            profileRow = 0;
            profileRemoveAsk = -1;
        };
        if (pressed & (BTN_LEFT | VPAD_BUTTON_L | VPAD_BUTTON_ZL)) move(-1);
        else if (pressed & (BTN_RIGHT | VPAD_BUTTON_R | VPAD_BUTTON_ZR)) move(1);
        else if (pressed & BTN_DOWN) { if (!isAddTile(profileCursor) && profileRow < 2) profileRow++; }
        else if (pressed & BTN_UP) { if (profileRow > 0) profileRow--; }
        else if (pressed & VPAD_BUTTON_A) {
            profileRemoveAsk = -1;
            if (profileRow == 1 && !isAddTile(profileCursor)) toggleKeepLoggedIn(profileCursor);
            else if (profileRow == 2 && !isAddTile(profileCursor)) logOutProfile(profileCursor);
            else activateProfileTile(profileCursor);
        } else if (pressed & VPAD_BUTTON_Y) {
            profileRemoveAsk = -1;
            if (!isAddTile(profileCursor)) toggleKeepLoggedIn(profileCursor);
        } else if (pressed & VPAD_BUTTON_X) {
            if (isAddTile(profileCursor)) return;
            if (profileRemoveAsk == profileCursor) removeProfile(profileCursor);
            else profileRemoveAsk = profileCursor;
        } else if (pressed & VPAD_BUTTON_B) {
            if (profileRemoveAsk >= 0) profileRemoveAsk = -1;
            else if (profilesFromApp && loggedIn) { mode = Mode::Browse; profilesFromApp = false; }
        }
    }

    void activateProfileTile(int i) {
        if (isAddTile(i)) { // a new profile: the sign-in screen, with the server filled in
            const Profile* ref = cfg.lastProfile >= 0 && cfg.lastProfile < (int)cfg.profiles.size()
                                     ? &cfg.profiles[(size_t)cfg.lastProfile] : nullptr;
            showSignInFor(ref ? ref->host : cfg.host, ref ? ref->port : cfg.port, "", "", false);
        } else {
            selectProfile(i);
        }
    }

    // What drawProfiles needs. `advance` also steps the bounce animations by the time since the last
    // call (once per drawn frame); a hit test passes false.
    ui::ProfilesModel profilesModel(bool advance) const {
        ui::ProfilesModel m;
        const int n = tileCount();
        if ((int)bounces.size() != n) bounces.assign((size_t)n, ui::Bounce());
        float dt = 0.0f;
        if (advance) {
            const OSTime now = OSGetTime();
            dt = (float)OSTicksToMilliseconds(now - bounceLast) / 1000.0f;
            bounceLast = now;
        }
        for (int i = 0; i < n; i++) {
            ui::ProfileTile t;
            if (isAddTile(i)) {
                t.add = true;
                t.name = "Add profile";
            } else {
                const Profile& p = cfg.profiles[(size_t)i];
                t.name = p.name;
                t.keep = p.keepLoggedIn;
                t.loggedOut = p.accessToken.empty();
                t.color = ui::profileColor(p.color);
                t.letter = profiles::initial(p.name);
                // The user's Jellyfin picture, if the server has one (else the letter stays). Fetched like
                // any artwork; only from the server the app is talking to.
                if (g_images && g_imageClient && !p.userId.empty() && !p.host.empty()) {
                    // Each profile's picture comes from its own server (which may differ from the one signed in to).
                    const bool same = p.host == client.host() && p.port == client.port();
                    const std::string userPath = "/Users/" + p.userId + "/Images/Primary?maxWidth=256&maxHeight=256&quality=90&format=Jpg";
                    t.picture = g_images->get(same ? userPath : "//" + p.host + ":" + std::to_string(p.port) + userPath,
                                              256, 256, &t.pictureAspect);
                }
            }
            if (advance) bounces[(size_t)i].step(dt, i == profileCursor && errorMessage.empty());
            t.bounce = bounces[(size_t)i].shape();
            m.tiles.push_back(t);
        }
        m.cursor = profileCursor;
        m.focusRow = profileRow;
        m.hints = {{"< >", "Move"}, {"A", profileRow == 1 ? "Tick" : profileRow == 2 ? "Log out" : "Select"}, {"Y", "Auto log in"}};
        if (!cfg.profiles.empty() && !isAddTile(profileCursor)) m.hints.push_back({"X", "Remove"});
        if (profilesFromApp) m.hints.push_back({"B", "Back"});
        if (profileRemoveAsk >= 0 && profileRemoveAsk < (int)cfg.profiles.size()) {
            m.message = "Press X again to remove " + cfg.profiles[(size_t)profileRemoveAsk].name + " from this console. B cancels.";
            m.messageIsError = true;
        }
        return m;
    }

    enum LoginRow { RowServer, RowUser, RowPassword, RowSignIn, RowQuickConnect, LOGIN_ROWS };

    bool applyServerField() {
        std::string host, err;
        int port = 0;
        bool https = false;
        if (!parseServerAddress(loginServer, host, port, https, err)) {
            loginMessage = err;
            loginMessageIsError = true;
            loginFocus = RowServer;
            return false;
        }
        if (!https) {
            // plain http, as ever: forget any https entry this address had
            setTlsServer(cfg.tlsServers, host, port, false, "");
            net::setServerTls(host, port, false, "");
            client.setServer(host, port);
            return true;
        }
        // https: look at the certificate first (report-only: nothing is sent), and ask only when it is one we
        // would not accept on our own.
        const TlsServer* known = findTlsServer(cfg.tlsServers, host, port);
        const std::string oldPin = known ? known->pin : std::string();
        showBusy("Checking the server's certificate", {loginServer});
        tlsconn::CertInfo info;
        std::string probeError;
        if (!net::probeCertificate(host, port, 10, info, probeError)) {
            loginMessage = "Can't make a secure connection to " + loginServer + ": " + probeError +
                           ". (Is it an https port? Jellyfin's own is usually 8920.)";
            loginMessageIsError = true;
            loginFocus = RowServer;
            return false;
        }
        std::string pin = oldPin;
        if (info.trusted) {
            pin.clear(); // a real certificate: no pin needed (and an old one would only get in the way)
        } else if (!(pin.size() == 64 && pin == info.fingerprint)) {
            if (!askToTrustCertificate(loginServer, info, /*changed=*/!oldPin.empty())) {
                loginMessage = "Not trusted -- sign-in cancelled. Use the server's http address, or trust its certificate.";
                loginMessageIsError = true;
                loginFocus = RowServer;
                return false;
            }
            pin = info.fingerprint;
        }
        setTlsServer(cfg.tlsServers, host, port, true, pin);
        net::setServerTls(host, port, true, pin);
        saveConfig();
        client.setServer(host, port);
        return true;
    }

    void activateLoginRow(int row) {
        std::string value, kbError;
        switch (row) {
            case RowServer:
                if (ui::promptKeyboard(u"Server address, e.g. 192.168.1.100:8096", loginServer, false, true, value, kbError)) {
                    loginServer = value;
                    loginFocus = RowUser;
                }
                break;
            case RowUser:
                if (ui::promptKeyboard(u"Jellyfin username", loginUser, false, true, value, kbError)) {
                    loginUser = value;
                    loginFocus = RowPassword;
                }
                break;
            case RowPassword:
                if (ui::promptKeyboard(u"Password", "", true, true, value, kbError)) {
                    loginPassword = value;
                    loginFocus = RowSignIn;
                }
                break;
            case RowSignIn: {
                if (!applyServerField()) return;
                if (loginUser.empty()) {
                    loginMessage = "Enter your username (or use Quick Connect).";
                    loginMessageIsError = true;
                    loginFocus = RowUser;
                    return;
                }
                showBusy("Signing in", {loginUser + " @ " + loginServer});
                if (client.authenticate(loginUser, loginPassword)) {
                    loginPassword.clear();
                    enterLibrary();
                } else {
                    const std::string& e = client.lastError();
                    loginMessage = e.find("status 401") != std::string::npos ? "Wrong username or password."
                                 : e.find("status 0") != std::string::npos
                                     ? "Can't reach " + loginServer + ". Check the address and that Jellyfin is running."
                                     : e;
                    loginMessageIsError = true;
                }
                break;
            }
            case RowQuickConnect:
                if (!applyServerField()) return;
                showBusy("Starting Quick Connect", {loginServer});
                if (client.quickConnectStart(quick)) {
                    mode = Mode::QuickConnect;
                    quickLastPoll = OSGetTime();
                    quickMessage = "Waiting for approval";
                    quickMessageIsError = false;
                } else {
                    loginMessage = client.lastError();
                    loginMessageIsError = true;
                }
                break;
        }
        if (!kbError.empty()) {
            loginMessage = "Keyboard: " + kbError;
            loginMessageIsError = true;
        }
    }

    void signInInput(uint32_t pressed, bool tapped, float tapX, float tapY) {
        if (tapped) {
            int row = ui::hitTestLogin(tapX, tapY, LOGIN_ROWS);
            if (row >= 0) {
                loginFocus = row;
                activateLoginRow(row);
            }
            return;
        }
        if ((pressed & VPAD_BUTTON_B) && !cfg.profiles.empty()) { showProfiles(false); return; }
        if (pressed & BTN_DOWN) loginFocus = (loginFocus + 1) % LOGIN_ROWS;
        else if (pressed & BTN_UP) loginFocus = (loginFocus - 1 + LOGIN_ROWS) % LOGIN_ROWS;
        else if (pressed & VPAD_BUTTON_A) activateLoginRow(loginFocus);
        else if (pressed & VPAD_BUTTON_X) activateLoginRow(RowQuickConnect);
    }

    // Called every frame while the Quick Connect code is on screen.
    void pollQuickConnect() {
        if (mode != Mode::QuickConnect || quickMessageIsError) return;
        if (secondsSince(quickLastPoll) < 2.0) return;
        quickLastPoll = OSGetTime();
        if (client.quickConnectApproved(quick.secret)) {
            showBusy("Signing in");
            if (client.quickConnectFinish(quick.secret)) {
                enterLibrary();
            } else {
                quickMessage = client.lastError();
                quickMessageIsError = true;
            }
        } else if (!client.lastError().empty()) {
            quickMessage = client.lastError();
            quickMessageIsError = true;
        }
    }

    // --- menu music ---

    // Music source: a library song the user picked (press - on it) wins; otherwise the local
    // default in sd:/wiiu/apps/ufin/assets/ (bgm.m4a, or bgm.mp3), if one is there.
    bool localMusicPresent() const {
        if (localMusicState < 0) {
            localMusicPath = findLocalMenuMusic();
            localMusicState = localMusicPath.empty() ? 0 : 1;
        }
        return localMusicState == 1;
    }
    bool musicSourceAvailable() const {
        return !cfg.menuMusicItemId.empty() || (localMusicPresent() && !localMusicFailed);
    }

    bool musicWanted() const {
        return mode == Mode::Browse && loggedIn && errorMessage.empty() && cfg.menuMusicEnabled && musicSourceAvailable();
    }

    void stopMusic() { music.stop(); }

    // --- Help window (sidebar > Help): what every button does, one page per controller ---
    bool helpOpen = false;
    int helpPage = 0;   // 0 GamePad, 1 Pro Controller, 2 Classic Controller, 3 Wii Remote
    int helpCursor = 0; // selected row of that page
    mutable int helpFrame = 0; // animation clock for the rotating model (drawn 60 times a second)

    void openHelp() {
        helpOpen = true;
        helpFrame = 0;
        helpCursor = help::firstEntry(help::rowsFor(help::controllerAt(helpPage)));
    }

    ui::HelpModel buildHelpModel() const {
        ui::HelpModel hm;
        for (int i = 0; i < help::kControllerCount; i++) hm.tabs.push_back(help::controllerName(help::controllerAt(i)));
        hm.tab = helpPage;
        const help::Controller c = help::controllerAt(helpPage);
        hm.name = help::controllerName(c);
        hm.note = help::controllerNote(c);
        for (const help::Row& r : help::rowsFor(c)) {
            ui::HelpRow row;
            row.header = r.header;
            row.button = r.button;
            row.action = r.action;
            row.highlight = r.highlight;
            hm.rows.push_back(row);
        }
        hm.cursor = helpCursor;
        hm.hints = {{"< >", "Controller"}, {"Up Down", "Scroll"}, {"B", "Close"}};
        hm.frame = helpFrame;
        return hm;
    }

    void setHelpPage(int page) {
        helpPage = ((page % help::kControllerCount) + help::kControllerCount) % help::kControllerCount;
        helpCursor = help::firstEntry(help::rowsFor(help::controllerAt(helpPage)));
    }

    // While the window is open it owns all input (nothing reaches the screen behind it).
    void handleHelpInput(uint32_t pressed, bool tapped, float tapX, float tapY) {
        if (pressed & VPAD_BUTTON_B) { helpOpen = false; return; }
        if (pressed & (VPAD_BUTTON_L | VPAD_BUTTON_ZL | BTN_LEFT)) { setHelpPage(helpPage - 1); return; }
        if (pressed & (VPAD_BUTTON_R | VPAD_BUTTON_ZR | BTN_RIGHT)) { setHelpPage(helpPage + 1); return; }
        const std::vector<help::Row> rows = help::rowsFor(help::controllerAt(helpPage));
        if (pressed & BTN_DOWN) helpCursor = help::stepEntry(rows, helpCursor, +1);
        else if (pressed & BTN_UP) helpCursor = help::stepEntry(rows, helpCursor, -1);
        if (tapped) {
            const ui::HelpHit hit = ui::hitTestHelp(buildHelpModel(), tapX, tapY);
            if (hit.kind == ui::HelpHit::Close) helpOpen = false;
            else if (hit.kind == ui::HelpHit::Tab) setHelpPage(hit.index);
            else if (hit.kind == ui::HelpHit::Row) helpCursor = hit.index;
        }
    }

    // --- Jellyfin backdrops ---
    BackdropTracker backdropTracker;
    TapCounter usageTaps{5, 1.5}; // five quick taps on the sidebar counter -> celebration

    // The backdrop the UI wants behind it right now: the selected title's own, else the page's
    // (a series page behind its seasons), else none -- then the normal background (the static
    // one, or the animated one chosen in Settings) shows.
    std::string backdropWanted() const {
        if (!cfg.jellyfinBackdrops || mode != Mode::Browse || !loggedIn || !errorMessage.empty() || stack.empty())
            return std::string();
        const Frame& f = stack.back();
        if (f.kind == Frame::Kind::Settings) return std::string();
        if (!sidebarFocused && f.selected >= 0 && f.selected < (int)f.items.size()) {
            const JellyfinItem& it = f.items[(size_t)f.selected];
            if (!it.backdropTag.empty())
                return client.buildBackdropPath(it.backdropItemId, it.backdropTag, BACKDROP_W, BACKDROP_H);
        }
        if (!f.backdropTag.empty())
            return client.buildBackdropPath(f.backdropItemId, f.backdropTag, BACKDROP_W, BACKDROP_H);
        return std::string();
    }

    // Every frame, before drawing (like updateMusic): picks the backdrop, loads it, and hands the
    // fading layers to the UI.
    void updateBackdrop() {
        const double now = (double)OSTicksToMilliseconds(OSGetTime()) / 1000.0;
        float curAspect = 0.0f, prevAspect = 0.0f;
        ui::Texture cur = 0, prev = 0;
        bool ready = false;
        if (g_backdrops && !backdropTracker.curPath().empty())
            ready = g_backdrops->get(backdropTracker.curPath(), BACKDROP_W, BACKDROP_H, &curAspect) != 0;
        backdropTracker.update(backdropWanted(), ready, now);
        if (g_backdrops) {
            // Asking every frame keeps both pictures alive in the cache while they cross-fade.
            if (!backdropTracker.curPath().empty())
                cur = g_backdrops->get(backdropTracker.curPath(), BACKDROP_W, BACKDROP_H, &curAspect);
            if (!backdropTracker.prevPath().empty())
                prev = g_backdrops->get(backdropTracker.prevPath(), BACKDROP_W, BACKDROP_H, &prevAspect);
        }
        ui::setScreenBackdrop(prev, prevAspect, cur, curAspect, cur ? backdropTracker.curAlpha() : 0.0f);
    }

    // Every frame: start or stop the music to match the settings/screen.
    // Main thread only: MenuMusic opens and closes the audio device on
    // the calling thread (AX must be shut down from core 1).
    void updateMusic() {
        if (music.running() && music.gaveUp()) {
            // The local file could not be decoded: free the audio device and do not retry.
            music.stop();
            localMusicFailed = true;
            say("The menu music file could not be played");
        }
        if (musicWanted() && !music.running() && !musicDeviceFailed) {
            bool ok;
            if (!cfg.menuMusicItemId.empty()) {
                const std::string itemId = cfg.menuMusicItemId;
                JellyfinClient* c = &client;
                ok = music.start(client.host(), client.port(), [c, itemId] {
                    static uint32_t n = 0;
                    return c->buildAudioStreamUrl(itemId, 0, makePlaySessionId((uint64_t)OSGetTime(), ++n)).path;
                });
            } else {
                ok = music.startFile(localMusicPath);
            }
            if (!ok) {
                musicDeviceFailed = true; // don't retry every frame
                OSReport("Ufin: menu music: couldn't open the audio device\n");
            }
        } else if (!musicWanted() && music.running()) {
            music.stop();
        }
    }

    void chooseMenuMusic(const JellyfinItem& item) {
        if (item.type != "Audio") {
            say("Select a song, then press - to use it as menu music");
            return;
        }
        stopMusic();
        cfg.menuMusicItemId = item.id;
        cfg.menuMusicName = itemDisplayName(item);
        cfg.menuMusicEnabled = true;
        saveConfig();
        say("Menu music: " + item.name);
    }

    // --- settings ---

    enum SettingsRow { SetMusic, SetMusicDefault, SetCrt, SetAccent, SetUnlockRainbow, SetLight, SetBackground, SetBackdrops, SetStyle, SetSnow, SetClock, SetLogging,
                       SetQuality, SetH264Profile, SetAutoplay, SetGamepadScreen, SetDefaultTracks, SetStrictVideoGeometry, SetTestPicture, SetAccount,
                       SetAutoUpdates, SetCheckUpdates, SetAbout,
                       // Customization page: the custom palette editor and the artwork-reactive accent
                       SetCustomPalette, SetCustomColours, SetPreview, SetHue, SetSat, SetVal, SetRed, SetGreen, SetBlue, SetHex,
                       SetArrange, SetAutoColours, SetArtwork,
                       // ... and the M3E button shapes
                       SetShapes, SetShapePlay, SetShapeSeek, SetShapeToggle, SetShapeDialog,
                       // ... their sizes (width, height per kind), the live button preview and "Reset defaults"
                       SetSizePlayW, SetSizePlayH, SetSizeSeekW, SetSizeSeekH, SetSizeToggleW, SetSizeToggleH, SetSizeDialogW, SetSizeDialogH,
                       SetShapePreview, SetShapesReset };

    // Settings has two pages (L / R or the tabs switch them): Customization (everything about how Ufin looks,
    // the page Settings opens on) and Settings (everything else). Each remembers its own selection.
    static constexpr int kSettingsPages = 2;
    int setPage = 0;
    int setSel[kSettingsPages] = {0, 0};
    int customSlot = 0;                       // which of the palette's three colours the editor shows
    mutable customize::HueMemory hueMem;      // the hue a grey colour had before it went grey
    bool paletteDirty = false;                // palette edits are applied at once, saved a moment after the last one
    OSTime paletteEditAt = 0;
    OSTime heldSince = 0, heldLastFire = 0;   // d-pad / touch repeat on the editor's sliders

    static const char* settingsPageName(int page) { return page == 0 ? "Customization" : "Settings"; }

    std::vector<SettingsRow> settingsRows() const {
        std::vector<SettingsRow> rows;
        if (setPage == 0) {
            rows = {SetStyle, SetLight, SetAccent};
            if (cfg.rainbowEasterEgg) rows.push_back(SetUnlockRainbow);
            rows.push_back(SetCustomPalette);
            if (cfg.customPalette)
                rows.insert(rows.end(), {SetCustomColours, SetPreview, SetHue, SetSat, SetVal, SetRed, SetGreen, SetBlue, SetHex,
                                         SetArrange, SetAutoColours});
            rows.push_back(SetArtwork);
            rows.push_back(SetShapes);
            if (cfg.customShapes && cfg.m3Expressive)
                rows.insert(rows.end(), {SetShapePreview,
                                         SetShapePlay, SetSizePlayW, SetSizePlayH, SetShapeSeek, SetSizeSeekW, SetSizeSeekH,
                                         SetShapeToggle, SetSizeToggleW, SetSizeToggleH, SetShapeDialog, SetSizeDialogW, SetSizeDialogH,
                                         SetShapesReset});
            rows.insert(rows.end(), {SetBackground, SetBackdrops, SetSnow, SetCrt, SetClock});
        } else {
            rows = {SetMusic, SetMusicDefault, SetAutoplay, SetDefaultTracks, SetGamepadScreen, SetStrictVideoGeometry, SetQuality, SetH264Profile,
                    SetLogging, SetTestPicture, SetAccount, SetAutoUpdates, SetCheckUpdates, SetAbout};
        }
        return rows;
    }

    static bool isShapeRow(SettingsRow r) { return r >= SetShapePlay && r <= SetShapeDialog; }
    static int shapeKindOf(SettingsRow r) { return (int)(r - SetShapePlay); }
    static bool isSizeRow(SettingsRow r) { return r >= SetSizePlayW && r <= SetSizeDialogH; }
    static int sizeIndexOf(SettingsRow r) { return (int)(r - SetSizePlayW); } // kind * 2 + (0 width, 1 height)
    static bool isSliderRow(SettingsRow r) { return (r >= SetHue && r <= SetBlue) || isSizeRow(r); }
    static customize::Channel channelOf(SettingsRow r) {
        switch (r) {
            case SetHue: return customize::Channel::Hue;
            case SetSat: return customize::Channel::Saturation;
            case SetVal: return customize::Channel::Brightness;
            case SetRed: return customize::Channel::Red;
            case SetGreen: return customize::Channel::Green;
            default: return customize::Channel::Blue;
        }
    }
    customize::Rgb8 editedColour() const { return customize::unpack(cfg.customColor[customSlot]); }

    int accentCount() const { return cfg.rainbowUnlocked ? (int)ui::Accent::COUNT : (int)ui::Accent::Rainbow; }

    static const char* onOff(bool on) { return on ? "On" : "Off"; }

    // Playback quality presets, cycled by the "Video quality" settings row.
    // The table (names, sizes, bitrates, texts) lives in quality_presets.h so
    // the host tests can check it; "Custom" is what a hand-edited config.json
    // shows until a preset is picked again.
    int qualityIndex() const { return quality::indexFor(cfg.videoBitrate, cfg.videoHeight); }

    ui::ListEntry settingsEntry(SettingsRow row) const {
        ui::ListEntry e;
        switch (row) {
            case SetMusic:
                e.name = "Menu music";
                e.icon = ui::Icon::Music;
                e.tag = onOff(cfg.menuMusicEnabled && musicSourceAvailable());
                if (!cfg.menuMusicItemId.empty())
                    e.detail = cfg.menuMusicName + "  -  loops quietly in the menus";
                else if (localMusicFailed)
                    e.detail = "The music file could not be played -- use AAC in .m4a. Or pick a library song with -";
                else if (localMusicPresent())
                    e.detail = "Plays the music file in the assets folder -- or select a song and press -";
                else
                    e.detail = "Put bgm.m4a in sd:/wiiu/apps/ufin/assets, or select a song and press -";
                break;
            case SetMusicDefault:
                e.name = "Restore default menu music";
                e.icon = ui::Icon::Music;
                if (!cfg.menuMusicItemId.empty())
                    e.detail = "A: stop using \"" + cfg.menuMusicName + "\" and go back to the built-in music (your files are kept)";
                else
                    e.detail = "Already using the default menu music (the file in the assets folder)";
                e.dimmed = cfg.menuMusicItemId.empty();
                break;
            case SetAccent: {
                e.name = "Accent colour";
                e.icon = ui::Icon::Palette;
                e.tag = ui::accentName((ui::Accent)cfg.accent);
                for (int a = 0; a < accentCount(); a++) e.widget.colours.push_back(ui::accentSwatchColour((ui::Accent)a));
                e.widget.kind = ui::RowWidget::Kind::Swatches;
                e.widget.selected = cfg.accent;
                e.dimmed = cfg.customPalette; // the Custom Palette replaces them while it is on
                e.detail = cfg.customPalette ? "Greyed out while Custom Palette is on -- turn it off to use these"
                           : "Tap a colour, or A / left / right to change it";
                break;
            }
            case SetCustomPalette:
                e.name = "Custom Palette";
                e.icon = ui::Icon::Palette;
                e.tag = onOff(cfg.customPalette);
                e.detail = cfg.customPalette ? "On: your own colours are the theme everywhere. A: back to the premade accents"
                                             : "A: make your own colours and gradient -- they replace the premade accents";
                break;
            case SetCustomColours:
                e.name = "Palette colours";
                e.icon = ui::Icon::Palette;
                e.tag = "Editing " + std::to_string(customSlot + 1);
                for (int i = 0; i < 3; i++) e.widget.colours.push_back(cfg.customColor[i] & 0xFFFFFF);
                e.widget.kind = ui::RowWidget::Kind::Swatches;
                e.widget.numbered = true;
                e.widget.selected = customSlot;
                e.detail = "Tap a colour, or A / left / right, to choose which one the controls below change";
                break;
            case SetPreview:
                e.name = "Live preview";
                e.icon = ui::Icon::Palette;
                e.widget.kind = ui::RowWidget::Kind::Preview;
                e.detail = "The theme as it is right now";
                break;
            case SetHue: case SetSat: case SetVal: case SetRed: case SetGreen: case SetBlue: {
                static const char* names[] = {"Hue", "Saturation", "Brightness", "Red", "Green", "Blue"};
                const customize::Channel ch = channelOf(row);
                const customize::Rgb8 c = editedColour();
                e.name = "Colour " + std::to_string(customSlot + 1) + "  -  " + names[row - SetHue];
                e.icon = ui::Icon::Palette;
                e.widget.kind = ui::RowWidget::Kind::Slider;
                e.widget.value = customize::channelValue(c, ch, hueMem);
                const float hue = customize::channelValue(c, customize::Channel::Hue, hueMem);
                const float sat = customize::channelValue(c, customize::Channel::Saturation, hueMem);
                const float val = customize::channelValue(c, customize::Channel::Brightness, hueMem);
                using ui::theme::fromHsv;
                char buf[16];
                switch (ch) {
                    case customize::Channel::Hue:
                        e.widget.hueTrack = true;
                        snprintf(buf, sizeof(buf), "%d deg", (int)(hue * 360.0f + 0.5f) % 360);
                        break;
                    case customize::Channel::Saturation:
                        e.widget.trackLo = customize::pack(fromHsv(hue, 0.0f, val));
                        e.widget.trackHi = customize::pack(fromHsv(hue, 1.0f, val));
                        snprintf(buf, sizeof(buf), "%d%%", (int)(sat * 100.0f + 0.5f));
                        break;
                    case customize::Channel::Brightness:
                        e.widget.trackLo = 0x000000;
                        e.widget.trackHi = customize::pack(fromHsv(hue, sat, 1.0f));
                        snprintf(buf, sizeof(buf), "%d%%", (int)(val * 100.0f + 0.5f));
                        break;
                    default: {
                        customize::Rgb8 lo = c, hi = c;
                        int v = 0;
                        if (ch == customize::Channel::Red)        { lo.r = 0; hi.r = 255; v = c.r; }
                        else if (ch == customize::Channel::Green) { lo.g = 0; hi.g = 255; v = c.g; }
                        else                                      { lo.b = 0; hi.b = 255; v = c.b; }
                        e.widget.trackLo = customize::pack(lo);
                        e.widget.trackHi = customize::pack(hi);
                        snprintf(buf, sizeof(buf), "%d", v);
                    }
                }
                e.tag = buf;
                e.detail = "Left / right to adjust, or tap - + or the bar";
                break;
            }
            case SetHex:
                e.name = "Colour " + std::to_string(customSlot + 1) + "  -  hex code";
                e.icon = ui::Icon::Palette;
                e.tag = ui::theme::toHex(editedColour());
                e.detail = "A: type a code such as #1E90FF";
                break;
            case SetArrange:
                e.name = "Gradient arrangement";
                e.icon = ui::Icon::Palette;
                e.tag = ui::theme::arrangeLabel(cfg.customArrange);
                e.detail = "A: next -- which colour is main, deep (focus) and second (gradients)";
                break;
            case SetAutoColours:
                e.name = "Auto colours";
                e.icon = ui::Icon::Palette;
                e.detail = "A: build colours 2 and 3 from colour 1 (a deeper shade and a partner hue)";
                break;
            case SetShapes:
                e.name = "Customize shapes";
                e.icon = ui::Icon::Sparkle;
                e.dimmed = !cfg.m3Expressive;
                e.tag = cfg.m3Expressive ? onOff(cfg.customShapes) : "M3E only";
                e.detail = cfg.m3Expressive ? (cfg.customShapes ? "On: pick a shape for each kind of button. A: back to the default shapes"
                                                                : "A: choose the shape of the buttons (pill, square, cookie...)")
                                            : "Needs the Material 3 Expressive UI style (UI style, above)";
                break;
            case SetShapePlay: case SetShapeSeek: case SetShapeToggle: case SetShapeDialog: {
                const int kind = shapeKindOf(row);
                e.name = ui::buttonKindName(kind);
                e.icon = ui::Icon::Sparkle;
                e.tag = ui::buttonShapeName(cfg.buttonShape[kind]);
                e.widget.kind = ui::RowWidget::Kind::Shapes;
                for (int i = 0; i < ui::buttonShapeCount(kind); i++) e.widget.shapes.push_back(i);
                e.widget.selected = cfg.buttonShape[kind];
                e.detail = kind == 3 ? "Wide buttons: tap a shape, or A / left / right" : "Tap a shape, or A / left / right";
                break;
            }
            case SetSizePlayW: case SetSizePlayH: case SetSizeSeekW: case SetSizeSeekH:
            case SetSizeToggleW: case SetSizeToggleH: case SetSizeDialogW: case SetSizeDialogH: {
                const int idx = sizeIndexOf(row), kind = idx / 2;
                const int lo = ui::buttonSizeMin(), hi = ui::buttonSizeMax(idx);
                e.name = std::string(ui::buttonKindName(kind)) + (idx % 2 ? "  -  Height" : "  -  Width");
                e.icon = ui::Icon::Sparkle;
                e.tag = std::to_string(cfg.buttonSize[idx]) + "%";
                e.widget.kind = ui::RowWidget::Kind::Slider;
                e.widget.value = (float)(cfg.buttonSize[idx] - lo) / (float)(hi - lo);
                e.widget.trackLo = 0x3A3F4A;
                e.widget.trackHi = 0xC8CDD8;
                e.detail = idx == 7 ? "Left / right or - + to resize (60% to 112%: any taller would touch the next row)" : "Left / right or - + to resize (60% to 130%)";
                break;
            }
            case SetShapePreview:
                e.name = "Button preview";
                e.icon = ui::Icon::Sparkle;
                e.widget.kind = ui::RowWidget::Kind::ButtonPreview;
                e.widget.shapes.assign(cfg.buttonShape, cfg.buttonShape + 4);
                e.widget.sizes.assign(cfg.buttonSize, cfg.buttonSize + 8);
                e.detail = "Play / pause, seek, toggle and dialog buttons as they will look";
                break;
            case SetShapesReset:
                e.name = "Reset defaults";
                e.icon = ui::Icon::Sparkle;
                e.detail = "A: every button back to its default shape and size";
                break;
            case SetArtwork:
                e.name = "Artwork accent";
                e.icon = ui::Icon::Sparkle;
                e.dimmed = !cfg.m3Expressive;
                e.tag = cfg.m3Expressive ? onOff(cfg.artworkAccent) : "M3E only";
                e.detail = cfg.m3Expressive ? "While music or a video plays, the accent follows the artwork. Menus keep your palette"
                                            : "Needs the Material 3 Expressive UI style (UI style, above)";
                break;
            case SetUnlockRainbow:
                e.name = "Rainbow accent";
                e.icon = ui::Icon::Palette;
                e.tag = onOff(cfg.rainbowUnlocked);
                e.detail = "Adds the Rainbow accent to the colour list above";
                break;
            case SetLight:
                e.name = "Light mode";
                e.icon = ui::Icon::Palette;
                e.tag = onOff(cfg.lightMode);
                e.detail = "A light palette instead of dark -- every accent colour works with either";
                break;
            case SetBackground: {
                ui::Background bg = (ui::Background)cfg.backgroundEffect;
                e.name = "Animated background";
                e.icon = ui::Icon::Sparkle;
                e.tag = ui::backgroundEffectName(bg);
                switch (bg) {
                    case ui::Background::Off:        e.detail = "No effect behind the menus"; break;
                    case ui::Background::Circles:    e.detail = "Soft glowing shapes drift behind the menus"; break;
                    case ui::Background::WiiBubbles: e.detail = "Rising bubbles, like the Wii U menu"; break;
                    case ui::Background::Gradient:   e.detail = "A slow, subtle animated colour gradient"; break;
                    case ui::Background::Polygons:   e.detail = "Faint drifting geometric shapes"; break;
                    case ui::Background::Starfield:  e.detail = "A field of gently twinkling points"; break;
                    case ui::Background::Grid:       e.detail = "A rippling grid -- Liquid Glass visibly bends its lines"; break;
                    case ui::Background::Aurora:     e.detail = "Soft northern-lights curtains that sway behind the glass"; break;
                    case ui::Background::Ripples:    e.detail = "Water rings spreading out -- they warp under Liquid Glass"; break;
                    case ui::Background::Beams:      e.detail = "Slanted shafts of light with crisp edges for the glass to bend"; break;
                    case ui::Background::Orbs:       e.detail = "Glassy beads drifting around -- panels magnify them"; break;
                    case ui::Background::Waves:      e.detail = "Rolling wave crests along the bottom of the screen"; break;
                    default: break;
                }
                break;
            }
            case SetStyle:
                e.name = "UI style";
                e.icon = ui::Icon::Sparkle;
                e.tag = ui::styleName(uiStyleOfConfig());
                if (cfg.liquidGlass)
                    e.detail = ui::glassBlurAvailable() ? "A: next style  -  frosted, rounded panels with soft Wii U-style lighting"
                                                        : "A: next style  -  glass panels without blur (blur unavailable on this run)";
                else if (cfg.m3Expressive)
                    e.detail = "A: next style  -  tonal surfaces, cookie and pill shapes, springy focus (uses your accent colour)";
                else
                    e.detail = "A: next style  -  Plain, Liquid Glass or Material 3 Expressive";
                break;
            case SetQuality: {
                e.name = "Video quality";
                e.icon = ui::Icon::Video;
                int idx = qualityIndex();
                int count;
                const quality::Preset* presets = quality::presets(count);
                e.tag = idx >= 0 ? presets[idx].name : "Custom";
                e.detail = idx >= 0 ? presets[idx].detail : "Set by hand in config.json -- A picks a preset instead";
                break;
            }
            case SetH264Profile: {
                const h264::Profile prof = h264::parse(cfg.videoProfile);
                e.name = "H.264 profile";
                e.icon = ui::Icon::Video;
                e.tag = h264::displayName(prof);
                e.detail = h264::description(prof);
                break;
            }
            case SetCrt:
                e.name = "CRT mode";
                e.icon = ui::Icon::Video;
                e.tag = onOff(cfg.crt);
                e.detail = "Scanlines, a phosphor glow and rounded tube corners -- over everything, video too";
                break;
            case SetSnow:
                e.name = "Snow";
                e.icon = ui::Icon::Snow;
                e.tag = onOff(cfg.snow);
                e.detail = "Gentle snowfall over the menus";
                break;
            case SetBackdrops:
                e.name = "Jellyfin backdrops";
                e.icon = ui::Icon::Video;
                e.tag = onOff(cfg.jellyfinBackdrops);
                e.detail = "The selected title's backdrop behind the whole UI (uses more network)";
                break;
            case SetLogging:
                e.name = "Logging";
                e.icon = ui::Icon::Info;
                e.tag = onOff(cfg.logging);
                e.detail = "Writes ufin_log.txt to the SD card -- off by default to spare the card";
                break;
            case SetClock:
                e.name = "Clock";
                e.icon = ui::Icon::Clock;
                e.tag = onOff(cfg.clock);
                e.detail = "The time in the top-right corner";
                break;
            case SetAutoplay:
                e.name = "Autoplay next episode";
                e.icon = ui::Icon::Episode;
                e.tag = onOff(cfg.autoplayNext);
                e.detail = "When an episode ends, the next one starts after a 10 second countdown";
                break;
            case SetGamepadScreen:
                e.name = "GamePad screen during video";
                e.icon = ui::Icon::Gamepad;
                e.tag = onOff(!cfg.gamepadOffInVideo);
                e.detail = "Off saves battery when you watch on the TV (- during a video switches it too)";
                break;
            case SetDefaultTracks:
                e.name = "Always use default audio & subtitles";
                e.icon = ui::Icon::Video;
                e.tag = onOff(cfg.alwaysDefaultTracks);
                e.detail = cfg.alwaysDefaultTracks
                    ? "On: videos start with the default tracks. Turn off to be asked each time"
                    : "Off: you are asked which audio and subtitles to use when a video starts";
                break;
            case SetStrictVideoGeometry:
                e.name = "Strict video format check";
                e.icon = ui::Icon::Video;
                e.tag = onOff(!cfg.allowUnalignedVideoGeometry);
                e.detail = cfg.allowUnalignedVideoGeometry
                    ? "Off: unusually-shaped videos play, but there's a small chance of a crash on real hardware"
                    : "On: unusually-shaped videos are refused with an error instead of risking a crash";
                break;
            case SetTestPicture:
                e.name = "Video test picture";
                e.icon = ui::Icon::Video;
                e.detail = "Diagnostic: a magenta picture through the video path -- B to exit";
                break;
            case SetAccount:
                e.name = "Signed in as " + (cfg.username.empty() ? std::string("user") : cfg.username);
                e.icon = ui::Icon::User;
                e.tag = "Switch profile";
                e.detail = serverText(cfg.host, cfg.port);
                break;
            case SetAutoUpdates:
                e.name = "Automatic update check";
                e.icon = ui::Icon::Info;
                e.tag = onOff(cfg.checkForUpdates);
                e.detail = "Look for a newer Ufin on GitHub a moment after you sign in";
                break;
            case SetCheckUpdates:
                e.name = "Check for updates";
                e.icon = ui::Icon::Info;
                e.tag = ufin::kVersion;
                e.detail = "Compare this version with the latest release on GitHub";
                break;
            case SetAbout:
                e.name = "About Ufin";
                e.icon = ui::Icon::Info;
                e.tag = ufin::kVersion;
                // Build time: makes an old build on the SD card easy to spot.
                e.detail = std::string("A Jellyfin client for the Wii U  -  built ") + __DATE__ + " " + __TIME__;
                break;
        }
        e.grouped = (row >= SetCustomColours && row <= SetAutoColours) || isShapeRow(row) || isSizeRow(row) ||
                    row == SetShapePreview || row == SetShapesReset; // the editors are one long panel each
        return e;
    }

    int styleApplied = 0; // the ui::Style whose GPU resources ui::gfx() currently holds (0 = none)

    ui::Style uiStyleOfConfig() const {
        return cfg.liquidGlass ? ui::Style::LiquidGlass : (cfg.m3Expressive ? ui::Style::Expressive : ui::Style::Plain);
    }

    // Applies the look & feel settings to the renderer.
    void applyLook() {
        if (cfg.accent >= accentCount()) cfg.accent = 0;
        // The palette: Custom Palette (three colours + arrangement) or the premade accent -- both reach every
        // renderer through the same theme colours. The artwork-reactive accent is an M3E-only extra on top.
        ui::setCustomPalette(cfg.customPalette, cfg.customColor, cfg.customArrange);
        ui::setAccent((ui::Accent)cfg.accent);
        g_artworkAccentOn = cfg.artworkAccent && cfg.m3Expressive && !cfg.liquidGlass;
        ui::setArtworkAccent(g_artworkAccentOn);
        {
            const int none[4] = {0, 0, 0, 0};
            const int normal[8] = {100, 100, 100, 100, 100, 100, 100, 100};
            const bool on = cfg.customShapes && cfg.m3Expressive && !cfg.liquidGlass;
            ui::setButtonShapes(on ? cfg.buttonShape : none); // (only M3E draws them)
            ui::setButtonSizes(on ? cfg.buttonSize : normal);
        }
        if (cfg.backgroundEffect < 0 || cfg.backgroundEffect >= (int)ui::Background::COUNT) cfg.backgroundEffect = 0;
        ui::setBackgroundEffect((ui::Background)cfg.backgroundEffect);
        ui::setLightMode(cfg.lightMode);
        // Liquid Glass: the style flag always follows the setting; the GPU
        // side (two tiny textures) is only allocated/released when the
        // setting actually changes. A failed allocation leaves flat glass.
        if (cfg.liquidGlass) cfg.m3Expressive = false; // exactly one style
        const ui::Style wanted = uiStyleOfConfig();
        ui::setStyle(wanted);
        ui::gfx().setVisualizerGlass(cfg.visualizerGlass);
        ui::gfx().setGlassBlur(cfg.glassBlur);
        // Both Liquid Glass and M3E share the tiny blur textures; only Liquid Glass also gets the refraction
        // snapshot (a full-screen GPU copy). The GPU side is rebuilt only when the style really changes.
        if ((int)wanted != styleApplied) {
            styleApplied = (int)wanted;
            const bool blurWanted = wanted != ui::Style::Plain;
            const bool ok = ui::gfx().setGlass(blurWanted, cfg.glassDebug, cfg.glassRefraction && wanted == ui::Style::LiquidGlass,
                                               cfg.glassRefractionStage);
            if (wanted == ui::Style::LiquidGlass) say(ok ? "Liquid Glass UI on" : "Liquid Glass: blur unavailable, using flat glass");
            else if (wanted == ui::Style::Expressive) say(ok ? "Material 3 Expressive on" : "Material 3 Expressive on (dialogs without blur)");
        }
    }

    void activateSetting(SettingsRow row) {
        switch (row) {
            case SetMusic:
                localMusicState = -1; // look for the music file again (it may have been added since)
                localMusicFailed = false;
                if (!musicSourceAvailable()) {
                    say("Put bgm.m4a in sd:/wiiu/apps/ufin/assets, or select a song and press -");
                    return;
                }
                cfg.menuMusicEnabled = !cfg.menuMusicEnabled;
                musicDeviceFailed = false;
                say(cfg.menuMusicEnabled ? "Menu music on" : "Menu music off");
                break;
            case SetMusicDefault:
                // Only the music choice goes back: no other setting is touched and no audio file is deleted.
                if (cfg.menuMusicItemId.empty()) { say("Already using the default menu music"); return; }
                stopMusic(); // fades out the library song cleanly; updateMusic() then starts the default file
                cfg.menuMusicItemId.clear();
                cfg.menuMusicName.clear();
                localMusicState = -1; // look for the default file again
                localMusicFailed = false;
                musicDeviceFailed = false;
                say(musicSourceAvailable() ? "Default menu music restored"
                                           : "Default restored, but no bgm.m4a was found in the assets folder");
                break;
            case SetAccent:
                if (cfg.customPalette) { say("Custom Palette is on -- turn it off to use the premade accents"); return; }
                cfg.accent = (cfg.accent + 1) % accentCount();
                say(std::string("Accent: ") + ui::accentName((ui::Accent)cfg.accent));
                break;
            case SetCustomPalette:
                cfg.customPalette = !cfg.customPalette;
                if (cfg.customPalette) {
                    customSlot = 0;
                    // Starting from the accent that was selected is friendlier than from a fixed blue.
                    if (!cfg.paletteEdited) seedCustomFromAccent();
                    say("Custom Palette on");
                } else {
                    say("Custom Palette off -- premade accents");
                    const int cnt = (int)settingsRows().size();
                    if (setSel[0] >= cnt) setSel[0] = cnt - 1;
                }
                break;
            case SetCustomColours:
                customSlot = (customSlot + 1) % 3;
                return; // (nothing to apply or save: the editor only shows another colour)
            case SetPreview:
                return;
            case SetHue: case SetSat: case SetVal: case SetRed: case SetGreen: case SetBlue:
                say("Use left / right to adjust, or tap - + or the bar");
                return;
            case SetHex: {
                std::string value, kbError;
                if (ui::promptKeyboard(u"Colour code, e.g. #1E90FF", ui::theme::toHex(editedColour()), false, false, value, kbError)) {
                    ui::theme::Rgb8 parsed;
                    if (ui::theme::parseHex(value, parsed)) {
                        cfg.customColor[customSlot] = customize::pack(parsed);
                        cfg.paletteEdited = true;
                    } else {
                        say("Not a colour code -- use 6 hex digits, like 1E90FF");
                        return;
                    }
                } else {
                    return;
                }
                break;
            }
            case SetArrange:
                cfg.customArrange = (cfg.customArrange + 1) % ui::theme::kArrangeCount;
                cfg.paletteEdited = true;
                break;
            case SetAutoColours: {
                const ui::theme::Rgb8 m = customize::unpack(cfg.customColor[0]);
                cfg.customColor[1] = customize::pack(ui::theme::autoDeep(m));
                cfg.customColor[2] = customize::pack(ui::theme::autoSecond(m));
                cfg.paletteEdited = true;
                say("Colours 2 and 3 built from colour 1");
                break;
            }
            case SetShapes:
                if (!cfg.m3Expressive) { say("Custom shapes need the Material 3 Expressive UI style"); return; }
                cfg.customShapes = !cfg.customShapes;
                say(cfg.customShapes ? "Customize shapes on" : "Default shapes");
                if (!cfg.customShapes) { const int cnt = (int)settingsRows().size(); if (setSel[0] >= cnt) setSel[0] = cnt - 1; }
                break;
            case SetShapePlay: case SetShapeSeek: case SetShapeToggle: case SetShapeDialog: {
                const int kind = shapeKindOf(row);
                cfg.buttonShape[kind] = (cfg.buttonShape[kind] + 1) % ui::buttonShapeCount(kind);
                say(std::string(ui::buttonKindName(kind)) + ": " + ui::buttonShapeName(cfg.buttonShape[kind]));
                break;
            }
            case SetSizePlayW: case SetSizePlayH: case SetSizeSeekW: case SetSizeSeekH:
            case SetSizeToggleW: case SetSizeToggleH: case SetSizeDialogW: case SetSizeDialogH:
                say("Use left / right to resize, or tap - + or the bar");
                return;
            case SetShapePreview:
                return;
            case SetShapesReset:
                for (int i = 0; i < 4; i++) cfg.buttonShape[i] = 0;
                for (int i = 0; i < 8; i++) cfg.buttonSize[i] = 100;
                say("Button shapes and sizes reset to the defaults");
                break;
            case SetArtwork:
                if (!cfg.m3Expressive) { say("The artwork accent needs the Material 3 Expressive UI style"); return; }
                cfg.artworkAccent = !cfg.artworkAccent;
                say(cfg.artworkAccent ? "Artwork accent on -- try playing a song" : "Artwork accent off");
                break;
            case SetUnlockRainbow:
                cfg.rainbowUnlocked = !cfg.rainbowUnlocked;
                if (cfg.rainbowUnlocked) cfg.accent = (int)ui::Accent::Rainbow;
                say(cfg.rainbowUnlocked ? "Rainbow accent on" : "Rainbow accent off");
                break;
            case SetLight:
                cfg.lightMode = !cfg.lightMode;
                break;
            case SetBackground:
                cfg.backgroundEffect = (cfg.backgroundEffect + 1) % (int)ui::Background::COUNT;
                say(std::string("Background: ") + ui::backgroundEffectName((ui::Background)cfg.backgroundEffect));
                break;
            case SetQuality: {
                int count;
                const quality::Preset* presets = quality::presets(count);
                const int idx = quality::nextIndex(qualityIndex()); // Custom -> Safe 720p, then cycles (wraps after 1080p High)
                cfg.videoBitrate = presets[idx].bitrate;
                cfg.videoHeight = presets[idx].height;
                say(std::string("Video quality: ") + presets[idx].name);
                break;
            }
            case SetH264Profile: {
                const h264::Profile prof = h264::next(h264::parse(cfg.videoProfile)); // Main -> Baseline -> High -> Main
                cfg.videoProfile = h264::wireName(prof);
                say(std::string("H.264 profile: ") + h264::displayName(prof));
                break;
            }
            case SetCrt:
                cfg.crt = !cfg.crt;
                if (cfg.crt) say("CRT mode -- adjust your tracking");
                break;
            case SetStyle: // Plain -> Liquid Glass -> Material 3 Expressive -> Plain
                if (cfg.liquidGlass) { cfg.liquidGlass = false; cfg.m3Expressive = true; }
                else if (cfg.m3Expressive) { cfg.m3Expressive = false; }
                else { cfg.liquidGlass = true; }
                break; // applyLook() (above) does the rest, and reports if the blur could not be set up
            case SetSnow:
                cfg.snow = !cfg.snow;
                if (cfg.snow) say("Let it snow");
                break;
            case SetBackdrops:
                cfg.jellyfinBackdrops = !cfg.jellyfinBackdrops;
                say(cfg.jellyfinBackdrops ? "Jellyfin backdrops on" : "Jellyfin backdrops off");
                break;
            case SetLogging:
                cfg.logging = !cfg.logging;
                UfinLogSetEnabled(cfg.logging ? 1 : 0);
                if (cfg.logging) OSReport("Ufin: logging turned on in Settings\n");
                say(cfg.logging ? "Logging on: writing ufin_log.txt" : "Logging off");
                break;
            case SetClock:
                cfg.clock = !cfg.clock;
                break;
            case SetAutoplay:
                cfg.autoplayNext = !cfg.autoplayNext;
                break;
            case SetGamepadScreen:
                cfg.gamepadOffInVideo = !cfg.gamepadOffInVideo;
                break;
            case SetDefaultTracks:
                cfg.alwaysDefaultTracks = !cfg.alwaysDefaultTracks;
                say(cfg.alwaysDefaultTracks ? "Videos start with the default audio & subtitles"
                                            : "You will be asked which audio & subtitles to use");
                break;
            case SetStrictVideoGeometry:
                cfg.allowUnalignedVideoGeometry = !cfg.allowUnalignedVideoGeometry;
                say(cfg.allowUnalignedVideoGeometry
                    ? "Strict video format check off"
                    : "Strict video format check on");
                break;
            case SetTestPicture:
                stopMusic();
                runGx2TestPattern();
                return;
            case SetAccount:
                showProfiles(true); // Who's watching? (your sign-in stays saved)
                return;
            case SetAutoUpdates:
                cfg.checkForUpdates = !cfg.checkForUpdates;
                say(cfg.checkForUpdates ? "Update check on start-up: on" : "Update check on start-up: off");
                break;
            case SetCheckUpdates:
                checkForUpdatesNow();
                return;
            case SetAbout:
                // Tap it seven times... (like a phone's build number). Reveals
                // the Rainbow accent toggle in Settings for good, rather than
                // switching it on outright -- see SetUnlockRainbow above.
                if (cfg.rainbowEasterEgg) {
                    say(std::string("Ufin ") + ufin::kVersion + " -- UI inspired by CafeMP. Thanks for watching!");
                    return;
                }
                aboutTaps++;
                if (aboutTaps >= 7) {
                    cfg.rainbowEasterEgg = true;
                    cfg.rainbowUnlocked = true;
                    cfg.accent = (int)ui::Accent::Rainbow;
                    say("Rainbow accent unlocked! Find it any time in Settings.");
                } else if (aboutTaps >= 3) {
                    say(std::to_string(7 - aboutTaps) + (7 - aboutTaps == 1 ? " more..." : " more..."));
                    return;
                } else {
                    say(std::string("Ufin ") + ufin::kVersion + " -- UI inspired by CafeMP");
                    return;
                }
                break;
        }
        applyLook();
        saveConfig();
    }

    // --- Settings > Customization: palette editing, pages ---

    // First time the Custom Palette is switched on: start from the premade accent in use (its three colours), so
    // the editor opens on something the user recognises.
    void seedCustomFromAccent() {
        const ui::Accent a = (ui::Accent)cfg.accent;
        if (a == ui::Accent::Rainbow) return; // (animated: keep the default)
        const ui::theme::Rgb8 m = customize::unpack(ui::accentSwatchColour(a));
        cfg.customColor[0] = customize::pack(m);
        cfg.customColor[1] = customize::pack(ui::theme::autoDeep(m));
        cfg.customColor[2] = customize::pack(ui::theme::autoSecond(m));
        cfg.paletteEdited = true;
    }

    // A palette edit takes effect at once and is saved a moment after the last one (a slider held down would
    // otherwise write config.json to the SD card dozens of times a second).
    void paletteChanged() {
        cfg.paletteEdited = true;
        applyLook();
        paletteDirty = true;
        paletteEditAt = OSGetTime();
    }
    void flushPalette(bool force) {
        if (!paletteDirty) return;
        if (!force && secondsSince(paletteEditAt) < 1.0) return;
        paletteDirty = false;
        saveConfig();
    }

    // One step of an editor slider (direction -1 / +1).
    void stepSlider(SettingsRow row, int direction, bool coarse) {
        if (isSizeRow(row)) {
            const int idx = sizeIndexOf(row);
            cfg.buttonSize[idx] = std::max(ui::buttonSizeMin(), std::min(ui::buttonSizeMax(idx), cfg.buttonSize[idx] + direction * (coarse ? 10 : 5)));
            applyLook();
            paletteDirty = true; // (saved a moment after the last step, like the palette)
            paletteEditAt = OSGetTime();
            return;
        }
        const customize::Channel ch = channelOf(row);
        cfg.customColor[customSlot] = customize::pack(customize::stepChannel(editedColour(), ch, direction, coarse, hueMem));
        paletteChanged();
    }
    // Tap on the bar itself: jump to that position.
    void setSlider(SettingsRow row, float value) {
        if (isSizeRow(row)) {
            const int idx = sizeIndexOf(row), lo = ui::buttonSizeMin(), hi = ui::buttonSizeMax(idx);
            const int v = lo + (int)std::lround(std::max(0.0f, std::min(1.0f, value)) * (float)(hi - lo) / 5.0f) * 5;
            cfg.buttonSize[idx] = std::max(lo, std::min(hi, v));
            applyLook();
            paletteDirty = true;
            paletteEditAt = OSGetTime();
            return;
        }
        const customize::Channel ch = channelOf(row);
        cfg.customColor[customSlot] = customize::pack(customize::setChannel(editedColour(), ch, value, hueMem));
        paletteChanged();
    }
    // Tap (or left / right) on a row of chips: the accent list, or the palette's three colours.
    void pickChip(SettingsRow row, int index) {
        if (row == SetCustomColours) {
            if (index >= 0 && index < 3) customSlot = index;
        } else if (isShapeRow(row)) {
            const int kind = shapeKindOf(row);
            if (index < 0 || index >= ui::buttonShapeCount(kind)) return;
            cfg.buttonShape[kind] = index;
            say(std::string(ui::buttonKindName(kind)) + ": " + ui::buttonShapeName(index));
            applyLook();
            saveConfig();
        } else if (row == SetAccent) {
            if (cfg.customPalette) { say("Custom Palette is on -- turn it off to use the premade accents"); return; }
            if (index < 0 || index >= accentCount()) return;
            cfg.accent = index;
            say(std::string("Accent: ") + ui::accentName((ui::Accent)cfg.accent));
            applyLook();
            saveConfig();
        }
    }

    void switchSettingsPage(int to) {
        if (to < 0 || to >= kSettingsPages || to == setPage) return;
        flushPalette(true);
        Frame& f = stack.back();
        setSel[setPage] = f.selected; // each page keeps where the cursor was
        setPage = to;
        f.selected = setSel[setPage];
        sidebarFocused = false;
        ui::pulse();
    }

    void openSettings() {
        if (stack.back().kind == Frame::Kind::Settings) return;
        setPage = 0; // Settings opens on Customization
        stack.resize(1);
        Frame f;
        f.kind = Frame::Kind::Settings;
        f.title = "Settings";
        stack.push_back(f);
        sidebarFocused = false;
    }

    bool canShuffle() const {
        for (const JellyfinItem& item : stack.back().items) {
            if (item.type == "Audio" || isShufflableContainer(item)) return true;
        }
        return false;
    }

    // --- browsing actions ---

    // --- modal screens (run their own frames) ---

    // A choice dialog over the current screen. Returns the chosen index,
    // or -1 for B.
    int choose(const std::string& title, const std::string& subtitle, const std::vector<std::string>& options,
               const JellyfinItem* artItem = nullptr) {
        ui::ChoiceModel m;
        m.title = title;
        m.subtitle = subtitle;
        m.options = options;
        m.hints = {{"A", "Choose"}, {"B", "Cancel"}};
        RepeatState rs;
        bool wasTouched = true; // ignore a touch still held from opening this
        while (WHBProcIsRunning()) {
            input::State in = input::read();
            uint32_t p = withRepeat(in, rs);
            bool tapped = in.touched && !wasTouched;
            wasTouched = in.touched;
            const int n = (int)options.size();
            if (tapped) {
                int hit = ui::hitTestChoice(in.touchX, in.touchY, n);
                if (hit >= 0) return hit;
            }
            if (p & BTN_DOWN) m.selected = (m.selected + 1) % n;
            else if (p & BTN_UP) m.selected = (m.selected - 1 + n) % n;
            else if (p & VPAD_BUTTON_A) return m.selected;
            else if (p & VPAD_BUTTON_B) return -1;
            if (artItem) m.art = artFor(*artItem, 200, 300, &m.artAspect);
            drawFrame([&] {
                draw();
                ui::drawChoice(m);
            });
        }
        return -1;
    }

    // "Up next" with a 10 second countdown. True to play it, false if the
    // user cancelled.
    bool upNext(const JellyfinItem& next) {
        const double total = 10.0;
        const OSTime start = OSGetTime();
        ui::UpNextModel m;
        m.showName = next.seriesName;
        m.episodeTitle = itemTag(next).empty() ? next.name : itemDisplayName(next);
        std::string code = itemTag(next);
        if (!code.empty()) m.episodeTitle = code.substr(0, code.find("  ")) + "  " + next.name;
        m.detail = next.runTimeTicks > 0 ? ui::formatTime(next.runTimeTicks / 10000000.0) : "";
        m.totalSeconds = total;
        m.hints = {{"A", "Play now"}, {"B", "Cancel"}};
        bool wasTouched = true;
        while (WHBProcIsRunning()) {
            input::State in = input::read();
            bool tapped = in.touched && !wasTouched;
            wasTouched = in.touched;
            if ((in.trigger & VPAD_BUTTON_A) || tapped) return true;
            if (in.trigger & VPAD_BUTTON_B) return false;
            m.secondsLeft = total - secondsSince(start);
            if (m.secondsLeft <= 0.0) return true;
            m.art = artFor(next, 300, 420, &m.artAspect);
            drawFrame([&] { ui::drawUpNext(m); });
        }
        return false;
    }

    // --- library chrome: sort flyout, floating buttons, playlists ---

    bool sortOpen = false;     // the sort flyout is showing
    int sortCursor = 0;        // its cursor (options..., then the direction row)
    int fabFocus = -1;         // the controller cursor is on this floating button (-1 = on the list)
    size_t chromeDepth = 0;    // stack depth the two above belong to (they reset when it changes)

    // Floating buttons the current list shows, left to right.
    std::vector<ui::FabKind> fabsFor(const Frame& f) const {
        std::vector<ui::FabKind> v;
        if (f.kind != Frame::Kind::Items || !library::sortable(f.sortKind)) return v;
        v.push_back(ui::FabKind::Sort);
        if (f.sortKind == library::Kind::Playlists) v.push_back(ui::FabKind::NewPlaylist);
        return v;
    }

    // The criteria to offer (indices into the kind's table), for the list as it is loaded now.
    std::vector<int> sortChoices(const Frame& f) const {
        return library::availableOptions(f.sortKind, f.items, !f.moreToLoad(), f.sort.option);
    }

    // Re-sorts `f` on the server and reloads it, keeping the highlighted item when it is still in the result.
    void applySort(Frame& f, const library::Sort& s) {
        const std::string keep = (f.selected >= 0 && f.selected < (int)f.items.size()) ? f.items[(size_t)f.selected].id : "";
        const int oldSelected = f.selected;
        f.sort = s;
        f.items.clear();
        showBusy("Sorting", {library::sortSummary(f.sortKind, s)});
        if (!loadFrame(client, f)) errorMessage = client.lastError();
        f.selected = 0;
        bool found = false;
        for (size_t i = 0; i < f.items.size() && !keep.empty(); i++)
            if (f.items[i].id == keep) { f.selected = (int)i; found = true; break; }
        (void)oldSelected; // (not found in the new order's first page: start at the top)
    }

    void openSortMenu(Frame& f) {
        const std::vector<int> ch = sortChoices(f);
        sortOpen = true;
        sortCursor = 0;
        for (size_t i = 0; i < ch.size(); i++) if (ch[i] == f.sort.option) sortCursor = (int)i;
    }

    // Sort flyout input. True when it took the input.
    bool sortMenuInput(Frame& f, uint32_t pressed, bool tapped, float tx, float ty) {
        const std::vector<int> ch = sortChoices(f);
        const int n = (int)ch.size();
        auto activate = [&](int row) {
            if (row < 0 || row > n) return;
            if (row == n) { // direction
                library::Sort s = f.sort;
                s.descending = !s.descending;
                applySort(f, s);
            } else {
                const int opt = ch[(size_t)row];
                library::Sort s = opt == f.sort.option ? f.sort : library::pickOption(f.sortKind, opt);
                if (opt != f.sort.option) { applySort(f, s); sortOpen = false; }
                else sortOpen = false; // the one already active: just close
            }
        };
        if (tapped) {
            ui::BrowserModel bm = browserModelForHits(f);
            const int hit = ui::hitTestSortMenu(bm, tx, ty);
            if (hit >= 0) { sortCursor = hit; activate(hit); }
            else if (hit == -1) sortOpen = false; // outside the card closes it
            return true;
        }
        if (pressed & BTN_DOWN) sortCursor = (sortCursor + 1) % (n + 1);
        else if (pressed & BTN_UP) sortCursor = (sortCursor - 1 + n + 1) % (n + 1);
        else if ((pressed & (BTN_LEFT | BTN_RIGHT)) && sortCursor == n) activate(n);
        else if (pressed & VPAD_BUTTON_A) activate(sortCursor);
        else if (pressed & (VPAD_BUTTON_B | VPAD_BUTTON_X)) sortOpen = false;
        return true;
    }

    // Just enough of the browser model for the touch tests (which floating buttons, the flyout's geometry).
    ui::BrowserModel browserModelForHits(const Frame& f) const {
        ui::BrowserModel bm;
        bm.fabs = fabsFor(f);
        bm.sortMenu.open = sortOpen;
        bm.sortMenu.anchor = 0;
        for (size_t i = 0; i < bm.fabs.size(); i++) if (bm.fabs[i] == ui::FabKind::Sort) bm.sortMenu.anchor = (int)i;
        for (int i : sortChoices(f)) bm.sortMenu.options.push_back(library::optionsFor(f.sortKind).options[i].label);
        return bm;
    }

    // Loads the rest of a long list (up to the cap), with a card, before something needs all of it.
    void loadAllOf(Frame& f) {
        if (!f.moreToLoad()) return;
        showBusy("Loading the list", {f.title});
        while (f.moreToLoad()) {
            if (!loadMore(client, f)) { errorMessage = client.lastError(); return; }
            if (!WHBProcIsRunning()) return;
        }
    }

    // Keeps the next page coming as the cursor nears the end of what is loaded.
    void loadAhead(Frame& f) {
        if (f.moreToLoad() && f.selected >= (int)f.items.size() - library::kLoadAhead) {
            if (!loadMore(client, f)) errorMessage = client.lastError();
        }
    }

    // L / R on a music library's top level: another view. Each view keeps its own sort and cursor.
    void switchMusicTab(Frame& f, int tab) {
        if (tab == f.tab || tab < 0 || tab >= library::MusicViewCount) return;
        f.tabSort[f.tab] = f.sort;
        f.tabSelected[f.tab] = f.selected;
        f.tab = tab;
        f.sortKind = kindForTab(tab);
        f.sort = f.tabSort[tab];
        f.items.clear();
        f.total = 0;
        sortOpen = false;
        fabFocus = -1;
        showBusy("Loading", {library::musicViewName(tab)});
        if (!loadFrame(client, f)) errorMessage = client.lastError();
        f.selected = std::max(0, std::min(f.tabSelected[tab], (int)f.items.size() - 1));
    }

    // --- playlists ---

    // The New playlist dialog. Creates on the server; on failure the dialog stays with what was typed.
    // firstItemId: a track to put in it from the start. True when it was created.
    bool createPlaylistDialog(const std::string& firstItemId, std::string& createdId, std::string& createdName) {
        ui::PlaylistFormModel m;
        m.showVisibility = library::supportsPlaylistVisibility(client.serverVersion());
        m.hints = {{"A", "Select"}, {"B", "Cancel"}};
        RepeatState rs;
        bool wasTouched = true;
        auto rows = [&]() { return m.showVisibility ? 4 : 3; }; // focus slots in use: name, [visibility], create, cancel
        auto slotOf = [&](int idx) { return (!m.showVisibility && idx >= 1) ? idx + 1 : idx; }; // focus index -> slot
        int fi = 0; // index among the slots in use
        auto edit = [&]() {
            std::string typed, err;
            if (ui::promptKeyboard(u"Playlist name", m.name, false, true, typed, err)) {
                m.name = typed;
                m.message.clear();
            } else if (!err.empty()) { m.message = "Keyboard: " + err; m.messageIsError = true; }
        };
        auto submit = [&]() -> bool {
            if (m.busy) return false; // one request at a time
            const std::string name = library::trimName(m.name);
            if (name.empty()) {
                m.message = "Give the playlist a name first.";
                m.messageIsError = true;
                fi = 0;
                return false;
            }
            m.busy = true;
            m.message = "Creating the playlist...";
            m.messageIsError = false;
            drawFrame([&] { draw(); ui::drawPlaylistForm(m); }); // show "Creating..." before the request blocks
            std::string id;
            const bool ok = client.createPlaylist(name, m.isPublic, firstItemId, id);
            m.busy = false;
            if (ok) { createdId = id; createdName = name; return true; }
            m.message = client.lastError(); // the entered values stay
            m.messageIsError = true;
            return false;
        };
        while (WHBProcIsRunning()) {
            input::State in = input::read();
            const uint32_t p = withRepeat(in, rs);
            const bool tapped = in.touched && !wasTouched;
            wasTouched = in.touched;
            m.focus = slotOf(fi);
            if (tapped) {
                const int hit = ui::hitTestPlaylistForm(m, in.touchX, in.touchY);
                if (hit == 0) { fi = 0; edit(); }
                else if (hit == 10 || hit == 11) { m.isPublic = (hit == 11); }
                else if (hit == 2) { if (submit()) return true; }
                else if (hit == 3) return false;
            } else if (p & BTN_DOWN) fi = (fi + 1) % rows();
            else if (p & BTN_UP) fi = (fi - 1 + rows()) % rows();
            else if ((p & (BTN_LEFT | BTN_RIGHT)) && m.focus == 1) m.isPublic = !m.isPublic;
            else if ((p & (BTN_LEFT | BTN_RIGHT)) && m.focus == 2) fi = (fi + 1) % rows();
            else if ((p & (BTN_LEFT | BTN_RIGHT)) && m.focus == 3) fi = (fi - 1 + rows()) % rows();
            else if (p & VPAD_BUTTON_A) {
                if (m.focus == 0) edit();
                else if (m.focus == 1) m.isPublic = !m.isPublic;
                else if (m.focus == 2) { if (submit()) return true; }
                else return false;
            } else if (p & VPAD_BUTTON_B) return false;
            drawFrame([&] { draw(); ui::drawPlaylistForm(m); });
        }
        return false;
    }

    // "Add to playlist": a compact list of the user's playlists ("New playlist" first). True when added.
    bool addToPlaylistFlow(const JellyfinItem& song) {
        if (song.type != "Audio" || song.id.empty()) return false; // never an artist or album container
        showBusy("Loading playlists");
        library::Query q;
        q.source = library::Query::Source::AllPlaylists;
        q.sortBy = "SortName";
        std::vector<JellyfinItem> lists;
        int total = 0;
        if (!client.queryItems(q, lists, total)) { say("Couldn't load playlists: " + client.lastError()); return false; }
        ui::PlaylistPickerModel m;
        m.subtitle = itemDisplayName(song);
        m.rows.push_back("New playlist");
        for (const JellyfinItem& pl : lists) m.rows.push_back(pl.name);
        if (lists.empty()) m.message = "You have no playlists yet - make the first one.";
        m.hints = {{"A", "Add"}, {"B", "Cancel"}};
        RepeatState rs;
        bool wasTouched = true;
        while (WHBProcIsRunning()) {
            input::State in = input::read();
            const uint32_t p = withRepeat(in, rs);
            const bool tapped = in.touched && !wasTouched;
            wasTouched = in.touched;
            int chosen = -1;
            if (tapped) chosen = ui::hitTestPlaylistPicker(m, in.touchX, in.touchY);
            else if (p & BTN_DOWN) m.selected = (m.selected + 1) % (int)m.rows.size();
            else if (p & BTN_UP) m.selected = (m.selected - 1 + (int)m.rows.size()) % (int)m.rows.size();
            else if (p & VPAD_BUTTON_A) chosen = m.selected;
            else if (p & VPAD_BUTTON_B) return false;
            if (chosen == 0) {
                std::string id, name;
                if (createPlaylistDialog(song.id, id, name)) { say("Created \"" + name + "\" with this song"); return true; }
                wasTouched = true;
            } else if (chosen > 0 && chosen <= (int)lists.size() && !m.busy) {
                m.busy = true;
                m.message = "Adding...";
                drawFrame([&] { draw(); ui::drawPlaylistPicker(m); });
                const JellyfinItem& pl = lists[(size_t)chosen - 1];
                if (client.addToPlaylist(pl.id, song.id)) { say("Added to \"" + pl.name + "\""); return true; }
                m.busy = false;
                m.message = "Couldn't add it: " + client.lastError();
            }
            drawFrame([&] { draw(); ui::drawPlaylistPicker(m); });
        }
        return false;
    }

    // Removes the selected track from the playlist being shown.
    void removeFromPlaylist(Frame& f) {
        if (!f.isPlaylist() || f.items.empty()) return;
        const JellyfinItem track = f.items[(size_t)f.selected];
        const int c = choose("Remove from \"" + f.title + "\"?", itemDisplayName(track), {"Remove", "Keep it"}, &track);
        if (c != 0) return;
        std::string entry = track.playlistItemId;
        if (entry.empty() || f.sort.option != 0 || f.sort.descending) {
            // A sorted view: look the entry up in the playlist's own listing.
            library::Query q;
            q.source = library::Query::Source::Playlist;
            q.parentId = f.id;
            q.limit = library::kMaxLoaded;
            std::vector<JellyfinItem> own;
            int total = 0;
            entry.clear();
            if (client.queryItems(q, own, total))
                for (const JellyfinItem& e : own) if (e.id == track.id && !e.playlistItemId.empty()) { entry = e.playlistItemId; break; }
        }
        if (entry.empty() || !client.removeFromPlaylist(f.id, entry)) {
            say("Couldn't remove it: " + (entry.empty() ? std::string("entry not found") : client.lastError()));
            return;
        }
        const int keep = f.selected;
        showBusy("Updating");
        loadFrame(client, f);
        f.selected = std::max(0, std::min(keep, (int)f.items.size() - 1));
        say("Removed from the playlist");
    }

    void activateFab(Frame& f, int index) {
        const std::vector<ui::FabKind> fabs = fabsFor(f);
        if (index < 0 || index >= (int)fabs.size()) return;
        fabFocus = index;
        if (fabs[(size_t)index] == ui::FabKind::Sort) openSortMenu(f);
        else newPlaylistFromView(f);
    }

    // The add-to-playlist action on a song (right stick up / 1+2 / the row's "+"). In a playlist it also offers
    // removing the track from it.
    // Adds an item (or the songs / episodes inside an album, show ...) to the Up Next list, from a list in the browser.
    void addToUpNext(const Frame& f, const JellyfinItem& it) {
        const size_t room = UserQueue::kMax - std::min(UserQueue::kMax, g_upNext.size());
        if (room == 0) { say("The Up Next list is full"); return; }
        std::vector<JellyfinItem> out;
        std::string err;
        if (!UserQueue::playable(it)) showBusy("Adding", {it.name});
        if (!expandForQueue(client, f, it, room, out, err)) { say(err.empty() ? "Can't add that" : err); return; }
        const size_t n = g_upNext.add(out);
        say(n == 0 ? "Nothing to add" : (n == 1 ? "Added to Up Next: " + itemDisplayName(out[0]) : "Added " + std::to_string(n) + " items to Up Next"));
    }

    bool canQueue(const JellyfinItem& it) const {
        return UserQueue::playable(it) || it.type == "Series" || it.type == "Season" || it.type == "MusicAlbum" ||
               it.type == "MusicArtist" || it.type == "Playlist" || it.type == "BoxSet";
    }

    // Sidebar > Up Next: plays what was queued (nothing is playing now).
    void playUpNextFromMenu() {
        if (g_upNext.empty()) { say("Up Next is empty -- add items with the right stick up (Wii Remote: 1 + 2)"); return; }
        queuePrefs = TrackPrefs();
        runUpNext();
        refreshAfterPlayback();
    }

    void songPlaylistAction(Frame& f, int index) {
        if (index < 0 || index >= (int)f.items.size()) return;
        const JellyfinItem song = f.items[(size_t)index];
        if (song.type != "Audio") return;
        f.selected = index;
        if (f.isPlaylist()) {
            const int c = choose(itemDisplayName(song), "This playlist", {"Add to another playlist", "Add to Up Next", "Remove from this playlist"}, &song);
            if (c == 0) addToPlaylistFlow(song);
            else if (c == 1) addToUpNext(f, song);
            else if (c == 2) removeFromPlaylist(f);
            return;
        }
        const int c = choose(itemDisplayName(song), "", {"Add to a playlist", "Add to Up Next"}, &song);
        if (c == 0) addToPlaylistFlow(song);
        else if (c == 1) addToUpNext(f, song);
    }

    // "New playlist" from the Playlists view: create, reload, and land on the new one.
    void newPlaylistFromView(Frame& f) {
        std::string id, name;
        if (!createPlaylistDialog("", id, name)) return;
        showBusy("Updating");
        f.items.clear();
        loadFrame(client, f);
        for (size_t i = 0; i < f.items.size(); i++) if (f.items[i].id == id) f.selected = (int)i;
        say("Created \"" + name + "\"");
    }

    // --- browsing actions ---

    bool homeStale = false; // Home's rows need reloading (something was watched)

    // After playing something: reload the list so ticks / resume points
    // are current; Home is reloaded when we get back to it.
    void refreshAfterPlayback() {
        showBusy("Updating");
        loadFrame(client, stack.back());
        if (stack.size() > 1) homeStale = true;
        else findLiveTv();
    }

    // Plays a video / channel: offers "Resume", and after an episode ends
    // autoplays the next one (keeping its audio / subtitle language) -- unless the Up Next list has
    // entries: the list the user edited wins over the automatic next episode (runUpNext plays it).
    // `fromQueue`: an Up Next entry, which starts from the beginning without the Resume question.
    // Returns true when the last item played to its end (not stopped with B, not an error).
    bool playVideo(const JellyfinItem& picked, bool fromQueue = false) {
        double startAt = 0.0;
        if (!fromQueue && shouldOfferResume(picked)) {
            std::string at = ui::formatTime(picked.positionTicks / 10000000.0);
            int c = choose(itemDisplayName(picked), "You stopped at " + at,
                           {"Resume from " + at, "Start from the beginning"}, &picked);
            if (c < 0) return false;
            if (c == 0) startAt = picked.positionTicks / 10000000.0;
        }
        stopMusic();
        if (!fromQueue) { queuePrefs = TrackPrefs(); g_sessionQuality = -1; } // a new play starts from the defaults again
        JellyfinItem current = picked;
        bool finished = false;
        while (WHBProcIsRunning()) {
            std::string error;
            PlayResult r = playItem(client, cfg, current, error, nullptr, startAt, &queuePrefs);
            if (queuePrefs.alwaysDefault && !cfg.alwaysDefaultTracks) {
                // "Always use default" in the first-play prompt: remembered, and editable in Settings.
                cfg.alwaysDefaultTracks = true;
                saveConfig();
                say("Audio & subtitle prompt off -- change it in Settings");
            }
            if (r == PlayResult::Error) {
                errorMessage = error;
                finished = false;
                break;
            }
            finished = (r == PlayResult::Completed);
            if (g_videoSkipped) { g_videoSkipped = false; finished = true; break; } // "Play now" in the remote's queue
            if (!finished || !g_upNext.empty() || current.type != "Episode" || !cfg.autoplayNext ||
                current.seriesId.empty()) {
                break;
            }
            std::vector<JellyfinItem> episodes;
            showBusy("Finding the next episode");
            if (!client.getEpisodes(current.seriesId, episodes)) break;
            const JellyfinItem* next = findNextEpisode(episodes, current.id);
            if (!next) break; // end of the season / series
            JellyfinItem nextCopy = *next;
            if (!upNext(nextCopy)) { finished = false; break; }
            current = nextCopy;
            startAt = 0.0;
        }
        return finished;
    }

    TrackPrefs queuePrefs; // audio / subtitle language carried from one video to the next (kept for the session of a play)

    // Plays the Up Next list: entry after entry until it is empty, B stops it (what is left stays queued),
    // an error is reported and stops it (the failed entry is gone, the rest is intact).
    void runUpNext() {
        while (WHBProcIsRunning() && !g_upNext.empty()) {
            JellyfinItem it;
            g_upNext.popFront(it);
            if (it.type == "Audio") {
                stopMusic();
                const uint32_t seed = (uint32_t)OSGetTime();
                PlayQueue q(std::vector<JellyfinItem>{it}, 0, false, seed);
                q.setRepeat(g_repeatMode == PlayQueue::Repeat::One ? PlayQueue::Repeat::One : PlayQueue::Repeat::Off);
                std::string error;
                const PlayResult r = playQueue(client, cfg, q, error);
                if (r == PlayResult::Error) { errorMessage = error; return; }
                if (r != PlayResult::Completed) return;
            } else {
                if (!playVideo(it, true)) return;
            }
        }
    }

    // After anything that played to its end: carry on with the Up Next list, then refresh.
    void finishPlayback(bool finished) {
        if (finished && errorMessage.empty()) runUpNext();
        g_sessionQuality = -1;
        g_qui.closeAll();
        refreshAfterPlayback();
    }

    void open(const JellyfinItem& picked) {
        const Frame& f = stack.back();
        if (isHomeRow(picked)) {
            Frame next;
            next.kind = picked.type == HOME_RESUME ? Frame::Kind::Resume
                      : picked.type == HOME_NEXT_UP ? Frame::Kind::NextUp : Frame::Kind::Favorites;
            next.title = picked.name;
            showBusy("Opening", {picked.name});
            if (loadFrame(client, next)) stack.push_back(next);
            else errorMessage = client.lastError();
        } else if (picked.type == "Audio") {
            if (stack.back().moreToLoad()) loadAllOf(stack.back()); // the queue is the whole list, not just the first page
            stopMusic();
            size_t start = 0;
            const uint32_t seed = (uint32_t)OSGetTime() ^ (uint32_t)((uint64_t)OSGetTime() >> 32);
            PlayQueue queue(audioTracks(f.items, (size_t)f.selected, start), start, g_shuffleOn, seed);
            queue.setRepeat(g_repeatMode);
            std::string error;
            const PlayResult pr = playQueue(client, cfg, queue, error);
            if (pr == PlayResult::Error) errorMessage = error;
            finishPlayback(pr == PlayResult::Completed);
        } else if (isPlayableItem(picked)) {
            finishPlayback(playVideo(picked));
        } else {
            Frame next = makeChildFrame(f, picked);
            showBusy("Opening", {picked.name});
            if (loadFrame(client, next)) stack.push_back(next);
            else errorMessage = client.lastError();
        }
    }

    void reloadHome() {
        homeStale = false;
        showBusy("Updating");
        loadFrame(client, stack[0]);
        findLiveTv();
    }

    // Y: heart / un-heart the selected item.
    void toggleFavorite(JellyfinItem& item) {
        if (isHomeRow(item) || item.type == "CollectionFolder" || item.type == "UserView") return;
        bool want = !item.favorite;
        if (client.setFavorite(item.id, want)) {
            item.favorite = want;
            homeStale = true;
            say(want ? "Added to favourites" : "Removed from favourites");
        } else {
            say("Couldn't change the favourite: " + client.lastError());
        }
    }

    // ZL: mark the selected item watched / unwatched.
    void toggleWatched(JellyfinItem& item) {
        if (isHomeRow(item) || item.type == "CollectionFolder" || item.type == "UserView" || item.type == "TvChannel") return;
        bool want = !item.played;
        if (client.setPlayed(item.id, want)) {
            item.played = want;
            if (want) {
                item.positionTicks = 0;
                if (item.unplayedCount > 0) item.unplayedCount = 0;
            }
            homeStale = true;
            say(want ? "Marked as watched" : "Marked as unwatched");
        } else {
            say("Couldn't change it: " + client.lastError());
        }
    }

    void search() {
        std::string term, keyboardError;
        bool entered = ui::promptKeyboard(u"Search movies, shows and music", term, keyboardError);
        if (!entered) {
            if (!keyboardError.empty()) errorMessage = "Keyboard: " + keyboardError;
            return;
        }
        Frame results;
        results.kind = Frame::Kind::Search;
        results.id = term;
        results.title = "Search: " + term;
        showBusy("Searching", {"\"" + term + "\""});
        if (loadFrame(client, results)) {
            if (stack.back().kind == Frame::Kind::Search) stack.pop_back();
            stack.push_back(results);
            sidebarFocused = false;
        } else {
            errorMessage = client.lastError();
        }
    }

    void goLiveTv() {
        for (const JellyfinItem& v : stack[0].items) {
            if (!isLiveTvView(v)) continue;
            stack.resize(1);
            Frame live;
            live.kind = Frame::Kind::LiveTv;
            live.id = v.id;
            live.title = v.name;
            showBusy("Loading channels");
            if (loadFrame(client, live)) {
                stack.push_back(live);
                sidebarFocused = false;
            } else {
                errorMessage = client.lastError();
            }
            return;
        }
    }

    void activateSection(Section s) {
        switch (s) {
            case Section::Home:
                stack.resize(1);
                sidebarFocused = false;
                if (homeStale) reloadHome();
                break;
            case Section::LiveTv:   goLiveTv(); break;
            case Section::Search:   search(); break;
            case Section::UpNext:   playUpNextFromMenu(); break;
            case Section::Settings: openSettings(); break;
            case Section::Help:     openHelp(); break;
        }
    }

    void shuffle() {
        Frame& f = stack.back();
        const JellyfinItem picked = f.items[(size_t)f.selected];
        std::vector<JellyfinItem> tracks;
        size_t ignored = 0;
        if (isShufflableContainer(picked)) {
            showBusy("Loading", {picked.name});
            std::vector<JellyfinItem> children;
            if (!client.getItems(picked.id, children)) {
                errorMessage = client.lastError();
                return;
            }
            tracks = audioTracks(children, (size_t)-1, ignored);
        } else {
            tracks = audioTracks(f.items, (size_t)-1, ignored);
        }
        if (tracks.empty()) {
            errorMessage = "Nothing to shuffle here -- select an album, a playlist, or a list of songs.";
            return;
        }
        stopMusic();
        uint32_t seed = (uint32_t)OSGetTime() ^ (uint32_t)((uint64_t)OSGetTime() >> 32);
        PlayQueue queue(tracks, (size_t)(seed % tracks.size()), true, seed);
        g_shuffleOn = true; // "+" turns the Shuffle button on, so it shows as on in the player
        queue.setRepeat(g_repeatMode);
        std::string error;
        const PlayResult pr = playQueue(client, cfg, queue, error);
        if (pr == PlayResult::Error) errorMessage = error;
        else if (pr == PlayResult::Completed) runUpNext();
    }

    // --- input ---

    void handleInput(uint32_t pressed, bool tapped, float tapX, float tapY) {
        if (!errorMessage.empty()) {
            // Error card: B dismisses it (retrying the connection if we never
            // got in); X goes to the sign-in screen when that's offered.
            if ((pressed & VPAD_BUTTON_X) && errorOffersSignIn) {
                errorMessage.clear();
                errorOffersSignIn = false;
                if (!cfg.profiles.empty()) showProfiles(false);
                else showSignIn();
            } else if ((pressed & (VPAD_BUTTON_B | VPAD_BUTTON_A)) || tapped) {
                errorMessage.clear();
                if (!loggedIn) {
                    errorOffersSignIn = false;
                    // try connecting again (sets a new error if it still fails)
                    if (connectingProfile >= 0 && connectingProfile < (int)cfg.profiles.size()) selectProfile(connectingProfile);
                    else startUp();
                }
            }
            return;
        }
        if (mode == Mode::Profiles) {
            profilesInput(pressed, tapped, tapX, tapY);
            return;
        }
        if (mode == Mode::SignIn) {
            signInInput(pressed, tapped, tapX, tapY);
            return;
        }
        if (mode == Mode::QuickConnect) {
            if (pressed & VPAD_BUTTON_B) {
                mode = Mode::SignIn;
                loginMessage.clear();
            } else if ((pressed & VPAD_BUTTON_A) && quickMessageIsError) {
                activateLoginRow(RowQuickConnect); // start again with a new code
            }
            return;
        }

        if (helpOpen) { handleHelpInput(pressed, tapped, tapX, tapY); return; }

        // The hidden usage celebration is on screen: any press or tap closes it (after its first
        // moments) and is otherwise swallowed, so it cannot also activate whatever is underneath.
        if (ui::usageCelebrationActive()) {
            if (pressed || tapped) ui::dismissUsageCelebration();
            return;
        }

        if (chromeDepth != stack.size()) { chromeDepth = stack.size(); sortOpen = false; fabFocus = -1; }
        Frame& f = stack.back();
        const bool settings = (f.kind == Frame::Kind::Settings);
        flushPalette(!settings); // palette edits are saved a moment after the last one, at once on leaving Settings
        const std::vector<SettingsRow> setRows = settingsRows();
        const int count = settings ? (int)setRows.size() : (int)f.items.size();
        if (f.selected >= count) f.selected = count > 0 ? count - 1 : 0;
        const std::vector<Section> secs = sections();
        if ((pressed & VPAD_BUTTON_A) && fabFocus < 0 && !sortOpen) ui::pulse(); // M3E: bounce the rows next to the activated one

        // The sort flyout, when open, owns the input.
        if (sortOpen && !settings) { sortMenuInput(f, pressed, tapped, tapX, tapY); return; }
        if (!settings) loadAhead(f);

        // A music library's views: L / R, or the tabs under the title.
        if (f.tab >= 0 && !sidebarFocused) {
            if (pressed & (VPAD_BUTTON_L | VPAD_BUTTON_R)) {
                switchMusicTab(f, customize::nextPage(f.tab, (pressed & VPAD_BUTTON_L) ? -1 : 1, library::MusicViewCount));
                return;
            }
            if (tapped) {
                const int tb = ui::hitTestSettingsTab(tapX, tapY, library::MusicViewCount);
                if (tb >= 0) { switchMusicTab(f, tb); return; }
            }
        }

        // Settings pages: L / R, or the tabs under the title.
        if (settings && (pressed & (VPAD_BUTTON_L | VPAD_BUTTON_R)) && !sidebarFocused) {
            switchSettingsPage(customize::nextPage(setPage, (pressed & VPAD_BUTTON_L) ? -1 : 1, kSettingsPages));
            return;
        }
        if (settings && tapped) {
            const int tab = ui::hitTestSettingsTab(tapX, tapY, kSettingsPages);
            if (tab >= 0) { switchSettingsPage(tab); return; }
        }

        if (tapped) {
            // Tapping the sidebar's "Time used" counter five times in a row (touch screen only).
            if (ui::hitUsageCounter(tapX, tapY)) {
                if (usageTaps.tap(monotonicSeconds())) {
                    const int64_t total = g_usage.totalSeconds(monotonicSeconds());
                    ui::showUsageCelebration(formatUsageBig(total), formatUsageSeconds(total));
                }
                return;
            }
            usageTaps.reset(); // a tap anywhere else breaks the run

            ui::ListWindow w = ui::computeListWindow(count, f.selected, ui::layout::visibleRows());
            if (!settings && !sidebarFocused) {
                // Floating buttons, and the "+" on a song row.
                const ui::BrowserModel bm = browserModelForHits(f);
                const int fb = ui::hitTestFab(bm, tapX, tapY);
                if (fb >= 0) { activateFab(f, fb); return; }
                for (int i = w.start; i < w.end; i++) {
                    if (f.items[(size_t)i].type != "Audio") continue;
                    ui::Rect b = ui::layout::rowAddButton(ui::layout::listRow(i - w.start));
                    b.x -= 8; b.y -= 4; b.w += 16; b.h += 8;
                    if (b.contains(tapX, tapY)) { songPlaylistAction(f, i); return; }
                }
            }
            ui::Hit hit = ui::hitTestBrowser(tapX, tapY, (int)secs.size(), w);
            if (hit.kind == ui::Hit::Kind::Sidebar || hit.kind == ui::Hit::Kind::Row) ui::pulse();
            if (hit.kind == ui::Hit::Kind::Sidebar) {
                sidebarCursor = hit.index;
                activateSection(secs[(size_t)hit.index]);
                return;
            }
            if (hit.kind == ui::Hit::Kind::Row) {
                sidebarFocused = false;
                f.selected = hit.index;
                if (settings) {
                    const SettingsRow row = setRows[(size_t)hit.index];
                    const ui::Rect rr = ui::layout::listRow(hit.index - w.start);
                    const ui::ListEntry entry = settingsEntry(row);
                    if (entry.widget.kind == ui::RowWidget::Kind::Swatches || entry.widget.kind == ui::RowWidget::Kind::Shapes) {
                        const int chip = ui::layout::swatchHit(rr, entry.widget.count(), tapX, tapY);
                        if (chip >= 0) { pickChip(row, chip); return; }
                    } else if (entry.widget.kind == ui::RowWidget::Kind::Slider) {
                        float frac = 0.0f;
                        const int part = ui::layout::sliderHit(rr, tapX, tapY, frac);
                        if (part == 1) { heldSince = OSGetTime(); heldLastFire = heldSince; stepSlider(row, -1, true); return; }
                        if (part == 2) { heldSince = OSGetTime(); heldLastFire = heldSince; stepSlider(row, +1, true); return; }
                        if (part == 3) { setSlider(row, frac); return; }
                        return; // (the rest of a slider row does nothing)
                    }
                    activateSetting(row);
                } else {
                    open(f.items[(size_t)hit.index]);
                }
                return;
            }
        }

        if (pressed & VPAD_BUTTON_X) { search(); return; }
        if ((pressed & VPAD_BUTTON_ZR) && !settings) {
            showBusy("Refreshing");
            if (!loadFrame(client, f)) errorMessage = client.lastError();
            if (stack.size() == 1) findLiveTv();
            return;
        }
        if (!sidebarFocused && !settings && count > 0 && (pressed & VPAD_STICK_R_EMULATION_UP)) {
            // Right stick up (1 + 2 on a Wii Remote): a song -> add to a playlist or to Up Next; a movie, episode,
            // album, artist, playlist, season or show -> add to Up Next.
            const JellyfinItem& cur = f.items[(size_t)f.selected];
            if (cur.type == "Audio") songPlaylistAction(f, f.selected);
            else if (canQueue(cur)) addToUpNext(f, cur);
            return;
        }
        if (!sidebarFocused && !settings && count > 0) {
            if (pressed & VPAD_BUTTON_Y) { toggleFavorite(f.items[(size_t)f.selected]); return; }
            if (pressed & VPAD_BUTTON_ZL) { toggleWatched(f.items[(size_t)f.selected]); return; }
        }

        if (sidebarFocused) {
            const int n = (int)secs.size();
            if (pressed & BTN_DOWN) sidebarCursor = (sidebarCursor + 1) % n;
            else if (pressed & BTN_UP) sidebarCursor = (sidebarCursor - 1 + n) % n;
            else if (pressed & (BTN_RIGHT | VPAD_BUTTON_B)) sidebarFocused = false;
            else if (pressed & VPAD_BUTTON_A) activateSection(secs[(size_t)sidebarCursor]);
            return;
        }

        // Floating buttons (Sort, New playlist): Right from the list reaches them, Left / B / Up / Down leave.
        if (!settings) {
            const size_t nf = fabsFor(f).size();
            if (nf > 0 && fabFocus >= 0) {
                if (pressed & BTN_RIGHT) fabFocus = std::min((int)nf - 1, fabFocus + 1);
                else if (pressed & BTN_LEFT) fabFocus--; // from the first one: back to the list (-1)
                else if (pressed & (BTN_UP | BTN_DOWN | VPAD_BUTTON_B)) fabFocus = -1;
                else if (pressed & VPAD_BUTTON_A) activateFab(f, fabFocus);
                return;
            }
            if (nf > 0 && (pressed & BTN_RIGHT)) { fabFocus = 0; return; }
        }

        // Settings > Customization: left / right work the selected row's control (a slider, or a row of chips),
        // held down they repeat; a finger held on a slider's - / + does too.
        if (settings && count > 0) {
            const SettingsRow cur = setRows[(size_t)f.selected];
            const bool slider = isSliderRow(cur);
            const bool chips = (cur == SetCustomColours) || (cur == SetAccent) || isShapeRow(cur);
            if (slider || chips) {
                int dir = (pressed & BTN_RIGHT) ? 1 : ((pressed & BTN_LEFT) ? -1 : 0);
                bool coarse = false;
                if (dir != 0) {
                    heldSince = OSGetTime();
                    heldLastFire = heldSince;
                } else if (slider && (g_dpadHeld & (BTN_LEFT | BTN_RIGHT)) && heldSince &&
                           OSTicksToMilliseconds(OSGetTime() - heldSince) >= 400 &&
                           OSTicksToMilliseconds(OSGetTime() - heldLastFire) >= 60) {
                    dir = (g_dpadHeld & BTN_RIGHT) ? 1 : -1;
                    heldLastFire = OSGetTime();
                } else if (slider && g_touchHeld && !tapped && heldSince) {
                    // A finger resting on - or +: repeat that button.
                    const ui::ListWindow win = ui::computeListWindow(count, f.selected, ui::layout::visibleRows());
                    const ui::Rect rr = ui::layout::listRow(f.selected - win.start);
                    float frac = 0.0f;
                    const int part = ui::layout::sliderHit(rr, g_touchHeldX, g_touchHeldY, frac);
                    if ((part == 1 || part == 2) && OSTicksToMilliseconds(OSGetTime() - heldSince) >= 400 &&
                        OSTicksToMilliseconds(OSGetTime() - heldLastFire) >= 70) {
                        dir = part == 2 ? 1 : -1;
                        coarse = true;
                        heldLastFire = OSGetTime();
                    }
                }
                if (dir != 0) {
                    if (slider) stepSlider(cur, dir, coarse);
                    else if (cur == SetCustomColours) customSlot = std::max(0, std::min(2, customSlot + dir));
                    else if (isShapeRow(cur)) pickChip(cur, std::max(0, std::min(ui::buttonShapeCount(shapeKindOf(cur)) - 1, cfg.buttonShape[shapeKindOf(cur)] + dir)));
                    else pickChip(SetAccent, std::max(0, std::min(accentCount() - 1, cfg.accent + dir)));
                    return;
                }
                if (!g_touchHeld && !(g_dpadHeld & (BTN_LEFT | BTN_RIGHT))) heldSince = 0;
                if (pressed & (BTN_LEFT | BTN_RIGHT)) return;
            }
        }

        const int page = ui::layout::visibleRows();
        if ((pressed & BTN_DOWN) && count > 0) f.selected = (f.selected + 1) % count;
        else if ((pressed & BTN_UP) && count > 0) {
            // (a long list that is only partly loaded does not wrap up to a row that is not the real last one)
            if (!(f.selected == 0 && f.moreToLoad())) f.selected = (f.selected - 1 + count) % count;
        }
        else if ((pressed & VPAD_BUTTON_R) && count > 0) f.selected = std::min(f.selected + page, count - 1);
        else if ((pressed & VPAD_BUTTON_L) && count > 0) f.selected = std::max(f.selected - page, 0);
        else if (pressed & BTN_LEFT) {
            sidebarFocused = true;
            Section cur = currentSection();
            for (size_t i = 0; i < secs.size(); i++) if (secs[i] == cur) sidebarCursor = (int)i;
        }
        else if ((pressed & VPAD_BUTTON_PLUS) && count > 0 && !settings) shuffle();
        else if ((pressed & VPAD_BUTTON_MINUS) && count > 0 && !settings) chooseMenuMusic(f.items[(size_t)f.selected]);
        else if ((pressed & VPAD_BUTTON_A) && count > 0) {
            if (settings) {
                activateSetting(setRows[(size_t)f.selected]);
            } else {
                const JellyfinItem picked = f.items[(size_t)f.selected]; // open() may reallocate the stack
                open(picked);
            }
        }
        else if (pressed & VPAD_BUTTON_B) {
            if (stack.size() > 1) {
                stack.pop_back();
                if (stack.size() == 1 && homeStale) reloadHome();
            } else {
                sidebarFocused = true;
            }
        }
    }

    // --- drawing ---

    void draw() const {
        if (!errorMessage.empty()) {
            ui::MessageModel m;
            m.title = loggedIn ? "Something went wrong" : "Can't reach the server";
            m.lines = {errorMessage, "",
                       loggedIn ? "Press B to go back."
                                : "Check that Jellyfin is running and that the Wii U and the server are on the "
                                  "same network."};
            m.hints = {{"B", loggedIn ? "Back" : "Retry"}};
            if (errorOffersSignIn) m.hints.push_back({"X", cfg.profiles.empty() ? "Sign in again" : "Profiles"});
            m.isError = true;
            ui::drawMessage(m);
        } else if (mode == Mode::Profiles) {
            ui::drawProfiles(profilesModel(true));
        } else if (mode == Mode::SignIn) {
            ui::LoginModel m;
            m.rows = {{"Server", loginServer, "192.168.1.100:8096  or  https://host", false, false, false},
                      {"Username", loginUser, "Your Jellyfin user", false, false, false},
                      {"Password", loginPassword, "Leave empty if you have none", true, false, false},
                      {"Sign in", "", "", false, true, true},
                      {"Use Quick Connect", "", "", false, true, false}};
            m.focused = loginFocus;
            m.message = loginMessage.empty()
                ? "Tip: Quick Connect signs in with a code approved from your phone -- no typing."
                : loginMessage;
            m.messageIsError = loginMessageIsError;
            m.hints = {{"A", loginFocus < 3 ? "Edit" : "Select"}, {"X", "Quick Connect"}};
            if (!cfg.profiles.empty()) m.hints.push_back({"B", "Profiles"});
            ui::drawLogin(m);
        } else if (mode == Mode::QuickConnect) {
            ui::QuickConnectModel m;
            m.code = quick.code;
            m.server = loginServer;
            m.message = quickMessage;
            m.messageIsError = quickMessageIsError;
            m.hints = {{"B", "Back"}};
            if (quickMessageIsError) m.hints.insert(m.hints.begin(), ui::Hint{"A", "New code"});
            ui::drawQuickConnect(m);
        } else {
            drawBrowserScreen();
        }
        if (cfg.snow) ui::drawSnow((double)OSTicksToMilliseconds(OSGetTime()) / 1000.0);
        ui::drawToast(toast, secondsSince(toastAt));
    }

    // How "add to playlist" is pressed on the controller used last (see input_map.h).
    static std::string addHintButton() {
        if (input::lastIsWiiRemote()) return "1+2"; // Wii Remote, with or without a Nunchuk
        return "R-stick up";
    }

    void drawBrowserScreen() const {
        const Frame& f = stack.back();
        const bool settings = (f.kind == Frame::Kind::Settings);
        ui::BrowserModel m;
        const std::vector<Section> secs = sections();
        const Section cur = currentSection();
        for (size_t i = 0; i < secs.size(); i++) {
            switch (secs[i]) {
                case Section::Home:     m.sidebar.push_back({"Home", ui::Icon::Home}); break;
                case Section::LiveTv:   m.sidebar.push_back({liveTvViewName, ui::Icon::LiveTv}); break;
                case Section::Search:   m.sidebar.push_back({"Search", ui::Icon::Search}); break;
                case Section::UpNext:
                    m.sidebar.push_back({g_upNext.empty() ? std::string("Up Next") : "Up Next (" + std::to_string(g_upNext.size()) + ")", ui::Icon::Music});
                    break;
                case Section::Settings: m.sidebar.push_back({"Settings", ui::Icon::Settings}); break;
                case Section::Help:     m.sidebar.push_back({"Help", ui::Icon::Help}); break;
            }
            if (!sidebarFocused && secs[i] == cur) m.sidebarSelected = (int)i;
        }
        if (sidebarFocused) m.sidebarSelected = sidebarCursor;
        m.sidebarFocused = sidebarFocused;
        m.footnote = cfg.username + " @ " + cfg.host;
        m.usageLabel = formatUsageShort(g_usage.totalSeconds(monotonicSeconds()));
        if (cfg.clock) {
            OSCalendarTime ct;
            OSTicksToCalendarTime(OSGetTime(), &ct);
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", ct.tm_hour, ct.tm_min);
            m.clock = buf;
        }

        m.title = f.title;
        if (stack.size() > 1 && !settings) {
            for (size_t i = 0; i < stack.size(); i++) {
                if (i) m.path += "  >  ";
                m.path += stack[i].title;
            }
        }
        m.selected = f.selected;

        if (settings) {
            for (SettingsRow row : settingsRows()) m.items.push_back(settingsEntry(row));
            m.tabs = {settingsPageName(0), settingsPageName(1)};
            m.tabSelected = setPage;
            m.hints = {{"A", "Change"}, {"L R", "Page"}};
            if (!m.items.empty()) {
                const SettingsRow cur = settingsRows()[(size_t)std::min(f.selected, (int)m.items.size() - 1)];
                if (isSliderRow(cur) || cur == SetCustomColours || cur == SetAccent || isShapeRow(cur)) m.hints.insert(m.hints.begin() + 1, ui::Hint{"< >", "Adjust"});
            }
            m.hints.push_back({"B", "Back"});
        } else {
            if (f.kind == Frame::Kind::LiveTv) m.emptyMessage = "No channels. Set up a tuner in Jellyfin.";
            if (f.kind == Frame::Kind::Search) m.emptyMessage = "No results for \"" + f.id + "\".";
            m.items.reserve(f.items.size());
            const ui::ListWindow visible =
                ui::computeListWindow((int)f.items.size(), f.selected, ui::layout::visibleRows());
            for (int i = 0; i < (int)f.items.size(); i++) {
                const JellyfinItem& item = f.items[(size_t)i];
                ui::ListEntry e;
                e.name = itemDisplayName(item);
                e.tag = itemTag(item);
                e.detail = itemDetail(item);
                e.icon = itemIcon(item);
                applyUserState(item, e);
                e.addable = (item.type == "Audio");
                if (i >= visible.start && i < visible.end) {
                    e.art = artFor(item, ui::layout::ROW_IMAGE_MAX_W, ui::layout::ROW_IMAGE_MAX_H, &e.artAspect);
                }
                m.items.push_back(e);
            }
            if (f.queried && f.total > (int)f.items.size()) m.total = f.total;
            if (f.tab >= 0) {
                for (int t = 0; t < library::MusicViewCount; t++) m.tabs.push_back(library::musicViewName(t));
                m.tabSelected = f.tab;
                m.path.clear(); // (the tabs sit where the breadcrumb would)
                static const char* const empties[] = {"No songs in this library.", "No artists in this library.",
                                                      "No albums in this library.",
                                                      "No playlists yet. Use the + button to make one."};
                m.emptyMessage = empties[f.tab];
            }
            m.fabs = fabsFor(f);
            m.fabFocus = sidebarFocused ? -1 : fabFocus;
            if (sortOpen && !m.fabs.empty()) {
                const library::OptionList ol = library::optionsFor(f.sortKind);
                const std::vector<int> ch = sortChoices(f);
                { ui::BrowserModel b = browserModelForHits(f); m.sortMenu = b.sortMenu; }
                m.sortMenu.current = 0;
                for (size_t i = 0; i < ch.size(); i++) if (ch[i] == f.sort.option) m.sortMenu.current = (int)i;
                m.sortMenu.cursor = sortCursor;
                m.sortMenu.descending = f.sort.descending;
                m.sortMenu.directionText = f.sort.descending ? ol.options[f.sort.option].descText : ol.options[f.sort.option].ascText;
            }
            if (sidebarFocused) {
                m.hints = {{"A", "Select"}, {"B", "Back to list"}};
            } else if (sortOpen) {
                m.hints = {{"A", "Choose"}, {"B", "Close"}};
            } else if (fabFocus >= 0 && !m.fabs.empty()) {
                m.hints = {{"A", m.fabs[(size_t)std::min(fabFocus, (int)m.fabs.size() - 1)] == ui::FabKind::Sort ? "Sort" : "New playlist"},
                           {"<", "Back to the list"}};
            } else {
                m.hints.push_back({"A", "Open"});
                m.hints.push_back({"B", stack.size() > 1 ? "Back" : "Menu"});
                m.hints.push_back({"X", "Search"});
                const JellyfinItem* cur = f.items.empty() ? nullptr : &f.items[(size_t)std::min(f.selected, (int)f.items.size() - 1)];
                bool song = cur && cur->type == "Audio";
                bool userItem = cur && !isHomeRow(*cur) && cur->type != "CollectionFolder" && cur->type != "UserView";
                if (userItem) m.hints.push_back({"Y", cur->favorite ? "Unfavourite" : "Favourite"});
                if (userItem && cur->type != "TvChannel") m.hints.push_back({"ZL", cur->played ? "Unwatched" : "Watched"});
                if (f.tab >= 0) m.hints.push_back({"L R", "View"});
                if (!m.fabs.empty()) m.hints.push_back({">", m.fabs.size() > 1 ? "Sort / New" : "Sort"});
                if (song) m.hints.push_back({addHintButton(), "Add to playlist"});
                if (canShuffle()) m.hints.push_back({"+", "Shuffle"});
                if (song) m.hints.push_back({"-", "Menu music"});
                if (m.hints.size() < 7) m.hints.push_back({"ZR", "Refresh"});
            }
        }
        ui::drawBrowser(m);
        if (helpOpen) {
            helpFrame++;
            ui::drawHelp(buildHelpModel());
        }
    }
};

int main(int argc, char** argv) {
    // Lets every existing OSReport(...) call in this codebase (video_output.cpp,
    // player.cpp, etc.) reach a PC over the network -- no serial cable needed.
    // WHBLogModuleInit() hooks the console log sink so plain OSReport calls get
    // captured, not just WHBLogPrintf; WHBLogUdpInit() then ships that over UDP
    // to whatever's listening (Aroma's logging module / notification plugin, or
    // wut's udplogserver.py, or `nc -ul 4405`) on the console's IP, port 4405.
    // Harmless to leave in a release build if nothing's listening.
    WHBLogModuleInit();
    WHBLogUdpInit();
    // The log can also go to a plain file on the SD card (see ufin_log.h) -- UDP
    // broadcast logging is silently dropped by a lot of home network
    // setups (firewalls, AP isolation, VLANs), so this is the fallback
    // that needs no network at all: pull sd:/wiiu/apps/ufin/ufin_log.txt off
    // the card after a run. It writes to the card, so it is OFF unless Settings >
    // Logging (config.json "logging") turns it on; until the config is read, early
    // lines are held in memory and written out only if it is on.

    WHBProcInit();
    input::init();
    OSReport("Ufin: build %s %s\n", __DATE__, __TIME__);
    if (!ui::gfx().init()) {
        // Nothing can be shown; wait for HOME so the system gets control back.
        OSReport("Ufin: graphics init failed, nothing to show\n");
        while (WHBProcIsRunning()) OSSleepTicks(OSMillisecondsToTicks(100));
        input::shutdown();
        WHBProcShutdown();
        UfinLogClose();
        WHBLogUdpDeinit();
        WHBLogModuleDeinit();
        return 0;
    }

    UfinConfig cfg;
    std::string configError;
    if (!loadConfigFromSD(cfg, configError)) {
        OSReport("Ufin: %s -- showing the sign-in screen\n", configError.c_str());
        // config.h still provides defaults for a developer build.
        if (std::string(UFIN_SERVER_HOST).find_first_not_of("x.") != std::string::npos) {
            cfg.host = UFIN_SERVER_HOST;
            cfg.port = UFIN_SERVER_PORT;
            cfg.username = UFIN_USERNAME;
            cfg.password = UFIN_PASSWORD;
        }
    }
    UfinLogSetEnabled(cfg.logging ? 1 : 0); // writes the held-back startup lines if logging is on
    g_usage.start(cfg.usageSeconds, monotonicSeconds()); // sidebar "Time used" counter
    if (cfg.deviceId.empty()) cfg.deviceId = "ufin-" + makePlaySessionId((uint64_t)OSGetTime(), 7).substr(0, 16);
    ui::gfx().setCrt(cfg.crt);

    // Start-up update check (on by default; Settings or Y on the prompt turn it off). It runs once, a
    // couple of seconds after sign-in, on a worker thread; its answer is handled in the loop below.
    bool updateCheckPending = true;      // starts once cfg.checkForUpdates is on (see the loop)
    int updateCheckFrames = 0;
    bool updateWorkerOurs = false;       // the worker thread is running for the start-up check
    bool updatePromptOpen = false;
    float updateShown = 0.0f;            // the notification bar's smoothed fill, 0..100
    ui::MessageModel updatePrompt;
    std::string updatePromptVersion;
    if (!cfg.checkForUpdates) OSReport("Ufin: update check at start-up: off (turned off in Settings / config.json)\n");

    registerTlsServers(cfg); // https servers saved before: every request to them goes over TLS
    JellyfinClient client(cfg.host, cfg.port);
    client.setDeviceId(cfg.deviceId);

    ImageCache::Backend imageBackend;
    imageBackend.fetch = [&client](const std::string& path, std::string& bytes) {
        // "//host:port/path": a picture on another server (a profile saved for a different Jellyfin). The user
        // pictures are public, so no sign-in is needed; https servers go through the same server table as ever.
        if (path.size() > 2 && path[0] == '/' && path[1] == '/') {
            const size_t slash = path.find('/', 2);
            const size_t colon = path.rfind(':', slash == std::string::npos ? path.size() : slash);
            if (slash == std::string::npos || colon == std::string::npos || colon < 2) return false;
            const std::string host = path.substr(2, colon - 2);
            const int port = atoi(path.substr(colon + 1, slash - colon - 1).c_str());
            HttpResponse resp = http_get(host, port, path.substr(slash));
            if (!resp.success) return false;
            bytes.swap(resp.body);
            return true;
        }
        return client.fetchBinary(path, bytes);
    };
    imageBackend.upload = [](const uint8_t* rgba, int w, int h) {
        const uint64_t tex = ui::uploadTexture(rgba, w, h);
        ui::glassNoteArtwork(tex, rgba, w, h); // faint artwork tint for Liquid Glass (no-op when off)
        return tex;
    };
    imageBackend.release = [](ImageCache::Handle h) {
        ui::glassForgetArtwork(h);
        ui::releaseTexture(h);
    };
    g_images = new ImageCache(imageBackend, 96);
    g_imageClient = &client;

    // Backdrops: same fetching, but no Liquid Glass artwork tint (they are not posters), and room
    // for only a handful of full-screen textures.
    ImageCache::Backend backdropBackend = imageBackend;
    backdropBackend.upload = [](const uint8_t* rgba, int w, int h) { return ui::uploadTexture(rgba, w, h); };
    backdropBackend.release = [](ImageCache::Handle h) { ui::releaseTexture(h); };
    g_backdrops = new ImageCache(backdropBackend, 4, BACKDROP_W);

    // Help window: controller models are .glb files in sd:/wiiu/apps/ufin/assets/models/.
    ui::setModelBackend([](const uint8_t* rgba, int w, int h) { return ui::uploadTexture(rgba, w, h); },
                        [](uint64_t h) { ui::releaseTexture(h); });

    App* app = new App(client);
    app->cfg = cfg;
    app->applyLook();
    app->stack.resize(1);
    app->stack[0].kind = Frame::Kind::Views;
    app->stack[0].title = "Libraries";
    app->startUp();

    RepeatState repeat;
    bool wasTouched = false;

    while (procRunningTracked()) {
        // GamePad + Wii Remotes / Pro Controllers, merged (see input.h).
        const input::State in = input::read();
        // Whatever the person does with the console is unpredictable: it seasons the TLS random pool (tls_conn.h).
        tlsconn::addEntropy(((uint64_t)(uint32_t)in.touchX << 32) ^ ((uint64_t)(uint32_t)in.touchY << 16) ^ in.hold ^ (uint64_t)OSGetTime());
        uint32_t pressed = withRepeat(in, repeat);
        g_dpadHeld = in.hold & (BTN_LEFT | BTN_RIGHT);
        g_touchHeld = in.touched && wasTouched; // (a finger that stays down; the first touch is the tap)
        g_touchHeldX = in.touchX;
        g_touchHeldY = in.touchY;
        bool tapped = in.touched && !wasTouched;
        float tapX = in.touchX, tapY = in.touchY;
        wasTouched = in.touched;

        // The update prompt, when there is one: over whatever screen this is, and it takes the input.
        if (updatePromptOpen) {
            if (pressed & VPAD_BUTTON_X) {
                app->cfg.skippedUpdateVersion = updatePromptVersion;
                app->saveConfig();
                updatePromptOpen = false;
            } else if (pressed & VPAD_BUTTON_Y) {
                app->cfg.checkForUpdates = false;
                app->saveConfig();
                updatePromptOpen = false;
            } else if (tapped || (pressed & (VPAD_BUTTON_A | VPAD_BUTTON_B))) {
                updatePromptOpen = false;
            } else {
                scrollCard(updatePrompt, pressed);
            }
        } else if (pressed || tapped) {
            app->handleInput(pressed, tapped, tapX, tapY);
        }
        app->pollQuickConnect();

        // Start once, a couple of seconds after launch, wherever the user is (the sign-in screen included);
        // turning the setting on later starts it too.
        if (updateCheckPending && app->cfg.checkForUpdates && ++updateCheckFrames >= 120) {
            updateCheckPending = false;
            updateWorkerOurs = startUpdateCheck();
        }
        // Its answer: shown the moment we are back in this loop, so never while a movie plays.
        if (updateWorkerOurs && !updatePromptOpen) {
            updatecheck::Outcome outcome;
            if (takeUpdateResult(outcome)) {
                updateWorkerOurs = false;
                if (outcome.kind == updatecheck::Outcome::Kind::Available && app->cfg.checkForUpdates) {
                    const std::string& latest = outcome.info.latestVersion;
                    if (latest == app->cfg.skippedUpdateVersion) {
                        OSReport("Ufin: update check: %s is available but the user skipped it\n", latest.c_str());
                    } else {
                        updatePrompt = ui::MessageModel();
                        updatePrompt.title = "Update available";
                        updatePrompt.lines = updatecheck::promptLines(outcome.info, ufin::kVersion);
                        updatePrompt.hints = {{"A", "Continue"}, {"X", "Don't remind me for this release"},
                                              {"Y", "Turn off update checks"}, {"Up Down", "Scroll"}};
                        updatePrompt.overlay = true;
                        updatePromptVersion = latest;
                        updatePromptOpen = true;
                        wasTouched = true; // a touch still held does not dismiss it
                    }
                }
            }
        }
        app->updateMusic();
        app->updateBackdrop();

        // Persist the use-time total every 30 minutes (and on exit, below) -- seldom, to spare the SD card.
        if (g_usage.dueForSave(monotonicSeconds())) {
            if (!app->cfg.host.empty()) app->saveConfig();
            g_usage.markSaved(monotonicSeconds());
        }

        // Paced by vsync: frame() waits for the previous flip.
        // A smoothed progress value for the "Checking for updates" notification (top centre, like the other notices).
        const bool checking = updateWorkerOurs && g_update.state.load() == 1;
        if (checking) {
            const float target = (float)g_update.progress.load();
            updateShown += (target - updateShown) * 0.08f;
        } else {
            updateShown = 0.0f;
        }
        drawFrame([&] {
            app->draw();
            if (checking) ui::drawProgressToast("Checking for updates", updateShown / 100.0f, app->toastActive() ? 84.0f : 24.0f);
            if (updatePromptOpen) ui::drawMessage(updatePrompt);
        });
    }

    if (g_update.state.load() == 1) { // do not leave a half-finished request behind
        g_update.cancel = true;
        for (int i = 0; i < 20 && g_update.state.load() == 1; i++) OSSleepTicks(OSMillisecondsToTicks(50));
    }
    VPADSetLcdMode(VPAD_CHAN_0, VPAD_LCD_ON); // never leave the GamePad dark
    if (!app->cfg.host.empty()) app->saveConfig(); // the use-time total, so short sessions count too
    delete app;      // stops the menu music
    ui::setScreenBackdrop(0, 0, 0, 0, 0); // the textures are about to go
    delete g_backdrops; // before graphics: releases its textures
    g_backdrops = nullptr;
    delete g_images; // before graphics: releases its textures
    g_images = nullptr;
    ui::gfx().shutdown();
    input::shutdown();
    WHBProcShutdown();
    UfinLogClose();
    WHBLogUdpDeinit();
    WHBLogModuleDeinit();
    return 0;
}
