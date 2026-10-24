// Song lyrics for the music player: the data, parsing Jellyfin's answer, and "which line is being
// sung". Pure logic (no ImGui, no network), host-tested in tests/host/test_lyrics.cpp.
//
// Jellyfin (10.9 and later) answers GET /Audio/{id}/Lyrics with
//   {"Metadata": {"IsSynced": true, ...}, "Lyrics": [{"Text": "...", "Start": 123400000}, ...]}
// where Start is in ticks (100 ns) and is only present when the lyrics are time-synced (an .lrc
// file or tags). Songs without lyrics (and servers older than 10.9) answer 404.
#pragma once
#include "vendor/cJSON.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lyrics {

struct Line {
    int64_t startTicks = -1; // -1 = no timestamp
    std::string text;        // may be empty (a pause between verses)
};

struct Lyrics {
    std::vector<Line> lines;
    bool synced = false; // every line (that has text) carries a start time

    bool available() const { return !lines.empty(); }
};

// Parses the JSON body. False (and `out` empty) when there is nothing to show: bad JSON, no lines,
// or only blank lines.
inline bool parse(const std::string& body, Lyrics& out) {
    out = Lyrics{};
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) return false;
    cJSON* arr = cJSON_GetObjectItem(json, "Lyrics");
    bool anyText = false, anyStart = false, missingStart = false;
    if (cJSON_IsArray(arr)) {
        const int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            cJSON* item = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(item)) continue;
            Line l;
            cJSON* text = cJSON_GetObjectItem(item, "Text");
            if (cJSON_IsString(text) && text->valuestring) l.text = text->valuestring;
            while (!l.text.empty() && (l.text.back() == '\r' || l.text.back() == '\n' || l.text.back() == ' ')) l.text.pop_back();
            cJSON* start = cJSON_GetObjectItem(item, "Start");
            if (cJSON_IsNumber(start)) { l.startTicks = (int64_t)start->valuedouble; anyStart = true; }
            else if (!l.text.empty()) missingStart = true;
            if (!l.text.empty()) anyText = true;
            out.lines.push_back(std::move(l));
        }
    }
    cJSON_Delete(json);
    if (!anyText) { out = Lyrics{}; return false; }
    out.synced = anyStart && !missingStart;
    // Drop leading / trailing blank lines (keeps the scroll honest); blank lines in the middle stay.
    while (!out.lines.empty() && out.lines.back().text.empty()) out.lines.pop_back();
    size_t first = 0;
    while (first < out.lines.size() && out.lines[first].text.empty()) first++;
    if (first > 0 && !out.synced) out.lines.erase(out.lines.begin(), out.lines.begin() + (long)first);
    return true;
}

// The line being sung at `seconds` into the song: the last one that has started. -1 before the first
// line (or when the lyrics are not synced). Lines are in time order; a binary search keeps it cheap
// enough to call every frame.
inline int currentLine(const Lyrics& l, double seconds) {
    if (!l.synced || l.lines.empty()) return -1;
    const int64_t pos = (int64_t)(seconds * 10000000.0);
    int lo = 0, hi = (int)l.lines.size() - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (l.lines[(size_t)mid].startTicks <= pos) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

} // namespace lyrics
