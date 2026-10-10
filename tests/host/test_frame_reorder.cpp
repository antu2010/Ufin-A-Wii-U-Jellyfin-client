// Host test for frame_reorder.h: B-frame decode order -> display order.
#include "check.h"
#include "media/frame_reorder.h"
#include <vector>

using R = FrameReorder<int>;

static std::vector<double> run(R& r, const std::vector<double>& decodeOrder, int* lateOut = nullptr) {
    std::vector<double> shown;
    std::vector<R::Entry> out;
    int late = 0;
    int id = 0;
    for (double p : decodeOrder) {
        out.clear();
        r.push(id++, p, out);
        for (auto& e : out) { if (e.late) late++; else shown.push_back(e.pts); }
    }
    out.clear();
    r.flush(out);
    for (auto& e : out) { if (e.late) late++; else shown.push_back(e.pts); }
    if (lateOut) *lateOut = late;
    return shown;
}

static bool ascending(const std::vector<double>& v) {
    for (size_t i = 1; i < v.size(); i++) if (v[i] < v[i - 1]) return false;
    return true;
}

int main() {
    // Baseline: depth 0 passes straight through, nothing held.
    {
        R r(0);
        std::vector<R::Entry> out;
        r.push(1, 0.0, out);
        CHECK_EQ((int)out.size(), 1);
        CHECK_EQ((int)r.held(), 0);
    }
    // Classic I P B B (decode 0 3 1 2 6 4 5 ...): depth 1 is enough.
    {
        R r(1);
        auto s = run(r, {0, 3, 1, 2, 6, 4, 5, 9, 7, 8});
        CHECK_EQ((int)s.size(), 10);
        CHECK(ascending(s));
        CHECK_EQ((int)r.lateCount(), 0);
    }
    // B-pyramid (decode 0 4 2 1 3 8 6 5 7): needs depth 2.
    {
        R r(2);
        auto s = run(r, {0, 4, 2, 1, 3, 8, 6, 5, 7});
        CHECK_EQ((int)s.size(), 9);
        CHECK(ascending(s));
        CHECK_EQ((int)r.lateCount(), 0);
    }
    // Depth too small: a late frame is flagged (dropped), the depth grows, and
    // the output never steps backwards.
    {
        R r(1);
        int late = 0;
        auto s = run(r, {0, 4, 2, 1, 3, 8, 6, 5, 7}, &late);
        CHECK(ascending(s));
        CHECK(late > 0);
        CHECK(r.depth() >= 2);
    }
    // Baseline-like monotonic input through a reorder buffer loses nothing.
    {
        R r(2);
        auto s = run(r, {0, 1, 2, 3, 4, 5});
        CHECK_EQ((int)s.size(), 6);
        CHECK(ascending(s));
    }
    // A frame with no timestamp flushes what is held, then passes through.
    {
        R r(2);
        std::vector<R::Entry> out;
        r.push(0, 0.0, out); r.push(1, 2.0, out);
        out.clear();
        r.push(2, NAN, out);
        CHECK_EQ((int)out.size(), 3);
        CHECK(out[0].pts == 0.0 && out[1].pts == 2.0 && std::isnan(out[2].pts));
        CHECK_EQ((int)r.held(), 0);
    }
    // Depth is capped.
    { R r(99); CHECK_EQ((int)r.depth(), (int)R::kMaxDepth); }
    return check::finish("test_frame_reorder");
}
