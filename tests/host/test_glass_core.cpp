// Liquid Glass core (src/ui/glass_core.*): blur dimensions, coordinate
// mapping, display scaling, the Dual-Kawase chain, and BackdropManager's
// resource / scheduling / fallback behaviour -- with a fake texture sink, no
// GPU.
#include "check.h"
#include "ui/glass_core.h"

#include <cmath>
#include <map>

using namespace ui::glass;

struct FakeGpu {
    std::map<uint64_t, std::vector<uint8_t>> tex;
    uint64_t next = 100;
    int creates = 0, updates = 0, releases = 0;
    bool failCreateAt = 0; // 1-based create index to fail, 0 = never
    int failCreateIndex = 0;
    bool failUpdates = false;
    std::vector<uint64_t> updateOrder;

    TextureSink sink() {
        TextureSink s;
        s.create = [this](const uint8_t* px, int w, int h) -> uint64_t {
            creates++;
            if (failCreateIndex && creates == failCreateIndex) return 0;
            uint64_t id = next++;
            tex[id].assign(px, px + (size_t)w * h * 4);
            return id;
        };
        s.update = [this](uint64_t id, const uint8_t* px, int w, int h) -> bool {
            if (failUpdates || !tex.count(id)) return false;
            updates++;
            updateOrder.push_back(id);
            tex[id].assign(px, px + (size_t)w * h * 4);
            return true;
        };
        s.release = [this](uint64_t id) {
            releases++;
            tex.erase(id);
        };
        return s;
    }
};

int main() {
    // ---- blur dimensions: ~1/4 of the real target ----
    {
        Dims d = blurDims(854, 480); // the GamePad
        CHECK_EQ(BLUR_DIVISOR, 4);
        CHECK_EQ(d.w, 214); // ceil(854/4)
        CHECK_EQ(d.h, 120);
        d = blurDims(1280, 720); // the TV
        CHECK_EQ(d.w, 320);
        CHECK_EQ(d.h, 180);
        d = blurDims(1920, 1080);
        CHECK_EQ(d.w, 480);
        CHECK_EQ(d.h, 270);
        d = blurDims(30, 20); // tiny target: never below MIN_DIM
        CHECK_EQ(d.w, MIN_DIM);
        CHECK_EQ(d.h, MIN_DIM);
        CHECK(!blurDims(0, 480).valid()); // nonsense in, nothing out
        CHECK(!blurDims(854, -1).valid());
        CHECK(!blurDims(854, 480, 0).valid());
        // One texture for both displays: the smaller per axis.
        Dims s = sharedDims(1280, 720, 854, 480);
        CHECK_EQ(s.w, 214);
        CHECK_EQ(s.h, 120);
        s = sharedDims(1280, 720, 0, 0); // GamePad unavailable
        CHECK_EQ(s.w, 320);
        CHECK(!sharedDims(0, 0, 0, 0).valid());
    }

    // ---- the real 1/4-res texture: 3 passes fit, and a frame's slice stays small ----
    {
        Dims real = sharedDims(1280, 720, 854, 480);
        CHECK_EQ(kawaseLevels(real, 3), 3); // 214x120 -> smallest level 27x15
        CHECK_EQ(BackdropManager::rowsPerUpdate(107), BackdropManager::ROWS_PER_UPDATE);
        CHECK_EQ(BackdropManager::rowsPerUpdate(214), BackdropManager::ROWS_PER_UPDATE / 2); // same pixels per frame
        CHECK(BackdropManager::rowsPerUpdate(100000) >= 2);
        CHECK(BackdropManager::rowsPerUpdate(0) >= 2);
        FakeGpu gpu;
        BackdropManager m;
        CHECK(m.enable(real, gpu.sink(), 3));
        CHECK_EQ(m.levels(), 3);
        CHECK_EQ(m.dims().w, 214);
    }

    // ---- real refraction: lens maths ----
    {
        Lens big = lensFor(820.0f, 380.0f, 37.7f);  // a dialog card
        CHECK(big.edge > 20.0f && big.edge <= 30.0f);       // a bounded band, never the whole panel
        CHECK(big.edge <= 37.7f - 0.6f);                     // fits inside the corner arc (mesh rings keep one vertex count)
        CHECK(big.bend > 0.0f && big.bend < big.edge);       // < edge so the picture never folds over by more than a sliver
        CHECK(big.bend > 0.5f * big.edge);                   // > edge/2: the Apple-style flipped sliver at the rim
        CHECK(big.magnify < 1.0f && big.magnify > 0.96f);    // a faint lens only
        // Interior magnification moves even a screen-wide panel's far corner by <= ~7 px.
        Lens wide = lensFor(1280.0f, 700.0f, 37.7f);
        CHECK(0.5f * 1280.0f * (1.0f - wide.magnify) <= 7.01f);
        // A pill (hint button): the band is limited by the corner radius.
        Lens pill = lensFor(120.0f, 28.0f, 13.0f);
        CHECK(pill.edge >= 3.0f && pill.edge <= 13.0f - 0.6f + 1e-4f);
        // A shape too small to bend reports no band (the caller draws it flat).
        CHECK(lensFor(6.0f, 6.0f, 2.0f).edge < 3.0f);
        CHECK(lensFor(0.0f, 0.0f, 0.0f).edge == 0.0f);
        // The profile: full bend at the rim, none at the inner edge of the band,
        // smooth and monotonic in between.
        CHECK_NEAR(lensShift(big, 0.0f), big.bend, 1e-5);
        CHECK_NEAR(lensShift(big, 1.0f), 0.0f, 1e-6);
        CHECK_NEAR(lensShift(big, -1.0f), big.bend, 1e-5); // out of range is clamped, not extrapolated
        CHECK_NEAR(lensShift(big, 2.0f), 0.0f, 1e-6);
        float prev = lensShift(big, 0.0f);
        bool mono = true;
        for (int i = 1; i <= 100; i++) {
            const float v = lensShift(big, i / 100.0f);
            if (v > prev + 1e-6f) mono = false;
            prev = v;
        }
        CHECK(mono);
        // Mesh ring depths: start at the rim, end at the band's inner edge, strictly rising.
        CHECK_NEAR(LENS_RING_DEPTH[0], 0.0f, 1e-6);
        CHECK_NEAR(LENS_RING_DEPTH[LENS_RINGS - 1], 1.0f, 1e-6);
        for (int i = 1; i < LENS_RINGS; i++) CHECK(LENS_RING_DEPTH[i] > LENS_RING_DEPTH[i - 1]);
        // ...and denser where the bending is steep (the rim) than deep inside.
        CHECK(LENS_RING_DEPTH[1] - LENS_RING_DEPTH[0] < LENS_RING_DEPTH[LENS_RINGS - 1] - LENS_RING_DEPTH[LENS_RINGS - 2]);
    }

    // ---- real refraction: when does a glass shape need a new snapshot? ----
    {
        SnapshotPlanner pl;
        const void* L = (const void*)0x10;
        // The first glass of a frame always takes one (it sees everything drawn before it).
        CHECK(pl.onSurface(1, L, 20, 20, 240, 700));        // sidebar panel
        // A shape sitting on top of that glass needs a fresh one (it must refract the panel too)...
        CHECK(pl.onSurface(1, L, 30, 120, 230, 190));       // a sidebar row
        // ...but shapes that only overlap the panel (already in the latest snapshot)
        // and not each other do not.
        CHECK(!pl.onSurface(1, L, 30, 200, 230, 270));
        CHECK(!pl.onSurface(1, L, 30, 280, 230, 350));
        // A list row on the right: far from everything so far.
        CHECK(!pl.onSurface(1, L, 272, 124, 1232, 192));
        CHECK(!pl.onSurface(1, L, 272, 198, 1232, 266));   // rows do not overlap each other
        CHECK_EQ(pl.snapshotsThisFrame(), 2);
        // A hint button over the right-hand rows' region would overlap glass drawn since the snapshot.
        CHECK(pl.onSurface(1, L, 300, 130, 400, 160));
        CHECK_EQ(pl.snapshotsThisFrame(), 3);
        // A new UI frame starts over: one for the first surface again.
        CHECK(pl.onSurface(2, L, 20, 20, 240, 700));
        CHECK_EQ(pl.snapshotsThisFrame(), 1);
        // The shadow margin counts: touching distance needs a new snapshot, a clear gap does not.
        CHECK(pl.onSurface(2, L, 240 + 2, 20, 400, 100));          // within MARGIN of the first
        CHECK(!pl.onSurface(2, L, 400 + 50, 400, 600, 500));
        // A different draw list renders at a different time: it needs its own snapshot.
        CHECK(pl.onSurface(2, (const void*)0x20, 800, 600, 900, 650));
        CHECK(!pl.onSurface(2, (const void*)0x20, 1000, 600, 1100, 650));
    }
    {
        // Worst case is bounded: stacked glass can never cost more than MAX_PER_FRAME copies.
        SnapshotPlanner pl;
        int taken = 0;
        for (int i = 0; i < 50; i++) taken += pl.onSurface(7, (const void*)0x30, 100, 100, 300, 200) ? 1 : 0; // all overlap
        CHECK_EQ(taken, SnapshotPlanner::MAX_PER_FRAME);
        CHECK_EQ(pl.snapshotsThisFrame(), SnapshotPlanner::MAX_PER_FRAME);
        // ...and a long list of non-overlapping shapes (rect memory stays bounded) takes exactly one.
        SnapshotPlanner list;
        int n = 0;
        for (int i = 0; i < 500; i++) n += list.onSurface(9, (const void*)0x30, (float)(i * 2), 700.0f, (float)(i * 2 + 1), 701.0f) ? 1 : 0;
        CHECK(n >= 1 && n <= SnapshotPlanner::MAX_PER_FRAME);
    }

    // ---- cross-fade: smooth motion from ~1 Hz regeneration ----
    {
        FakeGpu gpu;
        BackdropManager m;
        CHECK(m.enable(Dims{107, 60}, gpu.sink(), 2));
        CHECK_NEAR(m.blend(0.0), 1.0f, 1e-6); // nothing to fade yet
        BackdropParams p;
        double t = 0.0;
        while (gpu.updates < 2) { t += 1.0 / 60.0; m.update(t, p, false); }
        const double swap = t; // second result just landed
        CHECK(m.textureFrom() != m.texture());
        CHECK_NEAR(m.blend(swap), 0.0f, 1e-3);       // starts on the old picture (no pop)
        CHECK(m.blend(swap + 0.25) > 0.0f && m.blend(swap + 0.25) < 0.5f);
        CHECK(m.blend(swap + 0.5) > 0.3f && m.blend(swap + 0.5) < 0.7f);
        CHECK_NEAR(m.blend(swap + 5.0), 1.0f, 1e-6); // finishes on the new one
        CHECK_NEAR(m.blend(swap - 10.0), 0.0f, 1e-6); // clock going backwards is harmless
        // Monotonic, and moves every frame (24+ distinct steps per second at 60 fps).
        float prev = -1.0f; int distinct = 0; bool mono = true;
        for (int i = 0; i < 60; i++) {
            float b = m.blend(swap + i / 60.0);
            if (b < prev - 1e-6f) mono = false;
            if (b > prev + 1e-5f) distinct++;
            prev = b;
        }
        CHECK(mono);
        CHECK(distinct >= 24);
    }

    // ---- pass plan ----
    {
        Dims base{107, 60};
        CHECK_EQ(kawaseLevels(base, 2), 2);
        CHECK_EQ(kawaseLevels(base, 99), MAX_LEVELS); // never more than allocated
        CHECK_EQ(kawaseLevels(Dims{8, 8}, 3), 1);     // level 2 would be 2x2 < 4
        CHECK_EQ(kawaseLevels(Dims{0, 0}, 2), 0);
        Dims l1 = levelDims(base, 1), l2 = levelDims(base, 2);
        CHECK_EQ(l1.w, 54); CHECK_EQ(l1.h, 30);
        CHECK_EQ(l2.w, 27); CHECK_EQ(l2.h, 15);
    }

    // ---- coordinate mapping (virtual 1280x720 -> blur texture) ----
    {
        Uv full = uvForRect(0, 0, 1280, 720, 1280, 720);
        CHECK_NEAR(full.u0, 0.0f, 1e-6); CHECK_NEAR(full.v0, 0.0f, 1e-6);
        CHECK_NEAR(full.u1, 1.0f, 1e-6); CHECK_NEAR(full.v1, 1.0f, 1e-6);
        Uv r = uvForRect(320, 180, 640, 360, 1280, 720); // centre quarter
        CHECK_NEAR(r.u0, 0.25f, 1e-6); CHECK_NEAR(r.v0, 0.25f, 1e-6);
        CHECK_NEAR(r.u1, 0.75f, 1e-6); CHECK_NEAR(r.v1, 0.75f, 1e-6);
        CHECK(r.v0 < r.v1); // v = 0 is the top row, growing downwards (no GX2 flip needed)
        Uv off = uvForRect(-50, 700, 2000, 100, 1280, 720); // partly off-screen: clamped
        CHECK_NEAR(off.u0, 0.0f, 1e-6); CHECK_NEAR(off.u1, 1.0f, 1e-6); CHECK_NEAR(off.v1, 1.0f, 1e-6);
        Uv bad = uvForRect(1, 2, 3, 4, 0, 0); // degenerate screen: whole texture, no NaN
        CHECK_NEAR(bad.u1, 1.0f, 1e-6);

        // The same panel maps to the same UVs on both displays (they are laid
        // out in the same virtual space); only the physical scale differs.
        Scale tv = displayScale(1280, 720, 1280, 720), drc = displayScale(854, 480, 1280, 720);
        CHECK_NEAR(tv.x, 1.0f, 1e-6);
        CHECK_NEAR(drc.x, 854.0f / 1280.0f, 1e-6);
        CHECK_NEAR(drc.y, 480.0f / 720.0f, 1e-6);
        Scale hd = displayScale(1920, 1080, 1280, 720); // a 1080p TV
        CHECK_NEAR(hd.x, 1.5f, 1e-6);
        // The physical rectangle a panel covers on each display, in blur texels.
        Dims d = sharedDims(1280, 720, 854, 480);
        int tx, ty;
        texelForPoint(0, 0, 1280, 720, d, tx, ty);
        CHECK_EQ(tx, 0); CHECK_EQ(ty, 0);
        texelForPoint(1279.9f, 719.9f, 1280, 720, d, tx, ty);
        CHECK_EQ(tx, d.w - 1); CHECK_EQ(ty, d.h - 1);
        texelForPoint(9999, -5, 1280, 720, d, tx, ty); // clamped inside
        CHECK_EQ(tx, d.w - 1); CHECK_EQ(ty, 0);
        texelForPoint(640, 360, 1280, 720, d, tx, ty);
        CHECK_EQ(tx, d.w / 2); CHECK_EQ(ty, d.h / 2);
    }

    // ---- the blur chain itself ----
    {
        BlurChain chain;
        CHECK(!chain.init(Dims{0, 0}, 2));
        CHECK(!chain.ready());
        CHECK(!chain.init(Dims{5000, 5000}, 2)); // absurd size refused, not allocated
        CHECK(chain.init(Dims{107, 60}, 2));
        CHECK_EQ(chain.levels(), 2);
        chain.setLevels(9); // clamped to what was allocated
        CHECK_EQ(chain.levels(), 2);
        chain.setLevels(0);
        CHECK_EQ(chain.levels(), 1);
        chain.setLevels(2);
        CHECK(chain.memoryBytes() > 0 && chain.memoryBytes() < 1024 * 1024); // small

        std::vector<uint8_t> out(107 * 60 * 4, 7);
        BackdropParams p;
        chain.render(p, out.data());
        bool alphaOk = true;
        for (size_t i = 3; i < out.size(); i += 4) if (out[i] != 255) alphaOk = false;
        CHECK(alphaOk);
        // Dark base colour with a soft coloured glow, not black or white.
        auto px = [&](int x, int y, int c) { return (int)out[((size_t)y * 107 + x) * 4 + c]; };
        CHECK(px(53, 30, 0) > 8 && px(53, 30, 2) > 8);
        CHECK(px(53, 30, 0) < 200 && px(53, 30, 2) < 230);

        // Blur really blurs: a hard edge in the synthesized scene is softened,
        // i.e. neighbouring output texels never differ much.
        int maxStep = 0;
        for (int y = 0; y < 60; y++)
            for (int x = 1; x < 107; x++)
                for (int c = 0; c < 3; c++) maxStep = std::max(maxStep, std::abs(px(x, y, c) - px(x - 1, y, c)));
        CHECK(maxStep <= 6);

        // Deterministic; and animated params move the light, static ones don't.
        std::vector<uint8_t> again(out.size());
        chain.render(p, again.data());
        CHECK(again == out);
        BackdropParams later = p;
        later.time = 40.0;
        std::vector<uint8_t> moved(out.size());
        chain.render(later, moved.data());
        CHECK(moved != out);
        BackdropParams s1 = p, s2 = p;
        s1.animated = s2.animated = false;
        s2.time = 999.0;
        std::vector<uint8_t> a(out.size()), b(out.size());
        chain.render(s1, a.data());
        chain.render(s2, b.data());
        CHECK(a == b); // not animated: time is ignored

        // Artwork colour shows through, subtly.
        BackdropParams art = p;
        art.art = Rgb{1.0f, 0.1f, 0.1f};
        art.artStrength = 1.0f;
        std::vector<uint8_t> withArt(out.size());
        chain.render(art, withArt.data());
        long redA = 0, redB = 0;
        for (size_t i = 0; i < out.size(); i += 4) { redA += withArt[i]; redB += out[i]; }
        CHECK(redA > redB);
        CHECK(redA < redB * 2); // ...but subtle, never a red flood

        // One pass and two passes both work, and more passes are smoother.
        chain.setLevels(1);
        std::vector<uint8_t> one(out.size());
        chain.render(later, one.data());
        chain.setLevels(2);
        std::vector<uint8_t> two(out.size());
        chain.render(later, two.data());
        CHECK(one != two);
    }

    // ---- BackdropManager: resources, no per-frame allocation, scheduling ----
    {
        FakeGpu gpu;
        std::vector<std::string> logs;
        BackdropManager m;
        m.setLog([&](const char* s) { logs.push_back(s); });
        CHECK(m.mode() == Mode::Off);
        CHECK_EQ((int)m.texture(), 0);
        m.update(0.0, BackdropParams(), false); // Off: a no-op
        CHECK_EQ(gpu.updates, 0);

        CHECK(m.enable(Dims{107, 60}, gpu.sink(), 2));
        CHECK(m.mode() == Mode::Full);
        CHECK_EQ(gpu.creates, 3); // exactly three textures (fade from / to / free), ever
        CHECK(m.texture() != 0);
        CHECK(m.levels() == 2);
        bool sawInit = false, sawPasses = false, sawMode = false;
        for (auto& l : logs) {
            if (l.find("blur initialized: 107x60") != std::string::npos) sawInit = true;
            if (l.find("passes: 2") != std::string::npos) sawPasses = true;
            if (l.find("Glass mode: enabled") != std::string::npos) sawMode = true;
        }
        CHECK(sawInit && sawPasses && sawMode);
        // 107*60*4*2 textures + float chain: well under a megabyte.
        CHECK(m.memoryBytes() < 1024 * 1024);

        // 600 frames (10 s at 60 fps) of an animated menu: regenerated ~1x a
        // second in small slices, never a new texture, and most frames do no
        // blur work at all.
        BackdropParams p;
        uint32_t slicesBefore = m.stats().slices;
        for (int i = 0; i < 600; i++) m.update(i / 60.0, p, false);
        CHECK(gpu.creates == 3);
        CHECK(gpu.updates >= 7 && gpu.updates <= 13); // ~ 10 s / 1.0 s
        CHECK(m.stats().skipped > 250);
        const uint32_t slices = m.stats().slices - slicesBefore;
        CHECK(slices < 400); // well under one slice per frame

        // A write never lands on a texture still being sampled: the fade source
        // (previous write) and target (latest write) are both left alone.
        bool alternates = true;
        for (size_t i = 1; i < gpu.updateOrder.size(); i++)
            if (gpu.updateOrder[i] == gpu.updateOrder[i - 1]) alternates = false;
        for (size_t i = 2; i < gpu.updateOrder.size(); i++)
            if (gpu.updateOrder[i] == gpu.updateOrder[i - 2]) alternates = false;
        CHECK(alternates);
        CHECK(m.texture() == gpu.updateOrder.back());      // newest blur is the fade target
        CHECK(m.textureFrom() == gpu.updateOrder[gpu.updateOrder.size() - 2]); // previous one fades out

        // Each frame's blur work is bounded: one regeneration takes several
        // update() calls, never one giant call.
        {
            FakeGpu g3;
            BackdropManager sl;
            CHECK(sl.enable(Dims{107, 60}, g3.sink(), 2));
            int calls = 0;
            while (g3.updates == 0 && calls < 100) { sl.update(1000.0 + calls / 60.0, BackdropParams(), false); calls++; }
            CHECK(g3.updates == 1);
            CHECK(calls >= 195 / BackdropManager::ROWS_PER_UPDATE); // 195 rows of work / 16 per frame
            CHECK(calls <= 195 / BackdropManager::ROWS_PER_UPDATE + 2);
        }

        // A static scene is generated once and reused.
        FakeGpu g2;
        BackdropManager st;
        CHECK(st.enable(Dims{107, 60}, g2.sink(), 2));
        BackdropParams still;
        still.animated = false;
        for (int i = 0; i < 300; i++) st.update(i / 60.0, still, false);
        CHECK_EQ(g2.updates, 1);
        still.artStrength = 0.6f; // the scene changed (new artwork colour): regenerate
        still.art = Rgb{0.9f, 0.5f, 0.1f};
        for (int i = 0; i < 40; i++) st.update(6.0 + i / 60.0, still, false);
        CHECK_EQ(g2.updates, 2);

        // PLAYBACK WINS: while video is presented nothing is regenerated and
        // no texture is written, however long that goes on.
        int before = gpu.updates;
        uint64_t shownBefore = m.texture();
        for (int i = 0; i < 1000; i++) m.update(100.0 + i / 60.0, p, true);
        CHECK_EQ(gpu.updates, before);
        CHECK(m.texture() == shownBefore); // last blur stays available (GamePad remote screen)
        for (int i = 0; i < 40; i++) m.update(200.0 + i / 60.0, p, false); // and it resumes afterwards
        CHECK(gpu.updates >= before + 1);

        // Disable releases everything; twice is harmless; re-enable works.
        m.disable();
        CHECK(m.mode() == Mode::Off);
        CHECK_EQ(gpu.releases, 3);
        CHECK_EQ((int)m.texture(), 0);
        CHECK_EQ((int)gpu.tex.size(), 0); // nothing leaked
        m.disable();
        CHECK_EQ(gpu.releases, 3);
        CHECK(m.enable(Dims{107, 60}, gpu.sink(), 2));
        CHECK_EQ((int)gpu.tex.size(), 3);
        m.disable();
        CHECK_EQ((int)gpu.tex.size(), 0);
    }

    // ---- fallback: failures degrade to flat glass, never crash or leak ----
    {
        FakeGpu gpu;
        gpu.failCreateIndex = 2; // the second texture cannot be allocated
        std::vector<std::string> logs;
        BackdropManager m;
        m.setLog([&](const char* s) { logs.push_back(s); });
        CHECK(!m.enable(Dims{107, 60}, gpu.sink(), 2));
        CHECK(m.mode() == Mode::Flat);
        CHECK_EQ((int)m.texture(), 0);      // panels get no texture -> tint + gloss only
        CHECK_EQ((int)gpu.tex.size(), 0);   // the first texture was released again
        bool said = false;
        for (auto& l : logs) if (l.find("flat glass") != std::string::npos) said = true;
        CHECK(said);
        m.update(1.0, BackdropParams(), false); // harmless in Flat
        CHECK_EQ(gpu.updates, 0);

        FakeGpu bad;
        BackdropManager m2;
        CHECK(!m2.enable(Dims{0, 0}, bad.sink(), 2)); // invalid dims
        CHECK(m2.mode() == Mode::Flat);
        CHECK_EQ(bad.creates, 0); // never touched the GPU
        BackdropManager m3;
        CHECK(!m3.enable(Dims{107, 60}, TextureSink(), 2)); // no sink
        CHECK(m3.mode() == Mode::Flat);

        // Updates that keep failing end in flat glass with textures released.
        FakeGpu flaky;
        BackdropManager m4;
        CHECK(m4.enable(Dims{107, 60}, flaky.sink(), 2));
        flaky.failUpdates = true;
        for (int i = 0; i < 200; i++) m4.update(i / 60.0, BackdropParams(), false);
        CHECK(m4.mode() == Mode::Flat);
        CHECK_EQ((int)flaky.tex.size(), 0);
        CHECK(m4.stats().updateFailures >= 3);
    }

    return check::finish("test_glass_core");
}
