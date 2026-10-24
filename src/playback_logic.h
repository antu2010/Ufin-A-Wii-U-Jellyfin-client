// Decisions around playback that don't need the console: whether to offer
// "Resume", which episode comes next, and stepping through audio /
// subtitle tracks. Host-tested (tests/host/test_playback_logic.cpp).
#pragma once
#include "jellyfin_client.h"

#include <string>
#include <vector>

// Offer "Resume from ..." when the user stopped more than a minute in and
// not at the very end (Jellyfin clears the position once it counts an
// item as watched).
inline bool shouldOfferResume(const JellyfinItem& item) {
    const int64_t minute = 60LL * 10000000LL;
    if (item.positionTicks < minute) return false;
    if (item.runTimeTicks > 0 && item.positionTicks > item.runTimeTicks - 2 * minute) return false;
    return true;
}

// The episode after `currentId` in a show's episode list (all seasons, in
// order), or nullptr at the end / if it isn't in the list.
inline const JellyfinItem* findNextEpisode(const std::vector<JellyfinItem>& episodes, const std::string& currentId) {
    for (size_t i = 0; i < episodes.size(); i++) {
        if (episodes[i].id != currentId) continue;
        for (size_t j = i + 1; j < episodes.size(); j++) {
            if (episodes[j].type == "Episode") return &episodes[j];
        }
        return nullptr;
    }
    return nullptr;
}

// Label for the current choice in the track panel.
inline std::string trackLabel(const std::vector<MediaTrack>& tracks, int index, const char* noneLabel) {
    if (index < 0) return noneLabel;
    for (const MediaTrack& t : tracks) if (t.index == index) return t.title;
    return noneLabel;
}

// Next / previous track index (dir +1 / -1), wrapping. With `allowNone`
// (subtitles), "none" (-1) is one of the choices, before the first track.
inline int stepTrack(const std::vector<MediaTrack>& tracks, int current, int dir, bool allowNone) {
    std::vector<int> options;
    if (allowNone) options.push_back(-1);
    for (const MediaTrack& t : tracks) options.push_back(t.index);
    if (options.empty()) return current;
    int pos = 0;
    for (size_t i = 0; i < options.size(); i++) if (options[i] == current) pos = (int)i;
    int n = (int)options.size();
    return options[(size_t)(((pos + dir) % n + n) % n)];
}

// Keep the user's language when moving to the next episode: the track in
// `tracks` with the same language as `language`, else `fallback`.
inline int trackForLanguage(const std::vector<MediaTrack>& tracks, const std::string& language, int fallback) {
    if (language.empty()) return fallback;
    for (const MediaTrack& t : tracks) if (t.language == language) return t.index;
    return fallback;
}

// Whether the chosen subtitle track (stream index; -1 = none) is a text format.
// Feeds VideoStreamOptions::subtitleIsText; false when nothing is chosen or the
// track is unknown, so only a known text track changes the stream request.
inline bool subtitleTrackIsText(const std::vector<MediaTrack>& tracks, int index) {
    if (index < 0) return false;
    for (const MediaTrack& t : tracks) if (t.index == index) return t.isTextSubtitle;
    return false;
}

// The prompt shown when a video starts has four slots: 0 = audio, 1 = subtitles, 2 = OK,
// 3 = Always use default (the two buttons share the bottom row). Up / down walk the three rows
// and wrap; left / right on the button row swap the two buttons.
inline int trackPromptMove(int slot, int dy) {
    if (dy > 0) return slot == 0 ? 1 : slot == 1 ? 2 : 0;
    if (dy < 0) return slot == 0 ? 2 : slot == 1 ? 0 : 1;
    return slot;
}
inline int trackPromptSide(int slot) { return slot == 2 ? 3 : slot == 3 ? 2 : slot; }

// Whether a video that is about to start should ask for its audio / subtitle tracks: only when the
// user has not chosen "always use default", this is the first start of a viewing (an autoplayed next
// episode keeps the language already picked) and there is actually something to choose between.
inline bool shouldAskTracks(bool alwaysDefault, bool alreadyChosen, const VideoInfo& info) {
    if (alwaysDefault || alreadyChosen) return false;
    return info.audioTracks.size() > 1 || !info.subtitleTracks.empty();
}

inline std::string trackLanguage(const std::vector<MediaTrack>& tracks, int index) {
    for (const MediaTrack& t : tracks) if (t.index == index) return t.language;
    return "";
}
