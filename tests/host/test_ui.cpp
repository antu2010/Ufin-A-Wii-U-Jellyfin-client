// Renders every Ufin screen through Dear ImGui and a software rasterizer
// and checks the result: selection highlight under the right row,
// sidebar focus, scrollbar, text fitting, HUD states, and the layout /
// touch hit-testing maths. With UFIN_UI_PREVIEW=<dir> it also writes
// each screen as an image for eyeballing the design.
#include "check.h"
#include "soft_render.h"
#include "ui/app_ui.h"
#include "ui/glass_core.h"
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

    testQueueToggles(); // last: it uses glass refraction snapshots, which other tests count from zero
    ImGui::DestroyContext();
    return check::finish("test_ui");
}
