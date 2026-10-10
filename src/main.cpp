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
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "ufin_log.h"
#include "config_loader.h"
#include "h264_profile.h"
#include "quality_presets.h"
#include "login_utils.h"
#include "image_cache.h"
#include "input.h"
#include "item_labels.h"
#include "jellyfin_client.h"
#include "play_queue.h"
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
};

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
        case Frame::Kind::Items:  ok = client.getItems(f.id, f.items); break;
        case Frame::Kind::LiveTv: ok = client.getLiveTvChannels(f.items); break;
        case Frame::Kind::Search: ok = client.search(f.id, f.items); break;
        case Frame::Kind::Settings: ok = true; break; // built by the app, not the server
    }
    if (f.selected >= (int)f.items.size()) f.selected = (int)f.items.size() - 1;
    if (f.selected < 0) f.selected = 0;
    return ok;
}

// --- playback ---

// Volume the user last set with the on-screen slider (see
// ui::npVolumeRect / PlaybackControl::poll). Session-only -- kept here
// rather than in UfinConfig -- so it carries across tracks/seeks/movies
// for the life of the app without needing a config write on every drag.
static float g_playbackVolume = 1.0f;

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

    // Music visualizer (- during audio playback). Video already owns -
    // for the GamePad-screen toggle above, so this only ever applies
    // when isVideo is false -- the two can't conflict.
    bool visualizerOn = false;

    bool hasTrackChoice() const {
        return tracks && (tracks->audioTracks.size() > 1 || !tracks->subtitleTracks.empty());
    }

    void setGamepadScreen(bool off) {
        gamepadOff = off;
        VPADSetLcdMode(VPAD_CHAN_0, off ? VPAD_LCD_OFF : VPAD_LCD_ON);
        notice = off ? "GamePad screen off  (- to turn it back on)" : "GamePad screen on";
        noticeAt = OSGetTime();
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
        } else if (t & VPAD_BUTTON_MINUS) {
            visualizerOn = !visualizerOn;
        }

        // GamePad touch: the transport buttons drawn under the progress
        // bar on any Now Playing screen -- music (with or without the
        // visualizer) and the movie remote (TV-only mode). Nothing else
        // (Live TV, the normal video HUD) ever draws that screen, so
        // touches are only hit-tested here in exactly those two cases.
        // Edge-detected via touchLastHit so a held finger fires once,
        // same as a real button's press-edge `t` above.
        const bool nowPlayingShown = !isVideo || tvOnlyMode;
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
            if (t & VPAD_BUTTON_A) return PlayerCommand::togglePause();
            if (t & BTN_LEFT) {
                pendingDelta -= SKIP_BACK_SECONDS;
                lastSkipPress = OSGetTime();
            } else if (t & BTN_RIGHT) {
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

// Audio / subtitle language carried from one episode to the next.
struct TrackPrefs {
    bool set = false;
    std::string audioLanguage;
    std::string subtitleLanguage;
    bool subtitlesOff = true;
};

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
    std::string queueText;
    std::string nextText;
    int trackStep = 0;       // out: -1 previous, +1 next, 0 = ended/stopped normally
    double endPosition = 0;  // out: where playback was when it ended
};

// Plays one item start to finish (or until B), handling Live TV tuning,
// seeks (each one restarts the transcode), the on-screen HUD / Now
// Playing screen, and Jellyfin progress reporting. Returns the player
// result and fills errorMessage on PlayResult::Error.
static PlayResult playItem(JellyfinClient& client, const UfinConfig& cfg, const JellyfinItem& picked,
                           std::string& errorMessage, QueueInfo* queue = nullptr, double startAtSeconds = 0.0,
                           TrackPrefs* prefs = nullptr) {
    const bool isAudio = (picked.type == "Audio");
    const bool isLive = (picked.type == "TvChannel");
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

    ids.mediaSourceId = videoOptions.mediaSourceId;
    ids.liveStreamId = videoOptions.liveStreamId;
    ids.playSessionId = videoOptions.playSessionId;

    client.reportPlaybackStart(picked.id, ids);

    Player player;
    PlaybackControl control;
    playOptions.initialVolume = control.volume; // carries the user's last-set volume in
    control.canSeekAndPause = !isLive;
    control.inQueue = (queue != nullptr);
    control.isVideo = !isAudio;
    if (!isAudio && !isLive) {
        control.tracks = &info;
        control.audioIndex = info.defaultAudioIndex;
        control.subtitleIndex = info.defaultSubtitleIndex;
        if (prefs && prefs->set) {
            // Same language as the last episode, when this one has it.
            control.audioIndex = trackForLanguage(info.audioTracks, prefs->audioLanguage, info.defaultAudioIndex);
            control.subtitleIndex = prefs->subtitlesOff ? -1
                : trackForLanguage(info.subtitleTracks, prefs->subtitleLanguage, info.defaultSubtitleIndex);
            videoOptions.audioStreamIndex = control.audioIndex;
            videoOptions.subtitleStreamIndex = control.subtitleIndex;
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
        np.pressedButton = control.touchLastHit; // Liquid Glass pressed look only
            np.title = title;
            np.subtitle = movieSubtitle;
            np.positionSeconds = player.positionSeconds();
            np.durationSeconds = durationSeconds;
            np.paused = player.isPaused();
            np.volume = control.volume;
            if (control.pendingDelta != 0.0) {
                // Same wording as the audio Now Playing screen's pending-skip
                // subtitle below, for consistency.
                char buf[64];
                snprintf(buf, sizeof(buf), "Skipping %+d s ...", (int)control.pendingDelta);
                np.subtitle = buf;
            }
            np.hints = {{"A", np.paused ? "Play" : "Pause"}, {"<", "-10 s"}, {">", "+30 s"}};
            np.hints.push_back({"+", "Normal view"});
            np.hints.push_back({"B", "Stop"});
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
            vm.bandCount = media::AudioVisualizer::BANDS;
            for (int i = 0; i < vm.bandCount && i < ui::VisualizerModel::MAX_BANDS; i++) {
                vm.bands[i] = visualizer.levels()[i];
            }
            vm.hints = {{"A", vm.paused ? "Play" : "Pause"}, {"< >", "Seek"}};
            if (queue) vm.hints.push_back({"L R", "Prev / Next"});
            vm.hints.push_back({"-", "Now Playing"});
            vm.hints.push_back({"B", "Stop"});

            // The visualizer is a TV-only presentation. Keep the normal
            // Now Playing UI on the GamePad so controls, progress, artwork
            // and the visualizer toggle remain available there.
            ui::NowPlayingModel np;
        np.pressedButton = control.touchLastHit; // Liquid Glass pressed look only
            np.title = title;
            np.subtitle = picked.seriesName.empty() ? "Audio" : picked.seriesName;
            np.positionSeconds = player.positionSeconds();
            np.durationSeconds = durationSeconds;
            np.paused = player.isPaused();
            np.volume = control.volume;
            if (queue) {
                np.queueText = queue->queueText;
                np.nextText = queue->nextText;
            }
            if (control.pendingDelta != 0.0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Skipping %+d s ...", (int)control.pendingDelta);
                np.subtitle = buf;
            }
            np.hints = {{"A", np.paused ? "Play" : "Pause"}, {"< >", "Seek"}};
            if (queue) np.hints.push_back({"L R", "Prev / Next"});
            np.hints.push_back({"-", "Visualizer"});
            np.hints.push_back({"B", "Stop"});
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
        np.pressedButton = control.touchLastHit; // Liquid Glass pressed look only
        np.title = title;
        np.subtitle = picked.seriesName.empty() ? "Audio" : picked.seriesName;
        np.positionSeconds = player.positionSeconds();
        np.durationSeconds = durationSeconds;
        np.paused = player.isPaused();
        np.volume = control.volume;
        if (queue) {
            np.queueText = queue->queueText;
            np.nextText = queue->nextText;
        }
        if (control.pendingDelta != 0.0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Skipping %+d s ...", (int)control.pendingDelta);
            np.subtitle = buf;
        }
        np.hints = {{"A", np.paused ? "Play" : "Pause"}, {"< >", "Seek"}};
        if (queue) np.hints.push_back({"L R", "Prev / Next"});
        np.hints.push_back({"-", "Visualizer"});
        np.hints.push_back({"B", "Stop"});
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
// and the queue ends after the last track.
static PlayResult playQueue(JellyfinClient& client, const UfinConfig& cfg, PlayQueue& queue,
                            std::string& errorMessage) {
    while (!queue.empty() && WHBProcIsRunning()) {
        QueueInfo info;
        char buf[64];
        snprintf(buf, sizeof(buf), "Track %u of %u%s", (unsigned)(queue.position() + 1),
                 (unsigned)queue.size(), queue.shuffled() ? "  |  Shuffle" : "");
        info.queueText = buf;
        if (const JellyfinItem* next = queue.peekNext()) info.nextText = "Next: " + itemDisplayName(*next);

        PlayResult result = playItem(client, cfg, queue.current(), errorMessage, &info);
        if (result == PlayResult::Error) return result;

        if (info.trackStep < 0) {
            if (info.endPosition <= 3.0) queue.previous(); // else: replay this track
            continue;
        }
        if (info.trackStep > 0 || result == PlayResult::Completed) {
            if (!queue.next()) return PlayResult::Completed;
            continue;
        }
        return result; // B
    }
    return PlayResult::Stopped;
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

// --- the app ---

enum class Section { Home, LiveTv, Search, Settings, Help };
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
        loginServer = formatServerAddress(cfg.host, cfg.port);
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
        showBusy("Connecting", {formatServerAddress(cfg.host, cfg.port)});
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
        loginServer = formatServerAddress(host, port);
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
        showBusy("Connecting", {p.name, formatServerAddress(p.host, p.port)});
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
                if (g_images && g_imageClient && !p.userId.empty() && p.host == client.host() && p.port == client.port()) {
                    t.picture = g_images->get("/Users/" + p.userId + "/Images/Primary?maxWidth=256&maxHeight=256&quality=90&format=Jpg",
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
        if (!parseServerAddress(loginServer, host, port, err)) {
            loginMessage = err;
            loginMessageIsError = true;
            loginFocus = RowServer;
            return false;
        }
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

    enum SettingsRow { SetMusic, SetCrt, SetAccent, SetUnlockRainbow, SetLight, SetBackground, SetBackdrops, SetGlass, SetSnow, SetClock, SetLogging,
                       SetQuality, SetH264Profile, SetAutoplay, SetGamepadScreen, SetStrictVideoGeometry, SetTestPicture, SetAccount,
                       SetAbout };

    std::vector<SettingsRow> settingsRows() const {
        std::vector<SettingsRow> rows = {SetMusic, SetAutoplay, SetGamepadScreen, SetStrictVideoGeometry, SetQuality,
                SetH264Profile, SetAccent};
        if (cfg.rainbowEasterEgg) rows.push_back(SetUnlockRainbow);
        rows.insert(rows.end(), {SetLight, SetBackground, SetBackdrops, SetGlass, SetCrt, SetSnow, SetClock, SetLogging,
                                 SetTestPicture, SetAccount, SetAbout});
        return rows;
    }

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
            case SetAccent:
                e.name = "Accent colour";
                e.icon = ui::Icon::Palette;
                e.tag = ui::accentName((ui::Accent)cfg.accent);
                e.detail = cfg.rainbowUnlocked ? "A: next colour (Rainbow slowly cycles through them all)"
                                               : "A: next colour";
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
            case SetGlass:
                e.name = "Liquid Glass UI";
                e.icon = ui::Icon::Sparkle;
                e.tag = onOff(cfg.liquidGlass);
                e.detail = !cfg.liquidGlass
                    ? "Frosted, rounded panels with soft Wii U-style lighting"
                    : (ui::glassBlurAvailable() ? "Frosted, rounded panels with soft Wii U-style lighting"
                                                : "Glass panels without blur (blur unavailable on this run)");
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
                e.detail = formatServerAddress(cfg.host, cfg.port);
                break;
            case SetAbout:
                e.name = "About Ufin";
                e.icon = ui::Icon::Info;
                e.tag = "1.1.0";
                // Build time: makes an old build on the SD card easy to spot.
                e.detail = std::string("A Jellyfin client for the Wii U  -  built ") + __DATE__ + " " + __TIME__;
                break;
        }
        return e;
    }

    bool glassApplied = false; // whether ui::gfx() currently holds the glass blur resources

    // Applies the look & feel settings to the renderer.
    void applyLook() {
        if (cfg.accent >= accentCount()) cfg.accent = 0;
        ui::setAccent((ui::Accent)cfg.accent);
        if (cfg.backgroundEffect < 0 || cfg.backgroundEffect >= (int)ui::Background::COUNT) cfg.backgroundEffect = 0;
        ui::setBackgroundEffect((ui::Background)cfg.backgroundEffect);
        ui::setLightMode(cfg.lightMode);
        // Liquid Glass: the style flag always follows the setting; the GPU
        // side (two tiny textures) is only allocated/released when the
        // setting actually changes. A failed allocation leaves flat glass.
        ui::setGlass(cfg.liquidGlass);
        if (cfg.liquidGlass != glassApplied) {
            glassApplied = cfg.liquidGlass;
            const bool ok = ui::gfx().setGlass(cfg.liquidGlass, cfg.glassDebug, cfg.glassRefraction, cfg.glassRefractionStage);
            if (cfg.liquidGlass) say(ok ? "Liquid Glass UI on" : "Liquid Glass: blur unavailable, using flat glass");
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
            case SetAccent:
                cfg.accent = (cfg.accent + 1) % accentCount();
                say(std::string("Accent: ") + ui::accentName((ui::Accent)cfg.accent));
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
            case SetGlass:
                cfg.liquidGlass = !cfg.liquidGlass;
                break; // applyLook() (below) does the rest, and reports if the blur could not be set up
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
            case SetAbout:
                // Tap it seven times... (like a phone's build number). Reveals
                // the Rainbow accent toggle in Settings for good, rather than
                // switching it on outright -- see SetUnlockRainbow above.
                if (cfg.rainbowEasterEgg) {
                    say("Ufin 1.1.0 -- UI inspired by CafeMP. Thanks for watching!");
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
                    say("Ufin 1.1.0 -- UI inspired by CafeMP");
                    return;
                }
                break;
        }
        applyLook();
        saveConfig();
    }

    void openSettings() {
        if (stack.back().kind == Frame::Kind::Settings) return;
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
    // autoplays the next one (keeping its audio / subtitle language).
    void playVideo(const JellyfinItem& picked) {
        double startAt = 0.0;
        if (shouldOfferResume(picked)) {
            std::string at = ui::formatTime(picked.positionTicks / 10000000.0);
            int c = choose(itemDisplayName(picked), "You stopped at " + at,
                           {"Resume from " + at, "Start from the beginning"}, &picked);
            if (c < 0) return;
            if (c == 0) startAt = picked.positionTicks / 10000000.0;
        }
        stopMusic();
        TrackPrefs prefs;
        JellyfinItem current = picked;
        while (WHBProcIsRunning()) {
            std::string error;
            PlayResult r = playItem(client, cfg, current, error, nullptr, startAt, &prefs);
            if (r == PlayResult::Error) {
                errorMessage = error;
                break;
            }
            if (r != PlayResult::Completed || current.type != "Episode" || !cfg.autoplayNext ||
                current.seriesId.empty()) {
                break;
            }
            std::vector<JellyfinItem> episodes;
            showBusy("Finding the next episode");
            if (!client.getEpisodes(current.seriesId, episodes)) break;
            const JellyfinItem* next = findNextEpisode(episodes, current.id);
            if (!next) break;
            JellyfinItem nextCopy = *next;
            if (!upNext(nextCopy)) break;
            current = nextCopy;
            startAt = 0.0;
        }
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
            stopMusic();
            size_t start = 0;
            PlayQueue queue(audioTracks(f.items, (size_t)f.selected, start), start, false, 0);
            std::string error;
            if (playQueue(client, cfg, queue, error) == PlayResult::Error) errorMessage = error;
            refreshAfterPlayback();
        } else if (isPlayableItem(picked)) {
            playVideo(picked);
        } else {
            Frame next;
            next.kind = isLiveTvView(picked) ? Frame::Kind::LiveTv : Frame::Kind::Items;
            next.id = picked.id;
            next.title = picked.name;
            next.backdropItemId = picked.backdropItemId;
            next.backdropTag = picked.backdropTag;
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
        std::string error;
        if (playQueue(client, cfg, queue, error) == PlayResult::Error) errorMessage = error;
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

        Frame& f = stack.back();
        const bool settings = (f.kind == Frame::Kind::Settings);
        const std::vector<SettingsRow> setRows = settingsRows();
        const int count = settings ? (int)setRows.size() : (int)f.items.size();
        if (f.selected >= count) f.selected = count > 0 ? count - 1 : 0;
        const std::vector<Section> secs = sections();

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
            ui::Hit hit = ui::hitTestBrowser(tapX, tapY, (int)secs.size(), w);
            if (hit.kind == ui::Hit::Kind::Sidebar) {
                sidebarCursor = hit.index;
                activateSection(secs[(size_t)hit.index]);
                return;
            }
            if (hit.kind == ui::Hit::Kind::Row) {
                sidebarFocused = false;
                f.selected = hit.index;
                if (settings) activateSetting(setRows[(size_t)hit.index]);
                else open(f.items[(size_t)hit.index]);
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

        const int page = ui::layout::visibleRows();
        if ((pressed & BTN_DOWN) && count > 0) f.selected = (f.selected + 1) % count;
        else if ((pressed & BTN_UP) && count > 0) f.selected = (f.selected - 1 + count) % count;
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
            m.rows = {{"Server", loginServer, "e.g. 192.168.1.100:8096", false, false, false},
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
            m.hints = {{"A", "Change"}, {"B", "Back"}};
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
                if (i >= visible.start && i < visible.end) {
                    e.art = artFor(item, ui::layout::ROW_IMAGE_MAX_W, ui::layout::ROW_IMAGE_MAX_H, &e.artAspect);
                }
                m.items.push_back(e);
            }
            if (sidebarFocused) {
                m.hints = {{"A", "Select"}, {"B", "Back to list"}};
            } else {
                m.hints.push_back({"A", "Open"});
                m.hints.push_back({"B", stack.size() > 1 ? "Back" : "Menu"});
                m.hints.push_back({"X", "Search"});
                const JellyfinItem* cur = f.items.empty() ? nullptr : &f.items[(size_t)std::min(f.selected, (int)f.items.size() - 1)];
                bool song = cur && cur->type == "Audio";
                bool userItem = cur && !isHomeRow(*cur) && cur->type != "CollectionFolder" && cur->type != "UserView";
                if (userItem) m.hints.push_back({"Y", cur->favorite ? "Unfavourite" : "Favourite"});
                if (userItem && cur->type != "TvChannel") m.hints.push_back({"ZL", cur->played ? "Unwatched" : "Watched"});
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

    JellyfinClient client(cfg.host, cfg.port);
    client.setDeviceId(cfg.deviceId);

    ImageCache::Backend imageBackend;
    imageBackend.fetch = [&client](const std::string& path, std::string& bytes) {
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
        uint32_t pressed = withRepeat(in, repeat);
        bool tapped = in.touched && !wasTouched;
        float tapX = in.touchX, tapY = in.touchY;
        wasTouched = in.touched;

        if (pressed || tapped) app->handleInput(pressed, tapped, tapX, tapY);
        app->pollQuickConnect();
        app->updateMusic();
        app->updateBackdrop();

        // Persist the use-time total every 30 minutes (and on exit, below) -- seldom, to spare the SD card.
        if (g_usage.dueForSave(monotonicSeconds())) {
            if (!app->cfg.host.empty()) app->saveConfig();
            g_usage.markSaved(monotonicSeconds());
        }

        // Paced by vsync: frame() waits for the previous flip.
        drawFrame([&] { app->draw(); });
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
