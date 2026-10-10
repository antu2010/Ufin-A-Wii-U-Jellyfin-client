// Host test for h264_profile.h: names, default, parsing, menu order, ffmpeg profile mapping.
#include "check.h"
#include "h264_profile.h"
#include <cstring>
#include <string>

int main() {
    using namespace h264;
    CHECK_STR(wireName(Profile::Main), "main");
    CHECK_STR(wireName(Profile::Baseline), "baseline");
    CHECK_STR(wireName(Profile::High), "high");
    CHECK_STR(displayName(Profile::Main), "Main");
    CHECK_STR(displayName(Profile::Baseline), "Baseline");
    CHECK_STR(displayName(Profile::High), "High");
    for (int i = 0; i < kCount; i++) CHECK(strlen(description(kOrder[i])) > 0 && strlen(description(kOrder[i])) < 96);

    CHECK(parse("main") == Profile::Main);
    CHECK(parse("  HiGh ") == Profile::High);
    CHECK(parse("Baseline") == Profile::Baseline);
    CHECK(parse("") == kDefault);
    CHECK(parse("main&x=1") == kDefault);   // letters only -> "mainx" -> unknown

    // Menu order Main -> Baseline -> High -> Main.
    CHECK(kOrder[0] == Profile::Main);
    CHECK(next(Profile::Main) == Profile::Baseline);
    CHECK(next(Profile::Baseline) == Profile::High);
    CHECK(next(Profile::High) == Profile::Main);

    CHECK_STR(nameForProfileIdc(66), "baseline");
    CHECK_STR(nameForProfileIdc(77), "main");
    CHECK_STR(nameForProfileIdc(100), "high");
    CHECK_STR(nameForProfileIdc(0), "unknown");
    CHECK_STR(nameForProfileIdc(88), "?");
    CHECK(kDefault == Profile::Baseline); // default unchanged: see the note on kDefault
    return check::finish("test_h264_profile");
}
