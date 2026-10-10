// Host test for usage_timer.h: totals, saving cadence, background exclusion, tap detection, formats.
#include "check.h"
#include "usage_timer.h"

int main() {
    {   // Total = saved + this run.
        UsageTimer u;
        u.start(1000, 50.0);
        CHECK_EQ((int)u.totalSeconds(50.0), 1000);
        CHECK_EQ((int)u.totalSeconds(110.5), 1060);
        u.excludeSeconds(20.0);                    // suspended in the HOME menu for 20 s
        CHECK_EQ((int)u.totalSeconds(110.5), 1040);
        u.excludeSeconds(-5.0);                    // nonsense is ignored
        CHECK_EQ((int)u.totalSeconds(110.5), 1040);
        u.excludeSeconds(1e9);                     // can never go below what was saved
        CHECK_EQ((int)u.totalSeconds(120.0), 1000);
    }
    {   // A negative saved value (corrupt config) starts from zero.
        UsageTimer u;
        u.start(-7, 0.0);
        CHECK_EQ((int)u.totalSeconds(10.0), 10);
    }
    {   // Saving every 30 minutes, no more often.
        UsageTimer u;
        u.start(0, 100.0);
        CHECK(!u.dueForSave(100.0));
        CHECK(!u.dueForSave(100.0 + 1799.0));
        CHECK(u.dueForSave(100.0 + 1800.0));
        u.markSaved(100.0 + 1800.5);
        CHECK(!u.dueForSave(100.0 + 1800.5 + 1799.0));
        CHECK(u.dueForSave(100.0 + 1800.5 + 1800.0));
    }
    {   // Five quick taps in a row fire once; a pause starts over; the counter re-arms.
        TapCounter t(5, 1.5);
        CHECK(!t.tap(0.0)); CHECK(!t.tap(0.4)); CHECK(!t.tap(0.8)); CHECK(!t.tap(1.2));
        CHECK(t.tap(1.6));
        CHECK_EQ(t.count(), 0);
        CHECK(!t.tap(2.0));                        // a sixth tap is the first of a new run
        CHECK_EQ(t.count(), 1);
        TapCounter slow(5, 1.5);
        slow.tap(0.0); slow.tap(1.0); slow.tap(2.0); slow.tap(3.0);
        CHECK(!slow.tap(5.0));                     // 2 s gap: start over, not the fifth
        CHECK_EQ(slow.count(), 1);
        TapCounter reset(5, 1.5);
        reset.tap(0.0); reset.tap(0.2); reset.reset();
        CHECK_EQ(reset.count(), 0);
    }
    {   // Formats.
        CHECK_STR(formatUsageShort(0), "0m");
        CHECK_STR(formatUsageShort(59), "0m");
        CHECK_STR(formatUsageShort(34 * 60 + 5), "34m");
        CHECK_STR(formatUsageShort(12 * 3600 + 34 * 60), "12h 34m");
        CHECK_STR(formatUsageShort(3600), "1h 00m");
        CHECK_STR(formatUsageShort(3 * 86400 + 4 * 3600 + 59), "3d 4h");
        CHECK_STR(formatUsageShort(-5), "0m");
        CHECK_STR(formatUsageBig(42), "42s");
        CHECK_STR(formatUsageBig(5 * 60 + 7), "5m 07s");
        CHECK_STR(formatUsageBig(12 * 3600 + 34 * 60 + 5), "12h 34m");
        CHECK_STR(formatUsageSeconds(45245), "45,245 seconds in total");
        CHECK_STR(formatUsageSeconds(1), "1 second in total");
        CHECK_STR(formatUsageSeconds(999), "999 seconds in total");
        CHECK_STR(formatUsageSeconds(1000000), "1,000,000 seconds in total");
        CHECK_STR(formatUsageSeconds(0), "0 seconds in total");
    }
    return check::finish("test_usage_timer");
}
