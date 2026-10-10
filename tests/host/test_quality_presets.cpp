// The "Video quality" choices (src/quality_presets.h) and the transcode sizes
// behind them (src/video_size.h): the table's names and texts, that every
// option asks Jellyfin for something the decoder can take, and how the setting
// cycles. Host-only: proves the data, not the behaviour of the Wii U decoder.
#include "check.h"
#include "media/h264_geometry.h"
#include "quality_presets.h"
#include "video_size.h"

#include <set>
#include <string>

static bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int main() {
    int count = 0;
    const quality::Preset* p = quality::presets(count);
    CHECK_EQ(count, 9);

    // --- the exact list, in order: resolution ascending, then bitrate ---
    struct Want { const char* name; int bitrate; int height; };
    const Want want[] = {
        {"Low 480p", 750000, 480},   {"Safe 480p", 1500000, 480},
        {"Low 720p", 1000000, 720},  {"Safe 720p", 2500000, 720}, {"Medium 720p", 4000000, 720}, {"High 720p", 8000000, 720},
        {"Safe 1080p EXPERIMENTAL", 4000000, 1080}, {"Medium 1080p EXPERIMENTAL", 6000000, 1080},
        {"High 1080p EXPERIMENTAL", 8000000, 1080},
    };
    for (int i = 0; i < count && i < 9; i++) {
        CHECK_STR(p[i].name, want[i].name);
        CHECK_EQ(p[i].bitrate, want[i].bitrate);
        CHECK_EQ(p[i].height, want[i].height);
    }

    std::set<std::string> names;
    for (int i = 0; i < count; i++) {
        const std::string name = p[i].name, detail = p[i].detail;
        names.insert(name);
        CHECK(videosize::supportedHeight(p[i].height));
        CHECK(p[i].bitrate >= 300000 && p[i].bitrate <= 20000000); // inside buildVideoStreamUrl's clamp
        CHECK(!detail.empty());
        // The settings row shows the name at the right and the detail on the left; past
        // roughly this many characters combined the detail is cut off with "...", which
        // would hide the warning. (Checked against a render of the real list.)
        CHECK(name.size() + detail.size() <= 85);

        // Every option says what resolution it is...
        if (p[i].height == 480) {
            CHECK(contains(name, "480p") && !contains(name, "EXPERIMENTAL"));
            CHECK(contains(detail, "lower quality, better for really slow internet"));
            CHECK(contains(detail, "768x480"));
        } else if (p[i].height == 720) {
            CHECK(contains(name, "720p") && !contains(name, "EXPERIMENTAL"));
            CHECK(!contains(detail, "EXPERIMENTAL"));
        } else {
            // ...1080p is flagged experimental and keeps the strong-network warning.
            CHECK(contains(name, "1080p EXPERIMENTAL"));
            CHECK(contains(detail, "may crash"));
            CHECK(contains(detail, "needs a strong network"));
            CHECK(contains(detail, "1920x1080"));
        }
    }
    CHECK_EQ((int)names.size(), count); // names are unique: each maps back to one preset

    // The 720p texts are exactly what they were before.
    CHECK_STR(p[2].detail, "1 Mbps -- best for a weak or busy Wi-Fi network");
    CHECK_STR(p[3].detail, "2.5 Mbps -- the tested default");
    CHECK_STR(p[4].detail, "4 Mbps -- sharper picture on a solid connection");
    CHECK_STR(p[5].detail, "8 Mbps -- best quality, needs a strong network");

    // Within a resolution, bitrate (and tier) only goes up.
    for (int i = 1; i < count; i++) {
        CHECK(p[i].height >= p[i - 1].height);
        if (p[i].height == p[i - 1].height) CHECK(p[i].bitrate > p[i - 1].bitrate);
    }

    // The bitrate in each detail line matches the preset (so the text can't drift).
    CHECK(contains(p[0].detail, "750 kbps"));
    CHECK(contains(p[1].detail, "1.5 Mbps"));
    CHECK(contains(p[6].detail, "4 Mbps"));
    CHECK(contains(p[7].detail, "6 Mbps"));
    CHECK(contains(p[8].detail, "8 Mbps"));

    // --- sizes: what each height asks the server for, and the decoder's alignment ---
    CHECK_EQ(videosize::forHeight(480).width, 768);
    CHECK_EQ(videosize::forHeight(720).width, 1280);
    CHECK_EQ(videosize::forHeight(1080).width, 1920);
    CHECK_EQ(videosize::forHeight(999).width, 1280); // unknown -> stable 720p
    CHECK_EQ(videosize::forHeight(999).height, 720);
    CHECK(h264geom::isAligned(768, 480));   // 480p and 720p are aligned on both axes...
    CHECK(h264geom::isAligned(1280, 720));
    CHECK(!h264geom::isAligned(1920, 1080)); // ...1080p is not (hence Experimental)
    CHECK(!h264geom::isAligned(854, 480));   // and the textbook 854x480 is not either
    for (int i = 0; i < count; i++) {
        const videosize::Size s = videosize::forHeight(p[i].height);
        CHECK_EQ(s.height, p[i].height);
        // Only experimental 1080p may be unaligned.
        CHECK(h264geom::isAligned(s.width, s.height) == (p[i].height != 1080));
    }
    // Only exact pairs are honoured by the request path.
    CHECK_EQ(videosize::forRequest(768, 480).width, 768);
    CHECK_EQ(videosize::forRequest(854, 480).width, 1280);
    CHECK_EQ(videosize::forRequest(768, 720).height, 720);
    CHECK_EQ(videosize::forRequest(1920, 1080).height, 1080);
    CHECK_EQ(videosize::defaultBitrateFor(480), 1500000);
    CHECK_EQ(videosize::defaultBitrateFor(720), 2500000);
    CHECK_EQ(videosize::defaultBitrateFor(1080), 6000000);

    // --- matching the current settings back to a preset ---
    CHECK_EQ(quality::indexFor(750000, 480), 0);
    CHECK_EQ(quality::indexFor(2500000, 720), 3);
    CHECK_EQ(quality::indexFor(4000000, 720), 4);   // same bitrate, different resolution...
    CHECK_EQ(quality::indexFor(4000000, 1080), 6);  // ...is a different preset
    CHECK_EQ(quality::indexFor(8000000, 1080), 8);
    CHECK_EQ(quality::indexFor(2500000, 720), 3);       // the profile is its own setting, not part of a preset
    CHECK_EQ(quality::indexFor(3000000, 720), -1);  // hand-edited bitrate -> Custom
    CHECK_EQ(quality::indexFor(1500000, 720), -1);  // 1.5 Mbps only exists at 480p
    for (int i = 0; i < count; i++) CHECK_EQ(quality::indexFor(p[i].bitrate, p[i].height), i);

    // --- cycling ---
    CHECK_EQ(quality::defaultIndex(), 3);                 // Safe 720p, the tested default
    CHECK_EQ(quality::nextIndex(-1), 3);                  // Custom -> the tested default, not 480p
    CHECK_EQ(quality::nextIndex(0), 1);
    CHECK_EQ(quality::nextIndex(5), 6);                   // High 720p -> the only way into 1080p
    CHECK_EQ(quality::nextIndex(count - 1), 0);           // wraps from 1080p High back to 480p Low
    int visited = 0, idx = 0;
    do { idx = quality::nextIndex(idx); visited++; } while (idx != 0 && visited < 100);
    CHECK_EQ(visited, count);                             // every preset is reachable, once per lap

    // A fresh config (2.5 Mbps / baseline / 720) shows the default preset, not Custom.
    CHECK_EQ(quality::indexFor(2500000, 720), quality::defaultIndex());

    return check::finish("test_quality_presets");
}
