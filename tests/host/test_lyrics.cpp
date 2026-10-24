// Lyrics parsing and the "current line" lookup (src/lyrics.h).
#include "check.h"
#include "lyrics.h"

int main() {
    lyrics::Lyrics l;
    // Synced: every line has a start time.
    CHECK(lyrics::parse("{\"Metadata\":{\"IsSynced\":true},\"Lyrics\":[{\"Text\":\"One\",\"Start\":50000000},"
                        "{\"Text\":\"Two\",\"Start\":100000000},{\"Text\":\"\",\"Start\":150000000},{\"Text\":\"Three\",\"Start\":200000000}]}", l));
    CHECK(l.synced);
    CHECK_EQ((int)l.lines.size(), 4);
    CHECK_EQ(lyrics::currentLine(l, 0.0), -1);   // before the first line
    CHECK_EQ(lyrics::currentLine(l, 4.99), -1);
    CHECK_EQ(lyrics::currentLine(l, 5.0), 0);
    CHECK_EQ(lyrics::currentLine(l, 12.0), 1);
    CHECK_EQ(lyrics::currentLine(l, 16.0), 2);   // the blank line between verses counts as a line
    CHECK_EQ(lyrics::currentLine(l, 999.0), 3);  // after the last one
    CHECK_STR(l.lines[1].text, "Two");

    // Not synced: plain text, no timestamps.
    CHECK(lyrics::parse("{\"Lyrics\":[{\"Text\":\"La\"},{\"Text\":\"La la\\r\"},{\"Text\":\"\"},{\"Text\":\"La\"}]}", l));
    CHECK(!l.synced);
    CHECK_EQ(lyrics::currentLine(l, 30.0), -1);  // nothing to highlight
    CHECK_STR(l.lines[1].text, "La la");         // line endings stripped

    // Partly stamped is not "synced" (it would jump around).
    CHECK(lyrics::parse("{\"Lyrics\":[{\"Text\":\"A\",\"Start\":1000},{\"Text\":\"B\"}]}", l));
    CHECK(!l.synced);

    // Blank edges are trimmed for plain text.
    CHECK(lyrics::parse("{\"Lyrics\":[{\"Text\":\"\"},{\"Text\":\"X\"},{\"Text\":\"\"}]}", l));
    CHECK_EQ((int)l.lines.size(), 1);

    // Nothing to show.
    CHECK(!lyrics::parse("", l));
    CHECK(!lyrics::parse("{}", l));
    CHECK(!lyrics::parse("{\"Lyrics\":[]}", l));
    CHECK(!lyrics::parse("{\"Lyrics\":[{\"Text\":\"\"},{\"Text\":\"  \"}]}", l));
    CHECK(!lyrics::parse("not json", l));
    CHECK(!l.available());
    CHECK_EQ(lyrics::currentLine(l, 5.0), -1);
    return check::finish("test_lyrics");
}
