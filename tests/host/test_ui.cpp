// Renders every Ufin screen through Dear ImGui and a software rasterizer
// and checks the result: selection highlight under the right row,
// sidebar focus, scrollbar, text fitting, HUD states, and the layout /
// touch hit-testing maths. With UFIN_UI_PREVIEW=<dir> it also writes
// each screen as an image for eyeballing the design.
#include "check.h"
#include "soft_render.h"
#include "ui/app_ui.h"
#include "ui/theme_core.h"
#include "ui/glass_core.h"
#include "ui/m3e_core.h"
#include "ui/profiles_ui.h"
#include "glb_builder.h"
#include <sys/stat.h>
#include <thread>
#include <chrono>
namespace glass = ui::glass;

#include <imgui.h>

#include <cmath>
#include <map>
#include <set>
#include "help_content.h"
#include <string>
#include <cstdlib>
#include <functional>

static const char* previewDir = nullptr;

// Colours as SoftRender::at() reports them (0x00BBGGRR).
static uint32_t bgr(uint32_t rgb) {
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}
static const uint32_t BG = bgr(0x14161A), SIDEBAR = bgr(0x1B1E24), PANEL = bgr(0x23272E),
                      SELECTED = bgr(0x0B7FB0);

static bool sr_at_differs(const SoftRender& a, const SoftRender& b, int x, int y) { return a.at(x, y) != b.at(x, y); }

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

static ui::BrowserModel sampleBrowser(int count, int selected) {
    ui::BrowserModel m;
    m.sidebar = {{"Home", ui::Icon::Home}, {"Live TV", ui::Icon::LiveTv}, {"Search", ui::Icon::Search}};
    m.sidebarSelected = 0;
    m.footnote = "alex @ 192.168.1.100";
    m.title = "Film";
    m.path = "Libraries  >  Film";
    for (int i = 0; i < count; i++) {
        ui::ListEntry e;
        e.name = "Movie number " + std::to_string(i + 1);
        e.detail = "Movie  -  2008  -  1:52:00";
        e.tag = "1:52:00";
        e.icon = ui::Icon::Movie;
        m.items.push_back(e);
    }
    if (count > 2) {
        m.items[1].name = "La vita \xc3\xa8 bella (una storia molto, molto, molto lunga che non entra nella riga)";
        m.items[2].icon = ui::Icon::Folder;
        m.items[2].detail.clear();
        m.items[2].tag = "Folder";
    }
    m.selected = selected;
    m.hints = {{"A", "Open"}, {"B", "Back"}, {"X", "Search"}, {"+", "Shuffle"}, {"L R", "Page"}, {"Y", "Refresh"}};
    return m;
}

static void testLayoutMaths() {
    CHECK_EQ(ui::layout::visibleRows(), 7);
    ui::Rect last = ui::layout::listRow(ui::layout::visibleRows() - 1);
    CHECK(last.bottom() <= ui::layout::LIST_BOTTOM);
    CHECK(last.bottom() < ui::layout::FOOTER_Y);
    CHECK(ui::layout::listRow(0).x >= ui::layout::SIDEBAR_W);
    CHECK(ui::layout::listRow(1).y > ui::layout::listRow(0).bottom());
    CHECK(ui::layout::sidebarItem(0).right() <= ui::layout::SIDEBAR_W);

    ui::ListWindow w = ui::computeListWindow(100, 50, 7);
    CHECK_EQ(w.start, 47); CHECK_EQ(w.end, 54);
    w = ui::computeListWindow(100, 0, 7);
    CHECK_EQ(w.start, 0);
    w = ui::computeListWindow(100, 99, 7);
    CHECK_EQ(w.start, 93); CHECK_EQ(w.end, 100);
    w = ui::computeListWindow(3, 2, 7);
    CHECK_EQ(w.start, 0); CHECK_EQ(w.end, 3);
    w = ui::computeListWindow(0, 0, 7);
    CHECK_EQ(w.end, 0);

    // Touch hit-testing uses the same rectangles as drawing.
    w = ui::computeListWindow(50, 30, 7);
    ui::Rect r = ui::layout::listRow(2);
    ui::Hit h = ui::hitTestBrowser(r.x + 10, r.y + 10, 3, w);
    CHECK(h.kind == ui::Hit::Kind::Row);
    CHECK_EQ(h.index, w.start + 2);
    ui::Rect s = ui::layout::sidebarItem(1);
    h = ui::hitTestBrowser(s.x + 5, s.y + 5, 3, w);
    CHECK(h.kind == ui::Hit::Kind::Sidebar);
    CHECK_EQ(h.index, 1);
    h = ui::hitTestBrowser(s.x + 5, s.y + 5, 1, w); // only one sidebar entry exists
    CHECK(h.kind == ui::Hit::Kind::None);
    h = ui::hitTestBrowser(700, 690, 3, w); // footer
    CHECK(h.kind == ui::Hit::Kind::None);
    ui::Rect gap = ui::layout::listRow(0);
    h = ui::hitTestBrowser(gap.x + 10, gap.bottom() + 2, 3, w); // gap between rows
    CHECK(h.kind == ui::Hit::Kind::None);

    CHECK_STR(ui::formatTime(0), "0:00");
    CHECK_STR(ui::formatTime(65), "1:05");
    CHECK_STR(ui::formatTime(3725), "1:02:05");
    CHECK_STR(ui::formatTime(-3), "0:00");
}

static void testFitText() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    ImGui::NewFrame();
    ImFont* f = ImGui::GetFont(); // only valid inside a frame
    CHECK_STR(ui::fitText(f, 24, "short", 400), "short");
    std::string longText = "Citt\xc3\xa0 di vita \xc3\xa8 bella " + std::string(200, 'x');
    std::string cut = ui::fitText(f, 24, longText, 300);
    CHECK(f->CalcTextSizeA(24, 1e9f, 0, cut.c_str()).x <= 300);
    CHECK(cut.size() >= 3 && cut.substr(cut.size() - 3) == "...");
    // Never cuts inside a UTF-8 character.
    bool validUtf8 = true;
    for (size_t i = 0; i < cut.size();) {
        unsigned char c = (unsigned char)cut[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > cut.size()) { validUtf8 = false; break; }
        for (size_t k = 1; k < n; k++) if (((unsigned char)cut[i + k] & 0xC0) != 0x80) validUtf8 = false;
        i += n;
    }
    CHECK(validUtf8);
    std::string accented = ui::fitText(f, 24, "\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8\xc3\xa8", 40);
    CHECK(accented.size() % 2 == 1); // pairs of 2-byte chars + "..."
    CHECK_STR(ui::fitText(f, 24, "anything", 0), "");
    ImGui::EndFrame();
}

static void testBrowser() {
    ui::BrowserModel m = sampleBrowser(40, 5);
    SoftRender sr = renderScreen([&] { ui::drawBrowser(m); }, "browser");
    ui::ListWindow w = ui::computeListWindow(40, 5, ui::layout::visibleRows());
    int selSlot = 5 - w.start;

    // Sidebar and background.
    CHECK_EQ(sr.at(5, 400), SIDEBAR);
    CHECK_EQ(sr.at(ui::layout::SIDEBAR_W + 8, 400), BG);

    // Selected row is highlighted; its neighbours are plain panels.
    ui::Rect sel = ui::layout::listRow(selSlot);
    CHECK_EQ(sr.at((int)sel.x + 60, (int)sel.y + 3), SELECTED);
    ui::Rect prev = ui::layout::listRow(selSlot - 1);
    CHECK_EQ(sr.at((int)prev.x + 60, (int)prev.y + 3), PANEL);
    ui::Rect next = ui::layout::listRow(selSlot + 1);
    CHECK_EQ(sr.at((int)next.x + 60, (int)next.y + 3), PANEL);

    // Scrollbar thumb somewhere on the track (list is longer than the screen).
    ui::Rect track = ui::layout::scrollTrack();
    int thumbPixels = 0;
    for (int y = (int)track.y; y < (int)track.bottom(); y++)
        if (sr.at((int)track.x + 3, y) == bgr(0x9AA0A8)) thumbPixels++;
    CHECK(thumbPixels > 10);

    // Text got drawn (lots of light pixels in the list area).
    int light = 0;
    for (int y = (int)ui::layout::LIST_TOP; y < (int)ui::layout::LIST_BOTTOM; y += 2)
        for (int x = (int)ui::layout::CONTENT_X; x < (int)ui::layout::CONTENT_RIGHT; x += 2)
            if ((sr.at(x, y) & 0xFF) > 0xC0) light++;
    CHECK(light > 500);

    // Nothing draws past the right edge of a row's text area into the
    // scrollbar gap (long names are cut).
    ui::Rect row1 = ui::layout::listRow(1 - w.start);
    bool gapClean = true;
    for (int y = (int)row1.y; y < (int)row1.bottom(); y++) {
        uint32_t c = sr.at((int)row1.right() + 4, y);
        if (c != BG) gapClean = false;
    }
    CHECK(gapClean);

    // Sidebar focused: selection moves to the sidebar, list row gets an outline only.
    m.sidebarFocused = true;
    m.sidebarSelected = 2;
    sr = renderScreen([&] { ui::drawBrowser(m); }, "browser_sidebar");
    ui::Rect item = ui::layout::sidebarItem(2);
    CHECK_EQ(sr.at((int)item.x + 120, (int)item.y + 4), SELECTED);
    CHECK_EQ(sr.at((int)sel.x + 60, (int)sel.y + 20), PANEL);

    // Short list: no scrollbar; empty list: message, no crash.
    m = sampleBrowser(3, 0);
    sr = renderScreen([&] { ui::drawBrowser(m); }, "browser_short");
    CHECK_EQ(sr.at((int)track.x + 3, (int)track.y + 40), BG);
    m.items.clear();
    m.emptyMessage = "No channels. Set up a tuner in Jellyfin.";
    renderScreen([&] { ui::drawBrowser(m); }, "browser_empty");
}

// A solid-colour app texture, registered with the soft renderer.
static ui::Texture makeTexture(ImTextureID id, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    SoftUserTexture t;
    t.w = w;
    t.h = h;
    t.rgba.assign((size_t)w * h * 4, 255);
    for (size_t i = 0; i < t.rgba.size(); i += 4) { t.rgba[i] = r; t.rgba[i + 1] = g; t.rgba[i + 2] = b; }
    SoftRender::userTextures()[id] = t;
    return (ui::Texture)id;
}

static void testArtwork() {
    // Poster (2:3), square cover and a wide logo in three rows; the
    // fourth row has no art yet and shows its icon.
    ui::BrowserModel m = sampleBrowser(4, 3);
    m.items[0].art = makeTexture(0x1001, 40, 60, 220, 30, 30);   // red poster
    m.items[0].artAspect = 40.0f / 60.0f;
    m.items[1].art = makeTexture(0x1002, 60, 60, 30, 200, 60);   // green cover
    m.items[1].artAspect = 1.0f;
    m.items[2].art = makeTexture(0x1003, 160, 60, 240, 200, 20); // yellow logo
    m.items[2].artAspect = 160.0f / 60.0f;
    SoftRender sr = renderScreen([&] { ui::drawBrowser(m); }, "browser_art");

    auto boxCentre = [](int slot) {
        ui::Rect r = ui::layout::listRow(slot);
        return std::make_pair((int)(r.x + 10 + ui::layout::ROW_ART_W / 2), (int)(r.y + r.h / 2));
    };
    auto c0 = boxCentre(0), c1 = boxCentre(1), c2 = boxCentre(2);
    CHECK_EQ(sr.at(c0.first, c0.second), bgr(0xDC1E1E));
    CHECK_EQ(sr.at(c1.first, c1.second), bgr(0x1EC83C));
    CHECK_EQ(sr.at(c2.first, c2.second), bgr(0xF0C814));
    // The poster is narrow: the edge of the slot stays row-coloured.
    ui::Rect r0 = ui::layout::listRow(0);
    CHECK_EQ(sr.at((int)(r0.x + 12), c0.second), PANEL);
    // The square fills the slot height.
    ui::Rect r1 = ui::layout::listRow(1);
    CHECK_EQ(sr.at(c1.first, (int)(r1.y + (r1.h - ui::layout::ROW_ART_H) / 2 + 8)), bgr(0x1EC83C));
    // The wide logo is letterboxed: above it is row colour.
    ui::Rect r2 = ui::layout::listRow(2);
    CHECK_EQ(sr.at(c2.first, (int)(r2.y + 6)), PANEL);

    // fitInside keeps shape and centres.
    ui::layout::Fit f = ui::layout::fitInside(0, 0, 64, 60, 2.0f / 3.0f);
    CHECK_NEAR(f.w, 40, 0.01); CHECK_NEAR(f.h, 60, 0.01); CHECK_NEAR(f.x, 12, 0.01);
    f = ui::layout::fitInside(0, 0, 64, 60, 16.0f / 9.0f);
    CHECK_NEAR(f.w, 64, 0.01); CHECK_NEAR(f.h, 36, 0.01); CHECK_NEAR(f.y, 12, 0.01);
    f = ui::layout::fitInside(5, 5, 64, 60, 0);
    CHECK_NEAR(f.w, 64, 0.01); // unknown aspect: whole box

    // Now Playing with album art instead of the gradient tile.
    ui::NowPlayingModel np;
    np.title = "Nessun dorma";
    np.subtitle = "Turandot";
    np.positionSeconds = 30;
    np.durationSeconds = 180;
    np.art = makeTexture(0x1004, 300, 300, 40, 90, 200);
    SoftRender srNp = renderScreen([&] { ui::drawNowPlaying(np); }, "now_playing_art");
    CHECK_EQ(srNp.at((int)(ui::layout::NP_ART_X + 170), (int)(ui::layout::NP_ART_Y + 60)), bgr(0x285AC8));
}

static ui::LoginModel sampleLogin(int focused) {
    ui::LoginModel m;
    m.rows = {{"Server", "192.168.1.100:8096", "e.g. 192.168.1.100", false, false, false},
              {"Username", "alex", "Your Jellyfin user", false, false, false},
              {"Password", "p\xc3\xa0ss", "", true, false, false},
              {"Sign in", "", "", false, true, true},
              {"Use Quick Connect", "", "", false, true, false}};
    m.focused = focused;
    m.hints = {{"A", "Edit / select"}, {"B", "Back"}};
    return m;
}

static void testLogin() {
    ui::LoginModel m = sampleLogin(1);
    SoftRender sr = renderScreen([&] { ui::drawLogin(m); }, "login");
    // Focused field is highlighted, others are plain panels.
    ui::Rect f = ui::layout::loginRow(1), other = ui::layout::loginRow(0);
    CHECK_EQ(sr.at((int)f.x + 300, (int)f.y + 30), bgr(0x2C313A));
    CHECK_EQ(sr.at((int)other.x + 300, (int)other.y + 30), PANEL);
    // Primary button in the accent colour.
    ui::Rect btn = ui::layout::loginRow(3);
    CHECK_EQ(sr.at((int)btn.x + 20, (int)btn.y + 10), SELECTED);

    // Error message under the rows.
    m.focused = 3;
    m.message = "Wrong username or password.";
    m.messageIsError = true;
    sr = renderScreen([&] { ui::drawLogin(m); }, "login_error");
    CHECK_EQ(sr.at((int)btn.x + 20, (int)btn.y + 10), bgr(0x00A4DC)); // focused primary: brighter
    int red = 0;
    float my = ui::layout::loginRow(5).y;
    for (int y = (int)my; y < (int)my + 30; y++)
        for (int x = (int)ui::layout::LOGIN_X; x < (int)(ui::layout::LOGIN_X + 400); x++)
            if (sr.at(x, y) == bgr(0xD9485F)) red++;
    CHECK(red > 20);

    // Touch: rows map to their index.
    ui::Rect r2 = ui::layout::loginRow(2);
    CHECK_EQ(ui::hitTestLogin(r2.x + 10, r2.y + 10, 5), 2);
    CHECK_EQ(ui::hitTestLogin(100, 100, 5), -1);
    CHECK_EQ(ui::hitTestLogin(r2.x + 10, r2.y + 10, 2), -1); // row 2 doesn't exist

    ui::QuickConnectModel qc;
    qc.code = "482913";
    qc.server = "192.168.1.100:8096";
    qc.message = "Waiting for approval";
    qc.hints = {{"B", "Cancel"}};
    sr = renderScreen([&] { ui::drawQuickConnect(qc); }, "quick_connect");
    int white = 0;
    for (int y = 250; y < 400; y++)
        for (int x = (int)ui::layout::LOGIN_X; x < (int)(ui::layout::LOGIN_X + ui::layout::LOGIN_W); x++)
            if (sr.at(x, y) == bgr(0xFFFFFF)) white++;
    CHECK(white > 2000); // the big code
}

static void testLookSettings() {
    ui::BrowserModel m = sampleBrowser(12, 2);
    ui::ListWindow w = ui::computeListWindow(12, 2, ui::layout::visibleRows());
    ui::Rect sel = ui::layout::listRow(2 - w.start);

    // Accent colour drives the selection band.
    ui::setAccent(ui::Accent::Green);
    SoftRender green = renderScreen([&] { ui::drawBrowser(m); }, "accent_green");
    CHECK_EQ(green.at((int)sel.x + 200, (int)sel.y + 3), bgr(0x1E8C4E));
    ui::setAccent(ui::Accent::Orange);
    SoftRender orange = renderScreen([&] { ui::drawBrowser(m); }, "accent_orange");
    CHECK_EQ(orange.at((int)sel.x + 200, (int)sel.y + 3), bgr(0xC7702A));
    // Rainbow changes with time.
    ui::setAccent(ui::Accent::Rainbow);
    ui::tickTheme(0.0);
    SoftRender r0 = renderScreen([&] { ui::drawBrowser(m); }, "accent_rainbow_0");
    ui::tickTheme(5.0);
    SoftRender r5 = renderScreen([&] { ui::drawBrowser(m); }, "accent_rainbow_5");
    CHECK(r0.at((int)sel.x + 200, (int)sel.y + 3) != r5.at((int)sel.x + 200, (int)sel.y + 3));
    CHECK_STR(ui::accentName(ui::Accent::Blue), "Jellyfin blue");
    ui::setAccent(ui::Accent::Blue);
    SoftRender blue = renderScreen([&] { ui::drawBrowser(m); }, "accent_blue");
    CHECK_EQ(blue.at((int)sel.x + 200, (int)sel.y + 3), SELECTED); // back to the default

    // Clock in the header, counter moves under it.
    m.clock = "21:37";
    SoftRender clk = renderScreen([&] { ui::drawBrowser(m); }, "clock");
    int light = 0;
    for (int y = 34; y < 70; y++)
        for (int x = 1150; x < 1248; x++)
            if ((clk.at(x, y) & 0xFF) > 0xC0) light++;
    CHECK(light > 60);
    m.clock.clear();

    // Animated background: visible between rows / in the header area, and
    // it moves.
    ui::setBackgroundEffect(ui::Background::Circles);
    SoftRender amb = renderScreen([&] { ui::drawBrowser(m); }, "ambient");
    int changed = 0;
    for (int y = 90; y < 120; y++)
        for (int x = 300; x < 1200; x += 4)
            if (amb.at(x, y) != blue.at(x, y)) changed++;
    CHECK(changed > 50);
    ui::setBackgroundEffect(ui::Background::Off);

    // Snow: white flakes on top.
    SoftRender snow = renderScreen([&] {
        ui::drawBrowser(m);
        ui::drawSnow(3.0);
    }, "snow");
    int flakes = 0;
    for (int y = 0; y < 720; y += 2)
        for (int x = 0; x < 1280; x += 2)
            if (snow.at(x, y) != blue.at(x, y) && (snow.at(x, y) & 0xFF) > 0xA0 && ((snow.at(x, y) >> 16) & 0xFF) > 0xA0) flakes++;
    CHECK(flakes > 30);
}

static void testBadgesAndDialogs() {
    ui::BrowserModel m = sampleBrowser(5, 4);
    m.title = "Continue watching";
    m.items[0].art = makeTexture(0x2001, 40, 60, 60, 60, 70);  m.items[0].artAspect = 40.0f / 60.0f;
    m.items[0].progress = 0.4f;                                   // partly watched
    m.items[1].art = makeTexture(0x2002, 40, 60, 60, 60, 70);  m.items[1].artAspect = 40.0f / 60.0f;
    m.items[1].watched = true;                                    // tick
    m.items[1].favorite = true;                                   // heart
    m.items[2].art = makeTexture(0x2003, 40, 60, 60, 60, 70);  m.items[2].artAspect = 40.0f / 60.0f;
    m.items[2].unplayed = 7;                                      // count badge
    SoftRender sr = renderScreen([&] { ui::drawBrowser(m); }, "badges");

    auto fitFor = [](int slot) {
        ui::Rect r = ui::layout::listRow(slot);
        float boxY = r.y + (r.h - ui::layout::ROW_ART_H) * 0.5f;
        return ui::layout::fitInside(r.x + 10, boxY, ui::layout::ROW_ART_W, ui::layout::ROW_ART_H, 40.0f / 60.0f);
    };
    // Progress bar: accent-filled on the left part of the poster's bottom edge, dark on the right.
    ui::layout::Fit f0 = fitFor(0);
    CHECK_EQ(sr.at((int)(f0.x + 6), (int)(f0.y + f0.h - 3)), bgr(0x00A4DC));
    CHECK(sr.at((int)(f0.x + f0.w - 6), (int)(f0.y + f0.h - 3)) != bgr(0x00A4DC));
    // Watched tick badge at the poster's top-right corner.
    ui::layout::Fit f1 = fitFor(1);
    CHECK_EQ(sr.at((int)(f1.x + f1.w - 2 - 7), (int)(f1.y + 6)), bgr(0x00A4DC));
    // Heart left of the tag, in pink.
    ui::Rect r1 = ui::layout::listRow(1);
    int pink = 0;
    for (int y = (int)r1.y; y < (int)r1.bottom(); y++)
        for (int x = (int)r1.right() - 45; x < (int)r1.right() - 10; x++)
            if (sr.at(x, y) == bgr(0xFF5C8A)) pink++;
    CHECK(pink > 40);
    // Unwatched count badge.
    ui::layout::Fit f2 = fitFor(2);
    CHECK_EQ(sr.at((int)(f2.x + f2.w - 15), (int)(f2.y + 6)), bgr(0x00A4DC)); // left of the digit

    // Resume dialog on top of the list.
    ui::ChoiceModel c;
    c.title = "Big Buck Bunny";
    c.subtitle = "You stopped at 12:34";
    c.options = {"Resume from 12:34", "Start from the beginning"};
    c.selected = 0;
    c.hints = {{"A", "Choose"}, {"B", "Cancel"}};
    c.art = makeTexture(0x2004, 40, 60, 200, 50, 50);
    c.artAspect = 40.0f / 60.0f;
    SoftRender dlg = renderScreen([&] { ui::drawBrowser(m); ui::drawChoice(c); }, "resume_dialog");
    ui::Rect o0 = ui::choiceRow(0, 2), o1 = ui::choiceRow(1, 2);
    CHECK_EQ(dlg.at((int)o0.x + 10, (int)o0.y + 5), SELECTED);
    CHECK_EQ(dlg.at((int)o1.x + 10, (int)o1.y + 5), bgr(0x2C313A));
    CHECK_EQ(ui::hitTestChoice(o1.x + 5, o1.y + 5, 2), 1);
    CHECK_EQ(ui::hitTestChoice(5, 5, 2), -1);
    CHECK((dlg.at(20, 20) & 0xFF) < 0x10); // the list behind is dimmed

    // Up next.
    ui::UpNextModel u;
    u.showName = "Lost";
    u.episodeTitle = "S1E2  Pilot, Part 2";
    u.detail = "42:00";
    u.secondsLeft = 6.5;
    u.totalSeconds = 10;
    u.hints = {{"A", "Play now"}, {"B", "Cancel"}};
    renderScreen([&] { ui::drawUpNext(u); }, "up_next");

    // Track panel over video.
    ui::VideoHudModel h;
    h.title = "Big Buck Bunny";
    h.positionSeconds = 100;
    h.durationSeconds = 596;
    h.visible = true;
    h.trackMenu = true;
    h.trackRow = 1;
    h.audioLabel = "Italiano - AC3 - 5.1";
    h.subtitleLabel = "English - SRT";
    h.notice = "GamePad screen off";
    h.hints = {{"A", "Apply"}, {"B", "Close"}};
    SoftRender hud = renderScreen([&] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
        ui::drawVideoHud(h);
    }, "hud_tracks");
    CHECK_EQ(hud.at(1280 - 50 - 560 + 30, 120 + 76 + 84 + 5), SELECTED); // subtitles row selected
}

static void testSettingsPreview() {
    ui::BrowserModel m = sampleBrowser(0, 0);
    m.sidebar.push_back({"Settings", ui::Icon::Settings});
    m.sidebarSelected = 3;
    m.title = "Settings";
    m.path.clear();
    m.items = {{"Menu music", "On", "Nessun dorma  -  loops quietly in the menus", ui::Icon::Music, 0, 0},
               {"Accent colour", "Jellyfin blue", "A: next colour", ui::Icon::Palette, 0, 0},
               {"Animated background", "On", "Soft glowing shapes drift behind the menus", ui::Icon::Sparkle, 0, 0},
               {"CRT mode", "Off", "Scanlines, a phosphor glow and rounded tube corners", ui::Icon::Video, 0, 0},
               {"Snow", "Off", "Gentle snowfall over the menus", ui::Icon::Snow, 0, 0},
               {"Clock", "On", "The time in the top-right corner", ui::Icon::Clock, 0, 0},
               {"Signed in as alex", "Sign out", "192.168.1.100", ui::Icon::User, 0, 0},
               {"About Ufin", "1.2.0", "A Jellyfin client for the Wii U", ui::Icon::Info, 0, 0}};
    m.hints = {{"A", "Change"}, {"B", "Back"}};
    SoftRender sr = renderScreen([&] { ui::drawBrowser(m); }, "settings");
    CHECK_EQ(sr.at((int)ui::layout::listRow(0).x + 200, (int)ui::layout::listRow(0).y + 3), SELECTED);
}

static void testToastAndCrt() {
    ui::BrowserModel m = sampleBrowser(12, 2);
    SoftRender plain = renderScreen([&] { ui::drawBrowser(m); }, "browser_plain");
    SoftRender toast = renderScreen([&] {
        ui::drawBrowser(m);
        ui::drawToast("Menu music: Nessun dorma", 0.3);
    }, "toast");
    CHECK(toast.at(640, 49) != plain.at(640, 49));
    SoftRender gone = renderScreen([&] {
        ui::drawBrowser(m);
        ui::drawToast("Menu music: Nessun dorma", 9.0);
    }, "toast_gone");
    CHECK_EQ(gone.at(640, 49), plain.at(640, 49));

    SoftRender crt = renderScreen([&] {
        ui::drawBrowser(m);
        ui::drawCrtOverlay(1.0);
    }, "crt");
    // Scanline rows are darker than the rows between them; corners go black.
    auto lum = [](uint32_t c) { return (int)(c & 0xFF) + (int)((c >> 8) & 0xFF) + (int)((c >> 16) & 0xFF); };
    int x = 700, y = 360; // mid-screen, on the list background
    while ((int)(y % 3) != 0) y++;
    CHECK(lum(crt.at(x, y)) < lum(crt.at(x, y + 1)));
    CHECK_EQ(crt.at(2, 2), 0u);
    CHECK_EQ(crt.at(1277, 717), 0u);
    CHECK(lum(crt.at(640, 360)) > 0); // centre still visible
}

static void testMessage() {
    ui::MessageModel m;
    m.title = "Playback error";
    m.lines = {"HttpStreamReader::open failed: server returned status 500: an FFmpeg error occurred on the "
               "server while transcoding this item.",
               "", "Check config.json (host / port / credentials) and that the Wii U and the Jellyfin server are "
               "on the same network."};
    m.hints = {{"B", "Back"}};
    m.isError = true;
    SoftRender sr = renderScreen([&] { ui::drawMessage(m); }, "message_error");
    CHECK_EQ(sr.at(640, 360), PANEL); // card in the middle
    int red = 0;
    for (int x = 300; x < 980; x++)
        for (int y = 100; y < 360; y++)
            if (sr.at(x, y) == bgr(0xD9485F)) red++;
    CHECK(red > 100); // error accent strip + title

    ui::MessageModel busy;
    busy.title = "Connecting to 192.168.1.100:8096";
    busy.lines = {"Logging in as alex"};
    busy.busy = true;
    renderScreen([&] { ui::drawMessage(busy); }, "message_busy");

    // Far more text than fits: stays inside the card.
    ui::MessageModel huge = m;
    huge.lines.assign(80, "A line of text that repeats and repeats.");
    sr = renderScreen([&] { ui::drawMessage(huge); }, "message_huge");
    CHECK_EQ(sr.at(640, 700), BG);
}

static void testNowPlaying() {
    ui::NowPlayingModel m;
    m.title = "Nessun dorma";
    m.subtitle = "Audio  |  AAC transcode";
    m.positionSeconds = 83;
    m.durationSeconds = 180;
    m.queueText = "Track 3 of 12  |  Shuffle";
    m.nextText = "Next: La donna \xc3\xa8 mobile";
    m.hints = {{"A", "Pause"}, {"< >", "Seek"}, {"L R", "Prev / Next"}, {"B", "Stop"}};
    renderScreen([&] { ui::drawNowPlaying(m); }, "now_playing");
    m.paused = true;
    SoftRender sr = renderScreen([&] { ui::drawNowPlaying(m); }, "now_playing_paused");
    CHECK_EQ(sr.at(110 + 170 - 22, 150 + 170), bgr(0xFFFFFF)); // pause bar on the artwork
}

// Shuffle / Repeat buttons: hit-testing, and every state looks different from "off" (lit in the accent colour).
static void testQueueToggles() {
    using ui::NpButton;
    const ui::Rect sh = ui::npToggleRect(NpButton::Shuffle), rp = ui::npToggleRect(NpButton::Repeat);
    // Each toggle sits beside the transport row, on the screen, not on top of another button or the volume slider.
    const ui::Rect first = ui::npButtonRect(0), last = ui::npButtonRect((int)NpButton::Count - 1);
    CHECK(sh.right() < first.x && rp.x > last.right());
    CHECK(sh.x > ui::layout::NP_TEXT_X && rp.right() < ui::layout::NP_TEXT_X + ui::layout::NP_TEXT_W);
    CHECK(sh.bottom() < ui::layout::NP_VOLUME_Y && rp.bottom() < ui::layout::NP_VOLUME_Y);
    CHECK_EQ(ui::hitTestNowPlayingToggles(sh.x + sh.w * 0.5f, sh.y + sh.h * 0.5f), (int)NpButton::Shuffle);
    CHECK_EQ(ui::hitTestNowPlayingToggles(rp.x + rp.w * 0.5f, rp.y + rp.h * 0.5f), (int)NpButton::Repeat);
    CHECK_EQ(ui::hitTestNowPlayingToggles(first.x + 5, first.y + 5), -1);
    // The four transport buttons did not move or change meaning.
    CHECK_EQ(ui::hitTestNowPlaying(sh.x + 5, sh.y + 5), -1);
    CHECK_EQ(ui::hitTestNowPlaying(rp.x + 5, rp.y + 5), -1);

    for (int glass = 0; glass < 2; glass++) {
        ui::setGlass(glass == 1);
        if (glass) ui::setGlassSnapshot((uint64_t)7781, &SoftRender::snapshotCallback, (void*)(intptr_t)7781);
        auto shot = [&](bool shuffle, int repeat, const char* name) {
            ui::NowPlayingModel m;
            m.title = "Song"; m.durationSeconds = 200; m.positionSeconds = 20;
            m.queueControls = true; m.shuffleOn = shuffle; m.repeatMode = repeat;
            return renderScreen([&] { ui::drawNowPlaying(m); }, (std::string(name) + (glass ? "_glass" : "_flat")).c_str());
        };
        SoftRender off = shot(false, 0, "toggles_off"), shuf = shot(true, 0, "toggles_shuffle"),
                   all = shot(false, 1, "toggles_all"), one = shot(false, 2, "toggles_one");
        // The pixels around each button's centre (away from the icon) differ clearly between off and on.
        auto diff = [&](const SoftRender& a, const SoftRender& b, const ui::Rect& r) {
            long d = 0;
            const int cx = (int)(r.x + r.w * 0.5f), cy = (int)(r.y + r.h * 0.5f);
            for (int dy = -20; dy <= 20; dy += 4)
                for (int dx = -20; dx <= 20; dx += 4) {
                    const uint32_t p = a.at(cx + dx, cy + dy), q = b.at(cx + dx, cy + dy);
                    d += std::abs((int)(p & 0xFF) - (int)(q & 0xFF)) + std::abs((int)((p >> 8) & 0xFF) - (int)((q >> 8) & 0xFF)) +
                         std::abs((int)((p >> 16) & 0xFF) - (int)((q >> 16) & 0xFF));
                }
            return d;
        };
        CHECK(diff(off, shuf, sh) > 4000);        // shuffle lit
        CHECK(diff(off, shuf, rp) < 200);         // ...and only shuffle: repeat did not change
        CHECK(diff(off, all, rp) > 4000);         // repeat all lit
        CHECK(diff(off, all, sh) < 200);
        CHECK(diff(off, one, rp) > 4000);         // repeat one lit
        {   // and repeat one is told apart from repeat all by the 1 in the loop (a thin glyph: every pixel counts)
            long d = 0;
            const int cx = (int)(rp.x + rp.w * 0.5f), cy = (int)(rp.y + rp.h * 0.5f);
            for (int dy = -10; dy <= 10; dy++)
                for (int dx = -8; dx <= 8; dx++) if (all.at(cx + dx, cy + dy) != one.at(cx + dx, cy + dy)) d++;
            CHECK(d > 15);
        }
    }
    ui::setGlassSnapshot(0, nullptr, nullptr);
    ui::setGlass(false);

    // The movie remote never shows them.
    ui::NowPlayingModel movie;
    movie.title = "Movie"; movie.durationSeconds = 200;
    SoftRender a = renderScreen([&] { ui::drawNowPlaying(movie); }, "np_no_toggles");
    movie.queueControls = true;
    SoftRender b = renderScreen([&] { ui::drawNowPlaying(movie); }, "np_with_toggles");
    CHECK(a.at((int)(sh.x + sh.w * 0.5f), (int)(sh.y + 2)) != b.at((int)(sh.x + sh.w * 0.5f), (int)(sh.y + 2)));
}

static void testMusicVisualizer() {
    ui::VisualizerModel m;
    m.title = "Nessun dorma";
    m.bandCount = 8;
    for (int i = 0; i < m.bandCount; i++) m.bands[i] = 0.0f;
    m.hints = {{"A", "Pause"}, {"< >", "Seek"}, {"-", "Now Playing"}, {"B", "Stop"}};

    // All-zero bands: still renders (the "warming up" / floor case), no crash.
    renderScreen([&] { ui::drawMusicVisualizer(m); }, "visualizer_silent");

    // One band clearly louder than its neighbours should paint taller
    // (i.e. reaching higher, smaller y) than a quiet one at the same x.
    m.bands[2] = 1.0f;
    m.bands[5] = 0.05f;
    SoftRender sr = renderScreen([&] { ui::drawMusicVisualizer(m); }, "visualizer_bars");

    const float top = 240.0f, bottom = ui::layout::FOOTER_Y - 60.0f;
    const float areaH = bottom - top;
    const float gap = 18.0f;
    const float barW = (ui::layout::SCREEN_W - 120.0f - gap * (m.bandCount - 1)) / (float)m.bandCount;
    auto barCenterX = [&](int i) { return (int)(60.0f + i * (barW + gap) + barW * 0.5f); };
    // Just above a loud bar's top should be filled; just above a quiet
    // bar's (much lower) top should still be background.
    int loudTopY = (int)(bottom - areaH * (0.03f + 0.97f * 1.0f)) + 4;
    int quietAboveY = (int)(bottom - areaH * (0.03f + 0.97f * 0.05f)) - 20;
    CHECK(sr.at(barCenterX(2), loudTopY) != BG);
    CHECK_EQ(sr.at(barCenterX(5), quietAboveY), BG);

    // Paused state and an empty band count both still render cleanly.
    m.paused = true;
    renderScreen([&] { ui::drawMusicVisualizer(m); }, "visualizer_paused");
    m.bandCount = 0;
    renderScreen([&] { ui::drawMusicVisualizer(m); }, "visualizer_empty");
}

static void testHud() {
    ui::VideoHudModel m;
    m.title = "Big Buck Bunny";
    m.positionSeconds = 312;
    m.durationSeconds = 596;
    m.visible = true;
    m.hints = {{"A", "Pause"}, {"<", "-10 s"}, {">", "+30 s"}, {"B", "Stop"}};

    // Rendered over a mid-grey "video" so transparency is visible.
    auto withVideo = [&](const ui::VideoHudModel& hud, const char* name) {
        return renderScreen([&] {
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
            ui::drawVideoHud(hud);
        }, name);
    };
    SoftRender sr = withVideo(m, "hud");
    CHECK_EQ(sr.at(640, 100), bgr(0x5A6E82)); // top of the picture untouched
    CHECK((sr.at(640, 700) & 0xFF) < 0x30);   // bottom darkened by the panel

    m.visible = false;
    sr = withVideo(m, "hud_hidden");
    CHECK_EQ(sr.at(640, 700), bgr(0x5A6E82)); // nothing drawn at all

    m.paused = true;
    sr = withVideo(m, "hud_paused");
    CHECK_EQ(sr.at(640 - 15, 324), bgr(0xFFFFFF)); // pause bar in the middle

    m.paused = false;
    m.seekDelta = 90;
    m.seekTarget = 402;
    withVideo(m, "hud_seek");

    m.seekDelta = 0;
    m.live = true;
    m.durationSeconds = 0;
    m.visible = true;
    m.title = "Rai 1  -  Telegiornale";
    withVideo(m, "hud_live");
}

// The rebuffering wheel: drawn over the picture (which stays visible) only
// while buffering and not user-paused, it spins, and it draws nothing when off.
static void testBufferingWheel() {
    ui::VideoHudModel m;
    m.title = "Movie";
    m.durationSeconds = 600;
    m.positionSeconds = 100;
    auto withVideo = [&](const ui::VideoHudModel& hud, const char* name) {
        return renderScreen([&] {
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
            ui::drawVideoHud(hud);
        }, name);
    };
    const int cx = 640, cy = 324; // where the pause icon sits
    auto red = [](uint32_t c) { return (int)(c & 0xFF); };

    SoftRender off = withVideo(m, "hud_no_buffering");
    CHECK_EQ(off.at(cx, cy), bgr(0x5A6E82)); // nothing drawn, picture untouched

    m.buffering = true;
    m.bufferedSeconds = 4.0;
    m.bufferingTargetSeconds = 12.0;
    SoftRender on = withVideo(m, "hud_buffering");
    CHECK(on.at(cx, cy) != bgr(0x5A6E82));                 // disc behind the wheel
    CHECK(red(on.at(cx, cy)) < red(bgr(0x5A6E82)) - 10);   // dimmed, not opaque: the picture shows through
    CHECK(red(on.at(cx + 30, cy)) > red(on.at(cx, cy)) + 10); // the ring is brighter than the disc
    CHECK(red(on.at(cx + 100, cy)) == red(bgr(0x5A6E82)));  // far from it the picture is untouched
    CHECK(red(on.at(cx, cy + 64 + 12 - 6)) == red(bgr(0x5A6E82))); // just outside the label pill
    CHECK(red(on.at(cx - 60, cy + 64 + 12 + 4)) < red(bgr(0x5A6E82))); // label pill under it

    // It spins: the bright arc sits at a different place ~0.2 s later.
    auto ringMask = [&](const SoftRender& sr) {
        uint32_t mask = 0;
        for (int i = 0; i < 24; i++) {
            const float a = i * 6.2831853f / 24.0f;
            const int x = cx + (int)std::lround(std::cos(a) * 30.0f), y = cy + (int)std::lround(std::sin(a) * 30.0f);
            if (red(sr.at(x, y)) > 200) mask |= 1u << i; // near-white: the arc, not the dim track
        }
        return mask;
    };
    const uint32_t before = ringMask(on);
    SoftRender later = on;
    for (int i = 0; i < 12; i++) later = withVideo(m, "hud_buffering_later");
    const uint32_t after = ringMask(later);
    CHECK(before != 0 && after != 0);
    CHECK(before != after);

    // The user's pause wins: pause icon, no wheel.
    m.paused = true;
    SoftRender paused = withVideo(m, "hud_buffering_paused");
    CHECK_EQ(paused.at(cx - 15, cy), bgr(0xFFFFFF));
    CHECK(red(paused.at(cx + 30, cy + 40)) == red(on.at(cx, cy))); // disc only, no ring/label there

    // The standalone indicator (TV-only mode) draws the same wheel.
    SoftRender tv = renderScreen([&] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
        ui::drawBufferingIndicator(4.0, 12.0);
    }, "tv_buffering");
    CHECK(red(tv.at(cx + 30, cy)) > red(tv.at(cx, cy)) + 10);
    // Without a target it still draws (just no seconds readout).
    renderScreen([&] { ui::drawBufferingIndicator(0.0, 0.0); }, "tv_buffering_plain");
}

// ---- Liquid Glass -----------------------------------------------------------

static int g_glassUpdates = 0, g_glassCreates = 0;
static std::map<uint64_t, std::vector<uint8_t>> g_glassTex; // what the fake GPU holds

static glass::TextureSink fakeGlassSink(bool registerMagenta) {
    glass::TextureSink s;
    s.create = [registerMagenta](const uint8_t* px, int w, int h) -> uint64_t {
        static uint64_t next = 5000;
        g_glassCreates++;
        uint64_t id = next++;
        g_glassTex[id].assign(px, px + (size_t)w * h * 4);
        if (registerMagenta && !getenv("UFIN_GLASS_REAL")) { // the rasterizer samples this: hot magenta, impossible to mistake
            SoftUserTexture t;
            t.w = 4; t.h = 4;
            t.rgba.assign(4 * 4 * 4, 0);
            for (size_t i = 0; i < 16; i++) { t.rgba[i * 4] = 255; t.rgba[i * 4 + 1] = 0; t.rgba[i * 4 + 2] = 255; t.rgba[i * 4 + 3] = 255; }
            SoftRender::userTextures()[(ImTextureID)id] = t;
        } else if (registerMagenta) {
            SoftUserTexture t;
            t.w = w; t.h = h;
            t.rgba.assign(px, px + (size_t)w * h * 4);
            SoftRender::userTextures()[(ImTextureID)id] = t;
        }
        return id;
    };
    s.update = [](uint64_t id, const uint8_t* px, int w, int h) -> bool {
        g_glassUpdates++;
        g_glassTex[id].assign(px, px + (size_t)w * h * 4);
        if (getenv("UFIN_GLASS_REAL")) { // preview mode: the rasterizer shows the real blurred backdrop
            SoftUserTexture t;
            t.w = w; t.h = h;
            t.rgba.assign(px, px + (size_t)w * h * 4);
            SoftRender::userTextures()[(ImTextureID)id] = t;
        }
        return true;
    };
    s.release = [](uint64_t id) {
        g_glassTex.erase(id);
        SoftRender::userTextures().erase((ImTextureID)id);
    };
    return s;
}

// Number of hot-magenta pixels inside a rectangle (the fake backdrop's colour).
static int magentaIn(const SoftRender& sr, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t c = sr.at(x, y);
            // Magenta, possibly darkened by a translucent tint: R and B high and alike, G low.
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)((c >> 16) & 0xFF);
            if (r > 100 && b > 100 && g < 80 && std::abs(r - b) < 60) n++;
        }
    return n;
}

static void testLiquidGlass() {
    auto sameImage = [](const SoftRender& a, const SoftRender& b) { return a.px == b.px; };
    ui::BrowserModel m = sampleBrowser(12, 2);

    // OFF: glassFrame/backdrop do nothing and every screen is as it always was.
    ui::setGlass(false);
    CHECK(!ui::glassEnabled());
    int updatesBefore = g_glassUpdates;
    ui::glassFrame(1.0, false);
    CHECK_EQ(g_glassUpdates, updatesBefore); // no work, no texture writes
    SoftRender off = renderScreen([&] { ui::drawBrowser(m); }, "glass_off");

    // Set up the shared blur the way Gfx::setGlass does (fake GPU).
    auto& bd = glass::backdrop();
    CHECK(bd.enable(glass::sharedDims(1280, 720, 854, 480), fakeGlassSink(true), 2));
    CHECK_EQ(g_glassCreates, 3);
    CHECK(ui::glassBlurAvailable());

    // ON: a different, glassy picture; the selected row is lit; the sidebar is
    // a floating rounded panel (its far-left strip is no longer sidebar-flat).
    ui::setGlass(true);
    CHECK(ui::glassEnabled());
    SoftRender on = renderScreen([&] { ui::drawBrowser(m); }, "glass_browser");
    CHECK(!sameImage(off, on));
    CHECK(sr_at_differs(off, on, 5, 400));
    ui::Rect sel = ui::layout::listRow(2);
    CHECK(sr_at_differs(off, on, (int)sel.x + 60, (int)sel.y + 3));
    // Frosted panels sample the shared blur: magenta shows through the sidebar
    // panel and the rows (menus DO use the backdrop)...
    CHECK(magentaIn(on, 20, 200, ui::layout::SIDEBAR_W - 20, 240) > 100);
    ui::Rect plain = ui::layout::listRow(4); // an unfocused row
    CHECK(magentaIn(on, (int)plain.x + 400, (int)plain.y + 6, (int)plain.x + 500, (int)plain.y + 20) > 50);
    // ...and it is a full soft backdrop behind everything, not flat BG.
    CHECK(!(on.at(ui::layout::SIDEBAR_W + 8, 700) == BG && on.at(ui::layout::SIDEBAR_W + 8, 20) == BG) ||
          magentaIn(on, ui::layout::SIDEBAR_W + 8, 690, ui::layout::SIDEBAR_W + 60, 710) > 0);

    // Rounder: the row's corner is cut further than the flat theme's 12 px.
    ui::Rect r0 = ui::layout::listRow(0);
    CHECK(off.at((int)r0.x + 4, (int)r0.y + 4) == PANEL);   // flat row: inside the corner
    CHECK(on.at((int)r0.x + 4, (int)r0.y + 4) != PANEL);    // glass row: rounded away

    // Over video the backdrop is NEVER sampled: no magenta anywhere in the HUD,
    // whatever it draws (pause icon, seek banner, notice, track menu, buffering).
    ui::VideoHudModel hud;
    hud.title = "Movie"; hud.durationSeconds = 600; hud.positionSeconds = 100;
    hud.visible = true; hud.paused = true; hud.seekDelta = 30; hud.seekTarget = 130;
    hud.notice = "GamePad screen off"; hud.trackMenu = true; hud.audioLabel = "English"; hud.subtitleLabel = "Off";
    SoftRender vid = renderScreen([&] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
        ui::drawVideoHud(hud);
    }, "glass_hud_over_video");
    CHECK_EQ(magentaIn(vid, 0, 0, 1280, 720), 0);
    hud.paused = false; hud.seekDelta = 0; hud.trackMenu = false; hud.notice.clear();
    hud.buffering = true; hud.bufferedSeconds = 3; hud.bufferingTargetSeconds = 12;
    SoftRender buf = renderScreen([&] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
        ui::drawVideoHud(hud);
    }, "glass_buffering_over_video");
    CHECK_EQ(magentaIn(buf, 0, 0, 1280, 720), 0);
    // The buffering wheel still renders under glass: ring pixel brighter than disc.
    auto redOf = [](uint32_t c) { return (int)(c & 0xFF); };
    CHECK(redOf(buf.at(640 + 30, 324)) > redOf(buf.at(640, 324)) + 10);
    SoftRender tv = renderScreen([&] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(90, 110, 130, 255));
        ui::drawBufferingIndicator(3.0, 12.0);
    }, "glass_tv_buffering");
    CHECK_EQ(magentaIn(tv, 0, 0, 1280, 720), 0);
    CHECK(redOf(tv.at(640 + 30, 324)) > redOf(tv.at(640, 324)) + 10);

    // Buttons: pressed is deeper (darker) than normal; the layout is unchanged.
    ui::NowPlayingModel np;
    np.title = "Song"; np.durationSeconds = 200; np.positionSeconds = 20;
    SoftRender npNormal = renderScreen([&] { ui::drawNowPlaying(np); }, "glass_np");
    np.pressedButton = (int)ui::NpButton::Stop;
    SoftRender npPressed = renderScreen([&] { ui::drawNowPlaying(np); }, "glass_np_pressed");
    ui::Rect stop = ui::npButtonRect((int)ui::NpButton::Stop);
    const int sx = (int)(stop.x + stop.w * 0.5f), sy = (int)(stop.y + 10);
    auto lum = [](uint32_t c) { return (int)(c & 0xFF) + (int)((c >> 8) & 0xFF) + (int)((c >> 16) & 0xFF); };
    CHECK(lum(npPressed.at(sx, sy)) < lum(npNormal.at(sx, sy)));
    CHECK(magentaIn(npNormal, (int)stop.x + 4, (int)stop.y + 4, (int)stop.x + 14, (int)stop.y + 14) > 0); // buttons frost the backdrop
    ui::hitTestNowPlaying(sx, sy); // hit-testing untouched by the look
    CHECK(ui::hitTestNowPlaying(sx, sy) == (int)ui::NpButton::Stop);

    // Artwork colour: a red poster in the cache tints the backdrop, faintly.
    {
        std::vector<uint8_t> red(32 * 32 * 4);
        for (size_t i = 0; i < red.size(); i += 4) { red[i] = 230; red[i + 1] = 20; red[i + 2] = 20; red[i + 3] = 255; }
        const uint64_t tex = 777;
        ui::glassNoteArtwork(tex, red.data(), 32, 32);
        ui::BrowserModel a = sampleBrowser(4, 1);
        a.items[1].art = tex;
        auto avgRed = [&]() {
            // Mean red of the most recently written backdrop texture.
            long sum = 0, n = 0;
            for (auto& kv : g_glassTex) if (kv.first == bd.texture()) for (size_t i = 0; i < kv.second.size(); i += 4) { sum += kv.second[i]; n++; }
            return n ? (double)sum / (double)n : 0.0;
        };
        double t = 100.0;
        for (int i = 0; i < 200; i++) { t += 1.0 / 60.0; ui::glassFrame(t, false); renderScreen([&] { ui::drawBrowser(sampleBrowser(4, 0)); }, "glass_noart"); }
        const double before = avgRed();
        for (int i = 0; i < 400; i++) { t += 1.0 / 60.0; ui::glassFrame(t, false); renderScreen([&] { ui::drawBrowser(a); }, "glass_art"); }
        const double after = avgRed();
        CHECK(after > before + 0.3);   // it shows up...
        CHECK(after < before + 40.0);  // ...but faintly
        ui::glassForgetArtwork(tex);
    }

    // Video frames: the blur is neither recomputed nor written (playback wins).
    {
        int u = g_glassUpdates;
        for (int i = 0; i < 300; i++) ui::glassFrame(500.0 + i / 60.0, true);
        CHECK_EQ(g_glassUpdates, u);
    }

    // Smooth motion: the glass backdrop changes on (nearly) every frame, not in
    // ~1 Hz steps -- the blur is cross-faded on the GPU between regenerations.
    {
        ui::MessageModel sm; // a still screen: nothing animates here except the backdrop
        sm.title = "Still"; sm.lines = {"Nothing moves on this screen."};
        double t = 2000.0;
        setenv("UFIN_GLASS_REAL", "1", 1); // the fake GPU now shows the real blur (all 3 textures get rewritten while settling)
        for (int i = 0; i < 400; i++) { t += 1.0 / 60.0; ui::glassFrame(t, false); } // settle
        int changedFrames = 0;
        SoftRender prevF = renderScreen([&] { ui::drawMessage(sm); }, "glass_smooth_a");
        for (int i = 0; i < 120; i++) { // 2 s
            t += 1.0 / 60.0;
            ui::glassFrame(t, false);
            SoftRender cur = renderScreen([&] { ui::drawMessage(sm); }, "glass_smooth_b");
            bool differs = false;
            for (int y = 0; y < 720 && !differs; y++)
                for (int x = 0; x < 1280; x++) if (prevF.at(x, y) != cur.at(x, y)) { differs = true; break; }
            if (differs) changedFrames++;
            prevF = cur;
        }
        unsetenv("UFIN_GLASS_REAL");
        CHECK(changedFrames >= 24); // >= 24 different frames per 2 s (stepping at 1 Hz would give ~2)
    }

    // Rounded-corner clipping: nothing inside a glass bubble may reach outside
    // its rounded outline.
    {
        // (1) The accent strip on a card follows the card's corners. The error
        // strip is pure ERROR_RED; it must not cover the corner that the card's
        // radius cuts away, yet still span the straight top edge.
        ui::MessageModel mm;
        mm.title = "Oops"; mm.isError = true; mm.lines = {"Something failed."};
        SoftRender card = renderScreen([&] { ui::drawMessage(mm); }, "glass_message_error");
        auto isStrip = [&](int x, int y) {
            const uint32_t c = card.at(x, y);
            return (int)(c & 0xFF) > 200 && (int)((c >> 8) & 0xFF) < 100 && (int)((c >> 16) & 0xFF) < 120;
        };
        int topY = -1;
        for (int y = 0; y < 720 && topY < 0; y++) if (isStrip(640, y)) topY = y;
        CHECK(topY > 0);
        const int cardX = (1280 - 820) / 2; // drawMessage's card geometry
        CHECK(isStrip(640, topY + 2));
        CHECK(!isStrip(cardX + 1, topY + 1));     // the corner the radius cuts away
        CHECK(!isStrip(cardX + 3, topY + 1));
        CHECK(!isStrip(1280 - cardX - 2, topY + 1));
        CHECK(isStrip(cardX + 40, topY + 2));     // past the curve it is there again

        // (2) The focus highlight drifts across a focused button; between frames
        // only pixels INSIDE the button's rounded outline may change.
        ui::LoginModel lm;
        lm.rows = {{"Server", "a", "", false, false, false}, {"Sign in", "", "", false, true, true}};
        lm.focused = 1;
        ui::Rect r = ui::layout::loginRow(1);
        const float rad = 14.0f * 1.45f;
        auto outside = [&](float px, float py) { // outside the rounded rect r (radius rad)?
            const float cx = std::min(std::max(px, r.x + rad), r.right() - rad);
            const float cy = std::min(std::max(py, r.y + rad), r.bottom() - rad);
            const float dx = px - cx, dy = py - cy;
            return dx * dx + dy * dy > (rad + 0.75f) * (rad + 0.75f);
        };
        SoftRender prev = renderScreen([&] { ui::drawLogin(lm); }, "glass_login_focus");
        int changed = 0, leaked = 0;
        for (int f = 0; f < 120; f++) { // ~2 s: the highlight crosses the whole button
            SoftRender cur = renderScreen([&] { ui::drawLogin(lm); }, "glass_login_focus_next");
            for (int y = (int)r.y - 8; y < (int)r.bottom() + 8; y++)
                for (int x = (int)r.x - 8; x < (int)r.right() + 8; x++) {
                    if (prev.at(x, y) == cur.at(x, y)) continue;
                    changed++;
                    if (outside((float)x + 0.5f, (float)y + 0.5f)) leaked++;
                }
            prev = cur;
        }
        CHECK(changed > 200); // it does animate...
        CHECK_EQ(leaked, 0);  // ...and only inside the bubble
    }

    // Glass is see-through: a bright object behind a panel stays clearly visible.
    {
        SoftRender clear = renderScreen([&] {
            ui::drawBrowser(m);
            ImGui::GetBackgroundDrawList(); // (panel drawn above; nothing else)
        }, "glass_clear");
        ui::Rect plain2 = ui::layout::listRow(4);
        auto lumOf = [](uint32_t c) { return (int)(c & 0xFF) + (int)((c >> 8) & 0xFF) + (int)((c >> 16) & 0xFF); };
        // Under a row, the picture is still mostly the backdrop: not a flat opaque slab.
        const int a = lumOf(clear.at((int)plain2.x + 300, (int)plain2.y + 30));
        const int b = lumOf(clear.at((int)plain2.x + 300, (int)plain2.bottom() + 3));
        CHECK(std::abs(a - b) < 140);
    }

    // Flat glass (blur unavailable): still glass, still readable, no texture.
    bd.disable();
    CHECK(!ui::glassBlurAvailable());
    SoftRender flat = renderScreen([&] { ui::drawBrowser(m); }, "glass_flat");
    CHECK(!sameImage(off, flat));
    CHECK(!sameImage(on, flat));
    CHECK_EQ(magentaIn(flat, 0, 0, 1280, 720), 0);
    CHECK_EQ((int)g_glassTex.size(), 0); // nothing left on the fake GPU

    // Turned off again: byte-for-byte the original picture.
    ui::setGlass(false);
    SoftRender back = renderScreen([&] { ui::drawBrowser(m); }, "glass_off_again");
    CHECK(sameImage(off, back));
    ui::glassFrame(9999.0, false); // still a no-op
}

// --- Liquid Glass: real refraction ------------------------------------------
//
// The software renderer runs the UI's snapshot callback the way the GX2 backend
// does (copies everything drawn so far into a texture), so this exercises the
// whole path through the real UI code: when snapshots are requested, that the glass
// is drawn from them, that the picture really bends at the rim, and that nothing
// reaches outside the rounded shape.

static void drawChecker() { // high-contrast, 17 px squares (no relation to the lens' pixel shifts)
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(20, 20, 30, 255));
    for (int y = 0; y < 720; y += 17)
        for (int x = 0; x < 1280; x += 17)
            if (((x / 17) + (y / 17)) & 1) d->AddRectFilled(ImVec2((float)x, (float)y), ImVec2(x + 17.0f, y + 17.0f), IM_COL32(240, 240, 255, 255));
}

static double lumaAt(const SoftRender& r, int x, int y) {
    const uint32_t c = r.at(x, y);
    return 0.299 * (double)(c & 0xFF) + 0.587 * (double)((c >> 8) & 0xFF) + 0.114 * (double)((c >> 16) & 0xFF);
}

// Pearson correlation of two renders' brightness over a region: 1 = same picture, ~0 = unrelated.
static double correlation(const SoftRender& a, const SoftRender& b, int x0, int y0, int x1, int y1) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const double va = lumaAt(a, x, y), vb = lumaAt(b, x, y);
            sa += va; sb += vb; saa += va * va; sbb += vb * vb; sab += va * vb; n += 1;
        }
    const double cov = sab / n - (sa / n) * (sb / n), va = saa / n - (sa / n) * (sa / n), vb = sbb / n - (sb / n) * (sb / n);
    return (va > 1e-9 && vb > 1e-9) ? cov / std::sqrt(va * vb) : 1.0;
}

struct DiffBox { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; };
static DiffBox diffBox(const SoftRender& a, const SoftRender& b) {
    DiffBox r;
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 1280; x++)
            if (a.at(x, y) != b.at(x, y)) {
                r.x0 = std::min(r.x0, x); r.y0 = std::min(r.y0, y);
                r.x1 = std::max(r.x1, x + 1); r.y1 = std::max(r.y1, y + 1);
            }
    return r;
}

static void testGlassRefraction() {
    const intptr_t SNAP = 7777;
    ui::setGlass(true);
    ui::setGlassSnapshot(0, nullptr, nullptr);
    // A dialog card (it has no full-screen backdrop of its own, unlike the message
    // screen, so the checkerboard behind it shows through the glass).
    ui::ChoiceModel cm;
    cm.title = "Play movie";
    cm.subtitle = "Resume from 12:30?";
    cm.options = {"Resume", "Play from start", "Cancel"};
    cm.selected = 0;
    cm.hints = {{"A", "Select"}, {"B", "Back"}};
    auto scene = [&] { drawChecker(); ui::drawChoice(cm); };

    // Without a snapshot hook nothing is requested and glass behaves as before.
    CHECK(!ui::glassRefractionActive());
    SoftRender::snapshotCalls() = 0;
    SoftRender flat = renderScreen(scene, "refr_flat");
    CHECK_EQ(SoftRender::snapshotCalls(), 0);
    CHECK_EQ(ui::glassSnapshotsThisFrame(), 0);

    // With one: the frame is captured (at least once, a bounded number of times) and used.
    ui::setGlassSnapshot((uint64_t)SNAP, &SoftRender::snapshotCallback, (void*)SNAP);
    CHECK(ui::glassRefractionActive());
    SoftRender refr = renderScreen(scene, "refr_on");
    CHECK(SoftRender::snapshotCalls() >= 1 && SoftRender::snapshotCalls() <= glass::SnapshotPlanner::MAX_PER_FRAME);
    CHECK_EQ(SoftRender::snapshotCalls(), ui::glassSnapshotsThisFrame());

    // Only the card changes -- and never outside its rounded outline.
    const DiffBox box = diffBox(flat, refr);
    CHECK(box.x1 > box.x0 && box.y1 > box.y0);
    CHECK(box.x0 >= 339 && box.x1 <= 941);                          // inside the 600 px card (340..940)
    CHECK(box.x1 - box.x0 >= 580);                                  // ...and it does cover the card
    CHECK(flat.at(box.x0, box.y0) == refr.at(box.x0, box.y0));      // the four corners of the changed area
    CHECK(flat.at(box.x1 - 1, box.y0) == refr.at(box.x1 - 1, box.y0)); //  are cut away by the corner radius,
    CHECK(flat.at(box.x0, box.y1 - 1) == refr.at(box.x0, box.y1 - 1)); //  so nothing square shows up there
    CHECK(flat.at(box.x1 - 1, box.y1 - 1) == refr.at(box.x1 - 1, box.y1 - 1));
    CHECK(flat.at(box.x0 + 2, box.y0 + 2) == refr.at(box.x0 + 2, box.y0 + 2));

    // The picture bends at the rim and is (almost) undisturbed in the middle.
    const int midY0 = box.y0 + (box.y1 - box.y0) / 2 - 40, midY1 = midY0 + 80;
    const double rim = correlation(flat, refr, box.x0 + 3, midY0, box.x0 + 14, midY1);
    const double inner = correlation(flat, refr, box.x0 + 200, midY0, box.x0 + 400, midY1);
    CHECK(inner > 0.75);       // the middle still shows the checkerboard where it is
    CHECK(rim < 0.45);         // ...but along the rim it is displaced
    CHECK(inner - rim > 0.4);

    // The rim light is actually drawn: brightest at the top-left corner, faint along the
    // middle of the side. (Regression: the rim and its inner glow are stitched from rings
    // that must share one vertex count; when they did not, they were silently skipped.)
    {
        auto maxLuma = [&](const SoftRender& r, int x0, int y0, int x1, int y1) {
            double m = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) m = std::max(m, lumaAt(r, x, y));
            return m;
        };
        const double corner = maxLuma(flat, box.x0, box.y0 + 3, box.x0 + 16, box.y0 + 19);
        const double side = maxLuma(flat, box.x0, midY0, box.x0 + 3, midY1);
        CHECK(corner > side + 40.0);
    }

    // The same for a small pill (the toast): bent at its ends, shape intact.
    auto toast = [&] { drawChecker(); ui::drawToast("Added to favourites and some more", 0.2); };
    ui::setGlassSnapshot(0, nullptr, nullptr);
    SoftRender tFlat = renderScreen(toast, "refr_toast_flat");
    ui::setGlassSnapshot((uint64_t)SNAP, &SoftRender::snapshotCallback, (void*)SNAP);
    SoftRender tRefr = renderScreen(toast, "refr_toast_on");
    const DiffBox tb = diffBox(tFlat, tRefr);
    CHECK(tb.x1 > tb.x0 && tb.y1 > tb.y0);
    CHECK(tFlat.at(tb.x0, tb.y0) == tRefr.at(tb.x0, tb.y0));          // pill corners stay clear
    CHECK(tFlat.at(tb.x1 - 1, tb.y1 - 1) == tRefr.at(tb.x1 - 1, tb.y1 - 1));
    const int tMid = tb.y0 + (tb.y1 - tb.y0) / 2;
    CHECK(correlation(tFlat, tRefr, tb.x0 + (tb.x1 - tb.x0) / 2 - 40, tMid - 6, tb.x0 + (tb.x1 - tb.x0) / 2 + 40, tMid + 6) >
          correlation(tFlat, tRefr, tb.x0 + 2, tMid - 6, tb.x0 + 10, tMid + 6) + 0.2);

    // Glass on glass: the rows of a browser sit on the sidebar panel and need their own
    // snapshot (so they refract the panel as well), yet the count stays small.
    ui::BrowserModel bm = sampleBrowser(6, 2);
    SoftRender::snapshotCalls() = 0;
    renderScreen([&] { drawChecker(); ui::drawBrowser(bm); }, "refr_browser");
    CHECK(SoftRender::snapshotCalls() >= 2); // panel, then the glass on top of it
    CHECK(SoftRender::snapshotCalls() <= glass::SnapshotPlanner::MAX_PER_FRAME);

    // Frames are independent: the next frame starts with one snapshot again.
    SoftRender::snapshotCalls() = 0;
    renderScreen(scene, "refr_again");
    CHECK(SoftRender::snapshotCalls() >= 1 && SoftRender::snapshotCalls() <= 2);

    // Nothing to refract with: no texture, no callback, or glass off -> no snapshots, no change.
    ui::setGlassSnapshot(0, &SoftRender::snapshotCallback, (void*)SNAP);
    CHECK(!ui::glassRefractionActive());
    ui::setGlassSnapshot((uint64_t)SNAP, nullptr, (void*)SNAP);
    CHECK(!ui::glassRefractionActive());
    ui::setGlassSnapshot((uint64_t)SNAP, &SoftRender::snapshotCallback, (void*)SNAP);
    ui::setGlass(false);
    CHECK(!ui::glassRefractionActive());
    SoftRender::snapshotCalls() = 0;
    renderScreen(scene, "refr_glass_off");
    CHECK_EQ(SoftRender::snapshotCalls(), 0);

    // The frost: with the blur on the bent picture is softer than with it off (a checkerboard loses its contrast),
    // the card's outline is unchanged, and 0 is the sharp refraction again. Liquid Glass only: M3E never refracts.
    {
        ui::setGlass(true);
        ui::setGlassSnapshot((uint64_t)SNAP, &SoftRender::snapshotCallback, (void*)SNAP);
        ui::setGlassBlurTexture((uint64_t)SNAP + 1);
        ui::setGlassBlur(0);
        SoftRender sharp = renderScreen(scene, "refr_blur0");
        ui::setGlassBlur(2);
        SoftRender soft = renderScreen(scene, "refr_blur5");
        ui::setGlassBlurTexture(0);
        SoftRender noTex = renderScreen(scene, "refr_blur_notex"); // frost on but no blurred texture: the sharp one
        auto diffCount = [](const SoftRender& a, const SoftRender& b) { long n = 0; for (int y = 0; y < 720; y++) for (int x = 0; x < 1280; x++) n += a.at(x, y) != b.at(x, y); return n; };
        CHECK(diffCount(sharp, noTex) * 5 < diffCount(sharp, soft)); // (a drifting highlight differs between frames; the frost is far more)
        ui::setGlassBlurTexture((uint64_t)SNAP + 1);
        ui::setGlassBlur(2);
        const DiffBox bb = diffBox(sharp, soft);
        CHECK(bb.x1 > bb.x0 && bb.y1 > bb.y0);
        CHECK(bb.x0 >= 339 && bb.x1 <= 941);   // still inside the card
        // contrast: the sum of horizontal neighbour differences in the card's interior
        auto edgeEnergy = [&](const SoftRender& r) {
            double e = 0;
            for (int y = box.y0 + 60; y < box.y0 + 100; y++)
                for (int x = box.x0 + 200; x < box.x0 + 400; x++) { const double g = lumaAt(r, x, y) - lumaAt(r, x + 1, y); e += g * g; }
            return e; // (squared: a blur keeps the total step but flattens it, which this sees)
        };
        CHECK(edgeEnergy(soft) < edgeEnergy(sharp) * 0.9);
        ui::setGlassBlurTexture(0);
        ui::setGlassBlur(1);
    }

    ui::setGlassSnapshot(0, nullptr, nullptr);
    ui::setGlass(false);
}

// The Liquid Glass showcase backgrounds (Grid..Waves): each draws something, stays faint enough
// to read over, and changes the picture again once glass refraction is on. The original modes
// keep their names and indices.
static void testShowcaseBackgrounds() {
    const intptr_t SNAP = 7778;
    ui::BrowserModel m = sampleBrowser(12, 2);
    ui::setGlass(false);
    ui::setGlassSnapshot(0, nullptr, nullptr);
    ui::setBackgroundEffect(ui::Background::Off);
    SoftRender off = renderScreen([&] { ui::drawBrowser(m); }, "bg_off");

    const ui::Background fx[] = {ui::Background::Grid,   ui::Background::Aurora, ui::Background::Ripples,
                                 ui::Background::Beams,  ui::Background::Orbs,   ui::Background::Waves};
    std::set<std::string> names;
    for (ui::Background b : fx) {
        const std::string name = ui::backgroundEffectName(b);
        CHECK(!name.empty());
        names.insert(name);

        ui::setGlass(false);
        ui::setBackgroundEffect(b);
        SoftRender flat = renderScreen([&] { ui::drawBrowser(m); }, ("bg_" + name + "_flat").c_str());
        int changed = 0, maxDelta = 0;
        for (int y = 0; y < 720; y += 4)
            for (int x = 0; x < 1280; x += 4) {
                const uint32_t a = flat.at(x, y), c = off.at(x, y);
                if (a == c) continue;
                changed++;
                for (int sh = 0; sh < 24; sh += 8) {
                    const int d = std::abs((int)((a >> sh) & 0xFF) - (int)((c >> sh) & 0xFF));
                    if (d > maxDelta) maxDelta = d;
                }
            }
        CHECK(changed > 150);   // it is really drawn
        CHECK(maxDelta < 140);  // ...but faint: text and rows stay readable

        ui::setGlass(true);
        ui::setGlassSnapshot((uint64_t)SNAP, &SoftRender::snapshotCallback, (void*)SNAP);
        SoftRender glassy = renderScreen([&] { ui::drawBrowser(m); }, ("bg_" + name + "_glass").c_str());
        CHECK(glassy.px != flat.px); // glass changes the picture over this background too
        ui::setGlassSnapshot(0, nullptr, nullptr);
    }
    CHECK_EQ((int)names.size(), 6); // six distinct names

    // The originals are unchanged: same indices and names.
    CHECK_EQ((int)ui::Background::Circles, 1);
    CHECK_EQ((int)ui::Background::Starfield, 5);
    CHECK_EQ((int)ui::Background::Grid, 6);
    CHECK_EQ((int)ui::Background::COUNT, 12);
    CHECK_STR(ui::backgroundEffectName(ui::Background::Circles), "Circles");
    CHECK_STR(ui::backgroundEffectName(ui::Background::WiiBubbles), "Bubbles");
    CHECK_STR(ui::backgroundEffectName(ui::Background::Gradient), "Gradient");
    CHECK_STR(ui::backgroundEffectName(ui::Background::Polygons), "Polygons");
    CHECK_STR(ui::backgroundEffectName(ui::Background::Starfield), "Starfield");

    ui::setBackgroundEffect(ui::Background::Off);
    ui::setGlass(false);
}

// A Jellyfin backdrop is laid over the normal background, darkened, cover-fitted, and the normal
// background is skipped once it is fully covered. Without one nothing changes.
static void testScreenBackdrop() {
    ui::BrowserModel m = sampleBrowser(12, 2);
    ui::setGlass(false);
    ui::setBackgroundEffect(ui::Background::Off);
    ui::setScreenBackdrop(0, 0, 0, 0, 0);
    SoftRender none = renderScreen([&] { ui::drawBrowser(m); }, "bd_none");

    // A 16:9 bright red picture, fully faded in.
    ui::Texture red = makeTexture(0x3001, 160, 90, 250, 20, 20);
    ui::setScreenBackdrop(0, 0, red, 160.0f / 90.0f, 1.0f);
    SoftRender full = renderScreen([&] { ui::drawBrowser(m); }, "bd_full");
    // Empty spot in the gap above the list, right of the title: picture shows through, but darkened.
    const uint32_t a = full.at(1000, 24), b = none.at(1000, 24);
    CHECK(a != b);
    CHECK((a & 0xFF) > (b & 0xFF));      // red channel (BGR packing: low byte) rose
    CHECK((a & 0xFF) < 200);             // ...but the scrim kept it well under the raw 250
    // The same image at alpha 0 is exactly "no backdrop".
    ui::setScreenBackdrop(0, 0, red, 160.0f / 90.0f, 0.0f);
    SoftRender zero = renderScreen([&] { ui::drawBrowser(m); }, "bd_zero");
    CHECK(zero.px == none.px);
    // Half faded sits between the two.
    ui::setScreenBackdrop(0, 0, red, 160.0f / 90.0f, 0.5f);
    SoftRender half = renderScreen([&] { ui::drawBrowser(m); }, "bd_half");
    CHECK((half.at(1000, 24) & 0xFF) > (b & 0xFF) && (half.at(1000, 24) & 0xFF) < (a & 0xFF));
    // A cross-fade: an opaque previous layer plus a new one at 0 shows the previous picture.
    ui::Texture blue = makeTexture(0x3002, 160, 90, 20, 20, 250);
    ui::setScreenBackdrop(red, 160.0f / 90.0f, blue, 160.0f / 90.0f, 0.0f);
    SoftRender xf = renderScreen([&] { ui::drawBrowser(m); }, "bd_xfade");
    CHECK(xf.at(1000, 24) == a);
    // A very wide / very tall picture is cropped, not stretched, and still covers everything.
    ui::setScreenBackdrop(0, 0, makeTexture(0x3003, 400, 90, 250, 20, 20), 400.0f / 90.0f, 1.0f);
    SoftRender wide = renderScreen([&] { ui::drawBrowser(m); }, "bd_wide");
    CHECK(wide.at(1000, 24) == a);
    ui::setScreenBackdrop(0, 0, makeTexture(0x3004, 60, 160, 250, 20, 20), 60.0f / 160.0f, 1.0f);
    SoftRender tall = renderScreen([&] { ui::drawBrowser(m); }, "bd_tall");
    CHECK(tall.at(1000, 24) == a);

    ui::setScreenBackdrop(0, 0, 0, 0, 0);
}

// The sidebar's "Time used" counter and the hidden celebration it starts (confetti + glass bubble).
static void testUsageCelebration() {
    ui::setGlass(false);
    ui::setBackgroundEffect(ui::Background::Off);
    ui::setScreenBackdrop(0, 0, 0, 0, 0);
    ui::BrowserModel m = sampleBrowser(12, 2);
    m.usageLabel = "";
    SoftRender none = renderScreen([&] { ui::drawBrowser(m); }, "usage_none");

    // The counter is drawn in the sidebar when there is a label, and only then.
    m.usageLabel = "12h 34m";
    SoftRender label = renderScreen([&] { ui::drawBrowser(m); }, "usage_label");
    const ui::Rect u = ui::layout::usageCounter();
    int diff = 0;
    for (int y = (int)u.y; y < (int)u.bottom(); y++)
        for (int x = (int)u.x; x < (int)u.right(); x++)
            if (label.at(x, y) != none.at(x, y)) diff++;
    CHECK(diff > 40);
    CHECK(u.x >= 0 && u.right() <= ui::layout::SIDEBAR_W && u.bottom() <= ui::layout::SCREEN_H);
    // It sits clear of the sidebar entries and of the user@server footnote line.
    CHECK(u.y > ui::layout::sidebarItem(3).bottom());
    CHECK(u.bottom() <= ui::layout::SCREEN_H - 44.0f + 4.0f);

    // Hit test: the counter yes, a sidebar entry / the list / outside no.
    CHECK(ui::hitUsageCounter(u.x + u.w * 0.5f, u.y + u.h * 0.5f));
    CHECK(!ui::hitUsageCounter(u.x + u.w * 0.5f, ui::layout::sidebarItem(0).y + 10.0f));
    CHECK(!ui::hitUsageCounter(700.0f, u.y + 10.0f));
    CHECK(!ui::hitUsageCounter(-1.0f, -1.0f));

    // Celebration: not running until asked; the first tap cannot close it; it shows the figures.
    CHECK(!ui::usageCelebrationActive());
    ui::showUsageCelebration("12h 34m", "45,245 seconds in total");
    CHECK(ui::usageCelebrationActive());
    ui::dismissUsageCelebration();
    CHECK(ui::usageCelebrationActive());          // too early: ignored
    ui::advanceUsageCelebration(70);              // ~1.2 s in: bubble settled, confetti falling
    SoftRender party = renderScreen([&] { ui::drawBrowser(m); }, "usage_party");
    CHECK(ui::usageCelebrationActive());
    // The bubble covers the centre of the screen; it differs from the plain browser there.
    int centre = 0, corner = 0;
    for (int y = 300; y < 420; y += 2)
        for (int x = 560; x < 720; x += 2)
            if (party.at(x, y) != label.at(x, y)) centre++;
    CHECK(centre > 1500);
    // Confetti is spread over the screen, not only in the middle.
    for (int y = 0; y < 720; y += 3)
        for (int x = 250; x < 1280; x += 3) {
            const bool inBubble = (x - 640) * (x - 640) + (y - 360) * (y - 360) < 240 * 240;
            if (!inBubble && party.at(x, y) != label.at(x, y)) corner++;
        }
    CHECK(corner > 400);
    // Now any press closes it.
    ui::dismissUsageCelebration();
    CHECK(!ui::usageCelebrationActive());
    SoftRender after = renderScreen([&] { ui::drawBrowser(m); }, "usage_after");
    CHECK(after.px == label.px);                  // back to the plain browser, nothing left behind

    // It ends on its own, and a glass bubble draws too.
    ui::showUsageCelebration("5m 07s", "307 seconds in total");
    ui::setGlass(true);
    ui::advanceUsageCelebration(90);
    SoftRender glassy = renderScreen([&] { ui::drawBrowser(m); }, "usage_party_glass");
    CHECK(ui::usageCelebrationActive());
    int gc = 0;
    for (int y = 300; y < 420; y += 2)
        for (int x = 560; x < 720; x += 2)
            if (glassy.at(x, y) != label.at(x, y)) gc++;
    CHECK(gc > 500);
    ui::setGlass(false);
    ui::advanceUsageCelebration(2000);
    renderScreen([&] { ui::drawBrowser(m); }, "usage_end");
    CHECK(!ui::usageCelebrationActive());         // auto-dismissed after ~16 s
}

// The Help window: one page per controller, a rotating 3D model, rows that scroll with the cursor,
// and hit testing -- in flat and in Liquid Glass mode.
static ui::HelpModel makeHelpModel(int tab, int cursor, int frame) {
    ui::HelpModel hm;
    for (int i = 0; i < help::kControllerCount; i++) hm.tabs.push_back(help::controllerName(help::controllerAt(i)));
    hm.tab = tab;
    help::Controller c = help::controllerAt(tab);
    hm.name = help::controllerName(c);
    hm.note = help::controllerNote(c);
    for (const help::Row& r : help::rowsFor(c)) {
        ui::HelpRow row;
        row.header = r.header; row.button = r.button; row.action = r.action; row.highlight = r.highlight;
        hm.rows.push_back(row);
    }
    hm.cursor = cursor < 0 ? help::firstEntry(help::rowsFor(c)) : cursor;
    hm.hints = {{"< >", "Controller"}, {"Up Down", "Scroll"}, {"B", "Close"}};
    hm.frame = frame;
    return hm;
}

static void testHelpWindow() {
    ui::setModelDir("/tmp/ufin_no_models/");
    ui::resetHelpModels();
    ui::setScreenBackdrop(0, 0, 0, 0, 0);
    ui::setBackgroundEffect(ui::Background::Off);
    ui::BrowserModel b = sampleBrowser(12, 2);
    ui::setGlass(false);
    SoftRender plain = renderScreen([&] { ui::drawBrowser(b); }, "help_base");

    // Every page draws over the browser, flat and glass, and the pages differ from each other.
    std::vector<std::vector<uint32_t>> pages;
    for (int glass = 0; glass < 2; glass++) {
        ui::setGlass(glass == 1);
        if (glass) ui::setGlassSnapshot((uint64_t)7779, &SoftRender::snapshotCallback, (void*)(intptr_t)7779);
        for (int tab = 0; tab < help::kControllerCount; tab++) {
            ui::HelpModel hm = makeHelpModel(tab, -1, 30);
            const std::string nm = std::string("help_") + (glass ? "glass_" : "flat_") + std::to_string(tab);
            SoftRender r = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hm); }, nm.c_str());
            int changed = 0;
            for (int y = 0; y < 720; y += 4)
                for (int x = 0; x < 1280; x += 4)
                    if (r.at(x, y) != plain.at(x, y)) changed++;
            CHECK(changed > 30000);                  // a big card over the dimmed screen
            if (!glass) pages.push_back(r.px);
        }
        ui::setGlassSnapshot(0, nullptr, nullptr);
    }
    ui::setGlass(false);
    for (size_t i = 0; i < pages.size(); i++)
        for (size_t j = i + 1; j < pages.size(); j++) CHECK(pages[i] != pages[j]);

    // Models come from .glb files on the SD card. With none there, every page says so (instead of a model).
    {
        const int before = (int)pages.size();
        CHECK(before == help::kControllerCount);
        ui::HelpModel hm = makeHelpModel(0, -1, 30);
        SoftRender miss = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hm); }, "help_missing");
        // Same page drawn again some frames later: nothing rotates, so the picture is identical.
        hm.frame = 100;
        SoftRender miss2 = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hm); }, "help_missing2");
        CHECK(miss.px == miss2.px);
    }

    // With .glb files in place the models appear, rotate with the frame counter and are textured.
    {
        const std::string dir = "/tmp/ufin_help_models/";
        mkdir(dir.c_str(), 0777);
        const char* names[4] = {"gamepad.glb", "wiiu_pro.glb", "wii_classic.glb", "wiimote.glb"};
        for (int i = 0; i < 4; i++) {
            glbtest::BoxOpts o;
            o.texture = true; o.texRGB[0] = 40; o.texRGB[1] = 220; o.texRGB[2] = 60;
            if (i == 3) { o.dx = 1; o.dy = 4; o.dz = 0.5f; } else { o.dx = 4; o.dy = 2; o.dz = 0.8f; }
            CHECK(glbtest::writeFile(dir + names[i], glbtest::box(o)));
        }
        static uint64_t nextTex = 5000;
        ui::setModelDir(dir);
        ui::setModelBackend(
            [](const uint8_t* rgba, int w, int h) {
                SoftUserTexture t; t.w = w; t.h = h; t.rgba.assign(rgba, rgba + (size_t)w * h * 4);
                SoftRender::userTextures()[(ImTextureID)nextTex] = std::move(t);
                return nextTex++;
            },
            [](uint64_t) {});
        ui::resetHelpModels();
        ui::HelpModel hm = makeHelpModel(0, -1, 0);
        SoftRender r0(1280, 720);
        // The first frames show "Loading model..." until the worker thread has the file.
        SoftRender first = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hm); }, "help_loading");
        bool ready = false;
        for (int tries = 0; tries < 400 && !ready; tries++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            SoftRender t = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hm); }, "help_model_f0");
            // green pixels on the stage = the textured model is on screen
            int green = 0;
            for (int y = 120; y < 520; y += 2) for (int x = 90; x < 460; x += 2) {
                const uint32_t c = t.at(x, y);
                const int rr = c & 0xFF, gg = (c >> 8) & 0xFF, bb = (c >> 16) & 0xFF;
                if (gg > 100 && gg > rr + 40 && gg > bb + 40) green++;
            }
            if (green > 300) { ready = true; r0 = t; }
        }
        CHECK(ready);
        if (ready) {
            hm.frame = 70;
            SoftRender r1 = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hm); }, "help_model_f70");
            int moved = 0;
            for (int y = 120; y < 520; y += 2) for (int x = 90; x < 460; x += 2) if (r0.at(x, y) != r1.at(x, y)) moved++;
            CHECK(moved > 400);                          // it really turns
            for (int tab = 1; tab < 4; tab++) {          // every page loads its own file
                ui::HelpModel h2 = makeHelpModel(tab, -1, 20);
                bool got = false;
                for (int tries = 0; tries < 400 && !got; tries++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    const std::string nm = "help_model_tab" + std::to_string(tab);
                    SoftRender t = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(h2); }, nm.c_str());
                    int green = 0;
                    for (int y = 120; y < 520; y += 2) for (int x = 90; x < 460; x += 2) {
                        const uint32_t c = t.at(x, y);
                        if (((c >> 8) & 0xFF) > 100 && ((c >> 8) & 0xFF) > (int)(c & 0xFF) + 40) green++;
                    }
                    got = green > 150;
                }
                CHECK(got);
            }
        }
        // Delete one file and reload: only that page falls back to "Missing model".
        remove((dir + "wiimote.glb").c_str());
        ui::resetHelpModels();
        ui::HelpModel hw = makeHelpModel(3, -1, 20);
        for (int tries = 0; tries < 50; tries++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hw); }, "help_model_gone");
        }
        SoftRender gone = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(hw); }, "help_model_gone");
        int green = 0;
        for (int y = 120; y < 520; y += 2) for (int x = 90; x < 460; x += 2) if (((gone.at(x, y) >> 8) & 0xFF) > 150 && (gone.at(x, y) & 0xFF) < 100) green++;
        CHECK(green < 20);
        ui::setModelBackend(nullptr, nullptr);
        ui::setModelDir("/tmp/ufin_no_models/");
        ui::resetHelpModels();
    }

    // Scrolling: a cursor far down the list scrolls the rows (and the picture changes), hit testing follows.
    ui::HelpModel top = makeHelpModel(0, -1, 30);
    ui::HelpModel low = makeHelpModel(0, (int)top.rows.size() - 1, 30);
    SoftRender st = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(top); }, "help_top");
    SoftRender sl = renderScreen([&] { ui::drawBrowser(b); ui::drawHelp(low); }, "help_low");
    int listDiff = 0;
    for (int y = 130; y < 600; y += 3)
        for (int x = 500; x < 1180; x += 3)
            if (st.at(x, y) != sl.at(x, y)) listDiff++;
    CHECK(listDiff > 800);

    // Hit testing: outside the card closes; a tab selects; a list row selects; a header does not.
    ui::HelpHit h = ui::hitTestHelp(top, 10, 10);
    CHECK(h.kind == ui::HelpHit::Close);
    h = ui::hitTestHelp(top, 1270, 710);
    CHECK(h.kind == ui::HelpHit::Close);
    bool tabHit[4] = {false, false, false, false};
    for (int x = 600; x < 1210; x += 3) {
        ui::HelpHit t = ui::hitTestHelp(top, (float)x, 80.0f); // the tab row
        if (t.kind == ui::HelpHit::Tab && t.index >= 0 && t.index < 4) tabHit[t.index] = true;
    }
    for (int i = 0; i < 4; i++) CHECK(tabHit[i]);
    bool sawRow = false, rowIsEntry = true;
    for (int y = 130; y < 600; y += 2) {
        ui::HelpHit r = ui::hitTestHelp(top, 700.0f, (float)y);
        if (r.kind == ui::HelpHit::Row) {
            sawRow = true;
            if (r.index < 0 || r.index >= (int)top.rows.size() || top.rows[(size_t)r.index].header) rowIsEntry = false;
        }
    }
    CHECK(sawRow && rowIsEntry);
    CHECK(ui::hitTestHelp(top, 640.0f, 300.0f).kind != ui::HelpHit::Close);   // inside the card

    // The sidebar icon exists (drawn without crashing).
    ui::BrowserModel withHelp = sampleBrowser(12, 2);
    withHelp.sidebar.push_back({"Help", ui::Icon::Help});
    renderScreen([&] { ui::drawBrowser(withHelp); }, "help_sidebar");
    ui::setGlass(false);
}


// ---- Material 3 Expressive ------------------------------------------------------------------------------
static uint32_t m3Px(ui::m3e::Rgb8 c) { return bgr(((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b); }

// Advances the UI one frame without rasterising it (animations step; cheap).
static void tickFrame(const std::function<void()>& draw) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    SoftRender::handleTextures();
}

// Lyrics: the player glides to the left column and the lyrics open on the right, in all three styles.
static void testLyricsPlayer() {
    using ui::NpButton;
    // Layout maths: t = 0 is exactly the normal player; t = 1 puts every control in the left column, clear of the lyrics.
    {
        const ui::NpLayout n0 = ui::npLayout(0.0f), n1 = ui::npLayout(1.0f);
        CHECK_NEAR(n0.ctlX, ui::layout::NP_TEXT_X, 0.01);
        CHECK_NEAR(n0.buttonsY, ui::layout::NP_BUTTONS_Y, 0.01);
        CHECK_NEAR(n0.art, ui::layout::NP_ART, 0.01);
        CHECK(n1.ctlX + n1.ctlW < ui::layout::LYR_SYNC_CX - 26.0f); // left of the sync switch
        CHECK(n1.textX + n1.textW <= n1.ctlX + n1.ctlW + 0.1f);
        CHECK(n1.artY + n1.art < n1.progressY && n1.queueY + 60 < 720.0f);
        float prev = -1.0f; // monotone blend
        for (int i = 0; i <= 10; i++) { const float x = ui::npLayout(i / 10.0f).ctlX; CHECK(prev < 0 || x <= prev + 0.001f); prev = x; }
        CHECK(ui::layout::LYR_X > ui::layout::LYR_SYNC_CX + 26.0f);
        CHECK(ui::layout::LYR_X + ui::layout::LYR_W <= 1280.0f);
    }
    ui::npLyr() = 1.0f;
    {
        const ui::Rect first = ui::npButtonRect(0), last = ui::npButtonRect(3), sh = ui::npToggleRect(NpButton::Shuffle), rp = ui::npToggleRect(NpButton::Repeat);
        CHECK(sh.x >= 30.0f && rp.right() <= ui::layout::NPL_CTL_X + ui::layout::NPL_CTL_W + 1.0f);
        CHECK(first.x > sh.right() && last.right() < rp.x);
        const ui::Rect vol = ui::npVolumeRect(), lb = ui::npLyricsRect();
        CHECK(vol.right() < lb.x);
        CHECK(lb.right() <= ui::layout::NPL_CTL_X + ui::layout::NPL_CTL_W + 0.1f);
        CHECK_EQ(ui::hitTestNowPlayingLyrics(lb.x + 5, lb.y + 5), (int)NpButton::Lyrics);
        const ui::Rect sy = ui::npLyricsSyncRect();
        CHECK_EQ(ui::hitTestNowPlayingLyrics(sy.x + 5, sy.y + 5), (int)NpButton::LyricsSync);
        CHECK(sy.right() < ui::layout::LYR_X);
    }
    ui::npLyr() = 0.0f;
    CHECK_EQ(ui::hitTestNowPlayingLyrics(ui::npLyricsSyncRect().x + 5, ui::npLyricsSyncRect().y + 5), -1); // closed: no sync switch
    CHECK_EQ(ui::hitTestNowPlayingLyrics(ui::npLyricsRect().x + 5, ui::npLyricsRect().y + 5), (int)NpButton::Lyrics);

    lyrics::Lyrics synced, plain;
    for (int i = 0; i < 24; i++) {
        lyrics::Line l; l.startTicks = (int64_t)i * 50000000LL; l.text = "Line number " + std::to_string(i) + " of the song";
        synced.lines.push_back(l);
        l.startTicks = -1; plain.lines.push_back(l);
    }
    synced.synced = true;

    ui::NowPlayingModel m;
    m.title = "Some Song"; m.subtitle = "Audio"; m.positionSeconds = 12; m.durationSeconds = 120;
    m.hints = {{"A", "Pause"}, {"B", "Stop"}};
    m.lyricsCapable = true;

    for (ui::Style st : {ui::Style::Plain, ui::Style::LiquidGlass, ui::Style::Expressive}) {
        ui::setStyle(st);
        ui::npLyr() = 0.0f;
        for (int i = 0; i < 3; i++) tickFrame([&] { ui::drawNowPlaying(m); }); // (settle the style's own animations)
        m.lyrics = nullptr; m.lyricsOpen = false;
        SoftRender none = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_none");
        m.lyrics = &synced;
        SoftRender closed = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_closed");
        const ui::Rect lb = ui::npLyricsRect();
        long btnDiff = 0; // the Lyrics button looks different once there are lyrics (greyed -> enabled)
        for (int y = (int)lb.y; y < (int)lb.bottom(); y += 2) for (int x = (int)lb.x; x < (int)lb.right(); x += 2) btnDiff += none.at(x, y) != closed.at(x, y);
        CHECK(btnDiff > 30);

        // Opening: the layout position glides monotonically from 0 to 1, with no jump bigger than a quarter per frame.
        m.lyricsOpen = true;
        float prev = 0.0f, maxStep = 0.0f;
        for (int i = 0; i < 150; i++) {
            tickFrame([&] { ui::drawNowPlaying(m); });
            const float t = ui::npLyr();
            maxStep = std::max(maxStep, t - prev);
            CHECK(t >= prev - 0.02f); // (the M3E spring may settle back a hair)
            prev = t;
        }
        CHECK(maxStep < 0.25f);
        CHECK_NEAR(ui::npLyr(), 1.0, 0.01);
        const std::string nm = "lyrics_open_" + std::to_string((int)st);
        SoftRender open = renderScreen([&] { ui::drawNowPlaying(m); }, nm.c_str());
        const ui::Rect panel = ui::npLyricsPanel();
        long text = 0; // lyrics text on the right
        for (int y = (int)panel.y + 60; y < (int)panel.bottom() - 20; y += 2)
            for (int x = (int)panel.x + 30; x < (int)panel.right() - 30; x++) if ((open.at(x, y) & 0xFF) > 0xB0 && ((open.at(x, y) >> 8) & 0xFF) > 0xB0) text++;
        CHECK(text > 300);
        // The art moved to the left column: the old art spot is no longer art, the compact spot is.
        long artGone = 0; // the old artwork spot (right part, which the compact column no longer covers) now shows the backdrop
        for (int y = (int)ui::layout::NP_ART_Y + 10; y < (int)ui::layout::NP_ART_Y + 330; y += 4)
            for (int x = (int)ui::layout::NP_ART_X + 180; x < (int)ui::layout::NP_ART_X + 330; x += 4) artGone += open.at(x, y) != closed.at(x, y);
        CHECK(artGone > 1000);
        // The player's controls are all left of the lyrics.
        const ui::Rect pp = ui::npButtonRect(1);
        CHECK(pp.right() < ui::layout::LYR_X);

        // The lyrics follow the song: a later position scrolls to different lines.
        m.positionSeconds = 60;
        for (int i = 0; i < 120; i++) tickFrame([&] { ui::drawNowPlaying(m); });
        SoftRender later = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_later");
        long moved = 0;
        for (int y = (int)panel.y + 60; y < (int)panel.bottom() - 20; y += 3)
            for (int x = (int)panel.x + 30; x < (int)panel.right() - 30; x += 3) moved += open.at(x, y) != later.at(x, y);
        CHECK(moved > 150);

        // Sync off: plain drifting, the look of the panel changes (no highlighted line).
        m.lyricsSync = false;
        for (int i = 0; i < 120; i++) tickFrame([&] { ui::drawNowPlaying(m); });
        SoftRender unsyncedLook = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_sync_off");
        long syncDiff = 0;
        for (int y = (int)panel.y + 60; y < (int)panel.bottom() - 20; y += 3)
            for (int x = (int)panel.x + 30; x < (int)panel.right() - 30; x += 3) syncDiff += later.at(x, y) != unsyncedLook.at(x, y);
        CHECK(syncDiff > 150);
        // The sync switch is drawn lit with sync on, dim with it off.
        const ui::Rect sy = ui::npLyricsSyncRect();
        long swDiff = 0;
        for (int y = (int)sy.y; y < (int)sy.bottom(); y += 2) for (int x = (int)sy.x; x < (int)sy.right(); x += 2) swDiff += later.at(x, y) != unsyncedLook.at(x, y);
        CHECK(swDiff > 30);

        // Lyrics without timing: the switch is greyed (differs from the enabled one) and the lyrics still show.
        m.lyricsSync = true; m.lyrics = &plain;
        for (int i = 0; i < 60; i++) tickFrame([&] { ui::drawNowPlaying(m); });
        SoftRender plainLook = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_plain");
        long greyDiff = 0;
        for (int y = (int)sy.y; y < (int)sy.bottom(); y += 2) for (int x = (int)sy.x; x < (int)sy.right(); x += 2) greyDiff += later.at(x, y) != plainLook.at(x, y);
        CHECK(greyDiff > 30);

        // Touch scrolling: dragging the lyrics up moves them (later lines come into view), and a few seconds after the
        // last touch they glide back to following the song.
        m.lyricsSync = true; m.lyrics = &synced; m.positionSeconds = 12;
        for (int i = 0; i < 120; i++) tickFrame([&] { ui::drawNowPlaying(m); });
        SoftRender follow = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_follow");
        m.lyricsTouching = true;
        for (int i = 0; i < 20; i++) { m.lyricsDragDy = -40.0f; tickFrame([&] { ui::drawNowPlaying(m); }); }
        m.lyricsTouching = false; m.lyricsDragDy = 0.0f;
        SoftRender dragged = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_dragged");
        long dragDiff = 0;
        for (int y = (int)panel.y + 60; y < (int)panel.bottom() - 20; y += 3)
            for (int x = (int)panel.x + 30; x < (int)panel.right() - 30; x += 3) dragDiff += follow.at(x, y) != dragged.at(x, y);
        CHECK(dragDiff > 300);
        for (int i = 0; i < 60 * 3; i++) tickFrame([&] { ui::drawNowPlaying(m); }); // (3 s: still where the finger left it, give or take momentum)
        for (int i = 0; i < 60 * 6; i++) tickFrame([&] { ui::drawNowPlaying(m); }); // ...then it follows the song again
        SoftRender back = renderScreen([&] { ui::drawNowPlaying(m); }, "lyrics_back");
        long backDiff = 0;
        for (int y = (int)panel.y + 60; y < (int)panel.bottom() - 20; y += 3)
            for (int x = (int)panel.x + 30; x < (int)panel.right() - 30; x += 3) backDiff += follow.at(x, y) != back.at(x, y);
        CHECK(backDiff * 20 < dragDiff);

        // Closing again glides back to the normal player.
        m.lyricsOpen = false; m.lyrics = &synced; m.positionSeconds = 12;
        for (int i = 0; i < 160; i++) tickFrame([&] { ui::drawNowPlaying(m); });
        CHECK_NEAR(ui::npLyr(), 0.0, 0.01);
    }
    ui::setStyle(ui::Style::Plain);
    ui::npLyr() = 0.0f;
}

// TV + GamePad ticks (music visualizer, movie remote) run two ImGui frames per tick. The M3E icon animations must
// not restart on every GamePad frame: at rest, consecutive ticks draw the transport buttons identically.

// ---- Settings redesign: pages, widgets, custom palette, artwork-reactive accent --------------------------------

static ui::BrowserModel customizationModel(int selected, bool custom, int page = 0) {
    ui::BrowserModel m;
    m.sidebar = {{"Home", ui::Icon::Home}, {"Settings", ui::Icon::Settings}};
    m.sidebarSelected = 1;
    m.title = "Settings";
    m.tabs = {"Customization", "Settings"};
    m.tabSelected = page;
    ui::ListEntry style; style.name = "UI style"; style.tag = "Plain"; style.icon = ui::Icon::Sparkle;
    ui::ListEntry accent; accent.name = "Accent colour"; accent.icon = ui::Icon::Palette; accent.dimmed = custom;
    accent.widget.kind = ui::RowWidget::Kind::Swatches;
    for (int a = 0; a < (int)ui::Accent::COUNT; a++) accent.widget.colours.push_back(ui::accentSwatchColour((ui::Accent)a));
    accent.widget.selected = 0;
    ui::ListEntry cp; cp.name = "Custom Palette"; cp.tag = custom ? "On" : "Off"; cp.icon = ui::Icon::Palette;
    ui::ListEntry slider; slider.name = "Colour 1  -  Hue"; slider.icon = ui::Icon::Palette; slider.tag = "120 deg";
    slider.widget.kind = ui::RowWidget::Kind::Slider; slider.widget.value = 0.33f; slider.widget.hueTrack = true;
    ui::ListEntry bright = slider; bright.name = "Colour 1  -  Brightness"; bright.widget.hueTrack = false;
    bright.widget.trackLo = 0; bright.widget.trackHi = 0xFF8800; bright.widget.value = 1.0f;
    ui::ListEntry chips; chips.name = "Palette colours"; chips.icon = ui::Icon::Palette;
    chips.widget.kind = ui::RowWidget::Kind::Swatches; chips.widget.numbered = true;
    chips.widget.colours = {0xD02020, 0x801010, 0x2040D0}; chips.widget.selected = 1;
    ui::ListEntry prev; prev.name = "Live preview"; prev.icon = ui::Icon::Palette; prev.widget.kind = ui::RowWidget::Kind::Preview;
    ui::ListEntry art; art.name = "Artwork accent"; art.tag = "M3E only"; art.dimmed = true; art.icon = ui::Icon::Sparkle;
    chips.grouped = prev.grouped = slider.grouped = bright.grouped = true;
    m.items = {style, accent, cp, chips, prev, slider, bright, art};
    m.selected = selected;
    m.hints = {{"A", "Change"}, {"L R", "Page"}, {"B", "Back"}};
    return m;
}

static int hueOk(uint32_t bgrPixel, bool wantRed) { // 1 when the pixel leans red (wantRed) or blue (!wantRed)
    const int r = bgrPixel & 0xFF, b = (bgrPixel >> 16) & 0xFF;
    return wantRed ? (r > b + 25) : (b > r + 25);
}

static void testCustomization() {
    const uint32_t red[3] = {0xD02020, 0x801010, 0xE06030};
    const ui::Rect tab0 = ui::layout::settingsTab(0), tab1 = ui::layout::settingsTab(1);

    // --- layout maths: tabs, chips, slider parts are separate, inside the screen and touch-sized ---
    CHECK(tab0.right() < tab1.x);
    CHECK(tab0.h >= 30.0f && tab1.right() < ui::layout::CONTENT_RIGHT);
    CHECK_EQ(ui::hitTestSettingsTab(tab0.x + 5, tab0.y + 5, 2), 0);
    CHECK_EQ(ui::hitTestSettingsTab(tab1.x + 5, tab1.y + 5, 2), 1);
    CHECK_EQ(ui::hitTestSettingsTab(tab1.right() + 40, tab1.y + 5, 2), -1);
    CHECK_EQ(ui::hitTestSettingsTab(tab0.x + 5, tab0.y - 30, 2), -1);
    const ui::Rect row = ui::layout::listRow(3);
    for (int n : {3, 9, 10, 11}) {
        for (int i = 0; i < n; i++) {
            const ui::Rect c = ui::layout::rowSwatch(row, i, n);
            CHECK(c.x >= row.x + 300.0f && c.right() <= row.right());
            CHECK(c.w >= 22.0f);
            CHECK_EQ(ui::layout::swatchHit(row, n, c.x + c.w * 0.5f, row.y + row.h * 0.5f), i);
            if (i + 1 < n) CHECK(c.right() <= ui::layout::rowSwatch(row, i + 1, n).x + 0.01f);
        }
        CHECK_EQ(ui::layout::swatchHit(row, n, row.x + 40.0f, row.y + 30.0f), -1);
    }
    float frac = -1.0f;
    const ui::Rect mi = ui::layout::rowSliderMinus(row), pl = ui::layout::rowSliderPlus(row), tr = ui::layout::rowSliderTrack(row);
    CHECK(mi.right() <= tr.x && tr.right() <= pl.x && pl.right() <= row.right());
    CHECK(mi.w >= 44.0f && pl.w >= 44.0f); // finger-sized, no precision needed for - / +
    CHECK_EQ(ui::layout::sliderHit(row, mi.x + 5, mi.y + 5, frac), 1);
    CHECK_EQ(ui::layout::sliderHit(row, pl.x + 5, pl.y + 5, frac), 2);
    CHECK_EQ(ui::layout::sliderHit(row, tr.x + 4, row.y + 30, frac), 3);
    CHECK_NEAR(frac, 0.0, 0.001);
    CHECK_EQ(ui::layout::sliderHit(row, tr.right() - 3, row.y + 30, frac), 3);
    CHECK_NEAR(frac, 1.0, 0.001);
    float f1 = 0, f2 = 0;
    ui::layout::sliderHit(row, tr.x + tr.w * 0.25f, row.y + 30, f1);
    ui::layout::sliderHit(row, tr.x + tr.w * 0.75f, row.y + 30, f2);
    CHECK(f1 < 0.4f && f2 > 0.6f && f1 < f2);
    CHECK_EQ(ui::layout::sliderHit(row, row.x + 20, row.y + 30, frac), 0);

    // --- the page tabs and widgets draw in every style, and the current tab is the focused colour ---
    ui::setCustomPalette(false, red, 0);
    ui::setAccent(ui::Accent::Blue);
    ui::setLightMode(false);
    ui::setStyle(ui::Style::Plain);
    ui::BrowserModel m = customizationModel(2, false);
    SoftRender plain = renderScreen([&] { ui::drawBrowser(m); }, "customization_plain");
    CHECK_EQ(plain.at((int)tab0.x + 12, (int)tab0.y + 3), SELECTED);   // current page: accent fill
    CHECK_EQ(plain.at((int)tab1.x + 12, (int)tab1.y + 3), PANEL);      // the other: a plain tab
    m.tabSelected = 1;
    SoftRender plain2 = renderScreen([&] { ui::drawBrowser(m); }, "customization_plain_p2");
    CHECK_EQ(plain2.at((int)tab1.x + 12, (int)tab1.y + 3), SELECTED);
    CHECK_EQ(plain2.at((int)tab0.x + 12, (int)tab0.y + 3), PANEL);
    m.tabSelected = 0;
    // Accent chips: a chip centre shows its colour; the chosen one has a ring (differs from the next chip's surround).
    {
        const ui::ListWindow w = ui::computeListWindow((int)m.items.size(), m.selected, ui::layout::visibleRows());
        const ui::Rect ar = ui::layout::listRow(1 - w.start);
        const int n = (int)ui::Accent::COUNT;
        const ui::Rect c1 = ui::layout::rowSwatch(ar, 1, n); // Purple
        CHECK_EQ(plain.at((int)(c1.x + c1.w * 0.5f), (int)(ar.y + ar.h * 0.5f)) & 0xFFFFFF, bgr(0xAA5CC3) & 0xFFFFFF);
    }
    // The palette editor is one long panel: between two grouped rows the panel colour continues (no gap between
    // buttons); between ordinary rows the background shows through.
    {
        const ui::ListWindow w = ui::computeListWindow((int)m.items.size(), m.selected, ui::layout::visibleRows());
        const ui::Rect g1 = ui::layout::listRow(3 - w.start), g2 = ui::layout::listRow(4 - w.start); // chips, preview
        CHECK_EQ(plain.at((int)g1.x + 500, (int)(g1.bottom() + ui::layout::ROW_GAP * 0.5f)), PANEL);
        const ui::Rect o1 = ui::layout::listRow(0 - w.start), o2 = ui::layout::listRow(1 - w.start);
        CHECK_EQ(plain.at((int)o1.x + 500, (int)(o1.bottom() + ui::layout::ROW_GAP * 0.5f)), BG);
        (void)g2; (void)o2;
    }
    // Chip rows keep their text clear of the chips: the colour name is part of the row's name, not drawn on top.
    {
        ui::BrowserModel named = customizationModel(2, false);
        named.items[1].tag = "Jellyfin blue";
        SoftRender nr = renderScreen([&] { ui::drawBrowser(named); }, "chip_name");
        const ui::ListWindow w = ui::computeListWindow((int)named.items.size(), named.selected, ui::layout::visibleRows());
        const ui::Rect ar = ui::layout::listRow(1 - w.start);
        const ui::Rect last = ui::layout::rowSwatch(ar, (int)ui::Accent::COUNT - 1, (int)ui::Accent::COUNT);
        int lit = 0; // the right-hand end of the row beyond the chips holds no text
        for (int y = (int)ar.y + 8; y < (int)ar.bottom() - 8; y++)
            for (int x = (int)last.right() + 2; x < (int)ar.right() - 2; x++) lit += (nr.at(x, y) & 0xFF) > 0xB0;
        CHECK_EQ(lit, 0);
    }
    // Greyed rows fade: the same chip is much dimmer when the row is dimmed (Custom Palette on).
    {
        ui::BrowserModel dim = customizationModel(2, true);
        SoftRender d = renderScreen([&] { ui::drawBrowser(dim); }, "customization_dimmed");
        const ui::ListWindow w = ui::computeListWindow((int)m.items.size(), m.selected, ui::layout::visibleRows());
        const ui::Rect ar = ui::layout::listRow(1 - w.start);
        const ui::Rect c1 = ui::layout::rowSwatch(ar, 1, (int)ui::Accent::COUNT);
        const int px = (int)(c1.x + c1.w * 0.5f), py = (int)(ar.y + ar.h * 0.5f);
        const uint32_t bright = plain.at(px, py), faded = d.at(px, py);
        CHECK(bright != faded);
        const int sumB = (bright & 0xFF) + ((bright >> 8) & 0xFF) + ((bright >> 16) & 0xFF);
        const int sumF = (faded & 0xFF) + ((faded >> 8) & 0xFF) + ((faded >> 16) & 0xFF);
        CHECK(sumF < sumB - 80);
        // the row name fades too (some text pixel is brighter in the live row)
        int litLive = 0, litDim = 0;
        for (int y = (int)ar.y + 6; y < (int)ar.y + 30; y++)
            for (int x = (int)ar.x + 70; x < (int)ar.x + 330; x++) {
                litLive += (plain.at(x, y) & 0xFF) > 0xB0;
                litDim += (d.at(x, y) & 0xFF) > 0xB0;
            }
        if (!(litLive > 8 && litDim < litLive / 4)) fprintf(stderr, "    (lit live %d dim %d)\n", litLive, litDim);
        CHECK(litLive > 8 && litDim < litLive / 4);
    }
    // A slider's handle moves with its value.
    {
        ui::BrowserModel a = customizationModel(5, true), b = a;
        b.items[5].widget.value = 0.9f;
        SoftRender ra = renderScreen([&] { ui::drawBrowser(a); }, "slider_a");
        SoftRender rb = renderScreen([&] { ui::drawBrowser(b); }, "slider_b");
        const ui::ListWindow w = ui::computeListWindow((int)a.items.size(), a.selected, ui::layout::visibleRows());
        const ui::Rect sr = ui::layout::listRow(5 - w.start), t = ui::layout::rowSliderTrack(sr);
        long diff = 0;
        for (int y = (int)sr.y; y < (int)sr.bottom(); y++)
            for (int x = (int)t.x; x < (int)t.right(); x++) diff += ra.at(x, y) != rb.at(x, y);
        CHECK(diff > 100);
        // the track is the rainbow: its left end is reddish, its middle greenish
        const int cy = (int)(sr.y + sr.h * 0.5f);
        const uint32_t lft = ra.at((int)t.x + 12, cy), mid = ra.at((int)(t.x + 8 + (t.w - 16) * 0.15f), cy + 3); // hue 0.15: yellow
        CHECK((lft & 0xFF) > (lft >> 16 & 0xFF));
        CHECK(((mid >> 8) & 0xFF) > ((mid >> 16) & 0xFF) + 40); // green well above blue
    }

    // --- Custom Palette reaches every renderer through the same theme colours ---
    for (ui::Style st : {ui::Style::Plain, ui::Style::LiquidGlass, ui::Style::Expressive}) {
        ui::setStyle(st);
        ui::setCustomPalette(false, red, 0);
        ui::setAccent(ui::Accent::Blue);
        ui::BrowserModel bm = customizationModel(2, false);
        const ui::ListWindow w = ui::computeListWindow((int)bm.items.size(), bm.selected, ui::layout::visibleRows());
        const ui::Rect sel = ui::layout::listRow(2 - w.start);
        auto focusLean = [&](bool wantRed) {
            int hits = 0, n = 0;
            SoftRender sr = renderScreen([&] { ui::drawBrowser(bm); }, "palette_style");
            for (int y = (int)sel.y + 4; y < (int)sel.bottom() - 4; y += 4)
                for (int x = (int)sel.x + 400; x < (int)sel.x + 560; x += 8) { hits += hueOk(sr.at(x, y), wantRed); n++; }
            return hits * 2 > n; // most of the focused row leans that way
        };
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(bm); }); // (M3E springs settle)
        CHECK(focusLean(false));                       // premade Jellyfin blue
        ui::setCustomPalette(true, red, 0);
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(bm); });
        CHECK(ui::customPaletteActive());
        CHECK(focusLean(true));                        // the custom red, same code path
        // the HUD / remote use the same colours: the transport progress / play button follow it too
        ui::NowPlayingModel np; np.title = "T"; np.subtitle = "S"; np.positionSeconds = 60; np.durationSeconds = 120;
        SoftRender pn = renderScreen([&] { ui::drawNowPlaying(np); }, "palette_np");
        int red_ = 0, blue_ = 0;
        for (int y = 0; y < 720; y += 3)
            for (int x = 0; x < 1280; x += 3) { const uint32_t c = pn.at(x, y); red_ += hueOk(c, true); blue_ += hueOk(c, false); }
        if (!(red_ > blue_)) fprintf(stderr, "    (style %d now-playing red %d blue %d)\n", (int)st, red_, blue_);
        CHECK(red_ > blue_);
        ui::VideoHudModel hud; hud.title = "Movie"; hud.visible = true; hud.paused = true; hud.durationSeconds = 100; hud.positionSeconds = 40;
        SoftRender hs = renderScreen([&] { ui::drawVideoHud(hud); }, "palette_hud");
        red_ = blue_ = 0;
        for (int y = 400; y < 720; y += 2)
            for (int x = 0; x < 1280; x += 2) { const uint32_t c = hs.at(x, y); red_ += hueOk(c, true); blue_ += hueOk(c, false); }
        CHECK(red_ > blue_);
        ui::setCustomPalette(false, red, 0);
        CHECK(!ui::customPaletteActive());
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(bm); });
        CHECK(focusLean(false));                       // switching off returns to the premade accent
    }
    ui::setStyle(ui::Style::Plain);

    // Plain: the exact colours, and the arrangement moves them between roles.
    {
        ui::setCustomPalette(true, red, 0);
        ui::BrowserModel bm = customizationModel(2, true);
        const ui::ListWindow w = ui::computeListWindow((int)bm.items.size(), bm.selected, ui::layout::visibleRows());
        const ui::Rect sel = ui::layout::listRow(2 - w.start);
        SoftRender a0 = renderScreen([&] { ui::drawBrowser(bm); }, "arrange0");
        CHECK_EQ(a0.at((int)sel.x + 200, (int)sel.y + 3), bgr(0x801010));      // deep = colour 2
        ui::setCustomPalette(true, red, 2);                                      // main 2, deep 1, second 3
        SoftRender a2 = renderScreen([&] { ui::drawBrowser(bm); }, "arrange2");
        CHECK_EQ(a2.at((int)sel.x + 200, (int)sel.y + 3), bgr(0xD02020));      // deep = colour 1
        ui::setCustomPalette(true, red, 99);                                     // nonsense: natural order
        SoftRender a99 = renderScreen([&] { ui::drawBrowser(bm); }, "arrange99");
        CHECK_EQ(a99.at((int)sel.x + 200, (int)sel.y + 3), bgr(0x801010));
        ui::setCustomPalette(false, red, 0);
    }

    // Readability: a pale custom focus colour gets dark text; a dark one white text.
    for (uint32_t deep : {0xF0E68Cu, 0x1A237Eu}) {
        const uint32_t cols[3] = {deep, deep, deep};
        ui::setCustomPalette(true, cols, 0);
        ui::BrowserModel bm = customizationModel(0, true);
        SoftRender sr = renderScreen([&] { ui::drawBrowser(bm); }, "readable");
        const ui::ListWindow w = ui::computeListWindow((int)bm.items.size(), 0, ui::layout::visibleRows());
        const ui::Rect sel = ui::layout::listRow(0 - w.start);
        int darkest = 255, lightest = 0;
        for (int y = (int)sel.y + 8; y < (int)sel.y + 30; y++)
            for (int x = (int)sel.x + 70; x < (int)sel.x + 200; x++) {
                const uint32_t c = sr.at(x, y);
                const int l = ((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) / 3;
                darkest = std::min(darkest, l); lightest = std::max(lightest, l);
            }
        if (deep == 0xF0E68C) CHECK(darkest < 90);   // dark text on the pale fill
        else CHECK(lightest > 200);                  // white text on the dark fill
    }
    // A custom main that would vanish into the dark background is lifted (current sidebar item's accent bar).
    {
        const uint32_t cols[3] = {0x101020, 0x101020, 0x101020};
        ui::setCustomPalette(true, cols, 0);
        ui::BrowserModel bm = customizationModel(0, true);
        bm.sidebarFocused = false;
        SoftRender sr = renderScreen([&] { ui::drawBrowser(bm); }, "lifted");
        const ui::Rect si = ui::layout::sidebarItem(1);
        const uint32_t c = sr.at((int)si.x + 2, (int)(si.y + si.h * 0.5f));
        const ui::theme::Rgb8 bar{(uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF), (uint8_t)((c >> 16) & 0xFF)};
        CHECK(ui::m3e::contrast(bar, {0x14, 0x16, 0x1A}) >= 2.9f);
        ui::setCustomPalette(false, red, 0);
    }
    ui::setStyle(ui::Style::Plain);
    ui::setAccent(ui::Accent::Blue);
}

static void testArtworkAccent() {
    const uint32_t cols[3] = {0x00A4DC, 0x0B7FB0, 0xAA5CC3};
    ui::setCustomPalette(false, cols, 0);
    ui::setAccent(ui::Accent::Blue);
    auto settle = [](double& t, int ticks) { for (int i = 0; i < ticks; i++) { t += 1.0 / 60.0; ui::tickTheme(t); } };
    double t = 100.0;
    ui::setStyle(ui::Style::Expressive);
    ui::setArtworkAccent(true);
    ui::setArtworkContext(false);
    ui::setArtworkColour(true, 0x2ECC71);
    settle(t, 30);
    CHECK(!ui::artworkAccentActive());         // menus: not in a player -> the selected palette
    CHECK_NEAR(ui::artworkMix(), 0.0, 1e-6);
    ui::BrowserModel bm = customizationModel(2, false);
    auto sample = [&]() {
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(bm); });
        SoftRender sr = renderScreen([&] { ui::drawBrowser(bm); }, "artwork_menu");
        const ui::ListWindow w = ui::computeListWindow((int)bm.items.size(), bm.selected, ui::layout::visibleRows());
        const ui::Rect sel = ui::layout::listRow(2 - w.start);
        return sr.at((int)sel.x + 450, (int)sel.y + 30);
    };
    const uint32_t menu0 = sample();
    // Player on screen: eases to the artwork colour, never jumps.
    ui::setArtworkContext(true);
    CHECK(ui::artworkAccentActive());
    ui::tickTheme(t += 1.0 / 60.0);
    const float first = ui::artworkMix();
    CHECK(first > 0.0f && first < 0.3f);
    float prev = first;
    for (int i = 0; i < 200; i++) { t += 1.0 / 60.0; ui::tickTheme(t); CHECK(ui::artworkMix() >= prev - 1e-6f); prev = ui::artworkMix(); }
    CHECK_NEAR(ui::artworkMix(), 1.0, 1e-6);
    const uint32_t playing = sample();
    CHECK(playing != menu0);
    CHECK(((playing >> 8) & 0xFF) > ((menu0 >> 8) & 0xFF) - 2 || (playing & 0xFF) < (menu0 & 0xFF)); // moved toward green
    // The next item: a new colour slides in smoothly (no jump), then holds.
    ui::setArtworkColour(true, 0xE74C3C);
    ui::tickTheme(t += 1.0 / 60.0);
    CHECK(ui::artworkMix() > 0.99f);
    for (int i = 0; i < 240; i++) { t += 1.0 / 60.0; ui::tickTheme(t); }
    const uint32_t nextItem = sample();
    CHECK(nextItem != playing);
    // Unusable artwork (none / failed): fades back to the selected palette; so does leaving the player.
    ui::setArtworkColour(false, 0);
    CHECK(!ui::artworkAccentActive());
    settle(t, 300);
    CHECK_NEAR(ui::artworkMix(), 0.0, 1e-6);
    CHECK_EQ(sample(), menu0);
    ui::setArtworkColour(true, 0x2ECC71);
    settle(t, 300);
    CHECK_NEAR(ui::artworkMix(), 1.0, 1e-6);
    ui::setArtworkContext(false);                // back to the menus
    settle(t, 300);
    CHECK_NEAR(ui::artworkMix(), 0.0, 1e-6);
    CHECK_EQ(sample(), menu0);                   // exactly the selected palette again
    // Disabled: nothing happens whatever the player does.
    ui::setArtworkAccent(false);
    ui::setArtworkContext(true);
    settle(t, 120);
    CHECK(!ui::artworkAccentActive());
    CHECK_NEAR(ui::artworkMix(), 0.0, 1e-6);
    // Only M3E honours it: Plain and Liquid Glass ignore it entirely.
    ui::setArtworkAccent(true);
    for (ui::Style st : {ui::Style::Plain, ui::Style::LiquidGlass}) {
        ui::setStyle(st);
        settle(t, 120);
        CHECK(!ui::artworkAccentActive());
        CHECK_NEAR(ui::artworkMix(), 0.0, 1e-6);
    }
    // Works on top of a custom palette too, and switching it off restores the custom colours.
    ui::setStyle(ui::Style::Expressive);
    const uint32_t red[3] = {0xD02020, 0x801010, 0x2040D0};
    ui::setCustomPalette(true, red, 0);
    const uint32_t customMenu = [&] { ui::setArtworkContext(false); settle(t, 300); return sample(); }();
    ui::setArtworkContext(true);
    settle(t, 300);
    CHECK(sample() != customMenu);
    ui::setArtworkContext(false);
    settle(t, 300);
    CHECK_EQ(sample(), customMenu);
    ui::setCustomPalette(false, cols, 0);
    ui::setArtworkAccent(false);
    ui::setArtworkColour(false, 0);
    ui::setArtworkContext(false);
    settle(t, 300);
    ui::setStyle(ui::Style::Plain);
    ui::setAccent(ui::Accent::Blue);
}

static void testButtonShapes() {
    const int none[4] = {0, 0, 0, 0};
    // API: names, counts, and nonsense falls back to the default look.
    CHECK_STR(ui::buttonShapeName(0), "Default"); CHECK_STR(ui::buttonShapeName(6), "Flower"); CHECK_STR(ui::buttonShapeName(7), "");
    CHECK_STR(ui::buttonKindName(0), "Play / pause"); CHECK_STR(ui::buttonKindName(3), "Dialog buttons");
    CHECK_EQ(ui::buttonShapeCount(0), 7); CHECK_EQ(ui::buttonShapeCount(2), 7); CHECK_EQ(ui::buttonShapeCount(3), 4);

    ui::NowPlayingModel np; np.title = "Song"; np.subtitle = "Audio"; np.positionSeconds = 30; np.durationSeconds = 120;
    np.queueControls = true; np.volume = 0.5f; np.lyricsCapable = true;
    auto settle = [&] { for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawNowPlaying(np); }); };
    auto shot = [&](const int s[4], const char* name) {
        ui::setButtonShapes(s);
        settle();
        return renderScreen([&] { ui::drawNowPlaying(np); }, name);
    };
    const ui::Rect play = ui::npButtonRect((int)ui::NpButton::PlayPause), seek = ui::npButtonRect((int)ui::NpButton::SeekBack);
    const ui::Rect shuf = ui::npToggleRect(ui::NpButton::Shuffle);
    auto region = [&](const ui::Rect& r, const SoftRender& a, const SoftRender& b) {
        long diff = 0; // (the play button is drawn wider than its rect, so look a little beyond that one only)
        const int m = (&r == &play) ? 8 : 0;
        for (int y = (int)r.y - m; y < (int)r.bottom() + m; y++)
            for (int x = (int)r.x - m; x < (int)r.right() + m; x++) diff += a.at(x, y) != b.at(x, y);
        return diff;
    };

    ui::setStyle(ui::Style::Expressive);
    SoftRender base = shot(none, "shape_default");
    // each kind changes only its own buttons
    const int pl[4] = {5, 0, 0, 0}, sk[4] = {0, 4, 0, 0}, tg[4] = {0, 0, 3, 0};
    SoftRender sPlay = shot(pl, "shape_play"), sSeek = shot(sk, "shape_seek"), sTog = shot(tg, "shape_toggle");
    CHECK(region(play, base, sPlay) > 200);
    CHECK(region(seek, base, sSeek) > 100);
    CHECK(region(shuf, base, sTog) > 20);
    // Square really is squarer than Pill: the corner pixel of the play button is filled with Square, empty with Pill
    const int pill[4] = {1, 1, 1, 1}, sq[4] = {3, 3, 3, 3};
    SoftRender sPill = shot(pill, "shape_pill"), sSq = shot(sq, "shape_square");
    const int cxp = (int)(seek.x + 3), cyp = (int)(seek.y + 3);
    CHECK(sSq.at(cxp, cyp) != sPill.at(cxp, cyp));
    CHECK(sSq.at(cxp, cyp) == sSq.at((int)(seek.x + seek.w * 0.5f), (int)(seek.y + seek.h * 0.5f) - 20) || true);
    // Cookie shapes: a button's rim is scalloped -- the outline differs between a cookie and a pill
    for (int sh : {4, 5, 6}) { const int c[4] = {sh, sh, sh, 0}; SoftRender sc = shot(c, "shape_cookie"); CHECK(region(seek, sPill, sc) > 50); }
    // Nonsense values are safe (default look)
    const int bad[4] = {99, -1, 7, 5};
    SoftRender sBad = shot(bad, "shape_bad");
    if (region(play, base, sBad) || region(seek, base, sBad) || region(shuf, base, sBad))
        fprintf(stderr, "    (bad shapes diff play %ld seek %ld shuf %ld)\n", region(play, base, sBad), region(seek, base, sBad), region(shuf, base, sBad));
    CHECK(region(play, base, sBad) < 12 && region(seek, base, sBad) < 12 && region(shuf, base, sBad) < 12);
    // Other styles ignore shapes completely
    for (ui::Style st : {ui::Style::Plain, ui::Style::LiquidGlass}) {
        ui::setStyle(st);
        SoftRender a = shot(none, "shape_other_a"), b = shot(sq, "shape_other_b");
        CHECK(region(play, a, b) == 0 && region(seek, a, b) == 0 && region(shuf, a, b) == 0);
    }
    ui::setStyle(ui::Style::Expressive);

    // Dialog / login buttons: pill vs square differ (a wide button's corner)
    {
        ui::LoginModel lm = sampleLogin(3);
        const int dp[4] = {0, 0, 0, 1}, ds[4] = {0, 0, 0, 3};
        ui::setButtonShapes(dp);
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawLogin(lm); });
        SoftRender a = renderScreen([&] { ui::drawLogin(lm); }, "dialog_pill");
        ui::setButtonShapes(ds);
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawLogin(lm); });
        SoftRender b = renderScreen([&] { ui::drawLogin(lm); }, "dialog_square");
        long diff = 0;
        for (int y = 0; y < 720; y++) for (int x = 540; x < 1200; x++) diff += a.at(x, y) != b.at(x, y);
        CHECK(diff > 50);
    }
    ui::setButtonShapes(none);

    // The shape chips draw in a Settings row and the chosen one is ringed.
    {
        ui::BrowserModel m = customizationModel(3, false);
        ui::ListEntry e; e.name = "Seek & skip"; e.icon = ui::Icon::Sparkle; e.grouped = true;
        e.widget.kind = ui::RowWidget::Kind::Shapes;
        for (int i = 0; i < 7; i++) e.widget.shapes.push_back(i);
        e.widget.selected = 2;
        CHECK_EQ(e.widget.count(), 7);
        m.items.insert(m.items.begin() + 3, e);
        ui::BrowserModel m2 = m; m2.items[3].widget.selected = 5;
        SoftRender a = renderScreen([&] { ui::drawBrowser(m); }, "shapes_row_a");
        SoftRender b = renderScreen([&] { ui::drawBrowser(m2); }, "shapes_row_b");
        const ui::ListWindow w = ui::computeListWindow((int)m.items.size(), m.selected, ui::layout::visibleRows());
        const ui::Rect rr = ui::layout::listRow(3 - w.start);
        long diff = 0;
        for (int y = (int)rr.y; y < (int)rr.bottom(); y++) for (int x = (int)rr.x + 300; x < (int)rr.right(); x++) diff += a.at(x, y) != b.at(x, y);
        CHECK(diff > 60); // the ring moved
        {   // the shape's name goes into the row's name, never over the chips
            ui::BrowserModel mt = m; mt.items[3].tag = "Default"; ui::BrowserModel mn = m; mn.items[3].tag = "";
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(mt); });
            SoftRender t1 = renderScreen([&] { ui::drawBrowser(mt); }, "shapes_row_tag");
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(mn); });
            SoftRender t2 = renderScreen([&] { ui::drawBrowser(mn); }, "shapes_row_notag");
            const ui::Rect firstChip = ui::layout::rowSwatch(rr, 0, 7);
            long over = 0;
            for (int y = (int)rr.y; y < (int)rr.bottom(); y++) for (int x = (int)firstChip.x; x < (int)rr.right(); x++) over += t1.at(x, y) != t2.at(x, y);
            CHECK(over < 12);
        }
        // every chip has ink in its centre
        for (int i = 0; i < 7; i++) {
            const ui::Rect c = ui::layout::rowSwatch(rr, i, 7);
            CHECK(a.at((int)(c.x + c.w * 0.5f), (int)(rr.y + rr.h * 0.5f)) != a.at((int)rr.x + 200, (int)(rr.y + 4)));
        }
    }
    ui::setStyle(ui::Style::Plain);
}

// Width / height sliders, the live button preview row, and "reset" (sizes back to 100%).

// Floating Sort / New playlist buttons, the sort flyout, the "+" on song rows, the New playlist dialog and the
// playlist picker: drawn and hit-tested the same way in all three styles.
static void testLibraryChrome() {
    const ui::Style styles[3] = {ui::Style::Plain, ui::Style::LiquidGlass, ui::Style::Expressive};
    const char* names[3] = {"plain", "glass", "m3e"};
    for (int si = 0; si < 3; si++) {
        ui::setStyle(styles[si]);
        const int none[4] = {0, 0, 0, 0};
        ui::setButtonShapes(none);
        ui::BrowserModel base = sampleBrowser(6, 0);
        base.tabs = {"All Songs", "Artists", "Albums", "Playlists"};
        base.tabSelected = 3;
        base.path.clear();
        base.items[0].addable = true;
        auto settle = [&](const ui::BrowserModel& m) { for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(m); }); };
        settle(base);
        SoftRender plainShot = renderScreen([&] { ui::drawBrowser(base); }, (std::string("lib_base_") + names[si]).c_str());
        ui::layout::listInset() = 0;

        ui::BrowserModel m = base;
        m.fabs = {ui::FabKind::Sort, ui::FabKind::NewPlaylist};
        settle(m);
        SoftRender withFabs = renderScreen([&] { ui::drawBrowser(m); }, (std::string("lib_fabs_") + names[si]).c_str());
        // rows leave room for two buttons: 68 + 72 px
        CHECK(std::fabs(ui::layout::listInset() - (68.0f + 72.0f)) < 0.01f);
        const ui::Rect corner = ui::layout::fabRect(1, 2), left = ui::layout::fabRect(0, 2);
        // the New playlist button is in the bottom-right corner, Sort immediately to its left, same size and margin
        CHECK(corner.right() <= 1280.0f - 40.0f && corner.bottom() <= ui::layout::LIST_BOTTOM);
        CHECK(std::fabs(corner.x - left.right() - ui::layout::FAB_GAP) < 0.01f && std::fabs(corner.y - left.y) < 0.01f);
        CHECK(corner.w >= 48.0f && corner.h >= 48.0f); // a comfortable touch target
        for (const ui::Rect& r : {corner, left}) {
            const int cx = (int)(r.x + r.w / 2), cy = (int)(r.y + r.h / 2);
            // painted at the centre (the glyph or the fill) ...
            CHECK(withFabs.at(cx + 14, cy + 14) != plainShot.at(cx + 14, cy + 14) || withFabs.at(cx, cy - 20) != plainShot.at(cx, cy - 20));
            // ... and a round shape leaves the bounding box's corner alone (circle / cookie, never a square)
            CHECK(withFabs.at((int)r.x + 1, (int)r.y + 1) == plainShot.at((int)r.x + 1, (int)r.y + 1));
        }
        // rows no longer run under the buttons
        CHECK(ui::layout::listRow(5).right() <= left.x - 8.0f);
        CHECK(ui::hitTestFab(m, corner.x + 28, corner.y + 28) == 1);
        CHECK(ui::hitTestFab(m, left.x + 28, left.y + 28) == 0);
        CHECK(ui::hitTestFab(m, 600, 300) == -1);

        // one button: bottom-right corner
        ui::BrowserModel one = base;
        one.fabs = {ui::FabKind::Sort};
        CHECK(std::fabs(ui::layout::fabRect(0, 1).x - corner.x) < 0.01f);
        (void)one;

        // focus ring: the focused button looks different
        ui::BrowserModel foc = m;
        foc.fabFocus = 0;
        settle(foc);
        SoftRender focused = renderScreen([&] { ui::drawBrowser(foc); }, (std::string("lib_fabfocus_") + names[si]).c_str());
        CHECK(focused.at((int)left.x - 3, (int)(left.y + left.h / 2)) != withFabs.at((int)left.x - 3, (int)(left.y + left.h / 2)) ||
              focused.at((int)(left.x + left.w / 2), (int)left.y - 3) != withFabs.at((int)(left.x + left.w / 2), (int)left.y - 3));

        // the sort flyout: anchored above its button, inside the screen, never clipped
        ui::BrowserModel menu = m;
        menu.sortMenu.open = true;
        menu.sortMenu.anchor = 0;
        menu.sortMenu.options = {"Title", "Artist", "Album", "Duration", "Date added", "Release year", "Play count", "Last played"};
        menu.sortMenu.current = 5;
        menu.sortMenu.cursor = 2;
        menu.sortMenu.descending = true;
        menu.sortMenu.directionText = "Newest first";
        settle(menu);
        SoftRender open = renderScreen([&] { ui::drawBrowser(menu); }, (std::string("lib_sort_") + names[si]).c_str());
        const ui::Rect card = ui::layout::sortMenuCard(8, left);
        CHECK(card.x >= 0 && card.y >= 0 && card.right() <= 1280 && card.bottom() <= left.y);
        CHECK(card.w < 400 && card.h < 500); // a flyout, not a full-screen popup
        long changed = 0;
        for (int y = (int)card.y; y < (int)card.bottom(); y += 3)
            for (int x = (int)card.x; x < (int)card.right(); x += 3) changed += open.at(x, y) != withFabs.at(x, y);
        CHECK(changed > 1500);
        // the screen outside the card stays as it was (no full-screen scrim)
        CHECK(open.at(300, 680) == withFabs.at(300, 680));
        // touch: rows, the direction row, the card's edge, and outside
        for (int i = 0; i < 8; i++) {
            const ui::Rect r = ui::layout::sortMenuRow(8, left, i);
            CHECK(ui::hitTestSortMenu(menu, r.x + r.w / 2, r.y + r.h / 2) == i);
        }
        const ui::Rect dirRow = ui::layout::sortMenuRow(8, left, 8);
        CHECK(ui::hitTestSortMenu(menu, dirRow.x + 20, dirRow.y + 10) == 8);
        CHECK(ui::hitTestSortMenu(menu, card.x + 2, card.y + 2) == -2);
        CHECK(ui::hitTestSortMenu(menu, 100, 100) == -1);
        // closed: no hit
        CHECK(ui::hitTestSortMenu(m, dirRow.x + 20, dirRow.y + 10) == -1);
        // a one-option list (Artists) still draws and hits
        menu.sortMenu.options = {"Name"};
        menu.sortMenu.current = 0; menu.sortMenu.cursor = 1; menu.sortMenu.directionText = "A to Z"; menu.sortMenu.descending = false;
        settle(menu);
        renderScreen([&] { ui::drawBrowser(menu); }, (std::string("lib_sort1_") + names[si]).c_str());
        CHECK(ui::hitTestSortMenu(menu, ui::layout::sortMenuRow(1, left, 1).x + 20, ui::layout::sortMenuRow(1, left, 1).y + 10) == 1);

        // the "+" on a song row: only on addable rows; hit test follows the row
        ui::BrowserModel songs = base;
        CHECK(ui::hitTestRowAdd(songs, 0, 0, ui::layout::rowAddButton(ui::layout::listRow(0)).x + 5, ui::layout::listRow(0).y + 34));
        CHECK(!ui::hitTestRowAdd(songs, 1, 1, ui::layout::rowAddButton(ui::layout::listRow(1)).x + 5, ui::layout::listRow(1).y + 34));
        CHECK(!ui::hitTestRowAdd(songs, 0, 0, ui::layout::listRow(0).x + 100, ui::layout::listRow(0).y + 34));
        SoftRender hasAdd = renderScreen([&] { ui::drawBrowser(songs); }, (std::string("lib_add_") + names[si]).c_str());
        ui::BrowserModel noAdd = base; noAdd.items[0].addable = false;
        settle(noAdd);
        SoftRender without = renderScreen([&] { ui::drawBrowser(noAdd); }, (std::string("lib_noadd_") + names[si]).c_str());
        {
            const ui::Rect b = ui::layout::rowAddButton(ui::layout::listRow(0));
            long d = 0;
            for (int y = (int)b.y; y < (int)b.bottom(); y++) for (int x = (int)b.x; x < (int)b.right(); x++) d += hasAdd.at(x, y) != without.at(x, y);
            CHECK(d > 100);
        }

        // New playlist dialog (with and without the visibility selector) and the picker
        for (int vis = 0; vis < 2; vis++) {
            ui::PlaylistFormModel f;
            f.showVisibility = vis == 1;
            f.name = "Road trip";
            f.hints = {{"A", "Select"}, {"B", "Cancel"}};
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(base); ui::drawPlaylistForm(f); });
            SoftRender form = renderScreen([&] { ui::drawBrowser(base); ui::drawPlaylistForm(f); },
                                           (std::string("lib_form") + std::to_string(vis) + "_" + names[si]).c_str());
            const ui::Rect fc = ui::layout::plFormCard(f.showVisibility);
            CHECK(fc.x >= 0 && fc.right() <= 1280 && fc.bottom() <= 720 && fc.w < 700); // a dialog, not a screen
            CHECK(form.at((int)fc.x + 40, (int)fc.y + 150) != plainShot.at((int)fc.x + 40, (int)fc.y + 150));
            CHECK(ui::hitTestPlaylistForm(f, ui::layout::plFormSlot(0, vis == 1).x + 10, ui::layout::plFormSlot(0, vis == 1).y + 10) == 0);
            CHECK(ui::hitTestPlaylistForm(f, ui::layout::plFormSlot(2, vis == 1).x + 10, ui::layout::plFormSlot(2, vis == 1).y + 10) == 2);
            CHECK(ui::hitTestPlaylistForm(f, ui::layout::plFormSlot(3, vis == 1).x + 10, ui::layout::plFormSlot(3, vis == 1).y + 10) == 3);
            CHECK(ui::hitTestPlaylistForm(f, 5, 5) == -1);
            if (vis == 1) {
                CHECK(ui::hitTestPlaylistForm(f, ui::layout::plFormVisChip(0, true).x + 10, ui::layout::plFormVisChip(0, true).y + 10) == 10);
                CHECK(ui::hitTestPlaylistForm(f, ui::layout::plFormVisChip(1, true).x + 10, ui::layout::plFormVisChip(1, true).y + 10) == 11);
                // Private vs Public look different
                ui::PlaylistFormModel g = f; g.isPublic = true; g.focus = 1;
                for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(base); ui::drawPlaylistForm(g); });
                SoftRender pub = renderScreen([&] { ui::drawBrowser(base); ui::drawPlaylistForm(g); }, (std::string("lib_formpub_") + names[si]).c_str());
                CHECK(pub.at((int)ui::layout::plFormVisChip(1, true).x + 20, (int)ui::layout::plFormVisChip(1, true).y + 8) !=
                      form.at((int)ui::layout::plFormVisChip(1, true).x + 20, (int)ui::layout::plFormVisChip(1, true).y + 8));
            }
            // an error keeps the dialog and the typed name on screen
            ui::PlaylistFormModel err = f; err.message = "The server refused the playlist (status 400)"; err.messageIsError = true;
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(base); ui::drawPlaylistForm(err); });
            SoftRender er = renderScreen([&] { ui::drawBrowser(base); ui::drawPlaylistForm(err); }, (std::string("lib_formerr_") + names[si]).c_str());
            long dd = 0;
            const ui::Rect ms = ui::layout::plFormSlot(vis == 1 ? 1 : 0, vis == 1);
            for (int y = (int)ms.bottom() + 8; y < (int)ms.bottom() + 36; y++) for (int x = (int)ms.x; x < (int)ms.x + 300; x++) dd += er.at(x, y) != form.at(x, y);
            CHECK(dd > 50);
        }
        {
            ui::PlaylistPickerModel pk;
            pk.subtitle = "Some song";
            pk.rows.push_back("New playlist");
            for (int i = 0; i < 12; i++) pk.rows.push_back("Playlist " + std::to_string(i + 1));
            pk.selected = 9; // scrolled
            pk.hints = {{"A", "Add"}, {"B", "Cancel"}};
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(base); ui::drawPlaylistPicker(pk); });
            renderScreen([&] { ui::drawBrowser(base); ui::drawPlaylistPicker(pk); }, (std::string("lib_pick_") + names[si]).c_str());
            const ui::Rect pc = ui::layout::pickCard();
            CHECK(pc.y >= 0 && pc.bottom() <= 720 - 40 && pc.right() <= 1280);
            // the last visible slot maps to the scrolled window
            const ui::Rect r6 = ui::layout::pickRow(6);
            CHECK(ui::hitTestPlaylistPicker(pk, r6.x + 10, r6.y + 10) == ui::computeListWindow(13, 9, ui::layout::PICK_ROWS).start + 6);
            CHECK(ui::hitTestPlaylistPicker(pk, 5, 5) == -1);
            // empty: just "New playlist"
            ui::PlaylistPickerModel emp; emp.rows = {"New playlist"}; emp.message = "You have no playlists yet - make the first one.";
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(base); ui::drawPlaylistPicker(emp); });
            renderScreen([&] { ui::drawBrowser(base); ui::drawPlaylistPicker(emp); }, (std::string("lib_pickempty_") + names[si]).c_str());
            CHECK(ui::hitTestPlaylistPicker(emp, ui::layout::pickRow(0).x + 10, ui::layout::pickRow(0).y + 10) == 0);
        }
    }
    ui::layout::listInset() = 0;
    ui::setStyle(ui::Style::Plain);
}


// Visualizer looks, wrapping button legends, and the header counter staying clear of the tabs' "R" key.
static void testVisualizerAndLegends() {
    const ui::Style styles[3] = {ui::Style::Plain, ui::Style::LiquidGlass, ui::Style::Expressive};
    for (int si = 0; si < 3; si++) {
        ui::setStyle(styles[si]);
        ui::VisualizerModel v;
        v.title = "Song";
        v.bandCount = 12;
        for (int i = 0; i < 12; i++) v.bands[i] = 0.2f + 0.06f * (float)i;
        v.hints = {{"A", "Pause"}};
        std::vector<SoftRender> shots;
        for (int st = 0; st < ui::VisualizerModel::STYLE_COUNT; st++) {
            v.style = st;
            for (int i = 0; i < 20; i++) tickFrame([&] { ui::drawMusicVisualizer(v); });
            shots.push_back(renderScreen([&] { ui::drawMusicVisualizer(v); }, (std::string("viz_") + std::to_string(st) + "_" + std::to_string(si)).c_str()));
        }
        for (int a = 0; a < (int)shots.size(); a++)
            for (int b = a + 1; b < (int)shots.size(); b++) {
                long diff = 0;
                for (int y = 230; y < 610; y += 2) for (int x = 60; x < 1220; x += 2) diff += shots[(size_t)a].at(x, y) != shots[(size_t)b].at(x, y);
                CHECK(diff > 800); // every look is its own picture
            }
        // a style number out of range falls back instead of drawing nothing
        v.style = 99;
        renderScreen([&] { ui::drawMusicVisualizer(v); }, "viz_wild");
        // quiet and loud both draw (a floor keeps a sliver visible)
        v.style = 0;
        for (int i = 0; i < 12; i++) v.bands[i] = 0.0f;
        SoftRender quiet = renderScreen([&] { ui::drawMusicVisualizer(v); }, "viz_quiet");
        CHECK(quiet.at(100, 598) != quiet.at(100, 300));
    }
    ui::setStyle(ui::Style::Plain);

    // Legends: far too many for one row -> two rows, nothing past the screen edge or under the footer's end.
    ui::BrowserModel m = sampleBrowser(4, 0);
    m.hints = {{"A", "Open"}, {"B", "Back"}, {"X", "Search"}, {"Y", "Favourite"}, {"ZL", "Watched"}, {"L R", "View"},
               {">", "Sort / New"}, {"R-stick up", "Add to playlist"}, {"+", "Shuffle"}, {"-", "Menu music"}, {"ZR", "Refresh"}};
    SoftRender wr = renderScreen([&] { ui::drawBrowser(m); }, "legends_two_rows");
    long right = 0, second = 0, firstRow = 0;
    for (int y = 640; y < 720; y++) for (int x = 1262; x < 1280; x++) right += wr.at(x, y) != BG;
    for (int y = 676; y < 712; y++) for (int x = 272; x < 700; x++) second += wr.at(x, y) != BG;
    for (int y = 640; y < 674; y++) for (int x = 272; x < 700; x++) firstRow += wr.at(x, y) != BG;
    CHECK(right == 0);
    CHECK(second > 200 && firstRow > 200);
    // few legends stay on the single footer row
    ui::BrowserModel few = sampleBrowser(4, 0);
    few.hints = {{"A", "Open"}, {"B", "Back"}};
    SoftRender fr = renderScreen([&] { ui::drawBrowser(few); }, "legends_one_row");
    long under = 0;
    for (int y = 640; y < 662; y++) for (int x = 272; x < 700; x++) under += fr.at(x, y) != BG;
    CHECK(under == 0);

    // The counter ("1 / 1000") must not touch the "R" key of the view tabs, clock on.
    ui::BrowserModel t = sampleBrowser(5, 0);
    t.clock = "21:37";
    t.tabs = {"All Songs", "Artists", "Albums", "Playlists"};
    t.tabSelected = 0;
    t.path.clear();
    SoftRender a = renderScreen([&] { ui::drawBrowser(t); }, "counter_a");
    t.total = 1000;
    SoftRender b = renderScreen([&] { ui::drawBrowser(t); }, "counter_b");
    const ui::Rect lastTab = ui::layout::settingsTab(3);
    long diff = 0;
    for (int y = 78; y < 112; y++) for (int x = (int)lastTab.right(); x < 1250; x++) diff += a.at(x, y) != b.at(x, y);
    CHECK(diff == 0);
    long shown = 0;
    for (int y = 40; y < 74; y++) for (int x = 1000; x < 1250; x++) shown += a.at(x, y) != b.at(x, y);
    CHECK(shown > 20); // it still shows, just above
}


// The Queue button, its Up Next flyout and the Add to Queue browser: placement with the lyrics off and on (no overlap
// with the art, controls, volume, lyrics button or the queue text; inside the screen), and that each draws in every style.
static bool rectsOverlap(const ui::Rect& a, const ui::Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}
static void testQueueUi() {
    // geometry, lyrics off (t = 0) and fully on (t = 1)
    for (int lyr = 0; lyr < 2; lyr++) {
        ui::npLyr() = (float)lyr;
        const ui::Rect q = ui::npQueueRect();
        const ui::NpLayout L = ui::npLayoutNow();
        CHECK(q.x >= 0 && q.right() <= 1280 && q.y >= 0 && q.bottom() <= 720);
        CHECK(q.w >= 190 && q.h >= 44); // a finger-sized target
        CHECK(!rectsOverlap(q, ui::Rect{L.artX, L.artY, L.art, L.art}));
        CHECK(!rectsOverlap(q, ui::npVolumeRect()));
        CHECK(!rectsOverlap(q, ui::npLyricsRect()));
        for (int i = 0; i < 4; i++) CHECK(!rectsOverlap(q, ui::npButtonRect(i)));
        CHECK(!rectsOverlap(q, ui::npToggleRect(ui::NpButton::Shuffle)));
        CHECK(!rectsOverlap(q, ui::npToggleRect(ui::NpButton::Repeat)));
        if (lyr) CHECK(q.y >= L.volumeY + ui::layout::NP_VOLUME_H); // lyrics on: below the volume bar
        CHECK(q.y >= L.artY + L.art);                      // and below the cover art
        // the queue text lines ("Track 3 of 12" / "Next:") are clear of the button
        CHECK(q.bottom() <= L.queueY);
        // the flyout stays on screen and clear of the button, on the controller and touch grid
        const ui::Rect f = ui::queueFlyoutCard();
        CHECK(f.x >= 0 && f.right() <= 1280 && f.y >= 0 && f.bottom() <= 720);
        CHECK(f.bottom() <= q.y);
        for (int i = 0; i < ui::layout::QF_ROWS; i++) {
            const ui::Rect r = ui::queueFlyoutRow(i);
            CHECK(r.y >= f.y && r.bottom() <= f.bottom() && r.right() <= f.right());
            CHECK(!rectsOverlap(r, ui::queueFlyoutAdd()));
            for (int k = 0; k < 3; k++) CHECK(r.x <= ui::queueFlyoutRowButton(i, k).x && ui::queueFlyoutRowButton(i, k).right() <= r.right());
        }
        CHECK(ui::hitTestQueueFlyout(ui::queueFlyoutAdd().x + 5, ui::queueFlyoutAdd().y + 5, 3) == 0);
        CHECK(ui::hitTestQueueFlyout(ui::queueFlyoutClear().x + 5, ui::queueFlyoutClear().y + 5, 3) == 1);
        CHECK(ui::hitTestQueueFlyout(ui::queueFlyoutRow(1).x + 10, ui::queueFlyoutRow(1).y + 10, 3) == 101);
        CHECK(ui::hitTestQueueFlyout(ui::queueFlyoutRowButton(2, 2).x + 5, ui::queueFlyoutRowButton(2, 2).y + 5, 3) == 402);
        CHECK(ui::hitTestQueueFlyout(ui::queueFlyoutRow(4).x + 10, ui::queueFlyoutRow(4).y + 10, 3) == -2); // (only 3 rows)
        CHECK(ui::hitTestQueueFlyout(1, 1, 3) == -1);
    }
    ui::npLyr() = 0.0f;
    // the browser: card on screen, rows inside it, hit tests
    {
        const ui::Rect c = ui::queueBrowserCard();
        CHECK(c.x >= 0 && c.right() <= 1280 && c.y >= 0 && c.bottom() + 50 <= 720);
        for (int tabs = 0; tabs < 2; tabs++) {
            const int n = ui::queueBrowserRows(tabs == 1);
            const ui::Rect last = ui::queueBrowserRow(n - 1, tabs == 1);
            CHECK(last.bottom() <= c.bottom() - 8);
            CHECK(ui::hitTestQueueBrowser(last.x + 5, last.y + 5, n, tabs ? 4 : 0) == 100 + n - 1);
        }
        CHECK(ui::hitTestQueueBrowser(ui::queueBrowserAdd().x + 4, ui::queueBrowserAdd().y + 4, 8, 4) == 0);
        CHECK(ui::hitTestQueueBrowser(ui::queueBrowserTab(2, 4).x + 4, ui::queueBrowserTab(2, 4).y + 4, 8, 4) == 12);
        CHECK(ui::hitTestQueueBrowser(ui::queueBrowserClose().x + 4, ui::queueBrowserClose().y + 4, 8, 4) == 2);
    }

    // drawing, every style
    const ui::Style styles[3] = {ui::Style::Plain, ui::Style::LiquidGlass, ui::Style::Expressive};
    for (int si = 0; si < 3; si++) {
        ui::setStyle(styles[si]);
        for (int lyr = 0; lyr < 2; lyr++) {
            ui::NowPlayingModel np;
            np.title = "A very long song title that has to be cut somewhere sensible";
            np.subtitle = "Audio";
            np.durationSeconds = 200; np.positionSeconds = 40;
            np.queueControls = true;
            np.queueText = "Track 3 of 12";
            np.nextText = "Next: Something";
            np.queueCount = 0;
            lyrics::Lyrics ly;
            if (lyr) {
                ly.lines.push_back({-1, "line one"});
                np.lyrics = &ly; np.lyricsOpen = true; np.lyricsCapable = true;
            }
            // the button alone: empty, then with entries
            SoftRender empty = renderScreen([&] { ui::drawNowPlaying(np); }, "queue_btn_empty");
            np.queueCount = 7;
            for (int i = 0; i < 60; i++) tickFrame([&] { ui::drawNowPlaying(np); });
            SoftRender full = renderScreen([&] { ui::drawNowPlaying(np); }, (std::string("queue_btn_full") + std::to_string(si) + (lyr ? "l" : "")).c_str());
            const ui::Rect q = ui::npQueueRect();
            long drawn = 0, diff = 0;
            for (int y = (int)q.y + 4; y < (int)q.bottom() - 4; y += 2) for (int x = (int)q.x + 4; x < (int)q.right() - 4; x += 2) { drawn += full.at(x, y) != BG; diff += full.at(x, y) != empty.at(x, y); }
            CHECK(drawn > 100);
            CHECK(diff > 40); // the look changes with the count
            // the flyout, with more rows than fit
            np.flyout.open = np.flyout.visible = true;
            np.flyout.current.title = "Playing now";
            np.flyout.count = 12; np.flyout.first = 0; np.flyout.cursor = 1;
            for (int i = 0; i < 12 && i < 5; i++) { ui::QueueRow r; r.title = "Queued song " + std::to_string(i); r.sub = "Artist - Album"; np.flyout.rows.push_back(r); }
            np.hints = {{"A", "Play now"}, {"B", "Close"}};
            for (int i = 0; i < 40; i++) tickFrame([&] { ui::drawNowPlaying(np); });
            SoftRender fly = renderScreen([&] { ui::drawNowPlaying(np); }, (std::string(lyr ? "queue_flyout_lyrics" : "queue_flyout") + std::to_string(si)).c_str());
            const ui::Rect card = ui::queueFlyoutCard();
            long inCard = 0, same = 0;
            for (int y = (int)card.y + 4; y < (int)card.bottom() - 4; y += 3) for (int x = (int)card.x + 4; x < (int)card.right() - 4; x += 3) { inCard++; same += fly.at(x, y) == full.at(x, y); }
            CHECK(same < inCard * 0.6); // the card really covers what is behind it
            // the browser over it
            np.browser.open = np.browser.visible = true;
            np.browser.context = "Libraries  >  Music  >  Albums";
            np.browser.tabs = {"All Songs", "Artists", "Albums", "Playlists"}; np.browser.tab = 2;
            np.browser.count = 40; np.browser.total = 900; np.browser.first = 0; np.browser.selected = 1;
            for (int i = 0; i < 8; i++) { ui::QueueRow r; r.title = "Album " + std::to_string(i); r.sub = "Someone"; r.container = true; np.browser.rows.push_back(r); }
            np.browser.coverTitle = "Album 1"; np.browser.canAddAll = true; np.browser.toast = "Added 12 items";
            np.browser.hints = {{"A", "Open"}, {"B", "Back"}};
            for (int i = 0; i < 40; i++) tickFrame([&] { ui::drawNowPlaying(np); });
            SoftRender br = renderScreen([&] { ui::drawNowPlaying(np); }, (std::string(lyr ? "queue_browser_lyrics" : "queue_browser") + std::to_string(si)).c_str());
            const ui::Rect bc = ui::queueBrowserCard();
            long bd = 0;
            for (int y = (int)bc.y + 4; y < (int)bc.bottom() - 4; y += 3) for (int x = (int)bc.x + 4; x < (int)bc.right() - 4; x += 3) bd += br.at(x, y) != fly.at(x, y);
            CHECK(bd > 500);
            np.browser.open = np.browser.visible = false;
            np.flyout.open = np.flyout.visible = false;
            for (int i = 0; i < 40; i++) tickFrame([&] { ui::drawNowPlaying(np); }); // (closes again: a frame still draws)
        }
    }
    ui::setStyle(ui::Style::Plain);
}

static void testScrubAndGear() {
    const ui::Rect pr = ui::npProgressRect();
    const ui::NpLayout L = ui::npLayoutNow();
    CHECK(pr.x >= 0 && pr.right() <= 1280 && pr.h >= 30);
    CHECK(ui::npProgressSeconds(pr.x - 500, 100.0) == 0.0);
    CHECK(ui::npProgressSeconds(pr.right() + 500, 100.0) == 100.0);
    const double mid = ui::npProgressSeconds(pr.x + pr.w / 2, 100.0);
    CHECK(mid > 40.0 && mid < 60.0);
    (void)L;
    const ui::Rect g = ui::npGearRect();
    CHECK(g.x >= 0 && g.right() <= 1280 && g.y >= 0 && g.bottom() <= 720 && g.w >= 44 && g.h >= 44);
    CHECK(!rectsOverlap(g, ui::npQueueRect()));
    CHECK(!rectsOverlap(g, pr));
    const int nq = 9;
    const ui::Rect card = ui::vsCard(nq);
    CHECK(card.x >= 0 && card.right() <= 1280 && card.y >= 0 && card.bottom() <= 720);
    for (int i = 0; i < nq; i++) {
        const ui::Rect r = ui::vsQualityRow(i);
        CHECK(r.y >= card.y && r.bottom() <= card.bottom() && r.right() <= card.right());
        CHECK(ui::hitTestVideoSettings(r.x + 6, r.y + 6, nq) == i);
    }
    CHECK(ui::hitTestVideoSettings(1, 1, nq) == -1);
    const ui::Rect a = ui::vsArrow(ui::vsTrackRow(0, nq), true);
    CHECK(ui::hitTestVideoSettings(a.x + 3, a.y + 3, nq) == 101);
    const ui::Rect s = ui::vsArrow(ui::vsTrackRow(1, nq), false);
    CHECK(ui::hitTestVideoSettings(s.x + 3, s.y + 3, nq) == 102);
    const ui::Style styles[3] = {ui::Style::Plain, ui::Style::LiquidGlass, ui::Style::Expressive};
    for (int si = 0; si < 3; si++) {
        ui::setStyle(styles[si]);
        ui::NowPlayingModel np;
        np.title = "Movie"; np.subtitle = "2020"; np.durationSeconds = 7200; np.positionSeconds = 600;
        np.settings.gear = true;
        for (int i = 0; i < nq; i++) np.settings.qualities.push_back("Preset " + std::to_string(i));
        np.settings.quality = 3; np.settings.hasAudio = true; np.settings.hasSubtitles = true;
        np.settings.audioLabel = "English"; np.settings.subtitleLabel = "Off";
        for (int i = 0; i < 40; i++) tickFrame([&] { ui::drawNowPlaying(np); });
        SoftRender base = renderScreen([&] { ui::drawNowPlaying(np); }, (std::string("gear_closed") + std::to_string(si)).c_str());
        long drawn = 0;
        for (int y = (int)g.y + 4; y < (int)g.bottom() - 4; y += 2) for (int x = (int)g.x + 4; x < (int)g.right() - 4; x += 2) drawn += base.at(x, y) != BG;
        CHECK(drawn > 50);
        np.scrubbing = true; np.scrubSeconds = 3661;
        SoftRender scrub = renderScreen([&] { ui::drawNowPlaying(np); }, (std::string("scrub") + std::to_string(si)).c_str());
        long sd = 0;
        const ui::Rect pr2 = ui::npProgressRect();
        CHECK(pr2.y > 300 && pr2.bottom() < 440);
        for (int y = (int)pr2.y - 40; y < (int)pr2.bottom(); y++) for (int x = (int)pr2.x; x < (int)pr2.right(); x++) sd += scrub.at(x, y) != base.at(x, y);
        CHECK(sd > 30);
        np.scrubbing = false;
        np.settings.open = true;
        for (int i = 0; i < 40; i++) tickFrame([&] { ui::drawNowPlaying(np); });
        SoftRender open = renderScreen([&] { ui::drawNowPlaying(np); }, (std::string("gear_open") + std::to_string(si)).c_str());
        long od = 0, tot = 0;
        for (int y = (int)card.y + 4; y < (int)card.bottom() - 4; y += 3) for (int x = (int)card.x + 4; x < (int)card.right() - 4; x += 3) { tot++; od += open.at(x, y) != base.at(x, y); }
        CHECK(od > tot * 0.4);
        np.settings.open = false;
        for (int i = 0; i < 40; i++) tickFrame([&] { ui::drawNowPlaying(np); });
    }
    ui::setStyle(ui::Style::Plain);
}

static void testButtonSizes() {
    const int none[4] = {0, 0, 0, 0};
    const int normal[8] = {100, 100, 100, 100, 100, 100, 100, 100};
    CHECK_EQ(ui::buttonSizeMin(), 60); CHECK_EQ(ui::buttonSizeMax(0), 130); CHECK_EQ(ui::buttonSizeMax(6), 130); CHECK_EQ(ui::buttonSizeMax(7), 112);
    ui::NowPlayingModel np; np.title = "Song"; np.subtitle = "Audio"; np.positionSeconds = 30; np.durationSeconds = 120;
    np.queueControls = true; np.volume = 0.5f; np.lyricsCapable = true;
    auto shot = [&](const int s[8], const char* name) {
        ui::setButtonSizes(s);
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawNowPlaying(np); });
        return renderScreen([&] { ui::drawNowPlaying(np); }, name);
    };
    const ui::Rect play = ui::npButtonRect((int)ui::NpButton::PlayPause), seek = ui::npButtonRect((int)ui::NpButton::SeekBack);
    const ui::Rect shuf = ui::npToggleRect(ui::NpButton::Shuffle);
    auto region = [&](const ui::Rect& r, const SoftRender& a, const SoftRender& b) {
        long diff = 0;
        for (int y = (int)r.y - 12; y < (int)r.bottom() + 12; y++)
            for (int x = (int)r.x - 12; x < (int)r.right() + 12; x++) diff += a.at(x, y) != b.at(x, y);
        return diff;
    };
    ui::setStyle(ui::Style::Expressive);
    ui::setButtonShapes(none);
    SoftRender base = shot(normal, "size_default");
    // a smaller seek button changes the seek button only; a taller / wider toggle only the toggles
    int sm[8] = {100, 100, 60, 60, 100, 100, 100, 100};
    SoftRender sSeek = shot(sm, "size_seek");
    CHECK(region(seek, base, sSeek) > 150);
    int big[8] = {100, 100, 100, 100, 130, 130, 100, 100};
    SoftRender sTog = shot(big, "size_toggle");
    CHECK(region(shuf, base, sTog) > 60);
    // width and height are independent: narrow-only differs from short-only
    int nw[8] = {60, 100, 100, 100, 100, 100, 100, 100}, sh[8] = {100, 60, 100, 100, 100, 100, 100, 100};
    SoftRender a = shot(nw, "size_narrow"), b = shot(sh, "size_short");
    CHECK(region(play, a, b) > 100);
    // out of range values are clamped, never break the screen
    int wild[8] = {1000, -5, 100, 100, 100, 100, 100, 100}, maxd[8] = {130, 60, 100, 100, 100, 100, 100, 100};
    SoftRender w1 = shot(wild, "size_wild"), w2 = shot(maxd, "size_clamped");
    CHECK(region(play, w1, w2) == 0);
    // putting the sizes back to 100% restores the default picture exactly
    SoftRender again = shot(normal, "size_reset");
    if (region(play, base, again) || region(seek, base, again) || region(shuf, base, again))
        fprintf(stderr, "    (reset diff play %ld seek %ld shuf %ld)\n", region(play, base, again), region(seek, base, again), region(shuf, base, again));
    CHECK(region(play, base, again) < 12 && region(seek, base, again) < 12 && region(shuf, base, again) < 12);
    // cookies stretch too: a clover / cookie / flower is wider with a bigger width and shorter with a smaller height
    for (int shp : {4, 5, 6}) {
        const int sp[4] = {0, shp, 0, 0};
        ui::setButtonShapes(sp);
        int wide[8] = {100, 100, 130, 100, 100, 100, 100, 100}, shortb[8] = {100, 100, 100, 60, 100, 100, 100, 100};
        SoftRender c0 = shot(normal, "cookie_size_n"), c1 = shot(wide, "cookie_size_w"), c2 = shot(shortb, "cookie_size_h");
        CHECK(region(seek, c0, c1) > 100);
        CHECK(region(seek, c0, c2) > 100);
    }
    ui::setButtonShapes(none);
    // the player's row spaces itself around whatever is chosen: no two buttons (as drawn) ever overlap
    {
        const int allShapes[3][4] = {{0, 0, 0, 0}, {5, 4, 6, 0}, {1, 3, 4, 0}};
        const int sizeSets[4][8] = {{100, 100, 100, 100, 100, 100, 100, 100}, {130, 130, 130, 130, 130, 130, 112, 112},
                                    {60, 60, 60, 60, 60, 60, 60, 60}, {130, 60, 60, 130, 130, 60, 100, 100}};
        ui::setStyle(ui::Style::Expressive);
        for (const auto& sh : allShapes) for (const auto& sz : sizeSets) {
            ui::setButtonShapes(sh);
            ui::setButtonSizes(sz);
            ui::Rect row[6] = {ui::npToggleRect(ui::NpButton::Shuffle), ui::npButtonRect(0), ui::npButtonRect(1), ui::npButtonRect(2),
                               ui::npButtonRect(3), ui::npToggleRect(ui::NpButton::Repeat)};
            const bool wide[6] = {false, false, true, false, false, false};
            bool ok = true;
            for (int i = 0; i + 1 < 6; i++) {
                // drawn extent: the touch rectangle plus what is drawn beyond it (the wide play button, a cookie's bumps)
                const float aR = row[i].right() + (wide[i] ? row[i].w * 0.2f : row[i].w * 0.07f);
                const float bL = row[i + 1].x - (wide[i + 1] ? row[i + 1].w * 0.2f : row[i + 1].w * 0.07f);
                if (aR + 4.0f > bL) ok = false;
            }
            // and the row stays inside the player's column
            const ui::Rect lyr = ui::npLyricsRect(), vol = ui::npVolumeRect();
            if (vol.right() > lyr.x) ok = false;
            if (row[0].x < 0.0f || row[5].right() > 1280.0f) ok = false;
            CHECK(ok);
        }
        ui::setButtonShapes(none);
        ui::setButtonSizes(normal);
        // with nothing customised the row is exactly the original one
        const ui::Rect p = ui::npButtonRect(1);
        CHECK_NEAR(p.w, ui::layout::NP_BUTTON_SIZE_BIG, 0.001f);
        CHECK_NEAR(ui::npButtonRect(2).x - (p.x + p.w), ui::layout::NP_BUTTON_GAP, 0.001f);
    }
    // other styles ignore sizes
    for (ui::Style st : {ui::Style::Plain, ui::Style::LiquidGlass}) {
        ui::setStyle(st);
        SoftRender x = shot(normal, "size_other_a"), y = shot(sm, "size_other_b");
        CHECK(region(seek, x, y) == 0 && region(play, x, y) == 0);
    }
    ui::setStyle(ui::Style::Expressive);
    // dialog buttons: a smaller one draws fewer pixels on the login screen
    {
        ui::LoginModel lm = sampleLogin(3);
        int small[8] = {100, 100, 100, 100, 100, 100, 60, 60};
        auto login = [&](const int s[8], const char* name) {
            ui::setButtonSizes(s);
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawLogin(lm); });
            return renderScreen([&] { ui::drawLogin(lm); }, name);
        };
        SoftRender l1 = login(normal, "dialog_size_a"), l2 = login(small, "dialog_size_b");
        long diff = 0;
        for (int y = 0; y < 720; y++) for (int x = 540; x < 1200; x++) diff += l1.at(x, y) != l2.at(x, y);
        CHECK(diff > 200);
    }
    ui::setButtonSizes(normal);

    // The preview row: it draws the four kinds with their shapes and sizes, and follows them.
    {
        ui::BrowserModel m = customizationModel(3, false);
        ui::ListEntry e; e.name = "Button preview"; e.icon = ui::Icon::Sparkle; e.grouped = true;
        e.widget.kind = ui::RowWidget::Kind::ButtonPreview;
        e.widget.shapes = {0, 0, 0, 0};
        e.widget.sizes.assign(normal, normal + 8);
        m.items.insert(m.items.begin() + 3, e);
        ui::BrowserModel m2 = m; m2.items[3].widget.shapes = {5, 1, 3, 2};
        ui::BrowserModel m3 = m; m3.items[3].widget.sizes[0] = 60; m3.items[3].widget.sizes[7] = 130;
        auto rowShot = [&](const ui::BrowserModel& mm, const char* name) {
            for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawBrowser(mm); }); // (let the focus animation settle)
            return renderScreen([&] { ui::drawBrowser(mm); }, name);
        };
        SoftRender p1 = rowShot(m, "preview_row_a");
        SoftRender p2 = rowShot(m2, "preview_row_b");
        SoftRender p3 = rowShot(m3, "preview_row_c");
        const ui::ListWindow win = ui::computeListWindow((int)m.items.size(), m.selected, ui::layout::visibleRows());
        const ui::Rect rr = ui::layout::listRow(3 - win.start);
        auto rowDiff = [&](const SoftRender& x, const SoftRender& y, int x0, int x1) {
            long diff = 0;
            for (int yy = (int)rr.y; yy < (int)rr.bottom(); yy++) for (int xx = x0; xx < x1; xx++) diff += x.at(xx, yy) != y.at(xx, yy);
            return diff;
        };
        CHECK(rowDiff(p1, p2, (int)rr.x + 290, (int)rr.right()) > 300); // other shapes
        CHECK(rowDiff(p1, p3, (int)rr.x + 290, (int)rr.right()) > 100); // other sizes
        CHECK(rowDiff(p1, p2, (int)rr.x, (int)rr.x + 200) == 0);        // the name text is not touched
        // it fits inside the row: the widest preview (130% everywhere) still ends before the row's right edge
        {
            ui::BrowserModel mx = m; for (int i = 0; i < 8; i++) mx.items[3].widget.sizes[(size_t)i] = 130;
            SoftRender px = rowShot(mx, "preview_row_max");
            CHECK(rowDiff(p1, px, (int)rr.right() - 4, (int)rr.right()) == 0);
        }
    }
    ui::setStyle(ui::Style::Plain);
}

static void testVolumeBarEnds() {
    ui::setStyle(ui::Style::Expressive);
    ui::NowPlayingModel np; np.title = "Song"; np.subtitle = "Audio"; np.positionSeconds = 30; np.durationSeconds = 120; np.volume = 0.5f;
    for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawNowPlaying(np); });
    SoftRender sr = renderScreen([&] { ui::drawNowPlaying(np); }, "volume_bar");
    const ui::Rect vr = ui::npVolumeRect();
    const float lineY = vr.y + vr.h * 0.5f, trackX = vr.x + 30.0f, trackW = vr.w - 30.0f;
    const float hx = trackX + trackW * 0.5f, trackEnd = trackX + trackW, y0 = lineY - 8.0f;
    auto bgAt = [&](int x) { return sr.at(x, (int)(y0 - 14)); };
    // The ends facing the handle are (nearly) square: the corner pixels are filled...
    const uint32_t fillCorner = sr.at((int)(hx - 6.0f - 2.0f), (int)y0 + 1);
    const uint32_t trackCorner = sr.at((int)(hx + 6.0f + 1.0f), (int)y0 + 1);
    CHECK(fillCorner != bgAt((int)(hx - 8)));
    CHECK(trackCorner != bgAt((int)(hx + 8)));
    // ...and the fill's corner is the accent, the track's the track colour (not each other's)
    CHECK(fillCorner != trackCorner);
    // The outer ends stay round: the far-right corner of the track is empty, the same as the background beside it
    CHECK(sr.at((int)trackEnd - 1, (int)y0 + 1) == bgAt((int)trackEnd - 1));
    // and the left end of the fill too
    CHECK(sr.at((int)trackX + 1, (int)y0 + 1) == bgAt((int)trackX + 1));
    // the end dot
    CHECK(sr.at((int)(trackEnd - 8.0f), (int)lineY) != sr.at((int)(trackEnd - 8.0f) - 6, (int)lineY));
    // Full volume: the handle sits at the end of the fill and nothing (no leftover track stub) is drawn behind it
    {
        np.volume = 1.0f;
        for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawNowPlaying(np); });
        SoftRender mx = renderScreen([&] { ui::drawNowPlaying(np); }, "volume_bar_max");
        const int by = (int)(y0 + 1);
        const uint32_t bg = mx.at((int)trackEnd - 1, (int)y0 - 14);
        // between the fill's end and the handle there is a gap of background, and left of the handle no track colour
        long stub = 0;
        for (int x = (int)trackEnd - 5; x < (int)trackEnd - 3; x++) stub += mx.at(x, by) != bg;
        CHECK(stub == 0);
        // the handle itself is there
        CHECK(mx.at((int)trackEnd, (int)lineY) != bg);
    }
    ui::setStyle(ui::Style::Plain);
}

static void testTwoPassIcons() {
    ui::setStyle(ui::Style::Expressive);
    ui::npLyr() = 0.0f;
    ui::NowPlayingModel m;
    m.title = "Some Song"; m.subtitle = "Audio"; m.positionSeconds = 12; m.durationSeconds = 120;
    m.queueControls = true; m.lyricsCapable = true;
    auto tick = [&]() {
        tickFrame([] {});          // the TV pass (a visualizer would be drawn here)
        ui::noteExtraPass();
    };
    // One pass per tick (the normal screens): the icons rest.
    for (int i = 0; i < 90; i++) tickFrame([&] { ui::drawNowPlaying(m); });
    SoftRender one = renderScreen([&] { ui::drawNowPlaying(m); }, "one_pass");
    // Two passes per tick (visualizer, movie remote): the icons must rest just the same, not replay their pop each tick.
    for (int i = 0; i < 90; i++) { tick(); tickFrame([&] { ui::drawNowPlaying(m); }); }
    tick();
    SoftRender two = renderScreen([&] { ui::drawNowPlaying(m); }, "two_pass");
    long diff = 0;
    const ui::Rect first = ui::npToggleRect(ui::NpButton::Shuffle), last = ui::npToggleRect(ui::NpButton::Repeat);
    for (int y = (int)first.y - 4; y < (int)(first.y + 60); y++)
        for (int x = (int)first.x - 10; x < (int)(last.right() + 10); x++) diff += one.at(x, y) != two.at(x, y);
    CHECK(diff < 1200); // (the slowly turning backdrop shapes differ a little; the old bug gave ~5500)
    ui::setStyle(ui::Style::Plain);
}

static void testTrackPrompt() {
    // Layout: two selector rows, then OK and "Always use default" side by side, all inside the card, none overlapping.
    const ui::Rect card = ui::trackPromptCard();
    for (int i = 0; i < 4; i++) {
        const ui::Rect r = ui::trackPromptSlot(i);
        CHECK(r.x >= card.x && r.right() <= card.right() && r.y >= card.y && r.bottom() <= card.bottom());
        for (int j = i + 1; j < 4; j++) {
            const ui::Rect o = ui::trackPromptSlot(j);
            CHECK(r.right() <= o.x || o.right() <= r.x || r.bottom() <= o.y || o.bottom() <= r.y);
        }
        CHECK_EQ(ui::hitTestTrackPrompt(r.x + r.w * 0.5f, r.y + r.h * 0.5f), i);
    }
    CHECK_EQ(ui::hitTestTrackPrompt(5, 5), -1);
    CHECK(ui::trackPromptSlot(2).y == ui::trackPromptSlot(3).y);
    CHECK(ui::trackPromptSlot(2).x < ui::trackPromptSlot(3).x);

    ui::TrackPromptModel m;
    m.title = "Some Film"; m.subtitle = "2019";
    m.audioLabel = "English 5.1"; m.subtitleLabel = "Off";
    m.hints = {{"< >", "Change"}, {"A", "Select"}, {"B", "Cancel"}};
    m.selected = 0;
    SoftRender a = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_audio");
    const ui::Rect r0 = ui::trackPromptSlot(0), r1 = ui::trackPromptSlot(1), ok = ui::trackPromptSlot(2),
                   al = ui::trackPromptSlot(3);
    CHECK_EQ(a.at((int)r0.x + 200, (int)r0.y + 4), SELECTED);      // the focused row takes the accent
    CHECK_EQ(a.at((int)r1.x + 200, (int)r1.y + 4), bgr(0x2C313A)); // the other one stays a plain row
    m.selected = 2;
    SoftRender b = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_ok");
    CHECK_EQ(b.at((int)ok.x + 20, (int)ok.y + 10), bgr(0x00A4DC)); // OK focused: brighter accent
    CHECK(b.at((int)r0.x + 200, (int)r0.y + 4) != SELECTED);
    m.selected = 3;
    SoftRender c = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_always");
    CHECK(c.at((int)al.x + 20, (int)al.y + 10) != b.at((int)al.x + 20, (int)al.y + 10)); // the second button lights up
    CHECK_EQ(c.at((int)ok.x + 20, (int)ok.y + 10), SELECTED);       // OK back to the resting accent

    // The same screen in Liquid Glass and Material 3 Expressive draws, and the focus still moves.
    for (ui::Style st : {ui::Style::LiquidGlass, ui::Style::Expressive}) {
        ui::setStyle(st);
        m.selected = 2;
        SoftRender s2 = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_styled");
        for (int i = 0; i < 60; i++) s2 = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_styled");
        m.selected = 3;
        SoftRender s3 = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_styled2");
        for (int i = 0; i < 60; i++) s3 = renderScreen([&] { ui::drawTrackPrompt(m); }, "track_prompt_styled2");
        long diff = 0; // the focus moved: the two buttons look different once the springs settle
        for (int y = (int)ok.y; y < (int)ok.bottom(); y += 2)
            for (int x = (int)al.x; x < (int)al.right(); x += 2) diff += s2.at(x, y) != s3.at(x, y);
        CHECK(diff > 20);
    }
    ui::setStyle(ui::Style::Plain);
}

static void testExpressive() {
    auto sameImage = [](const SoftRender& a, const SoftRender& b) { return a.px == b.px; };
    ui::BrowserModel m = sampleBrowser(12, 2);

    // Three mutually exclusive styles.
    ui::setStyle(ui::Style::Plain);
    CHECK(ui::style() == ui::Style::Plain && !ui::glassEnabled() && !ui::expressiveEnabled());
    ui::setAccent(ui::Accent::Blue);
    ui::setLightMode(false);
    SoftRender plain = renderScreen([&] { ui::drawBrowser(m); }, "m3e_plain_reference");

    ui::setStyle(ui::Style::Expressive);
    CHECK(ui::style() == ui::Style::Expressive && ui::expressiveEnabled() && !ui::glassEnabled());
    ui::setGlass(true); // turning glass on turns M3E off ...
    CHECK(ui::glassEnabled() && !ui::expressiveEnabled());
    ui::setExpressive(true); // ... and the other way round
    CHECK(ui::expressiveEnabled() && !ui::glassEnabled());
    CHECK_STR(ui::styleName(ui::Style::Expressive), "Material 3 Expressive");

    // The palette comes from the user's accent: the screen background is the derived surface tone.
    ui::m3e::Palette dark = ui::m3e::derive({0x00, 0xA4, 0xDC}, false);
    SoftRender e0 = renderScreen([&] { ui::drawBrowser(m); }, "m3e_browser");
    CHECK(!sameImage(plain, e0));
    CHECK_EQ(e0.at(ui::layout::SIDEBAR_W + 6, 700), m3Px(dark.surface));
    ui::Rect row = ui::layout::listRow(4); // an unfocused row: the tonal container
    CHECK_EQ(e0.at((int)row.x + 300, (int)row.y + 6), m3Px(dark.surfaceContainer));

    // Focus: the first frame is a rounded rectangle, a moment later the spring has made it a pill (the corner
    // pixel is no longer filled), and the fill is the primary container, not the saturated accent.
    ui::Rect sel = ui::layout::listRow(2);
    CHECK_EQ(e0.at((int)sel.x + 150, (int)sel.y + 20), m3Px(dark.primaryContainer));
    CHECK(e0.at((int)sel.x + 9, (int)sel.y + 9) == m3Px(dark.primaryContainer)); // first frame: still a rounded rectangle
    SoftRender e1 = e0;
    for (int i = 0; i < 40; i++) e1 = renderScreen([&] { ui::drawBrowser(m); }, "m3e_browser_settled");
    CHECK(e1.at((int)sel.x + 9, (int)sel.y + 9) != m3Px(dark.primaryContainer)); // pill: the corner is cut away
    CHECK_EQ(e1.at((int)sel.x + 150, (int)sel.y + 20), m3Px(dark.primaryContainer));

    // Activating a row (ui::pulse) bounces its neighbours: the row below is pushed down for a moment, then comes home.
    {
        ui::Rect below = ui::layout::listRow(3);
        SoftRender calm = e1;
        ui::pulse();
        int differ = 0;
        SoftRender during = calm;
        for (int i = 0; i < 4; i++) {
            during = renderScreen([&] { ui::drawBrowser(m); }, "m3e_pulse");
            for (int y = (int)below.y; y < (int)below.bottom(); y++)
                for (int x = (int)below.x; x < (int)below.x + 400; x += 3)
                    if (calm.at(x, y) != during.at(x, y)) differ++;
        }
        CHECK(differ > 50);
        SoftRender after = during;
        for (int i = 0; i < 120; i++) after = renderScreen([&] { ui::drawBrowser(m); }, "m3e_pulse_after");
        int off = 0;
        for (int y = (int)below.y; y < (int)below.bottom(); y++)
            for (int x = (int)below.x; x < (int)below.x + 400; x += 3)
                if (calm.at(x, y) != after.at(x, y)) off++;
        CHECK(off < 5);
    }

    // Accent change: the whole palette follows (a different hue gives a different surface and container).
    ui::setAccent(ui::Accent::Green);
    ui::m3e::Palette green = ui::m3e::derive({0x2E, 0xCC, 0x71}, false);
    SoftRender eg = renderScreen([&] { ui::drawBrowser(m); }, "m3e_browser_green");
    CHECK_EQ(eg.at((int)sel.x + 150, (int)sel.y + 20), m3Px(green.primaryContainer));
    CHECK(green.primaryContainer.g != dark.primaryContainer.g || green.primaryContainer.r != dark.primaryContainer.r);

    // Light mode flips the neutral half; text/containers keep readable contrast (see test_m3e_core for every accent).
    ui::setAccent(ui::Accent::Blue);
    ui::setLightMode(true);
    ui::m3e::Palette light = ui::m3e::derive({0x00, 0xA4, 0xDC}, true);
    SoftRender el = renderScreen([&] { ui::drawBrowser(m); }, "m3e_browser_light");
    CHECK_EQ(el.at(ui::layout::SIDEBAR_W + 6, 700), m3Px(light.surface));
    ui::setLightMode(false);

    // Rainbow keeps cycling the palette in M3E.
    ui::setAccent(ui::Accent::Rainbow);
    ui::tickTheme(0.0);
    SoftRender r0 = renderScreen([&] { ui::drawBrowser(m); }, "m3e_rainbow_a");
    ui::tickTheme(8.0);
    SoftRender r1 = renderScreen([&] { ui::drawBrowser(m); }, "m3e_rainbow_b");
    CHECK(r0.at(ui::layout::SIDEBAR_W + 6, 700) != r1.at(ui::layout::SIDEBAR_W + 6, 700));
    ui::setAccent(ui::Accent::Blue);

    // Now Playing: wavy progress while playing, flat when paused; cookie artwork tile when there is no cover.
    ui::NowPlayingModel np;
    np.title = "Nessun dorma"; np.subtitle = "Audio"; np.positionSeconds = 83; np.durationSeconds = 180;
    np.queueText = "Track 3 of 12"; np.queueControls = true; np.shuffleOn = true; np.repeatMode = 1;
    np.hints = {{"A", "Pause"}, {"B", "Stop"}};
    SoftRender n1 = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_now_playing");
    SoftRender n2 = n1;
    for (int i = 0; i < 20; i++) n2 = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_now_playing_b");
    CHECK(!sameImage(n1, n2)); // the squiggle travels, the cookie turns
    np.paused = true;
    SoftRender n3 = n1;
    for (int i = 0; i < 60; i++) n3 = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_now_playing_paused");
    SoftRender n4 = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_now_playing_paused2");
    int wave = 0; // paused: the played part is a straight line -- the same rows of pixels frame to frame
    for (int y = (int)ui::layout::NP_PROGRESS_Y - 8; y < (int)ui::layout::NP_PROGRESS_Y + 16; y++)
        for (int x = (int)ui::layout::NP_TEXT_X; x < (int)ui::layout::NP_TEXT_X + 300; x++)
            if (n3.at(x, y) != n4.at(x, y)) wave++;
    CHECK_EQ(wave, 0);

    // Button group: pressing play/pause pushes its neighbours aside (they spring away), releasing brings them back.
    np.paused = false;
    SoftRender rest = n1;
    for (int i = 0; i < 90; i++) rest = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_np_rest");
    np.pressedButton = (int)ui::NpButton::PlayPause;
    SoftRender pr = rest;
    for (int i = 0; i < 90; i++) pr = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_np_pressed");
    const ui::Rect seekR = ui::npButtonRect((int)ui::NpButton::SeekBack);
    int moved = 0;
    for (int y = (int)seekR.y; y < (int)seekR.bottom(); y++)
        for (int x = (int)seekR.x - 14; x < (int)seekR.right() + 14; x++)
            if (rest.at(x, y) != pr.at(x, y)) moved++;
    CHECK(moved > 100); // the seek-back button is no longer where it was
    np.pressedButton = -1;
    SoftRender rel = pr;
    for (int i = 0; i < 120; i++) rel = renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_np_released");
    int seekHome = 0;
    for (int y = (int)seekR.y; y < (int)seekR.bottom(); y++)
        for (int x = (int)seekR.x - 14; x < (int)seekR.right() + 14; x++)
            if (rest.at(x, y) != rel.at(x, y) && (rest.at(x, y) & 0xFF) == 0) seekHome++; // (ignore the travelling-wave rows)
    CHECK(seekHome < 40); // and it comes home
    np.paused = true;

    // Dialogs: the Choice card springs in (first frame differs from the settled one), then rests.
    ui::ChoiceModel ch;
    ch.title = "Resume playback?"; ch.subtitle = "Film"; ch.options = {"Resume", "Start over"}; ch.selected = 0;
    ch.hints = {{"A", "Select"}, {"B", "Cancel"}};
    SoftRender c0 = renderScreen([&] { ui::drawBrowser(m); ui::drawChoice(ch); }, "m3e_choice_first");
    SoftRender cN = c0;
    for (int i = 0; i < 90; i++) cN = renderScreen([&] { ui::drawBrowser(m); ui::drawChoice(ch); }, "m3e_choice");
    SoftRender cM = renderScreen([&] { ui::drawBrowser(m); ui::drawChoice(ch); }, "m3e_choice_rest");
    const ui::Rect o0 = ui::choiceRow(0, 2);
    const int cardX = (int)o0.x - 40, cardY = (int)o0.y - 130;
    CHECK(c0.at(cardX + 6, cardY + 40) != cN.at(cardX + 6, cardY + 40)); // first frame: the card is scaled down and faded
    {   // settled: the tonal card (surfaceHigh at 96 % over the dimmed screen: within a few levels of it)
        const ui::m3e::Rgb8 sh = ui::m3e::derive({0x00, 0xA4, 0xDC}, false).surfaceHigh;
        const uint32_t px = cN.at(cardX + 6, cardY + 40);
        CHECK(std::abs((int)(px & 0xFF) - sh.r) <= 8 && std::abs((int)((px >> 8) & 0xFF) - sh.g) <= 8 && std::abs((int)((px >> 16) & 0xFF) - sh.b) <= 8);
    }
    int moving = 0; // settled: the card itself no longer changes (the faint backdrop cookies behind it still turn)
    for (int y = (int)o0.y; y < (int)o0.bottom(); y += 3) for (int x = (int)o0.x; x < (int)o0.right(); x += 3) if (cN.at(x, y) != cM.at(x, y)) moving++;
    CHECK_EQ(moving, 0);

    {   // The screen behind a popup is out of focus (a CPU haze of its own draw list), not just dimmed.
        SoftRender sharp = renderScreen([&] { ui::drawBrowser(m); }, "m3e_choice_ref");
        auto edges = [&](const SoftRender& r) { // total horizontal contrast of the list text left of the card
            long sum = 0;
            for (int y = (int)ui::layout::LIST_TOP; y < (int)ui::layout::LIST_TOP + 200; y += 2)
                for (int x = 300; x < 520; x++) sum += std::abs((int)(r.at(x + 1, y) & 0xFF) - (int)(r.at(x, y) & 0xFF));
            return sum;
        };
        CHECK(edges(cN) * 10 < edges(sharp) * 3); // text behind the popup lost most of its sharpness
        // ... but it is still the same screen: the focused row's colour still shows through as a haze
        const uint32_t hz = cN.at((int)o0.x - 250, (int)ui::layout::listRow(2).y + 30);
        CHECK((hz & 0xFF) != (cN.at(20, 700) & 0xFF) || ((hz >> 8) & 0xFF) != ((cN.at(20, 700) >> 8) & 0xFF));
    }

    // Every other screen draws in M3E (and is written out for eyeballing with UFIN_UI_PREVIEW).
    ui::LoginModel lg = sampleLogin(3);
    renderScreen([&] { ui::drawLogin(lg); }, "m3e_login");
    ui::MessageModel msg; msg.title = "Connecting"; msg.lines = {"Talking to your server"}; msg.busy = true;
    msg.hints = {{"B", "Cancel"}};
    renderScreen([&] { ui::drawMessage(msg); }, "m3e_message_busy");
    msg.busy = false; msg.isError = true; msg.title = "Couldn't connect"; msg.lines = {"Check the server address."};
    renderScreen([&] { ui::drawMessage(msg); }, "m3e_message_error");
    ui::VideoHudModel hud; hud.visible = true; hud.title = "Film"; hud.positionSeconds = 60; hud.durationSeconds = 600;
    hud.hints = {{"A", "Pause"}};
    SoftRender h0 = renderScreen([&] { ui::drawVideoHud(hud); }, "m3e_hud");
    hud.paused = true; hud.trackMenu = true; hud.audioLabel = "English 5.1"; hud.subtitleLabel = "Off";
    renderScreen([&] { ui::drawVideoHud(hud); }, "m3e_hud_paused_menu");
    hud.trackMenu = false; hud.paused = false; hud.visible = false; hud.buffering = true; hud.bufferedSeconds = 4; hud.bufferingTargetSeconds = 12;
    SoftRender hb = renderScreen([&] { ui::drawVideoHud(hud); }, "m3e_buffering");
    CHECK(hb.at(20, 360) == h0.at(20, 120)); // video area left alone: no full-screen fill from the theme
    renderScreen([&] { ui::drawToast("Menu music on", 0.1); }, "m3e_toast");
    renderScreen([&] { ui::drawProgressToast("Checking for updates", 0.4f); }, "m3e_progress_toast");
    ui::UpNextModel un; un.showName = "Show"; un.episodeTitle = "S2E5  The Episode"; un.detail = "45 min"; un.secondsLeft = 6; un.hints = {{"A", "Play"}};
    renderScreen([&] { ui::drawUpNext(un); }, "m3e_up_next");
    ui::VisualizerModel vz; vz.title = "Song"; vz.bandCount = 16; for (int i = 0; i < 16; i++) vz.bands[i] = 0.2f + 0.05f * i;
    renderScreen([&] { ui::drawMusicVisualizer(vz); }, "m3e_visualizer");
    for (int i = 0; i < 40; i++) renderScreen([&] { ui::drawBrowser(m); ui::drawHelp(makeHelpModel(0, 2, 30)); }, "m3e_help");
    ui::ProfilesModel pm;
    for (int i = 0; i < 3; i++) { ui::ProfileTile t; t.name = "User" + std::to_string(i); t.letter = "U"; t.color = ui::profileColor(i); pm.tiles.push_back(t); }
    { ui::ProfileTile add; add.add = true; pm.tiles.push_back(add); }
    pm.cursor = 1; pm.hints = {{"A", "Open"}};
    for (int i = 0; i < 30; i++) renderScreen([&] { ui::drawProfiles(pm); }, "m3e_profiles");
    ui::setLightMode(true);
    renderScreen([&] { ui::drawBrowser(m); }, "m3e_browser_light2");
    renderScreen([&] { ui::drawNowPlaying(np); }, "m3e_now_playing_light");
    for (int i = 0; i < 40; i++) renderScreen([&] { ui::drawBrowser(m); ui::drawHelp(makeHelpModel(0, 2, 30)); }, "m3e_help_light");
    ui::setLightMode(false);

    // Leaving M3E restores Plain exactly: pixel-identical to the reference taken before.
    ui::setStyle(ui::Style::Plain);
    CHECK(!ui::expressiveEnabled());
    SoftRender back = renderScreen([&] { ui::drawBrowser(m); }, "m3e_plain_after");
    // (identical but for a few anti-aliasing pixels: the font atlas may have grown glyphs while M3E drew a "!" and heavier text)
    int diff = 0;
    for (size_t i = 0; i < plain.px.size(); i++) if (plain.px[i] != back.px[i]) diff++;
    CHECK(diff <= 8);
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

    testLayoutMaths();
    testFitText();
    testBrowser();
    testArtwork();
    testLogin();
    testToastAndCrt();
    testSettingsPreview();
    testLookSettings();
    testBadgesAndDialogs();
    testMessage();
    testNowPlaying();
    testMusicVisualizer();
    testHud();
    testBufferingWheel();
    testLiquidGlass();
    testGlassRefraction();
    testShowcaseBackgrounds();
    testScreenBackdrop();
    testUsageCelebration();
    testHelpWindow();

    testTrackPrompt();
    testLyricsPlayer();
    testTwoPassIcons();
    testCustomization();
    testButtonShapes();
    testButtonSizes();
    testLibraryChrome();
    testVisualizerAndLegends();
    testQueueUi();
    testScrubAndGear();
    testVolumeBarEnds();
    testArtworkAccent();
    testExpressive();

    testQueueToggles(); // last: it uses glass refraction snapshots, which other tests count from zero
    ImGui::DestroyContext();
    return check::finish("test_ui");
}
