#include "item_labels.h"
#include "ui/layout.h"

#include <cstdio>

bool isPlayableItem(const JellyfinItem& item) {
    return item.type == "Movie" || item.type == "Episode" || item.type == "Video" ||
           item.type == "MusicVideo" || item.type == "Audio" || item.type == "TvChannel";
}

bool isLiveTvView(const JellyfinItem& item) {
    return item.collectionType == "livetv";
}

bool isHomeRow(const JellyfinItem& item) {
    return item.type == HOME_RESUME || item.type == HOME_NEXT_UP || item.type == HOME_FAVORITES;
}

JellyfinItem makeHomeRow(const char* type, int count) {
    JellyfinItem i;
    i.type = type;
    i.id = type;
    i.childCount = count;
    std::string t = type;
    i.name = t == HOME_RESUME ? "Continue watching" : t == HOME_NEXT_UP ? "Next up" : "Favourites";
    return i;
}

void applyUserState(const JellyfinItem& item, ui::ListEntry& e) {
    e.favorite = item.favorite;
    if (item.type == "Series" || item.type == "Season" || item.type == "BoxSet") {
        e.unplayed = item.unplayedCount > 0 ? item.unplayedCount : -1;
        e.watched = item.played && item.unplayedCount <= 0;
    } else {
        e.watched = item.played;
        if (!item.played && item.positionTicks > 0 && item.runTimeTicks > 0) {
            e.progress = (float)((double)item.positionTicks / (double)item.runTimeTicks);
        }
    }
}

bool isShufflableContainer(const JellyfinItem& item) {
    return item.type == "MusicAlbum" || item.type == "Playlist";
}

static std::string libraryKind(const std::string& collectionType) {
    if (collectionType == "movies") return "Movies";
    if (collectionType == "tvshows") return "TV Shows";
    if (collectionType == "music") return "Music";
    if (collectionType == "livetv") return "Live TV";
    if (collectionType == "boxsets") return "Collections";
    if (collectionType == "homevideos") return "Videos";
    if (collectionType == "musicvideos") return "Music Videos";
    if (collectionType == "playlists") return "Playlists";
    if (collectionType == "books") return "Books";
    return "Library";
}

static bool isLibrary(const JellyfinItem& item) {
    return item.type == "CollectionFolder" || item.type == "UserView";
}

static std::string duration(const JellyfinItem& item) {
    if (item.runTimeTicks <= 0) return "";
    return ui::formatTime((double)item.runTimeTicks / 10000000.0);
}

static std::string episodeCode(const JellyfinItem& item) {
    char buf[32] = "";
    if (item.parentIndexNumber >= 0 && item.indexNumber >= 0) {
        snprintf(buf, sizeof(buf), "S%dE%d", item.parentIndexNumber, item.indexNumber);
    } else if (item.indexNumber >= 0) {
        snprintf(buf, sizeof(buf), "E%d", item.indexNumber);
    }
    return buf;
}

static std::string joinParts(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "  " + b;
}

static std::string joinDetail(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "  -  " + b;
}

std::string itemDisplayName(const JellyfinItem& item) {
    if (item.type == "TvChannel" && !item.channelNumber.empty()) {
        return item.channelNumber + "  " + item.name;
    }
    if (item.type == "Audio" && item.indexNumber > 0) {
        return std::to_string(item.indexNumber) + ". " + item.name;
    }
    return item.name;
}

std::string itemTag(const JellyfinItem& item) {
    if (isHomeRow(item)) return item.childCount >= 0 ? std::to_string(item.childCount) : "";
    if (isLibrary(item)) return libraryKind(item.collectionType);
    if (item.type == "Movie" || item.type == "Video" || item.type == "MusicVideo") {
        return joinParts(item.productionYear > 0 ? std::to_string(item.productionYear) : "", duration(item));
    }
    if (item.type == "Episode") return joinParts(episodeCode(item), duration(item));
    if (item.type == "Audio") return duration(item);
    if (item.type == "Series") return item.productionYear > 0 ? std::to_string(item.productionYear) : "Series";
    if (item.type == "Season") return "Season";
    if (item.type == "BoxSet") return "Collection";
    if (item.type == "MusicAlbum") return "Album";
    if (item.type == "MusicArtist") return "Artist";
    if (item.type == "Playlist") return "Playlist";
    if (item.type == "TvChannel") return "LIVE";
    if (item.type == "Folder") return "Folder";
    return item.type;
}

std::string itemDetail(const JellyfinItem& item) {
    if (item.type == HOME_RESUME) return "Pick up where you left off";
    if (item.type == HOME_NEXT_UP) return "The next episode of the shows you're watching";
    if (item.type == HOME_FAVORITES) return "Everything you've marked with a heart (Y)";
    if (isLibrary(item)) return libraryKind(item.collectionType) + " library  -  A: open";
    if (isPlayableItem(item) && item.type != "TvChannel" && !item.played && item.positionTicks >= 600000000LL) {
        // Partly watched: say where it resumes.
        JellyfinItem plain = item;
        plain.positionTicks = 0;
        return "Resume at " + ui::formatTime((double)item.positionTicks / 10000000.0) + "  -  " + itemDetail(plain);
    }
    if (item.type == "TvChannel") {
        std::string what = item.currentProgram.empty() ? "Live channel" : "Now: " + item.currentProgram;
        return joinDetail(what, "A: watch");
    }
    if (item.type == "Episode") {
        std::string where;
        if (item.parentIndexNumber >= 0) where = "Season " + std::to_string(item.parentIndexNumber);
        if (item.indexNumber >= 0) {
            where += (where.empty() ? "" : ", ") + std::string("Episode ") + std::to_string(item.indexNumber);
        }
        std::string title = item.seriesName.empty() ? item.name : item.seriesName + ": " + item.name;
        return joinDetail(joinDetail(title, where), duration(item));
    }
    if (isPlayableItem(item)) {
        std::string year = item.productionYear > 0 ? std::to_string(item.productionYear) : "";
        return joinDetail(joinDetail(item.name, year), duration(item));
    }
    return joinDetail(item.name, itemTag(item));
}

ui::Icon itemIcon(const JellyfinItem& item) {
    if (item.type == HOME_RESUME) return ui::Icon::Resume;
    if (item.type == HOME_NEXT_UP) return ui::Icon::Series;
    if (item.type == HOME_FAVORITES) return ui::Icon::Heart;
    if (isLibrary(item)) {
        if (item.collectionType == "movies") return ui::Icon::Movie;
        if (item.collectionType == "tvshows") return ui::Icon::Series;
        if (item.collectionType == "music") return ui::Icon::Music;
        if (item.collectionType == "livetv") return ui::Icon::LiveTv;
        return ui::Icon::Library;
    }
    if (item.type == "Movie" || item.type == "Video" || item.type == "MusicVideo") return ui::Icon::Movie;
    if (item.type == "Series" || item.type == "Season") return ui::Icon::Series;
    if (item.type == "Episode") return ui::Icon::Episode;
    if (item.type == "Audio") return ui::Icon::Music;
    if (item.type == "MusicAlbum" || item.type == "Playlist") return ui::Icon::Album;
    if (item.type == "MusicArtist") return ui::Icon::Music;
    if (item.type == "TvChannel") return ui::Icon::Channel;
    if (item.type == "BoxSet") return ui::Icon::Collection;
    return ui::Icon::Folder;
}
