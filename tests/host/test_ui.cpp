// Renders every Ufin screen through Dear ImGui and a software rasterizer
// and checks the result: selection highlight under the right row,
// sidebar focus, scrollbar, text fitting, HUD states, and the layout /
// touch hit-testing maths. With UFIN_UI_PREVIEW=<dir> it also writes
// each screen as an image for eyeballing the design.
#include "check.h"
#include "soft_render.h"
#include "ui/app_ui.h"

#include <imgui.h>

#include <cstdlib>
#include <functional>

static const char* previewDir = nullptr;

// Colours as SoftRender::at() reports them (0x00BBGGRR).
static uint32_t bgr(uint32_t rgb) {
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}
static const uint32_t BG = bgr(0x14161A), SIDEBAR = bgr(0x1B1E24), PANEL = bgr(0x23272E),
                      SELECTED = bgr(0x0B7FB0);

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
    ui::setAmbientBackground(true);
    SoftRender amb = renderScreen([&] { ui::drawBrowser(m); }, "ambient");
    int changed = 0;
    for (int y = 90; y < 120; y++)
        for (int x = 300; x < 1200; x += 4)
            if (amb.at(x, y) != blue.at(x, y)) changed++;
    CHECK(changed > 50);
    ui::setAmbientBackground(false);

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
               {"About Ufin", "0.1.0", "A Jellyfin client for the Wii U", ui::Icon::Info, 0, 0}};
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
    testHud();

    ImGui::DestroyContext();
    return check::finish("test_ui");
}
