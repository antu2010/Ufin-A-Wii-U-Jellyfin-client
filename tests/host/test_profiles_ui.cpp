// The "Who's watching?" screen rendered to pixels: round pictures, a bounce that only the hovered
// picture does (and that really distorts it), the "Auto log in" boxes, dark / light, flat / Liquid
// Glass, a Jellyfin picture, and hit testing.
#include "check.h"
#include "soft_render.h"
#include "ui/app_ui.h"
#include "ui/profiles_ui.h"
#include <imgui.h>

#include <cmath>
#include <cstdlib>
#include <functional>

static const char* previewDir = nullptr;

static SoftRender renderScreen(const std::function<void()>& draw, const char* name) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    SoftRender::handleTextures();
    SoftRender sr(1280, 720);
    sr.render(ImGui::GetDrawData());
    if (previewDir) sr.savePPM(std::string(previewDir) + "/" + name + ".ppm");
    return sr;
}

// Geometry the screen uses (see profiles_ui.inc): 4 columns of 190 with 28 gaps, ground line at 410.
static float colCx(int n, int i) { return (1280.0f - (n * 190.0f + (n - 1) * 28.0f)) * 0.5f + i * 218.0f + 95.0f; }
static const float GROUND = 410.0f, D = 140.0f;

static ui::ProfilesModel sample(int cursor, int row = 0) {
    ui::ProfilesModel m;
    const char* names[3] = {"antu", "mum", "dad"};
    const uint32_t colours[3] = {0xE0483A, 0x27AE60, 0xF28A30};
    for (int i = 0; i < 3; i++) {
        ui::ProfileTile t;
        t.name = names[i];
        t.letter = std::string(1, (char)('A' + i));
        t.color = colours[i];
        m.tiles.push_back(t);
    }
    ui::ProfileTile add;
    add.add = true;
    add.name = "Add profile";
    m.tiles.push_back(add);
    m.cursor = cursor;
    m.focusRow = row;
    m.hints = {{"< >", "Move"}, {"A", "Select"}, {"Y", "Auto log in"}};
    return m;
}

// Counts pixels near an RGB colour inside a box and gives their bounding box.
struct Blob { int count = 0, x0 = 99999, y0 = 99999, x1 = -1, y1 = -1; int w() const { return x1 - x0 + 1; } int h() const { return y1 - y0 + 1; } };
static Blob find(const SoftRender& r, uint32_t rgb, int bx0, int by0, int bx1, int by1, int tol = 60) {
    Blob b;
    const int cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            const uint32_t p = r.at(x, y);
            const int pr = p & 0xFF, pg = (p >> 8) & 0xFF, pb = (p >> 16) & 0xFF;
            if (std::abs(pr - cr) < tol && std::abs(pg - cg) < tol && std::abs(pb - cb) < tol) {
                b.count++;
                b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x);
                b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
            }
        }
    return b;
}

static void testLooks() {
    const bool glassModes[2] = {false, true};
    const bool lightModes[2] = {false, true};
    std::vector<std::vector<uint32_t>> shots;
    for (int li = 0; li < 2; li++)
        for (int gi = 0; gi < 2; gi++) {
            ui::setLightMode(lightModes[li]);
            ui::setGlass(glassModes[gi]);
            if (glassModes[gi]) ui::setGlassSnapshot((uint64_t)7779, &SoftRender::snapshotCallback, (void*)(intptr_t)7779);
            const ui::ProfilesModel m = sample(1);
            const std::string nm = std::string("profiles_") + (lightModes[li] ? "light" : "dark") + (glassModes[gi] ? "_glass" : "_flat");
            SoftRender r = renderScreen([&] { ui::drawProfiles(m); }, nm.c_str());
            ui::setGlassSnapshot(0, nullptr, nullptr);

            // every picture is there, in its own colour, and round: the centre is filled, the square's corners are not
            const int n = 4;
            const uint32_t colours[3] = {0xE0483A, 0x27AE60, 0xF28A30};
            for (int i = 0; i < 3; i++) {
                const float cx = colCx(n, i), cy = GROUND - D * 0.5f;
                const Blob b = find(r, colours[i], (int)cx - 90, (int)cy - 90, (int)cx + 90, (int)cy + 90, 70);
                CHECK(b.count > 7000);                                            // a big disc (pi*70^2 = 15400 at best)
                CHECK(b.w() > 120 && b.w() < 160 && b.h() > 120 && b.h() < 160);  // ~140 across, both ways
                const float d = D * 0.5f * 0.94f;
                const uint32_t corner = r.at((int)(cx - d * 0.98f), (int)(cy - d * 0.98f));   // inside the square, outside the circle
                const int cr = corner & 0xFF, cg = (corner >> 8) & 0xFF, cb = (corner >> 16) & 0xFF;
                const int pr = (colours[i] >> 16) & 0xFF, pg = (colours[i] >> 8) & 0xFF, pb = colours[i] & 0xFF;
                CHECK(std::abs(cr - pr) + std::abs(cg - pg) + std::abs(cb - pb) > 90);   // not the avatar colour
            }
            // the screen is readable on its background: names and title are drawn
            int textPx = 0;
            for (int y = 120; y < 175; y++) for (int x = 380; x < 900; x++) if (r.at(x, y) != r.at(5, y)) textPx++;
            CHECK(textPx > 400);
            shots.push_back(r.px);
        }
    ui::setLightMode(false);
    ui::setGlass(false);
    for (size_t i = 0; i < shots.size(); i++)
        for (size_t j = i + 1; j < shots.size(); j++) CHECK(shots[i] != shots[j]);   // four genuinely different looks
}

static void testBounceOnlyOnHovered() {
    ui::setLightMode(false);
    ui::setGlass(false);
    ui::setBackgroundEffect(ui::Background::Off);
    ui::setScreenBackdrop(0, 0, 0, 0, 0);
    const int n = 4;

    // Drive the real animation: the cursor sits on picture 1 (hovered); the rest are Bounce objects that never get hover.
    ui::Bounce bounce[4];
    SoftRender first = renderScreen([&] { ui::drawProfiles(sample(1)); }, "bounce_rest");
    float minH = 999, maxH = 0, minW = 999, maxW = 0, topMost = 999;
    bool otherMoved = false;
    for (int f = 0; f < 120; f++) {
        for (int i = 0; i < 4; i++) bounce[i].step(1.0f / 60.0f, i == 1);
        ui::ProfilesModel m = sample(1);
        for (int i = 0; i < 4; i++) m.tiles[(size_t)i].bounce = bounce[i].shape();
        SoftRender r = renderScreen([&] { ui::drawProfiles(m); }, "bounce_frame");
        // the hovered picture (green, column 1): measure how wide and tall it is this frame
        const float cx = colCx(n, 1);
        const Blob b = find(r, 0x27AE60, (int)cx - 110, 150, (int)cx + 110, 440, 40);
        if (b.count > 3000) {
            minH = std::min(minH, (float)b.h()); maxH = std::max(maxH, (float)b.h());
            minW = std::min(minW, (float)b.w()); maxW = std::max(maxW, (float)b.w());
            topMost = std::min(topMost, (float)b.y0);
        }
        // every OTHER column is pixel-identical to the first (resting) frame, all the time
        for (int i : {0, 2, 3}) {
            const int x0 = (int)(colCx(n, i) - 100), x1 = (int)(colCx(n, i) + 100);
            for (int y = 150; y < 560 && !otherMoved; y += 2)
                for (int x = x0; x < x1; x += 2)
                    if (r.at(x, y) != first.at(x, y)) { otherMoved = true; break; }
        }
    }
    CHECK(!otherMoved);                          // the others do not bounce
    CHECK(topMost < GROUND - D - 25.0f);          // the hovered one leaves the ground (30% of 140 = 42 px at the top)
    CHECK(maxH > 150.0f);                         // stretched tall (>140)
    CHECK(minH < 118.0f);                         // squashed flat
    CHECK(maxW > 156.0f);                         // ...and wide
    CHECK(minW < 128.0f);                         // ...and thin when stretched
    // the cursor moves to picture 2: now column 2 is the one that moves
    for (auto& b : bounce) b = ui::Bounce();
    bool col1Moved = false, col2Moved = false;
    for (int f = 0; f < 90; f++) {
        for (int i = 0; i < 4; i++) bounce[i].step(1.0f / 60.0f, i == 2);
        ui::ProfilesModel m = sample(2);
        for (int i = 0; i < 4; i++) m.tiles[(size_t)i].bounce = bounce[i].shape();
        SoftRender r = renderScreen([&] { ui::drawProfiles(m); }, "bounce_frame2");
        for (int y = 150; y < 440; y += 2) {
            for (int x = (int)(colCx(n, 1) - 100); x < (int)(colCx(n, 1) + 100); x += 2) if (r.at(x, y) != first.at(x, y)) col1Moved = true;
            for (int x = (int)(colCx(n, 2) - 100); x < (int)(colCx(n, 2) + 100); x += 2) if (r.at(x, y) != first.at(x, y)) col2Moved = true;
        }
    }
    CHECK(col2Moved);
    // (column 1 differs from the all-rest frame only because the cursor ring moved off it: that is a ring, not a bounce)
    (void)col1Moved;
}

static void testCheckAndHits() {
    ui::setLightMode(false);
    ui::setGlass(false);
    const int n = 4;
    ui::ProfilesModel off = sample(0), on = sample(0);
    on.tiles[1].keep = true;
    SoftRender a = renderScreen([&] { ui::drawProfiles(off); }, "check_off");
    SoftRender b = renderScreen([&] { ui::drawProfiles(on); }, "check_on");
    // the ticked box of picture 1 is filled with the accent colour (Jellyfin blue 0x00A4DC); the others are not
    const float cx = colCx(n, 1);
    const Blob ticked = find(b, 0x00A4DC, (int)cx - 95, 470, (int)cx - 40, 520, 40);
    const Blob plain = find(a, 0x00A4DC, (int)cx - 95, 470, (int)cx - 40, 520, 40);
    CHECK(ticked.count > 250 && plain.count < 20);
    const Blob other = find(b, 0x00A4DC, (int)colCx(n, 2) - 95, 470, (int)colCx(n, 2) - 40, 520, 40);
    CHECK(other.count < 20);
    // the Add tile has no box and shows a plus
    const Blob addBox = find(b, 0x00A4DC, (int)colCx(n, 3) - 95, 470, (int)colCx(n, 3) + 95, 520, 40);
    CHECK(addBox.count < 20);

    // focus on a box is visible (outline in flat mode)
    SoftRender f = renderScreen([&] { ui::drawProfiles(sample(1, 1)); }, "check_focus");
    SoftRender nf = renderScreen([&] { ui::drawProfiles(sample(1, 0)); }, "check_nofocus");
    int diff = 0;
    for (int y = 470; y < 520; y++) for (int x = (int)cx - 95; x < (int)cx + 95; x++) if (f.at(x, y) != nf.at(x, y)) diff++;
    CHECK(diff > 200);

    // hit testing
    const ui::ProfilesModel m = sample(0);
    ui::ProfilesHit h = ui::hitTestProfiles(m, colCx(n, 2), GROUND - 60);
    CHECK(h.kind == ui::ProfilesHit::Picture && h.index == 2);
    h = ui::hitTestProfiles(m, colCx(n, 1), 445);                     // on the name
    CHECK(h.kind == ui::ProfilesHit::Picture && h.index == 1);
    h = ui::hitTestProfiles(m, colCx(n, 0) - 60, 494);                // on the box
    CHECK(h.kind == ui::ProfilesHit::Check && h.index == 0);
    h = ui::hitTestProfiles(m, colCx(n, 3), GROUND - 60);             // the Add tile
    CHECK(h.kind == ui::ProfilesHit::Picture && h.index == 3);
    h = ui::hitTestProfiles(m, colCx(n, 0), 540);                     // on the Log out button
    CHECK(h.kind == ui::ProfilesHit::Logout && h.index == 0);
    h = ui::hitTestProfiles(m, colCx(n, 3), 540);                     // the Add tile has none
    CHECK(h.kind == ui::ProfilesHit::None);
    h = ui::hitTestProfiles(m, colCx(n, 3), 494);                     // ...has no box
    CHECK(h.kind == ui::ProfilesHit::None);
    h = ui::hitTestProfiles(m, 20, 20);
    CHECK(h.kind == ui::ProfilesHit::None);
    h = ui::hitTestProfiles(m, 640, 600);
    CHECK(h.kind == ui::ProfilesHit::None);

    // centred whatever the count: 1, 2 and 5 pictures
    for (int count : {1, 2, 5}) {
        ui::ProfilesModel k;
        for (int i = 0; i < count; i++) { ui::ProfileTile t; t.name = "p" + std::to_string(i); t.letter = "P"; k.tiles.push_back(t); }
        double sum = 0;
        for (int i = 0; i < count; i++) {
            const ui::ProfilesHit hh = ui::hitTestProfiles(k, colCx(count, i), GROUND - 60);
            CHECK(hh.kind == ui::ProfilesHit::Picture && hh.index == i);
            sum += colCx(count, i);
        }
        CHECK_NEAR(sum / count, 640.0, 0.5);
        renderScreen([&] { ui::drawProfiles(k); }, ("count" + std::to_string(count)).c_str());
    }
    // a long name and a confirmation message draw without trouble
    ui::ProfilesModel longName = sample(0);
    longName.tiles[0].name = "A very very long profile name indeed";
    longName.message = "Press X again to remove A very very long profile name indeed from this console. B cancels.";
    longName.messageIsError = true;
    renderScreen([&] { ui::drawProfiles(longName); }, "long_name");
}

static void testPicture() {
    ui::setLightMode(false);
    ui::setGlass(false);
    // A Jellyfin picture: solid green 64x64, registered as a user texture; shown as a round crop.
    SoftUserTexture t;
    t.w = 64; t.h = 64;
    t.rgba.assign((size_t)64 * 64 * 4, 0);
    for (size_t i = 0; i < t.rgba.size(); i += 4) { t.rgba[i] = 20; t.rgba[i + 1] = 230; t.rgba[i + 2] = 40; t.rgba[i + 3] = 255; }
    SoftRender::userTextures()[(ImTextureID)4242] = t;
    ui::ProfilesModel m = sample(2);
    m.tiles[0].picture = 4242;
    m.tiles[0].pictureAspect = 1.5f;     // a wide photo: cropped, not squashed
    SoftRender r = renderScreen([&] { ui::drawProfiles(m); }, "picture");
    const float cx = colCx(4, 0), cy = GROUND - D * 0.5f;
    const Blob b = find(r, 0x14E628, (int)cx - 90, (int)cy - 90, (int)cx + 90, (int)cy + 90, 30);
    CHECK(b.count > 7000 && b.w() > 120 && b.w() < 160);
    const uint32_t corner = r.at((int)(cx - 66), (int)(cy - 66));
    CHECK(((corner >> 8) & 0xFF) < 150);                      // the corner of the square is not photo
    // the picture bounces and stretches with its tile
    m.cursor = 0;
    m.tiles[0].bounce.sy = 1.22f; m.tiles[0].bounce.sx = 0.86f; m.tiles[0].bounce.lift = 0.2f;
    SoftRender s = renderScreen([&] { ui::drawProfiles(m); }, "picture_stretch");
    const Blob sb = find(s, 0x14E628, (int)cx - 100, 150, (int)cx + 100, 440, 30);
    CHECK(sb.h() > b.h() + 15 && sb.w() < b.w() - 8);
}

// The "Add profile" disc: a plain plus (both arms, nothing in the corners between them), one flat disc
// (no seams from the way it is filled), in every look.
static void testAddTilePlus() {
    for (int li = 0; li < 2; li++)
        for (int gi = 0; gi < 2; gi++) {
            ui::setLightMode(li == 1);
            ui::setGlass(gi == 1);
            if (gi) ui::setGlassSnapshot((uint64_t)7779, &SoftRender::snapshotCallback, (void*)(intptr_t)7779);
            const ui::ProfilesModel m = sample(0);
            SoftRender r = renderScreen([&] { ui::drawProfiles(m); }, "add_plus");
            ui::setGlassSnapshot(0, nullptr, nullptr);
            const float cx = colCx(4, 3), cy = GROUND - D * 0.5f, rad = D * 0.5f;
            auto px = [&](float u, float v) { return r.at((int)(cx + u * rad), (int)(cy - v * rad)); };
            auto dist = [](uint32_t a, uint32_t b) {
                return std::abs((int)(a & 0xFF) - (int)(b & 0xFF)) + std::abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF)) +
                       std::abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
            };
            const uint32_t disc = px(0.0f, -0.62f);                   // bare disc, below the plus
            // all four arms are drawn in the plus colour, which differs clearly from the disc
            const float arm = 0.28f;
            CHECK(dist(px(arm, 0.0f), disc) > 60);
            CHECK(dist(px(-arm, 0.0f), disc) > 60);
            CHECK(dist(px(0.0f, arm), disc) > 60);
            CHECK(dist(px(0.0f, -arm), disc) > 60);
            // (flat only: Liquid Glass is now the real glass surface -- refraction, sheen and a lit edge --
            // so the disc colour is not uniform there; the arms must still stand out, checked above)
            if (gi == 0) {
            // the corners between the arms are bare disc, not plus
            CHECK(dist(px(0.22f, 0.22f), disc) < 40);
            CHECK(dist(px(-0.22f, 0.22f), disc) < 40);
            CHECK(dist(px(0.22f, -0.22f), disc) < 40);
            CHECK(dist(px(-0.22f, -0.22f), disc) < 40);
            // no seams: points out along the diagonals all match the bare disc colour (the glass highlight sits on
            // the upper-left one, on purpose, so that diagonal is skipped)
            for (int k = 1; k <= 4; k++) {
                const float t = 0.30f + 0.12f * (float)k;
                CHECK(dist(px(t * 0.7f, t * 0.7f), disc) < 40);
                CHECK(dist(px(t * 0.7f, -t * 0.7f), disc) < 40);
                CHECK(dist(px(-t * 0.7f, -t * 0.7f), disc) < 40);
            }
            }
        }
    ui::setLightMode(false);
    ui::setGlass(false);
}

int main(int argc, char** argv) {
    previewDir = getenv("UFIN_UI_PREVIEW");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    if (argc > 1) {
        ImFontConfig cfg;
        io.Fonts->AddFontFromFileTTF(argv[1], 24.0f, &cfg);
    } else {
        io.Fonts->AddFontDefault();
    }
    ui::applyTheme();
    ui::setBackgroundEffect(ui::Background::Off);
    ui::setScreenBackdrop(0, 0, 0, 0, 0);

    testLooks();
    testBounceOnlyOnHovered();
    testCheckAndHits();
    testPicture();
    testAddTilePlus();

    ImGui::DestroyContext();
    return check::finish("test_profiles_ui");
}
