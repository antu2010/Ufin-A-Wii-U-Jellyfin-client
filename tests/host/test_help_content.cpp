// Host test for help_content.h: every controller has a sensible page, the highlight masks only use
// buttons that controller really has, and the cursor stepping skips group titles.
#include "check.h"
#include "help_content.h"

using namespace help;

static bool hasRow(const std::vector<Row>& r, const char* button, const char* actionPart) {
    for (const Row& x : r)
        if (!x.header && x.button == button && x.action.find(actionPart) != std::string::npos) return true;
    return false;
}

int main() {
    CHECK_EQ(kControllerCount, 4);
    for (int p = 0; p < kControllerCount; p++) {
        Controller c = controllerAt(p);
        std::vector<Row> rows = rowsFor(c);
        CHECK(rows.size() > 20);
        CHECK(rows[0].header);                       // starts with a group title
        CHECK(std::string(controllerName(c)).size() > 3);
        CHECK(std::string(controllerNote(c)).size() > 10);
        int entries = 0, headers = 0;
        for (const Row& x : rows) {
            if (x.header) { headers++; CHECK(x.button.empty()); CHECK(!x.action.empty()); }
            else { entries++; CHECK(!x.button.empty()); CHECK(!x.action.empty()); CHECK(x.highlight != 0); }
        }
        CHECK_EQ(headers, 6);                        // Browsing, Settings, Video, Music, Live TV, Anywhere
        CHECK(entries > 20);
    }
    // Pages are distinct controllers, in a stable order.
    CHECK(controllerAt(0) == Controller::GamePad);
    CHECK(controllerAt(1) == Controller::Pro);
    CHECK(controllerAt(2) == Controller::Classic);
    CHECK(controllerAt(3) == Controller::WiiRemote);
    CHECK(controllerAt(99) == Controller::GamePad);  // out of range -> first page

    // The mappings in the README / input_map.h: GamePad, Pro and Classic share one set...
    std::vector<Row> gp = rowsFor(Controller::GamePad), pro = rowsFor(Controller::Pro), cl = rowsFor(Controller::Classic),
                     wr = rowsFor(Controller::WiiRemote);
    CHECK(hasRow(gp, "X", "Search") && hasRow(pro, "X", "Search") && hasRow(cl, "X", "Search"));
    CHECK(hasRow(gp, "ZL", "watched") && hasRow(gp, "ZR", "Refresh"));
    CHECK(hasRow(gp, "L / R", "Page up"));
    CHECK(hasRow(gp, "Y", "Audio and subtitles"));
    CHECK(hasRow(gp, "-", "menu music"));
    // ...only the GamePad has the touch screen...
    CHECK(hasRow(gp, "Touch", "sidebar"));
    CHECK(!hasRow(pro, "Touch", "sidebar") && !hasRow(cl, "Touch", "sidebar") && !hasRow(wr, "Touch", "sidebar"));
    // ...and the Wii Remote uses 1 / 2 and the Nunchuk instead of X / Y / ZL / ZR / L / R.
    CHECK(hasRow(wr, "1", "Search") && hasRow(wr, "2", "Favourite"));
    CHECK(hasRow(wr, "2", "Audio and subtitles"));
    CHECK(hasRow(wr, "Nunchuk C / Z", "Page up"));
    CHECK(!hasRow(wr, "X", "Search") && !hasRow(wr, "ZL", "watched") && !hasRow(wr, "L / R", "Page up"));
    // Highlights only name buttons the controller has.
    const uint32_t remoteHas = A | B | DPAD | PLUS | MINUS | HOME | ONE | TWO | NUN_C | NUN_Z | NUN_STICK;
    for (const Row& x : wr) CHECK((x.highlight & ~remoteHas) == 0);
    const uint32_t padHas = A | B | X | Y | DPAD | STICK_L | L | R | ZL | ZR | PLUS | MINUS | HOME | TOUCH;
    for (const Row& x : pro) CHECK((x.highlight & ~padHas) == 0);

    // Cursor: starts on the first real row, steps over group titles, wraps both ways.
    int c0 = firstEntry(gp);
    CHECK(c0 == 1 && !gp[(size_t)c0].header);
    int c = c0;
    for (int i = 0; i < 200; i++) {                  // many steps down, then back up
        c = stepEntry(gp, c, +1);
        CHECK(c >= 0 && !gp[(size_t)c].header);
    }
    for (int i = 0; i < 200; i++) {
        c = stepEntry(gp, c, -1);
        CHECK(c >= 0 && !gp[(size_t)c].header);
    }
    // Down from the last entry wraps to the first, up from the first wraps to the last.
    int last = (int)gp.size() - 1;
    CHECK(stepEntry(gp, last, +1) == c0);
    CHECK(stepEntry(gp, c0, -1) == last);
    CHECK_EQ(stepEntry(std::vector<Row>(), 0, 1), -1);
    return check::finish("test_help_content");
}
