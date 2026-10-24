// Library views, sort options, queries and the playlist calls, against a fake Jellyfin server.
#include "check.h"
#include "fake_http_server.h"
#include "jellyfin_client.h"
#include "library_logic.h"

#include <set>

static bool contains(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

static void testOptionTables() {
    using namespace library;
    const Kind kinds[] = {Kind::Songs, Kind::Artists, Kind::Albums, Kind::Playlists, Kind::Tracks,
                          Kind::Movies, Kind::Shows, Kind::Episodes};
    for (Kind k : kinds) {
        const OptionList ol = optionsFor(k);
        CHECK(ol.count >= 1);
        std::set<std::string> labels;
        for (int i = 0; i < ol.count; i++) {
            CHECK(labels.insert(ol.options[i].label).second); // no duplicates within a list
            CHECK(std::string(ol.options[i].ascText) != std::string(ol.options[i].descText));
            // only real Jellyfin ItemSortBy names
            static const char* known[] = {"SortName", "Artist", "Album", "AlbumArtist", "Runtime", "DateCreated",
                                          "ProductionYear", "PremiereDate", "PlayCount", "DatePlayed",
                                          "CommunityRating", "CriticRating", "ParentIndexNumber", "IndexNumber",
                                          "SeriesDatePlayed", ""};
            std::string by = ol.options[i].sortBy;
            size_t at = 0;
            while (true) {
                size_t c = by.find(',', at);
                std::string part = by.substr(at, c == std::string::npos ? std::string::npos : c - at);
                bool ok = false;
                for (const char* kn : known) if (part == kn) ok = true;
                CHECK(ok);
                if (c == std::string::npos) break;
                at = c + 1;
            }
        }
    }
    CHECK(optionsFor(Kind::None).count == 0);
    CHECK(!sortable(Kind::None));
    // The spec's lists
    CHECK(optionsFor(Kind::Songs).count == 8);
    CHECK(optionsFor(Kind::Movies).count == 8);
    CHECK(optionsFor(Kind::Playlists).count == 2);
    CHECK(std::string(optionsFor(Kind::Episodes).options[0].label) == "Episode order"); // season then episode
    CHECK(std::string(optionsFor(Kind::Episodes).options[0].sortBy) == "ParentIndexNumber,IndexNumber");
}

static void testKindAndAvailability() {
    using namespace library;
    JellyfinItem m; m.type = "Movie";
    JellyfinItem b; b.type = "BoxSet";
    JellyfinItem s; s.type = "Series";
    JellyfinItem e; e.type = "Episode";
    JellyfinItem a; a.type = "Audio";
    JellyfinItem f; f.type = "Folder";
    CHECK(kindOfItems({m, b}) == Kind::Movies);
    CHECK(kindOfItems({s, s}) == Kind::Shows);
    CHECK(kindOfItems({e}) == Kind::Episodes);
    CHECK(kindOfItems({a, a}) == Kind::Tracks);
    CHECK(kindOfItems({m, s}) == Kind::None);
    CHECK(kindOfItems({f}) == Kind::None);
    CHECK(kindOfItems({}) == Kind::None);

    // Critic rating is hidden when the whole list is loaded and nothing has one; kept while more may come.
    JellyfinItem a1 = m; a1.communityRating = 7.5f; a1.dateCreated = "2024-01-01T00:00:00Z"; a1.runTimeTicks = 1;
    std::vector<int> av = availableOptions(Kind::Movies, {a1}, true, 0);
    bool hasCritic = false, hasCommunity = false;
    for (int i : av) {
        if (std::string(optionsFor(Kind::Movies).options[i].label) == "Critic rating") hasCritic = true;
        if (std::string(optionsFor(Kind::Movies).options[i].label) == "Community rating") hasCommunity = true;
    }
    CHECK(!hasCritic);
    CHECK(hasCommunity);
    CHECK(availableOptions(Kind::Movies, {a1}, false, 0).size() == (size_t)optionsFor(Kind::Movies).count);
    // the active option is never hidden
    int critic = 5;
    bool kept = false;
    for (int i : availableOptions(Kind::Movies, {a1}, true, critic)) kept |= (i == critic);
    CHECK(kept);
}

static void testQueries() {
    using namespace library;
    Query q;
    q.parentId = "lib";
    q.source = Query::Source::Songs;
    Sort s = pickOption(Kind::Songs, 4); // date added: newest first by default
    CHECK(s.descending);
    applySort(q, Kind::Songs, s);
    q.limit = kPageSize; q.startIndex = 240;
    std::string p = queryPath(q, "u1", "&X=1");
    CHECK(contains(p, "IncludeItemTypes=Audio"));
    CHECK(contains(p, "Recursive=true"));
    CHECK(contains(p, "SortBy=DateCreated"));
    CHECK(contains(p, "SortOrder=Descending"));
    CHECK(contains(p, "StartIndex=240&Limit=120"));

    q = Query(); q.source = Query::Source::Artists; q.parentId = "lib";
    CHECK(contains(queryPath(q, "u1", ""), "/Artists/AlbumArtists?userId=u1&ParentId=lib"));
    q.source = Query::Source::AllPlaylists;
    CHECK(contains(queryPath(q, "u1", ""), "IncludeItemTypes=Playlist"));
    q.source = Query::Source::ArtistTracks; q.artistId = "ar1";
    CHECK(contains(queryPath(q, "u1", ""), "ArtistIds=ar1"));

    // A playlist: its own order through its own endpoint, a chosen sort through the generic query.
    q = Query(); q.source = Query::Source::Playlist; q.parentId = "pl";
    CHECK(contains(queryPath(q, "u1", ""), "/Playlists/pl/Items?userId=u1"));
    CHECK(!q.reversedPlaylist());
    q.descending = true;
    CHECK(q.reversedPlaylist());
    applySort(q, Kind::Tracks, pickOption(Kind::Tracks, 1)); // title
    CHECK(!q.reversedPlaylist());
    CHECK(contains(queryPath(q, "u1", ""), "ParentId=pl"));

    // Folders keep their season-then-episode default and honour a direction.
    q = Query(); q.source = Query::Source::Children; q.parentId = "season";
    CHECK(contains(queryPath(q, "u1", ""), "SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending"));
    q.descending = true;
    CHECK(contains(queryPath(q, "u1", ""), "SortOrder=Descending"));

    CHECK(sortSummary(Kind::Movies, pickOption(Kind::Movies, 4)) == "Community rating - Highest first");
    CHECK(sortSummary(Kind::Movies, defaultSort(Kind::Movies)) == "Title - A to Z");
}

static void testVersion() {
    using namespace library;
    CHECK(parseVersion("10.10.3").atLeast(10, 10));
    CHECK(supportsPlaylistVisibility(parseVersion("10.11.0")));
    CHECK(!supportsPlaylistVisibility(parseVersion("10.9.11")));
    CHECK(!supportsPlaylistVisibility(parseVersion("junk")));
    CHECK(!parseVersion("junk").known());
    CHECK(trimName("  hi there \n") == "hi there");
    CHECK(trimName("   ").empty());
}

int main() {
    testOptionTables();
    testKindAndAvailability();
    testQueries();
    testVersion();

    FakeHttpServer server;
    CHECK(server.start());
    FakeRoute auth;
    auth.body = "{\"AccessToken\":\"tok\",\"User\":{\"Id\":\"u1\",\"Name\":\"w\"}}";
    server.addRoute("/Users/AuthenticateByName", auth);
    FakeRoute items;
    items.body = "{\"Items\":[{\"Id\":\"s1\",\"Name\":\"Song\",\"Type\":\"Audio\",\"Album\":\"Alb\",\"AlbumArtist\":\"Band\","
                 "\"DateCreated\":\"2024-05-01T10:00:00Z\",\"PremiereDate\":\"1999-01-01T00:00:00Z\",\"CommunityRating\":8.5,"
                 "\"UserData\":{\"PlayCount\":3,\"LastPlayedDate\":\"2025-01-01T00:00:00Z\"},\"PlaylistItemId\":\"e9\"}],"
                 "\"TotalRecordCount\":500}";
    server.addRoute("/Users/u1/Items", items);
    FakeRoute ver; ver.body = "{\"ServerName\":\"x\",\"Version\":\"10.10.7\"}";
    server.addRoute("/System/Info/Public", ver);
    FakeRoute created; created.body = "{\"Id\":\"newpl\"}";
    server.addRoute("/Playlists", created);
    FakeRoute ok; ok.status = 204; ok.body = "";
    server.addRoute("/Playlists/pl1/Items", ok);
    FakeRoute fail; fail.status = 400; fail.body = "{}";
    server.addRoute("/Playlists/bad/Items", fail);

    JellyfinClient c("127.0.0.1", server.port());
    CHECK(c.authenticate("w", "x"));

    library::Query q; q.source = library::Query::Source::Songs; q.parentId = "lib"; q.limit = 120;
    std::vector<JellyfinItem> out; int total = 0;
    CHECK(c.queryItems(q, out, total));
    CHECK(out.size() == 1 && total == 500);
    CHECK(out[0].album == "Alb" && out[0].artist == "Band");
    CHECK(out[0].communityRating > 8.4f && out[0].criticRating < 0.0f);
    CHECK(out[0].playCount == 3 && out[0].lastPlayedDate == "2025-01-01T00:00:00Z");
    CHECK(out[0].dateCreated == "2024-05-01T10:00:00Z" && out[0].premiereDate.substr(0, 4) == "1999");
    CHECK(out[0].playlistItemId == "e9");
    // appending keeps what is loaded
    q.startIndex = 120;
    CHECK(c.queryItems(q, out, total, true));
    CHECK(out.size() == 2);

    CHECK(c.serverVersion().atLeast(10, 10));

    // Create: body carries the name, Audio, the user and (10.10+) IsPublic; nothing is faked.
    std::string newId;
    CHECK(c.createPlaylist("Road trip", true, "s1", newId));
    CHECK(newId == "newpl");
    bool saw = false;
    for (const FakeRequest& r : server.requests()) {
        if (r.method == "POST" && r.path == "/Playlists") {
            saw = true;
            CHECK(contains(r.body, "\"Name\":\"Road trip\""));
            CHECK(contains(r.body, "\"MediaType\":\"Audio\""));
            CHECK(contains(r.body, "\"IsPublic\":true"));
            CHECK(contains(r.body, "\"Ids\":[\"s1\"]"));
            CHECK(contains(r.body, "\"UserId\":\"u1\""));
        }
    }
    CHECK(saw);

    CHECK(c.addToPlaylist("pl1", "s1"));
    CHECK(!c.addToPlaylist("bad", "s1"));
    CHECK(contains(c.lastError(), "400"));
    CHECK(c.removeFromPlaylist("pl1", "e9"));

    bool sawAdd = false, sawDel = false;
    for (const FakeRequest& r : server.requests()) {
        if (r.method == "POST" && contains(r.path, "/Playlists/pl1/Items?ids=s1&userId=u1")) sawAdd = true;
        if (r.method == "DELETE" && contains(r.path, "/Playlists/pl1/Items?entryIds=e9")) sawDel = true;
    }
    CHECK(sawAdd && sawDel);

    // An older server: no IsPublic is sent.
    FakeHttpServer old;
    CHECK(old.start());
    old.addRoute("/Users/AuthenticateByName", auth);
    FakeRoute oldVer; oldVer.body = "{\"Version\":\"10.8.13\"}";
    old.addRoute("/System/Info/Public", oldVer);
    old.addRoute("/Playlists", created);
    JellyfinClient c2("127.0.0.1", old.port());
    CHECK(c2.authenticate("w", "x"));
    CHECK(c2.createPlaylist("Old", true, "", newId));
    for (const FakeRequest& r : old.requests())
        if (r.method == "POST" && r.path == "/Playlists") CHECK(!contains(r.body, "IsPublic"));

    return check::finish("test_library");
}
