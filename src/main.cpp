// Ufin -- browse Jellyfin libraries with the GamePad and play video,
// audio and Live TV via the media/ pipeline (HttpStreamIO -> Decoder ->
// VideoOutput/AudioOutput -> Player). Menus are drawn with the ui/ layer
// on top of OSScreen (ui/os_screen_display.h); video playback takes the
// display over with GX2 for its duration.

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>
#include <vpad/input.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "config.h"
#include "ufin_log.h"
#include "jellyfin_client.h"
#include "config_loader.h"
#include "item_labels.h"
#include "seek.h"
#include "media/player.h"
#include "media/video_output.h"
#include "ui/screens.h"
#include "ui/os_screen_display.h"
#include "ui/keyboard.h"

// One level of browsing. The whole path is kept on a stack with each
// level's list and selection, so B goes back instantly to exactly where
// you were instead of re-fetching (and losing your place).
struct Frame {
    enum class Kind { Views, Items, LiveTv, Search };
    Kind kind = Kind::Views;
    std::string id;     // parent id for Items, the search term for Search
    std::string title;  // shown in the breadcrumb
    std::vector<JellyfinItem> items;
    int selected = 0;
};

static OSScreenDisplay display;

// Buttons that count as up/down: D-pad and the left stick.
static const uint32_t BTN_UP = VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP;
static const uint32_t BTN_DOWN = VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN;
static const uint32_t BTN_PAGE_UP = VPAD_BUTTON_L | VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT;
static const uint32_t BTN_PAGE_DOWN = VPAD_BUTTON_R | VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT;

static void showMessage(const std::string& title, const std::vector<std::string>& lines,
                        const std::string& footer, bool isError = false) {
    ui::MessageScreenModel model;
    model.title = title;
    model.lines = lines;
    model.footer = footer;
    model.isError = isError;
    display.render([&](ui::Surface& s) { ui::drawMessageScreen(s, model); });
}

static void showError(const std::string& message) {
    showMessage("Ufin  |  Error",
                {message, "",
                 "Check config.json (host / port / credentials) and that the Wii U and the "
                 "Jellyfin server are on the same network."},
                "B: back", true);
}

static std::string breadcrumb(const std::vector<Frame>& stack) {
    if (stack.size() <= 1) return "Libraries";
    std::string out;
    for (size_t i = 1; i < stack.size(); i++) {
        if (!out.empty()) out += " > ";
        out += stack[i].title;
    }
    return out;
}

static void renderList(const std::vector<Frame>& stack) {
    const Frame& f = stack.back();
    ui::ListScreenModel model;
    model.title = "Ufin";
    model.location = breadcrumb(stack);
    model.selectedIndex = f.selected;
    model.footer = stack.size() > 1 ? "A: open   B: back   X: search   L/R: page   Y: refresh"
                                    : "A: open   X: search   L/R: page   Y: refresh";
    if (f.kind == Frame::Kind::LiveTv) model.emptyMessage = "No channels. Set up a tuner in Jellyfin.";
    if (f.kind == Frame::Kind::Search) model.emptyMessage = "No results for \"" + f.id + "\".";
    model.items.reserve(f.items.size());
    for (const JellyfinItem& item : f.items) {
        ui::ListEntry e;
        e.name = itemDisplayName(item);
        e.tag = itemTag(item);
        e.detail = itemDetail(item);
        model.items.push_back(e);
    }
    display.render([&](ui::Surface& s) { ui::drawListScreen(s, model); });
}

// (Re)loads a frame's items from the server. Keeps the selection in range.
static bool loadFrame(JellyfinClient& client, Frame& f) {
    bool ok = false;
    switch (f.kind) {
        case Frame::Kind::Views:  ok = client.getViews(f.items); break;
        case Frame::Kind::Items:  ok = client.getItems(f.id, f.items); break;
        case Frame::Kind::LiveTv: ok = client.getLiveTvChannels(f.items); break;
        case Frame::Kind::Search: ok = client.search(f.id, f.items); break;
    }
    if (f.selected >= (int)f.items.size()) f.selected = (int)f.items.size() - 1;
    if (f.selected < 0) f.selected = 0;
    return ok;
}

// Reads the GamePad for Player many times a second (which also keeps
// ProcUI serviced -- pressing HOME mid-playback otherwise leaves the
// system waiting on us) and turns presses into PlayerCommands:
//   B: stop    A: pause/resume    Left: -10 s    Right: +30 s
// Skips are collected for a moment before seeking, so pressing Right
// three times makes one +90 s restart of the transcode, not three.
struct PlaybackControl {
    static const int SKIP_BACK_SECONDS = 10;
    static const int SKIP_FORWARD_SECONDS = 30;
    static const int SEEK_COMMIT_MS = 600;

    bool canSeekAndPause = true;  // false for Live TV
    double durationSeconds = 0.0; // 0 = unknown (no upper clamp)
    const Player* player = nullptr;

    double pendingDelta = 0.0;
    OSTime lastSkipPress = 0;

    PlayerCommand poll() {
        if (!WHBProcIsRunning()) return PlayerCommand::stop();
        VPADStatus vpad;
        VPADReadError err;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
        if (err == VPAD_READ_SUCCESS) {
            if (vpad.trigger & VPAD_BUTTON_B) return PlayerCommand::stop();
            if (canSeekAndPause) {
                if (vpad.trigger & VPAD_BUTTON_A) return PlayerCommand::togglePause();
                if (vpad.trigger & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT)) {
                    pendingDelta -= SKIP_BACK_SECONDS;
                    lastSkipPress = OSGetTime();
                } else if (vpad.trigger & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT)) {
                    pendingDelta += SKIP_FORWARD_SECONDS;
                    lastSkipPress = OSGetTime();
                }
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

// Diagnostic: draws a flat magenta picture via the exact same
// shader/upload/present pipeline as real video, with no decode or
// streaming involved. Isolates whether the GX2 path can put anything on
// screen at all. B exits.
static void runGx2TestPattern() {
    display.hide();
    VideoOutput testOutput;
    if (testOutput.init(1280, 720, 16.0 / 9.0)) {
        bool testDone = false;
        while (!testDone && WHBProcIsRunning()) {
            testOutput.renderTestPattern();
            OSSleepTicks(OSMillisecondsToTicks(33));
            VPADStatus testVpad;
            VPADReadError testErr;
            VPADRead(VPAD_CHAN_0, &testVpad, 1, &testErr);
            if (testErr == VPAD_READ_SUCCESS && (testVpad.trigger & VPAD_BUTTON_B)) {
                testDone = true;
            }
        }
        testOutput.shutdown();
    }
    display.show();
}

// Sends Jellyfin's "still playing" progress reports from a background
// thread. They used to be sent from Player's onTick, which runs on the
// render loop: every report is a full blocking HTTP round trip (a fresh
// connection, then waiting for the server), so the picture froze for as
// long as Jellyfin took to answer, once a second. Now the render loop
// only stores the position, and this thread posts it every 10 seconds
// -- the interval Jellyfin's own clients use.
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

// Plays one item start to finish (or until B), handling the OSScreen/GX2
// hand-off, Live TV tuning and Jellyfin progress reporting. Returns the
// player result and fills errorMessage on PlayResult::Error.
static PlayResult playItem(JellyfinClient& client, const UfinConfig& cfg, const JellyfinItem& picked,
                           std::string& errorMessage) {
    const bool isAudio = (picked.type == "Audio");
    const bool isLive = (picked.type == "TvChannel");

    VideoStreamOptions videoOptions;
    videoOptions.videoBitrate = cfg.videoBitrate;
    videoOptions.profile = cfg.videoProfile;

    double durationSeconds = picked.runTimeTicks > 0 ? picked.runTimeTicks / 10000000.0 : 0.0;
    PlayOptions playOptions;
    PlaybackIds ids;

    showMessage(isLive ? "Ufin  |  Tuning" : "Ufin  |  Loading",
                {itemDisplayName(picked), "",
                 isLive ? "Asking the server to tune the channel and start the transcode..."
                        : "Waiting for the server to start streaming...",
                 "",
                 isLive ? "During playback: B stops."
                        : "During playback: A pauses, Left/Right skip back 10 s / forward 30 s, B stops."},
                "Please wait");

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
        // (see buildVideoStreamUrl for why), so the real shape of the
        // picture has to come from the item's metadata. If we can't get
        // it, assume 16:9.
        VideoInfo info;
        if (client.getVideoInfo(picked.id, info)) {
            videoOptions.mediaSourceId = info.mediaSourceId;
            if (info.runTimeTicks > 0) durationSeconds = info.runTimeTicks / 10000000.0;
        }
        playOptions.displayAspect = info.displayAspect > 0.0 ? info.displayAspect : 16.0 / 9.0;
    }

    ids.mediaSourceId = videoOptions.mediaSourceId;
    ids.liveStreamId = videoOptions.liveStreamId;
    ids.playSessionId = videoOptions.playSessionId;

    if (!isAudio) {
        // OSScreen and GX2 both drive the same display hardware; both
        // enabled at once caused a hard OSFatal hang on Cemu and real
        // hardware. Hide OSScreen for the duration of video playback and
        // show it again afterwards -- hide()/show() only toggle
        // OSScreenEnableEx, they don't tear down and rebuild OSScreen or
        // GX2's context (see the comment in os_screen_display.h for why
        // that distinction turned out to matter on real hardware). Audio-
        // only playback never touches GX2, so the Now Playing screen can
        // stay up.
        display.hide();
    }

    client.reportPlaybackStart(picked.id, ids);

    Player player;
    PlaybackControl control;
    control.canSeekAndPause = !isLive;
    control.durationSeconds = durationSeconds;
    control.player = &player;

    PlayResult result;
    double startAt = 0.0;
    {
    ProgressReporter reporter(client, picked.id, ids);
    while (true) {
        // Seeking restarts the transcode at the new position: the stream
        // itself can't be seeked, but Jellyfin starts one anywhere.
        videoOptions.startTimeTicks = (int64_t)(startAt * 10000000.0);
        StreamTarget target = isAudio
            ? client.buildAudioStreamUrl(picked.id, videoOptions.startTimeTicks)
            : client.buildVideoStreamUrl(picked.id, videoOptions);
        playOptions.startOffsetSeconds = startAt;

        result = player.play(target.host, target.port, target.path,
            [&]() { return control.poll(); },
            [&](double positionSeconds) {
                // Roughly once per second, on the render loop -- so
                // nothing slow here. The reporter thread tells Jellyfin;
                // for audio, redraw Now Playing.
                reporter.update(positionSeconds, player.isPaused());

                if (isAudio) {
                    ui::NowPlayingModel model;
                    model.title = itemDisplayName(picked);
                    model.subtitle = player.isPaused() ? "Paused" : "Audio  |  AAC transcode";
                    model.positionSeconds = positionSeconds;
                    model.durationSeconds = durationSeconds;
                    model.footer = "A: pause   Left/Right: -10 s / +30 s   B: stop";
                    display.render([&](ui::Surface& s) { ui::drawNowPlayingScreen(s, model); });
                }
            },
            playOptions);

        if (result != PlayResult::SeekRequested) break;
        startAt = player.seekTarget();
        reporter.update(startAt, false);
    }
    } // reporter stops here, before the final report

    client.reportPlaybackStopped(picked.id, (int64_t)(player.positionSeconds() * 10000000.0), ids);
    if (isLive) client.closeLiveStream(ids.liveStreamId);

    if (!isAudio) {
        display.show();
    }

    if (result == PlayResult::Error) {
        errorMessage = "Playback error: " + player.lastError();
    }
    return result;
}

// Up/down auto-repeat while held: first repeat after 400 ms, then every
// 70 ms -- scrolling a 500-movie library one press at a time is no fun.
struct RepeatState {
    uint32_t button = 0;
    OSTime pressedAt = 0;
    OSTime lastFire = 0;
};

static uint32_t withRepeat(const VPADStatus& vpad, RepeatState& rs) {
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
    // Also log to a plain file on the SD card (see ufin_log.h) -- UDP
    // broadcast logging is silently dropped by a lot of home network
    // setups (firewalls, AP isolation, VLANs), so this is the fallback
    // that needs no network at all. Pull
    // sd:/wiiu/apps/ufin/ufin_log.txt off the card after a run.
    UfinLogOpen();

    WHBProcInit();

    // GX2's context lives for the whole app from here on -- see the
    // comment on VideoOutput::initGX2Context() and the one at the top of
    // os_screen_display.h for why: tearing GX2 down and rebuilding it for
    // every playback/keyboard session (the original design) computed
    // correct frames that real hardware never actually displayed. Must
    // run before the very first display.init() below.
    VideoOutput::initGX2Context();

    display.init();

    UfinConfig cfg;
    std::string configError;
    bool usingDefaults = !loadConfigFromSD(cfg, configError);
    if (usingDefaults) {
        cfg.host = UFIN_SERVER_HOST;
        cfg.port = UFIN_SERVER_PORT;
        cfg.username = UFIN_USERNAME;
        cfg.password = UFIN_PASSWORD;
    }

    JellyfinClient client(cfg.host, cfg.port);

    {
        char buf[160];
        snprintf(buf, sizeof(buf), "Connecting to %s:%d ...", cfg.host.c_str(), cfg.port);
        std::vector<std::string> lines = {buf};
        if (usingDefaults) {
            lines.push_back("");
            lines.push_back(configError + " -- using the built-in config.h defaults.");
        }
        showMessage("Ufin", lines, "Please wait");
    }

    std::vector<Frame> stack(1);
    stack[0].kind = Frame::Kind::Views;
    stack[0].title = "Libraries";

    // Non-empty = an error screen is showing over the current list.
    std::string errorMessage;
    bool loggedIn = client.authenticate(cfg.username, cfg.password);
    if (!loggedIn) {
        errorMessage = client.lastError();
    } else if (!loadFrame(client, stack[0])) {
        errorMessage = client.lastError();
    }

    if (errorMessage.empty()) renderList(stack); else showError(errorMessage);

    VPADStatus vpad;
    VPADReadError vpadError;
    RepeatState repeat;

    while (WHBProcIsRunning()) {
        VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadError);
        if (vpadError != VPAD_READ_SUCCESS) {
            OSSleepTicks(OSMillisecondsToTicks(16));
            continue;
        }
        const uint32_t pressed = withRepeat(vpad, repeat);
        bool changed = false;

        if (!errorMessage.empty()) {
            // Error screen: B dismisses it. If we never got in, B retries
            // the login instead (e.g. after starting the server).
            if (pressed & VPAD_BUTTON_B) {
                errorMessage.clear();
                if (!loggedIn) {
                    showMessage("Ufin", {"Retrying..."}, "Please wait");
                    loggedIn = client.authenticate(cfg.username, cfg.password);
                    if (!loggedIn || !loadFrame(client, stack[0])) errorMessage = client.lastError();
                }
                changed = true;
            }
        } else if (pressed & VPAD_BUTTON_ZR) {
            runGx2TestPattern();
            changed = true;
        } else {
            Frame& f = stack.back();
            const int count = (int)f.items.size();
            const int page = 10;

            if ((pressed & BTN_DOWN) && count > 0) {
                f.selected = (f.selected + 1) % count;
                changed = true;
            } else if ((pressed & BTN_UP) && count > 0) {
                f.selected = (f.selected - 1 + count) % count;
                changed = true;
            } else if ((pressed & BTN_PAGE_DOWN) && count > 0) {
                f.selected = std::min(f.selected + page, count - 1);
                changed = true;
            } else if ((pressed & BTN_PAGE_UP) && count > 0) {
                f.selected = std::max(f.selected - page, 0);
                changed = true;
            } else if (pressed & VPAD_BUTTON_X) {
                // Search. swkbd draws with GX2, so OSScreen steps aside
                // like it does for video.
                display.hide();
                std::string term, keyboardError;
                bool entered = ui::promptKeyboard(u"Search movies, shows and music", term, keyboardError);
                display.show();
                if (entered) {
                    Frame results;
                    results.kind = Frame::Kind::Search;
                    results.id = term;
                    results.title = "Search: " + term;
                    showMessage("Ufin", {"Searching for \"" + term + "\" ..."}, "Please wait");
                    if (loadFrame(client, results)) {
                        stack.push_back(results);
                    } else {
                        errorMessage = client.lastError();
                    }
                } else if (!keyboardError.empty()) {
                    errorMessage = "Keyboard: " + keyboardError;
                }
                changed = true;
            } else if (pressed & VPAD_BUTTON_Y) {
                showMessage("Ufin", {"Refreshing..."}, "Please wait");
                if (!loadFrame(client, f)) errorMessage = client.lastError();
                changed = true;
            } else if ((pressed & VPAD_BUTTON_A) && count > 0) {
                // Copy: playItem() may take minutes and the stack can
                // reallocate when we push.
                const JellyfinItem picked = f.items[(size_t)f.selected];

                if (isPlayableItem(picked)) {
                    std::string error;
                    if (playItem(client, cfg, picked, error) == PlayResult::Error) {
                        errorMessage = error;
                    }
                } else {
                    Frame next;
                    next.kind = isLiveTvView(picked) ? Frame::Kind::LiveTv : Frame::Kind::Items;
                    next.id = picked.id;
                    next.title = picked.name;
                    showMessage("Ufin", {"Opening " + picked.name + " ..."}, "Please wait");
                    if (loadFrame(client, next)) {
                        stack.push_back(next);
                    } else {
                        errorMessage = client.lastError();
                    }
                }
                changed = true;
            } else if ((pressed & VPAD_BUTTON_B) && stack.size() > 1) {
                stack.pop_back();
                changed = true;
            }
        }

        if (changed) {
            if (errorMessage.empty()) renderList(stack); else showError(errorMessage);
        }

        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    display.shutdown();
    VideoOutput::shutdownGX2Context();
    WHBProcShutdown();
    UfinLogClose();
    WHBLogUdpDeinit();
    WHBLogModuleDeinit();
    return 0;
}
