// Ufin -- milestone 1: connect to Jellyfin, browse libraries and folders
// with the GamePad, no video playback yet.
//
// Rendering uses OSScreen directly (ClearBuffer + PutFont + FlipBuffers)
// instead of WHBLogConsole. WHBLogConsole is a scrolling debug-log style
// console and, as it turns out, doesn't actually support clearing what's
// on screen -- Free()+Init() left old and new content both visible at
// once. OSScreen is the lower-level API meant for exactly this: a
// redrawable grid of text with an explicit clear each frame.

#include <whb/proc.h>
#include <coreinit/screen.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/cache.h>
#include <vpad/input.h>
#include <coreinit/thread.h>

#include <vector>
#include <string>
#include <cstdio>

#include "config.h"
#include "jellyfin_client.h"

enum class Screen { CONNECTING, ERROR_SCREEN, VIEWS, ITEMS };

struct NavState {
    Screen screen = Screen::CONNECTING;
    std::vector<JellyfinItem> currentList;
    int selectedIndex = 0;
    std::string currentParentName;
    std::string errorMessage;
};

// Conservative on-screen limits. If text looks cut off or misplaced on
// real hardware, these are the first numbers to tune -- I don't have a
// way to verify the real character-grid size of OSScreen output without
// testing on console.
static const int MAX_COLS = 78;
static const int MAX_VISIBLE_ITEMS = 14;
static const int LIST_START_ROW = 3;

static void* tvBuffer = nullptr;
static void* drcBuffer = nullptr;

static void initScreen() {
    OSScreenInit();

    uint32_t tvSize = OSScreenGetBufferSizeEx(SCREEN_TV);
    uint32_t drcSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

    tvBuffer = MEMAllocFromDefaultHeapEx(tvSize, 0x100);
    drcBuffer = MEMAllocFromDefaultHeapEx(drcSize, 0x100);

    OSScreenSetBufferEx(SCREEN_TV, tvBuffer);
    OSScreenSetBufferEx(SCREEN_DRC, drcBuffer);

    OSScreenEnableEx(SCREEN_TV, TRUE);
    OSScreenEnableEx(SCREEN_DRC, TRUE);
}

static void shutdownScreen() {
    OSScreenEnableEx(SCREEN_TV, FALSE);
    OSScreenEnableEx(SCREEN_DRC, FALSE);
    if (tvBuffer) MEMFreeToDefaultHeap(tvBuffer);
    if (drcBuffer) MEMFreeToDefaultHeap(drcBuffer);
    tvBuffer = nullptr;
    drcBuffer = nullptr;
}

static std::string truncate(const std::string& s, int maxLen) {
    if ((int)s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + "...";
}

// Draws one full frame: clears both screens, writes out `lines` starting
// at row 0, then flips. This is a real clear every call -- no leftover
// content from the previous frame.
static void present(const std::vector<std::string>& lines) {
    OSScreenClearBufferEx(SCREEN_TV, 0x000000FF);
    OSScreenClearBufferEx(SCREEN_DRC, 0x000000FF);

    for (size_t row = 0; row < lines.size(); row++) {
        OSScreenPutFontEx(SCREEN_TV, 0, (int32_t)row, lines[row].c_str());
        OSScreenPutFontEx(SCREEN_DRC, 0, (int32_t)row, lines[row].c_str());
    }

    DCFlushRange(tvBuffer, OSScreenGetBufferSizeEx(SCREEN_TV));
    DCFlushRange(drcBuffer, OSScreenGetBufferSizeEx(SCREEN_DRC));

    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);
}

static void renderList(const NavState& nav) {
    std::vector<std::string> lines;

    if (nav.screen == Screen::CONNECTING) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Ufin - connecting to %s:%d ...",
                 UFIN_SERVER_HOST, UFIN_SERVER_PORT);
        lines.push_back(buf);
    } else if (nav.screen == Screen::ERROR_SCREEN) {
        lines.push_back("Ufin - error:");
        lines.push_back(truncate(nav.errorMessage, MAX_COLS));
        lines.push_back("");
        lines.push_back("Check config.h (host/port/credentials) and that");
        lines.push_back("the Wii U and Jellyfin server are on the same network.");
        lines.push_back("");
        lines.push_back("Press B to go back.");
    } else {
        std::string header = "Ufin  |  " + (nav.currentParentName.empty() ?
                              std::string("Libraries") : nav.currentParentName);
        lines.push_back(truncate(header, MAX_COLS));
        lines.push_back("D-pad Up/Down: move   A: open   B: back");
        lines.push_back("--------------------------------------------------------------");

        if (nav.currentList.empty()) {
            lines.push_back("(empty)");
        } else {
            int total = (int)nav.currentList.size();
            int windowStart = 0;
            if (total > MAX_VISIBLE_ITEMS) {
                windowStart = nav.selectedIndex - MAX_VISIBLE_ITEMS / 2;
                if (windowStart < 0) windowStart = 0;
                if (windowStart > total - MAX_VISIBLE_ITEMS) {
                    windowStart = total - MAX_VISIBLE_ITEMS;
                }
            }
            int windowEnd = std::min(total, windowStart + MAX_VISIBLE_ITEMS);

            if (windowStart > 0) {
                lines.push_back("  ^ more above ^");
            }

            for (int i = windowStart; i < windowEnd; i++) {
                const char* cursor = (i == nav.selectedIndex) ? ">" : " ";
                std::string entry = std::string(cursor) + " " + nav.currentList[i].name +
                                     "  [" + nav.currentList[i].type + "]";
                lines.push_back(truncate(entry, MAX_COLS));
            }

            if (windowEnd < total) {
                lines.push_back("  v more below v");
            }
        }
    }

    present(lines);
}

int main(int argc, char** argv) {
    WHBProcInit();
    initScreen();

    JellyfinClient client(UFIN_SERVER_HOST, UFIN_SERVER_PORT);

    NavState nav;
    std::vector<std::pair<std::string, std::string>> navStack;

    renderList(nav);

    if (!client.authenticate(UFIN_USERNAME, UFIN_PASSWORD)) {
        nav.screen = Screen::ERROR_SCREEN;
        nav.errorMessage = client.lastError();
        renderList(nav);
    } else if (!client.getViews(nav.currentList)) {
        nav.screen = Screen::ERROR_SCREEN;
        nav.errorMessage = client.lastError();
        renderList(nav);
    } else {
        nav.screen = Screen::VIEWS;
        nav.selectedIndex = 0;
        renderList(nav);
    }

    VPADStatus vpad;
    VPADReadError vpadError;

    while (WHBProcIsRunning()) {
        VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadError);
        if (vpadError != VPAD_READ_SUCCESS) {
            OSSleepTicks(OSMillisecondsToTicks(16));
            continue;
        }

        bool changed = false;

        if (nav.screen == Screen::VIEWS || nav.screen == Screen::ITEMS) {
            if (vpad.trigger & VPAD_BUTTON_DOWN) {
                if (!nav.currentList.empty()) {
                    nav.selectedIndex = (nav.selectedIndex + 1) % (int)nav.currentList.size();
                    changed = true;
                }
            } else if (vpad.trigger & VPAD_BUTTON_UP) {
                if (!nav.currentList.empty()) {
                    nav.selectedIndex--;
                    if (nav.selectedIndex < 0) nav.selectedIndex = (int)nav.currentList.size() - 1;
                    changed = true;
                }
            } else if (vpad.trigger & VPAD_BUTTON_A) {
                if (!nav.currentList.empty()) {
                    const JellyfinItem& picked = nav.currentList[nav.selectedIndex];

                    bool playable = (picked.type == "Movie" || picked.type == "Episode" ||
                                      picked.type == "Audio" || picked.type == "Video");

                    if (playable) {
                        nav.screen = Screen::ERROR_SCREEN;
                        nav.errorMessage = "Playback not implemented yet. Would play: " + picked.name;
                        changed = true;
                    } else {
                        std::vector<JellyfinItem> nextList;
                        if (client.getItems(picked.id, nextList)) {
                            navStack.push_back({picked.id, nav.currentParentName});
                            nav.currentParentName = picked.name;
                            nav.currentList = nextList;
                            nav.selectedIndex = 0;
                            nav.screen = Screen::ITEMS;
                            changed = true;
                        } else {
                            nav.screen = Screen::ERROR_SCREEN;
                            nav.errorMessage = client.lastError();
                            changed = true;
                        }
                    }
                }
            } else if (vpad.trigger & VPAD_BUTTON_B) {
                if (!navStack.empty()) {
                    auto parent = navStack.back();
                    navStack.pop_back();

                    if (navStack.empty()) {
                        if (client.getViews(nav.currentList)) {
                            nav.currentParentName = "";
                            nav.screen = Screen::VIEWS;
                        } else {
                            nav.screen = Screen::ERROR_SCREEN;
                            nav.errorMessage = client.lastError();
                        }
                    } else {
                        if (client.getItems(parent.first, nav.currentList)) {
                            nav.currentParentName = parent.second;
                            nav.screen = Screen::ITEMS;
                        } else {
                            nav.screen = Screen::ERROR_SCREEN;
                            nav.errorMessage = client.lastError();
                        }
                    }
                    nav.selectedIndex = 0;
                    changed = true;
                }
            }
        } else if (nav.screen == Screen::ERROR_SCREEN) {
            if (vpad.trigger & VPAD_BUTTON_B) {
                if (client.getViews(nav.currentList)) {
                    nav.screen = Screen::VIEWS;
                    nav.currentParentName = "";
                    navStack.clear();
                    nav.selectedIndex = 0;
                    changed = true;
                }
            }
        }

        if (changed) {
            renderList(nav);
        }

        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    shutdownScreen();
    WHBProcShutdown();
    return 0;
}
