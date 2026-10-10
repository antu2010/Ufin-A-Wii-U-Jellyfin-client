// Host test for ufin_log.cpp: logging is off unless enabled; early lines are held and written
// only if it is switched on; it can be turned off and on again. Run twice by run_tests.sh:
// `test_ufin_log on` and `test_ufin_log off` (the pending state lives for the whole process).
#include "check.h"

// Compile the logger into this test with the SD path pointed at /tmp.
#define UFIN_LOG_PATH "/tmp/ufin_log_test.txt"
#include "../../src/ufin_log.cpp"

#include <cstdio>
#include <cstring>
#include <string>

static std::string slurp() {
    FILE* f = fopen(UFIN_LOG_PATH, "rb");
    if (!f) return "<none>";
    std::string out;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}
static bool contains(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

int main(int argc, char** argv) {
    const bool on = argc > 1 && std::string(argv[1]) == "on";
    remove(UFIN_LOG_PATH);

    UfinLog("early line\n");                     // before the config is known
    CHECK_STR(slurp(), "<none>");                // nothing touches the card yet

    if (!on) {
        UfinLogSetEnabled(0);                    // config says logging is off
        UfinLog("later line\n");
        CHECK_STR(slurp(), "<none>");            // the file is never even created
        UfinLogSetEnabled(0);                    // idempotent
        CHECK_STR(slurp(), "<none>");
        return check::finish("test_ufin_log off");
    }

    UfinLogSetEnabled(1);                        // config says logging is on
    std::string a = slurp();
    CHECK(contains(a, "Ufin log start"));
    CHECK(contains(a, "early line"));            // held-back startup lines are written out
    UfinLog("later line\n");
    CHECK(contains(slurp(), "later line"));

    UfinLogSetEnabled(0);                        // switched off in Settings
    UfinLog("while off\n");
    CHECK(!contains(slurp(), "while off"));      // the file stops growing

    UfinLogSetEnabled(1);                        // and on again: a fresh file
    UfinLog("second run\n");
    std::string b = slurp();
    CHECK(contains(b, "second run"));
    CHECK(!contains(b, "early line"));
    CHECK(!contains(b, "while off"));
    UfinLogClose();
    remove(UFIN_LOG_PATH);
    return check::finish("test_ufin_log on");
}
