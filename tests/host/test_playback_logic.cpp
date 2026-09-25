// Resume offer, next episode, track stepping and carrying a language over.
#include "check.h"
#include "playback_logic.h"

static JellyfinItem ep(const std::string& id, const std::string& type = "Episode") {
    JellyfinItem i;
    i.id = id;
    i.type = type;
    return i;
}

int main() {
    const int64_t s = 10000000LL;
    JellyfinItem m;
    m.runTimeTicks = 6000 * s;
    m.positionTicks = 30 * s;
    CHECK(!shouldOfferResume(m));       // under a minute in
    m.positionTicks = 1200 * s;
    CHECK(shouldOfferResume(m));
    m.positionTicks = 5950 * s;
    CHECK(!shouldOfferResume(m));       // in the last two minutes
    m.runTimeTicks = 0;                 // unknown length: offer it
    CHECK(shouldOfferResume(m));

    std::vector<JellyfinItem> eps = {ep("a"), ep("b"), ep("x", "Season"), ep("c")};
    CHECK_STR(findNextEpisode(eps, "a")->id, "b");
    CHECK_STR(findNextEpisode(eps, "b")->id, "c"); // skips non-episodes
    CHECK(findNextEpisode(eps, "c") == nullptr);   // last one
    CHECK(findNextEpisode(eps, "zzz") == nullptr);
    CHECK(findNextEpisode({}, "a") == nullptr);

    std::vector<MediaTrack> audio(2), subs(2);
    audio[0].index = 1; audio[0].title = "English"; audio[0].language = "eng";
    audio[1].index = 2; audio[1].title = "Italiano"; audio[1].language = "ita";
    subs[0].index = 5; subs[0].title = "English SDH"; subs[0].language = "eng";
    subs[1].index = 6; subs[1].title = "Italiano"; subs[1].language = "ita";
    CHECK_EQ(stepTrack(audio, 1, +1, false), 2);
    CHECK_EQ(stepTrack(audio, 2, +1, false), 1);   // wraps
    CHECK_EQ(stepTrack(audio, 1, -1, false), 2);
    CHECK_EQ(stepTrack(subs, -1, +1, true), 5);    // off -> first
    CHECK_EQ(stepTrack(subs, 6, +1, true), -1);    // last -> off
    CHECK_EQ(stepTrack(subs, -1, -1, true), 6);
    CHECK_EQ(stepTrack({}, -1, +1, true), -1);     // nothing but "off"
    CHECK_EQ(stepTrack({}, 7, +1, false), 7);
    CHECK_STR(trackLabel(audio, 2, "Default"), "Italiano");
    CHECK_STR(trackLabel(subs, -1, "Off"), "Off");
    CHECK_STR(trackLabel(subs, 99, "Off"), "Off");

    CHECK_EQ(trackForLanguage(audio, "ita", 1), 2);
    CHECK_EQ(trackForLanguage(audio, "fra", 1), 1);
    CHECK_EQ(trackForLanguage(audio, "", 1), 1);
    CHECK_STR(trackLanguage(subs, 6), "ita");
    CHECK_STR(trackLanguage(subs, -1), "");
    return check::finish("test_playback_logic");
}
