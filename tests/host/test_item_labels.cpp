// Item presentation: names, tags and detail lines for each Jellyfin
// item type, and which items are playable / Live TV.
#include "check.h"
#include "item_labels.h"

static JellyfinItem make(const std::string& type) {
    JellyfinItem i;
    i.id = "x";
    i.name = "Name";
    i.type = type;
    return i;
}

int main() {
    JellyfinItem lib = make("CollectionFolder");
    lib.collectionType = "movies";
    CHECK_STR(itemTag(lib), "Movies");
    CHECK(!isPlayableItem(lib));
    CHECK(!isLiveTvView(lib));

    JellyfinItem live = make("UserView");
    live.collectionType = "livetv";
    CHECK(isLiveTvView(live));
    CHECK_STR(itemTag(live), "Live TV");

    JellyfinItem movie = make("Movie");
    movie.productionYear = 2008;
    movie.runTimeTicks = 6720LL * 10000000LL; // 1:52:00
    CHECK(isPlayableItem(movie));
    CHECK_STR(itemTag(movie), "2008  1:52:00");
    CHECK_STR(itemDetail(movie), "Name  -  2008  -  1:52:00");

    JellyfinItem ep = make("Episode");
    ep.parentIndexNumber = 2;
    ep.indexNumber = 5;
    ep.runTimeTicks = 2520LL * 10000000LL; // 42:00
    CHECK(isPlayableItem(ep));
    CHECK_STR(itemTag(ep), "S2E5  42:00");
    CHECK(itemDetail(ep).find("Season 2, Episode 5") != std::string::npos);
    ep.seriesName = "Lost";
    CHECK(itemDetail(ep).find("Lost: Name") == 0); // search results mix shows

    JellyfinItem ch = make("TvChannel");
    ch.channelNumber = "5";
    ch.currentProgram = "News";
    CHECK(isPlayableItem(ch));
    CHECK_STR(itemDisplayName(ch), "5  Name");
    CHECK_STR(itemTag(ch), "LIVE");
    CHECK(itemDetail(ch).find("Now: News") == 0);
    ch.currentProgram.clear();
    CHECK(itemDetail(ch).find("Live channel") == 0);

    JellyfinItem track = make("Audio");
    track.indexNumber = 3;
    track.runTimeTicks = 185LL * 10000000LL;
    CHECK_STR(itemDisplayName(track), "3. Name");
    CHECK_STR(itemTag(track), "3:05");

    CHECK_STR(itemTag(make("Series")), "Series");
    CHECK(!isPlayableItem(make("Series")));
    CHECK_STR(itemTag(make("MusicAlbum")), "Album");
    CHECK(isShufflableContainer(make("MusicAlbum")));
    CHECK(isShufflableContainer(make("Playlist")));
    CHECK(!isShufflableContainer(make("Series")));
    CHECK_STR(itemTag(make("BoxSet")), "Collection");
    CHECK_STR(itemTag(make("Something")), "Something");

    CHECK(itemIcon(movie) == ui::Icon::Movie);
    CHECK(itemIcon(ch) == ui::Icon::Channel);
    CHECK(itemIcon(lib) == ui::Icon::Movie); // a movies library
    CHECK(itemIcon(live) == ui::Icon::LiveTv);
    CHECK(itemIcon(make("MusicAlbum")) == ui::Icon::Album);
    CHECK(itemIcon(make("Folder")) == ui::Icon::Folder);

    // Home rows.
    JellyfinItem row = makeHomeRow(HOME_RESUME, 3);
    CHECK(isHomeRow(row));
    CHECK_STR(row.name, "Continue watching");
    CHECK_STR(itemTag(row), "3");
    CHECK(itemIcon(row) == ui::Icon::Resume);
    CHECK(!isPlayableItem(row));
    CHECK(itemIcon(makeHomeRow(HOME_FAVORITES, 1)) == ui::Icon::Heart);
    CHECK_STR(makeHomeRow(HOME_NEXT_UP, 0).name, "Next up");
    CHECK(!isHomeRow(movie));

    // Resume detail + user state on list rows.
    JellyfinItem part = make("Movie");
    part.runTimeTicks = 6000LL * 10000000LL;
    part.positionTicks = 754LL * 10000000LL; // 12:34
    part.favorite = true;
    CHECK(itemDetail(part).find("Resume at 12:34") == 0);
    ui::ListEntry le;
    applyUserState(part, le);
    CHECK(le.favorite);
    CHECK(!le.watched);
    CHECK(le.progress > 0.12f && le.progress < 0.13f);
    part.played = true;
    ui::ListEntry le2;
    applyUserState(part, le2);
    CHECK(le2.watched);
    CHECK(le2.progress == 0.0f);
    CHECK(itemDetail(part).find("Resume") == std::string::npos); // watched: no resume line
    JellyfinItem show = make("Series");
    show.unplayedCount = 5;
    ui::ListEntry le3;
    applyUserState(show, le3);
    CHECK_EQ(le3.unplayed, 5);
    CHECK(!le3.watched);
    show.unplayedCount = 0;
    show.played = true;
    ui::ListEntry le4;
    applyUserState(show, le4);
    CHECK(le4.watched);

    return check::finish("test_item_labels");
}
