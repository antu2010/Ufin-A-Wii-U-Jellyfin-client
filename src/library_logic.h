// library_logic.h -- the music library's views, the sort options of every browsable list, and the Jellyfin
// query each one turns into. Pure data and string building (no UI, no network, no Wii U includes), so the
// host tests check every option against what the server's API accepts.
//
// Sort fields are Jellyfin's own `ItemSortBy` names; every query appends SortName as a tie-break so the
// order is stable (pages never repeat or skip an item) and items missing the value land together.
#pragma once

#include "jellyfin_client.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace library {

// --- music views (the tabs of a music library) ---

enum MusicView { AllSongs = 0, Artists = 1, Albums = 2, Playlists = 3, MusicViewCount = 4 };

inline const char* musicViewName(int v) {
    switch (v) {
        case AllSongs: return "All Songs";
        case Artists: return "Artists";
        case Albums: return "Albums";
        case Playlists: return "Playlists";
    }
    return "";
}

// --- what a list holds, which decides its sort options ---

enum class Kind {
    None,       // not sortable (mixed or special lists, Now Playing, details)
    Songs,      // All Songs
    Artists,
    Albums,
    Playlists,
    Tracks,     // the songs inside an album, artist or playlist
    Movies,
    Shows,
    Episodes,
};

// What a field is about: lets the UI hide an option whose metadata the list does not have.
enum class Field {
    None,        // always there (name, ...)
    DateAdded, Year, Runtime, CommunityRating, CriticRating, PlayCount, LastPlayed, Artist, Album, AirDate,
};

struct Option {
    const char* label;     // shown in the flyout
    const char* sortBy;    // Jellyfin ItemSortBy list, e.g. "Artist,Album"
    Field needs;           // metadata the list must have for this option to be offered
    bool defaultDesc;      // the direction picked when this option is chosen ("newest first", "highest first")
    const char* ascText;   // what ascending means, shown next to the direction
    const char* descText;
};

struct OptionList {
    const Option* options;
    int count;
};

// "A to Z" style texts shared by the tables.
#define LIB_AZ "A to Z", "Z to A"
#define LIB_OLD "Oldest first", "Newest first"
#define LIB_LOW "Lowest first", "Highest first"
#define LIB_SHORT "Shortest first", "Longest first"
#define LIB_FEW "Fewest first", "Most first"

inline OptionList optionsFor(Kind k) {
    static const Option songs[] = {
        {"Title", "SortName", Field::None, false, LIB_AZ},
        {"Artist", "Artist,Album,ParentIndexNumber,IndexNumber", Field::Artist, false, LIB_AZ},
        {"Album", "Album,ParentIndexNumber,IndexNumber", Field::Album, false, LIB_AZ},
        {"Duration", "Runtime", Field::Runtime, false, LIB_SHORT},
        {"Date added", "DateCreated", Field::DateAdded, true, LIB_OLD},
        {"Release year", "ProductionYear,PremiereDate", Field::Year, true, LIB_OLD},
        {"Play count", "PlayCount", Field::PlayCount, true, LIB_FEW},
        {"Last played", "DatePlayed", Field::LastPlayed, true, LIB_OLD},
    };
    static const Option artists[] = {
        {"Name", "SortName", Field::None, false, LIB_AZ},
    };
    static const Option albums[] = {
        {"Title", "SortName", Field::None, false, LIB_AZ},
        {"Album artist", "AlbumArtist,ProductionYear,SortName", Field::Artist, false, LIB_AZ},
        {"Release year", "ProductionYear,PremiereDate", Field::Year, true, LIB_OLD},
        {"Date added", "DateCreated", Field::DateAdded, true, LIB_OLD},
    };
    static const Option playlists[] = {
        {"Name", "SortName", Field::None, false, LIB_AZ},
        {"Date created", "DateCreated", Field::DateAdded, true, LIB_OLD},
    };
    // Inside an album / artist / playlist: the list's own order first (track order; playlist order).
    static const Option tracks[] = {
        {"Default order", "", Field::None, false, "First to last", "Last to first"},
        {"Title", "SortName", Field::None, false, LIB_AZ},
        {"Artist", "Artist,Album,ParentIndexNumber,IndexNumber", Field::Artist, false, LIB_AZ},
        {"Duration", "Runtime", Field::Runtime, false, LIB_SHORT},
        {"Play count", "PlayCount", Field::PlayCount, true, LIB_FEW},
        {"Last played", "DatePlayed", Field::LastPlayed, true, LIB_OLD},
    };
    static const Option movies[] = {
        {"Title", "SortName", Field::None, false, LIB_AZ},
        {"Year", "ProductionYear,PremiereDate", Field::Year, true, LIB_OLD},
        {"Date added", "DateCreated", Field::DateAdded, true, LIB_OLD},
        {"Runtime", "Runtime", Field::Runtime, false, LIB_SHORT},
        {"Community rating", "CommunityRating", Field::CommunityRating, true, LIB_LOW},
        {"Critic rating", "CriticRating", Field::CriticRating, true, LIB_LOW},
        {"Play count", "PlayCount", Field::PlayCount, true, LIB_FEW},
        {"Last played", "DatePlayed", Field::LastPlayed, true, LIB_OLD},
    };
    static const Option shows[] = {
        {"Title", "SortName", Field::None, false, LIB_AZ},
        {"Premiere date", "PremiereDate,ProductionYear", Field::Year, true, LIB_OLD},
        {"Date added", "DateCreated", Field::DateAdded, true, LIB_OLD},
        {"Community rating", "CommunityRating", Field::CommunityRating, true, LIB_LOW},
        {"Last played", "SeriesDatePlayed", Field::LastPlayed, true, LIB_OLD},
    };
    static const Option episodes[] = {
        {"Episode order", "ParentIndexNumber,IndexNumber", Field::None, false, "First to last", "Last to first"},
        {"Title", "SortName", Field::None, false, LIB_AZ},
        {"Air date", "PremiereDate,ProductionYear", Field::AirDate, true, LIB_OLD},
        {"Date added", "DateCreated", Field::DateAdded, true, LIB_OLD},
        {"Runtime", "Runtime", Field::Runtime, false, LIB_SHORT},
        {"Last played", "DatePlayed", Field::LastPlayed, true, LIB_OLD},
    };
#define LIB_LIST(a) OptionList{a, (int)(sizeof(a) / sizeof(a[0]))}
    switch (k) {
        case Kind::Songs: return LIB_LIST(songs);
        case Kind::Artists: return LIB_LIST(artists);
        case Kind::Albums: return LIB_LIST(albums);
        case Kind::Playlists: return LIB_LIST(playlists);
        case Kind::Tracks: return LIB_LIST(tracks);
        case Kind::Movies: return LIB_LIST(movies);
        case Kind::Shows: return LIB_LIST(shows);
        case Kind::Episodes: return LIB_LIST(episodes);
        case Kind::None: break;
    }
#undef LIB_LIST
    return OptionList{nullptr, 0};
}

// A list with a single option (Artists: only by name) still gets the button: direction is the point.
inline bool sortable(Kind k) { return k != Kind::None; }

// The kind of an ordinary folder's list, from what is in it: every item the same type (a movie library, a
// show's seasons are NOT sortable -- they are few and in season order). Mixed folders get none.
inline Kind kindOfItems(const std::vector<JellyfinItem>& items) {
    if (items.empty()) return Kind::None;
    bool movie = true, show = true, episode = true, audio = true, playlist = true, album = true;
    for (const JellyfinItem& it : items) {
        if (it.type != "Movie" && it.type != "BoxSet") movie = false;
        if (it.type != "Series") show = false;
        if (it.type != "Episode") episode = false;
        if (it.type != "Audio") audio = false;
        if (it.type != "Playlist") playlist = false;
        if (it.type != "MusicAlbum") album = false;
    }
    if (playlist) return Kind::Playlists;
    if (album) return Kind::Albums;
    if (movie) return Kind::Movies;
    if (show) return Kind::Shows;
    if (episode) return Kind::Episodes;
    if (audio) return Kind::Tracks;
    return Kind::None;
}

// Does the loaded list have the metadata an option sorts by? `complete` = the whole result is loaded;
// only then can "nobody has it" be trusted (a later page may). Options needing nothing are always there.
inline bool hasField(Field f, const JellyfinItem& it) {
    switch (f) {
        case Field::None: return true;
        case Field::DateAdded: return !it.dateCreated.empty();
        case Field::Year: return it.productionYear > 0 || !it.premiereDate.empty();
        case Field::AirDate: return !it.premiereDate.empty();
        case Field::Runtime: return it.runTimeTicks > 0;
        case Field::CommunityRating: return it.communityRating >= 0.0f;
        case Field::CriticRating: return it.criticRating >= 0.0f;
        case Field::PlayCount: return it.playCount >= 0;
        case Field::LastPlayed: return !it.lastPlayedDate.empty();
        case Field::Artist: return !it.artist.empty();
        case Field::Album: return !it.album.empty();
    }
    return true;
}

// Indices (into optionsFor(k)) of the options to offer. Always keeps `keep` (the active one).
inline std::vector<int> availableOptions(Kind k, const std::vector<JellyfinItem>& items, bool complete, int keep) {
    std::vector<int> out;
    const OptionList ol = optionsFor(k);
    for (int i = 0; i < ol.count; i++) {
        bool ok = true;
        if (complete && ol.options[i].needs != Field::None && i != keep) {
            ok = false;
            for (const JellyfinItem& it : items)
                if (hasField(ol.options[i].needs, it)) { ok = true; break; }
        }
        if (ok) out.push_back(i);
    }
    return out;
}

// The sort a list starts in: its first option, ascending.
struct Sort {
    int option = 0;
    bool descending = false;
    bool operator==(const Sort& o) const { return option == o.option && descending == o.descending; }
};

inline Sort defaultSort(Kind) { return Sort{}; }

inline Sort pickOption(Kind k, int option) {
    const OptionList ol = optionsFor(k);
    Sort s;
    if (option < 0 || option >= ol.count) return s;
    s.option = option;
    s.descending = ol.options[option].defaultDesc;
    return s;
}

// What the button / flyout shows: "Release year - Newest first".
inline std::string sortSummary(Kind k, const Sort& s) {
    const OptionList ol = optionsFor(k);
    if (s.option < 0 || s.option >= ol.count) return "";
    const Option& o = ol.options[s.option];
    return std::string(o.label) + " - " + (s.descending ? o.descText : o.ascText);
}

// --- queries ---

// One request for a page of items. Only the fields set are sent.
struct Query {
    enum class Source { Children, Artists, ArtistTracks, Playlist, AllPlaylists, Songs, AllAlbums };
    Source source = Source::Children;
    std::string parentId;        // the library / folder / album / playlist
    std::string artistId;        // ArtistTracks
    std::string sortBy;          // "" = the server's own order for the source
    bool descending = false;
    int startIndex = 0;
    int limit = 0;               // 0 = no limit
    // A playlist in its own order, newest entry first: the server cannot sort that way, so the whole playlist
    // (up to kMaxLoaded) is fetched and reversed here. Such a query is never paged.
    bool reversedPlaylist() const { return source == Source::Playlist && sortBy.empty() && descending; }
};

constexpr int kPageSize = 120;     // items per request in a long list
constexpr int kLoadAhead = 30;     // fetch the next page when the cursor is this close to the end of what is loaded
constexpr int kMaxLoaded = 3000;   // never hold more than this many rows (memory on the console)

// The path + query for `q`, for user `userId`. `fields` is the shared "image / user data" tail.
inline std::string queryPath(const Query& q, const std::string& userId, const std::string& fields) {
    auto tail = [&](bool defaultSort, const char* defaultBy, bool tieBreak) {
        std::string s;
        if (!q.sortBy.empty()) {
            s += "&SortBy=" + q.sortBy + (tieBreak ? ",SortName" : "");
            s += q.descending ? "&SortOrder=Descending" : "&SortOrder=Ascending";
        } else if (defaultSort) {
            s += std::string("&SortBy=") + defaultBy + "&SortOrder=" + (q.descending ? "Descending" : "Ascending");
        }
        if (q.limit > 0) s += "&StartIndex=" + std::to_string(q.startIndex) + "&Limit=" + std::to_string(q.limit);
        return s + fields;
    };
    switch (q.source) {
        case Query::Source::Songs:
            return "/Users/" + userId + "/Items?ParentId=" + q.parentId +
                   "&IncludeItemTypes=Audio&Recursive=true" + tail(true, "SortName", false);
        case Query::Source::AllAlbums:
            return "/Users/" + userId + "/Items?ParentId=" + q.parentId +
                   "&IncludeItemTypes=MusicAlbum&Recursive=true" + tail(true, "SortName", false);
        case Query::Source::Artists:
            return "/Artists/AlbumArtists?userId=" + userId + "&ParentId=" + q.parentId + tail(true, "SortName", false);
        case Query::Source::ArtistTracks:
            return "/Users/" + userId + "/Items?ArtistIds=" + q.artistId +
                   (q.parentId.empty() ? "" : "&ParentId=" + q.parentId) + "&IncludeItemTypes=Audio&Recursive=true" +
                   tail(true, "Album,ParentIndexNumber,IndexNumber,SortName", false);
        case Query::Source::AllPlaylists:
            return "/Users/" + userId + "/Items?IncludeItemTypes=Playlist&Recursive=true&MediaTypes=Audio" +
                   tail(true, "SortName", false);
        case Query::Source::Playlist:
            // Default order = the playlist's own (its endpoint also carries each entry's PlaylistItemId);
            // a chosen sort goes through the generic items query, which sorts the whole playlist.
            if (q.sortBy.empty())
                return "/Playlists/" + q.parentId + "/Items?userId=" + userId +
                       (q.limit > 0 ? "&StartIndex=" + std::to_string(q.startIndex) + "&Limit=" + std::to_string(q.limit) : "") +
                       fields;
            return "/Users/" + userId + "/Items?ParentId=" + q.parentId + tail(false, "", true);
        case Query::Source::Children:
            return "/Users/" + userId + "/Items?ParentId=" + q.parentId +
                   (q.sortBy.empty() ? std::string("&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=") +
                                           (q.descending ? "Descending" : "Ascending") +
                                           (q.limit > 0 ? "&StartIndex=" + std::to_string(q.startIndex) + "&Limit=" + std::to_string(q.limit) : "") + fields
                                     : tail(false, "", true));
    }
    return "";
}

// Applies the active sort of a list to a query. An empty sortBy option ("Default order") leaves the source's
// own order; descending still applies to it (server-side, by name) only for options that name a field.
inline void applySort(Query& q, Kind k, const Sort& s) {
    const OptionList ol = optionsFor(k);
    q.sortBy.clear();
    q.descending = s.descending;
    if (s.option >= 0 && s.option < ol.count) q.sortBy = ol.options[s.option].sortBy;
}

// --- Jellyfin server version, for what the server can do ---

inline Version parseVersion(const std::string& s) {
    Version v;
    if (sscanf(s.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch) < 2) v = Version();
    return v;
}

// Playlist visibility (IsPublic) exists from Jellyfin 10.10. Older servers have no such setting: the
// dialog then hides the selector rather than pretending.
inline bool supportsPlaylistVisibility(const Version& v) { return v.atLeast(10, 10); }

// "My playlist" / names: trimmed; empty after trimming is invalid.
inline std::string trimName(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

} // namespace library
