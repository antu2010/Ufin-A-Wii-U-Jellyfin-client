// queue_ui.inc -- included at the end of app_ui.cpp (inside namespace ui, after library_ui.inc): the Queue button of
// Now Playing, its "Up Next" flyout and the "Add to Queue" popup browser. One implementation for Plain, Liquid Glass
// and M3E (themedSurface and the M3E helpers style it); the logic lives in main.cpp (QueueUi) and user_queue.h.

namespace ui {

// A square picture (cover / poster / still, fitted) or a placeholder note.
static void queueThumb(ImDrawList* d, Texture art, float aspect, float x, float y, float s, bool dim = false) {
    if (art) {
        const layout::Fit f = layout::fitInside(x, y, s, s, aspect > 0.0f ? aspect : 1.0f);
        d->AddImageRounded(ImTextureRef((ImTextureID)art), ImVec2(f.x, f.y), ImVec2(f.x + f.w, f.y + f.h), ImVec2(0, 0), ImVec2(1, 1),
                           dim ? IM_COL32(255, 255, 255, 150) : col::WHITE, std::min(8.0f, s * 0.14f));
        return;
    }
    d->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), g_m3e ? g_m3c.secondaryContainer : col::PANEL_HI, std::min(8.0f, s * 0.14f));
    const float cx = x + s * 0.5f, cy = y + s * 0.5f, u = s * 0.16f;
    const ImU32 c = IM_COL32(0xFF, 0xFF, 0xFF, 0x70);
    d->AddCircleFilled(ImVec2(cx - u * 0.6f, cy + u * 1.2f), u * 0.9f, c, 16);
    d->AddLine(ImVec2(cx + u * 0.3f, cy + u * 1.0f), ImVec2(cx + u * 0.3f, cy - u * 1.6f), c, 2.4f);
    d->AddLine(ImVec2(cx + u * 0.3f, cy - u * 1.6f), ImVec2(cx + u * 1.5f, cy - u * 1.0f), c, 2.4f);
}

// Wipes a card up out of its anchor (Plain / Liquid Glass; M3E springs instead): clips to the part that has grown.
struct QueueWipe {
    ImDrawList* d;
    bool on = false;
    QueueWipe(ImDrawList* dl_, const Rect& card, float e) : d(dl_) {
        if (g_m3e || e >= 0.995f) return;
        on = true;
        d->PushClipRect(ImVec2(card.x - 8.0f, card.bottom() - card.h * e - 4.0f), ImVec2(card.right() + 8.0f, card.bottom() + 12.0f), true);
    }
    ~QueueWipe() { if (on) d->PopClipRect(); }
};

static float easeOut(float e) { return 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e); }

static void queueButtonFill(ImDrawList* d, const Rect& r, bool strong, bool focus, float rad) {
    if (g_m3e) {
        m3eButton(r, strong ? accentMain() : g_m3c.secondaryContainer, focus, d);
    } else if (themed()) {
        themedSurface(d, r.x, r.y, r.right(), r.bottom(), rad, (strong || focus) ? GK::ButtonHot : GK::Button, true);
        if (strong) d->AddRectFilled(ImVec2(r.x + 3, r.y + 3), ImVec2(r.right() - 3, r.bottom() - 3), (accentMain() & 0x00FFFFFFu) | 0x70000000u, rad - 2.0f);
        if (focus) d->AddRect(ImVec2(r.x - 2, r.y - 2), ImVec2(r.right() + 2, r.bottom() + 2), col::TEXT, rad + 2.0f, 0, 2.5f);
    } else {
        d->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), strong ? accentDeep() : col::PANEL_HI, rad);
        if (focus) d->AddRect(ImVec2(r.x - 2, r.y - 2), ImVec2(r.right() + 2, r.bottom() + 2), accentMain(), rad + 2.0f, 0, 2.5f);
    }
}

// The list glyph: three lines with dots.
static void queueGlyph(ImDrawList* d, float cx, float cy, float s, ImU32 c) {
    for (int i = -1; i <= 1; i++) {
        const float y = cy + i * s * 0.55f;
        d->AddCircleFilled(ImVec2(cx - s * 0.9f, y), s * 0.14f, c, 10);
        d->AddLine(ImVec2(cx - s * 0.5f, y), ImVec2(cx + s * 0.95f, y), c, 2.4f);
    }
}

static void drawQueueButton(const NowPlayingModel& m) {
    if (m.queueCount < 0) return;
    const Rect r = npQueueRect();
    const bool strong = m.flyout.open || m.queueCount > 0;
    const float rad = g_m3e ? r.h * 0.5f : gr(14.0f);
    ImDrawList* d = dl();
    queueButtonFill(d, r, strong, m.queueFocus && !m.flyout.open, rad);
    const ImU32 ic = g_m3e ? (strong ? onMain() : g_m3c.onSecondaryContainer) : col::WHITE;
    const char* label = "Queue";
    const std::string badge = m.queueCount > 0 ? std::to_string(m.queueCount) + " up next" : "empty";
    const float fs = r.h > 50.0f ? TEXT_M : TEXT_S + 1.0f;
    const ImVec2 ls = textSize(fs, label);
    const ImVec2 bs = textSize(TEXT_S - 3.0f, badge);
    const float gap = 10.0f, gw = 26.0f;
    const bool showBadge = r.w > 250.0f;
    const float total = gw + gap + ls.x + (showBadge ? 14.0f + bs.x : 0.0f);
    float x = r.x + (r.w - total) * 0.5f;
    queueGlyph(d, x + gw * 0.5f, r.y + r.h * 0.5f, 11.0f, ic);
    x += gw + gap;
    d->AddText(font(), fs, ImVec2(x, r.y + (r.h - ls.y) * 0.5f), ic, label);
    if (showBadge) d->AddText(font(), TEXT_S - 3.0f, ImVec2(x + ls.x + 14.0f, r.y + (r.h - bs.y) * 0.5f), scaleAlpha(ic, 0.8f), badge.c_str());
    else if (m.queueCount > 0) { // narrow button (lyrics open): a count bubble on the corner
        const std::string n = std::to_string(m.queueCount);
        const ImVec2 ns = textSize(TEXT_S - 4.0f, n);
        const float bw = std::max(22.0f, ns.x + 10.0f);
        const float bx = r.right() - bw * 0.7f, by = r.y - 8.0f;
        d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + 22.0f), col::WHITE, 11.0f);
        d->AddText(font(), TEXT_S - 4.0f, ImVec2(bx + (bw - ns.x) * 0.5f, by + 11.0f - ns.y * 0.5f), IM_COL32(0x20, 0x20, 0x20, 0xFF), n.c_str());
    }
}

// Small round-ish icon buttons of a flyout row.
static void queueRowGlyph(ImDrawList* d, int k, const Rect& b, ImU32 c, bool disabled) {
    const float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
    if (disabled) c = scaleAlpha(c, 0.30f);
    if (k == 0) d->AddTriangleFilled(ImVec2(cx - 7, cy + 4), ImVec2(cx + 7, cy + 4), ImVec2(cx, cy - 6), c);
    else if (k == 1) d->AddTriangleFilled(ImVec2(cx - 7, cy - 4), ImVec2(cx + 7, cy - 4), ImVec2(cx, cy + 6), c);
    else {
        d->AddLine(ImVec2(cx - 6, cy - 6), ImVec2(cx + 6, cy + 6), c, 2.6f);
        d->AddLine(ImVec2(cx - 6, cy + 6), ImVec2(cx + 6, cy - 6), c, 2.6f);
    }
}

static float g_queueFlyAnim = 0.0f; // 0 closed .. 1 open (eased by the frame time)

void drawQueueFlyout(const NowPlayingModel& m) {
    const QueueFlyoutModel& q = m.flyout;
    const float step = std::min(0.05f, ImGui::GetIO().DeltaTime) / 0.16f;
    g_queueFlyAnim = q.open ? std::min(1.0f, g_queueFlyAnim + step) : std::max(0.0f, g_queueFlyAnim - step);
    if (!q.visible && !q.open) { g_queueFlyAnim = 0.0f; return; }
    if (g_queueFlyAnim <= 0.0f || m.browser.open) return; // (the browser covers it: drawing it too would show through the glass)
    const Rect card = queueFlyoutCard();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    int ev = 0; float es = 1.0f;
    // (M3E's entrance spring is shared: while the browser is open it belongs to the browser, so the flyout beneath just stays put)
    const bool spring = g_m3e && !m.browser.open;
    if (spring) m3eEntranceBegin(fg, m3e::hashRect(card.x, card.y, card.w, card.h), ev, es);
    QueueWipe wipe(fg, card, easeOut(g_queueFlyAnim));
    if (themed()) themedSurface(fg, card.x, card.y, card.right(), card.bottom(), gr(18.0f), g_m3e ? GK::Dialog : GK::Panel, true);
    else {
        fg->AddRectFilled(ImVec2(card.x, card.y), ImVec2(card.right(), card.bottom()), col::PANEL, 18.0f);
        fg->AddRect(ImVec2(card.x, card.y), ImVec2(card.right(), card.bottom()), col::PANEL_HI, 18.0f, 0, 1.5f);
    }
    // header: title, then the count or the latest feedback
    fgText(fg, TEXT_M, card.x + 20, card.y + 12, col::TEXT, q.title, card.w * 0.5f);
    const std::string right = !q.toast.empty() ? q.toast : (q.count == 0 ? std::string() : std::to_string(q.count) + " up next");
    if (!right.empty()) {
        const float w = std::min(card.w * 0.5f, textSize(TEXT_S, right).x);
        fgText(fg, TEXT_S, card.right() - 20.0f - w, card.y + 16, q.toast.empty() ? col::MUTED : accentMain(), right, w + 2.0f);
    }
    // what plays now
    {
        const Rect r = queueFlyoutCurrent();
        if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(12.0f), GK::Row, true);
        else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), col::PANEL_HI, 12.0f);
        fg->AddRectFilled(ImVec2(r.x, r.y + 10), ImVec2(r.x + 4, r.bottom() - 10), accentMain(), 2.0f);
        queueThumb(fg, q.current.art, 1.0f, r.x + 14, r.y + 8, 44);
        fgText(fg, TEXT_S - 4.0f, r.x + 70, r.y + 7, accentMain(), "NOW PLAYING");
        fgText(fg, TEXT_S, r.x + 70, r.y + 28, col::TEXT, q.current.title, r.w - 84);
    }
    // actions
    {
        const Rect a = queueFlyoutAdd(), c = queueFlyoutClear();
        const bool fa = (q.cursor == -2), fc = (q.cursor == -1);
        queueButtonFill(fg, a, true, fa, g_m3e ? a.h * 0.5f : gr(12.0f));
        const ImU32 tc = g_m3e ? onMain() : col::WHITE;
        const ImVec2 as = textSize(TEXT_S, "Add to Queue");
        fgText(fg, TEXT_S, a.x + (a.w - as.x) * 0.5f + 10.0f, a.y + (a.h - as.y) * 0.5f, tc, "Add to Queue");
        const float px = a.x + (a.w - as.x) * 0.5f - 12.0f, py = a.y + a.h * 0.5f;
        fg->AddLine(ImVec2(px - 7, py), ImVec2(px + 7, py), tc, 2.6f);
        fg->AddLine(ImVec2(px, py - 7), ImVec2(px, py + 7), tc, 2.6f);
        const bool none = q.count == 0;
        queueButtonFill(fg, c, q.confirmClear, fc, g_m3e ? c.h * 0.5f : gr(12.0f));
        const char* cl = q.confirmClear ? "Sure?" : "Clear";
        const ImVec2 cs = textSize(TEXT_S, cl);
        const ImU32 ct = q.confirmClear ? tc : (g_m3e ? g_m3c.onSecondaryContainer : col::WHITE);
        fgText(fg, TEXT_S, c.x + (c.w - cs.x) * 0.5f, c.y + (c.h - cs.y) * 0.5f, none ? scaleAlpha(ct, 0.4f) : ct, cl);
    }
    // upcoming rows
    const int n = q.count;
    if (n == 0) {
        const Rect r = queueFlyoutRow(0);
        fgText(fg, TEXT_S, r.x + 8, r.y + 14, col::MUTED, q.empty, r.w - 16);
    }
    const ListWindow w = computeListWindow(n, std::max(0, q.cursor), layout::QF_ROWS);
    for (int i = w.start; i < w.end; i++) {
        const int slot = i - w.start;
        if (i < q.first || i - q.first >= (int)q.rows.size()) continue;
        const QueueRow& qr = q.rows[(size_t)(i - q.first)];
        const Rect r = queueFlyoutRow(slot);
        const bool sel = (i == q.cursor);
        if (sel) {
            if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(12.0f), GK::RowFocus, true);
            else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentDeep(), 12.0f);
        } else if (!g_m3e) {
            fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), IM_COL32(0xFF, 0xFF, 0xFF, 0x0C), 12.0f);
        }
        const ImU32 tc = sel ? onFocus() : col::TEXT, sc = sel ? scaleAlpha(onFocus(), 0.75f) : col::MUTED;
        queueThumb(fg, qr.art, 1.0f, r.x + 8, r.y + (r.h - 38.0f) * 0.5f, 38);
        const float tx = r.x + 56.0f, tw = queueFlyoutRowButton(slot, 0).x - tx - 6.0f;
        const bool two = !qr.sub.empty();
        fgText(fg, TEXT_S, tx, r.y + (two ? 4.0f : (r.h - TEXT_S) * 0.5f), tc, qr.title, tw);
        if (two) fgText(fg, TEXT_S - 4.0f, tx, r.y + 4.0f + TEXT_S + 2.0f, sc, qr.sub, tw);
        for (int k = 0; k < 3; k++) {
            const Rect b = queueFlyoutRowButton(slot, k);
            fg->AddCircleFilled(ImVec2(b.x + b.w * 0.5f, b.y + b.h * 0.5f), b.w * 0.5f, sel ? IM_COL32(0xFF, 0xFF, 0xFF, 0x22) : IM_COL32(0xFF, 0xFF, 0xFF, 0x12), 20);
            queueRowGlyph(fg, k, b, tc, (k == 0 && i == 0) || (k == 1 && i == n - 1));
        }
    }
    if (n > layout::QF_ROWS) { // scroll bar
        const Rect top = queueFlyoutRow(0);
        const float trackH = layout::QF_ROWS * layout::QF_ROW_H - 8.0f, tx = card.right() - 7.0f;
        fg->AddRectFilled(ImVec2(tx, top.y), ImVec2(tx + 3, top.y + trackH), col::PANEL_HI, 1.5f);
        const float thumb = std::max(18.0f, trackH * layout::QF_ROWS / n);
        fg->AddRectFilled(ImVec2(tx, top.y + (trackH - thumb) * w.start / (float)(n - layout::QF_ROWS)), ImVec2(tx + 3, top.y + (trackH - thumb) * w.start / (float)(n - layout::QF_ROWS) + thumb), col::MUTED, 1.5f);
    }
    if (spring) m3eEntranceEnd(fg, ev, es, card.x + card.w * 0.5f, card.bottom());
}

void drawQueueBrowser(const NowPlayingModel& m) {
    const QueueBrowserModel& b = m.browser;
    if (!b.open) return;
    const Rect card = queueBrowserCard();
    DialogShell sh(card);
    ImDrawList* fg = sh.fg;
    fgText(fg, TEXT_L, card.x + 24, card.y + 12, col::TEXT, b.title, 300.0f);
    fgText(fg, TEXT_S, card.x + 24, card.y + 46, col::MUTED, b.context, card.w - 110.0f);
    { // close
        const Rect c = queueBrowserClose();
        fg->AddCircleFilled(ImVec2(c.x + c.w * 0.5f, c.y + c.h * 0.5f), c.w * 0.5f, g_m3e ? g_m3c.secondaryContainer : col::PANEL_HI, 24);
        const float cx = c.x + c.w * 0.5f, cy = c.y + c.h * 0.5f;
        fg->AddLine(ImVec2(cx - 7, cy - 7), ImVec2(cx + 7, cy + 7), col::TEXT, 2.6f);
        fg->AddLine(ImVec2(cx - 7, cy + 7), ImVec2(cx + 7, cy - 7), col::TEXT, 2.6f);
    }
    // left: the cover of what is highlighted
    const Rect cv = queueBrowserCover();
    queueThumb(fg, b.cover, b.coverAspect, cv.x, cv.y, cv.w);
    fgText(fg, TEXT_S, cv.x, cv.bottom() + 10, col::TEXT, b.coverTitle, cv.w);
    fgText(fg, TEXT_S - 3.0f, cv.x, cv.bottom() + 36, col::MUTED, b.coverSub, cv.w);
    { // Add / Add all
        const Rect a = queueBrowserAdd();
        queueButtonFill(fg, a, true, false, g_m3e ? a.h * 0.5f : gr(12.0f));
        const ImU32 tc = g_m3e ? onMain() : col::WHITE;
        const ImVec2 s = textSize(TEXT_M - 2.0f, b.addLabel);
        fgText(fg, TEXT_M - 2.0f, a.x + (a.w - s.x) * 0.5f, a.y + (a.h - s.y) * 0.5f, tc, b.addLabel);
        const Rect all = queueBrowserAddAll();
        queueButtonFill(fg, all, false, false, g_m3e ? all.h * 0.5f : gr(12.0f));
        const ImVec2 s2 = textSize(TEXT_S, "Add all listed");
        const ImU32 tc2 = g_m3e ? g_m3c.onSecondaryContainer : col::WHITE;
        fgText(fg, TEXT_S, all.x + (all.w - s2.x) * 0.5f, all.y + (all.h - s2.y) * 0.5f, b.canAddAll ? tc2 : scaleAlpha(tc2, 0.4f), "Add all listed");
    }
    const int nt = (int)b.tabs.size();
    for (int i = 0; i < nt; i++) { // tabs (a music library's views)
        const Rect t = queueBrowserTab(i, nt);
        const bool on = (i == b.tab);
        queueButtonFill(fg, t, on, false, g_m3e ? t.h * 0.5f : gr(10.0f));
        const ImU32 tc = g_m3e ? (on ? onMain() : g_m3c.onSecondaryContainer) : col::WHITE;
        const ImVec2 s = textSize(TEXT_S, b.tabs[(size_t)i]);
        fgText(fg, TEXT_S, t.x + (t.w - s.x) * 0.5f, t.y + (t.h - s.y) * 0.5f, on ? tc : scaleAlpha(tc, 0.75f), b.tabs[(size_t)i], t.w - 8);
    }
    // right: the list
    const int vis = queueBrowserRows(nt > 0);
    const int n = b.count;
    if (n == 0) {
        const Rect r = queueBrowserRow(0, nt > 0);
        fgText(fg, TEXT_M, r.x + 8, r.y + 10, col::MUTED, b.busy ? "Loading..." : b.empty, r.w - 16);
    }
    const ListWindow w = computeListWindow(n, b.selected, vis);
    for (int i = w.start; i < w.end; i++) {
        const int slot = i - w.start;
        if (i < b.first || i - b.first >= (int)b.rows.size()) continue;
        const Rect r = queueBrowserRow(slot, nt > 0);
        const bool sel = (i == b.selected);
        if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(12.0f), sel ? GK::RowFocus : GK::Row, true);
        else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), sel ? accentDeep() : col::PANEL_HI, 12.0f);
        const QueueRow& row = b.rows[(size_t)(i - b.first)];
        const ImU32 tc = sel ? onFocus() : col::TEXT, sc = sel ? scaleAlpha(onFocus(), 0.75f) : col::MUTED;
        queueThumb(fg, row.art, 1.0f, r.x + 6, r.y + (r.h - 34.0f) * 0.5f, 34);
        const float tx = r.x + 50.0f, tw = r.w - 50.0f - 40.0f;
        const bool two = !row.sub.empty();
        fgText(fg, TEXT_S, tx, r.y + (two ? 2.0f : (r.h - TEXT_S) * 0.5f), tc, row.title, tw);
        if (two) fgText(fg, TEXT_S - 4.0f, tx, r.y + 2.0f + TEXT_S, sc, row.sub, tw);
        if (row.container) { // a chevron: A opens it
            const float cx = r.right() - 20.0f, cy = r.y + r.h * 0.5f;
            fg->AddLine(ImVec2(cx - 4, cy - 7), ImVec2(cx + 3, cy), tc, 2.6f);
            fg->AddLine(ImVec2(cx + 3, cy), ImVec2(cx - 4, cy + 7), tc, 2.6f);
        }
    }
    if (n > vis) { // scroll bar, with how far the list is (b.total may exceed what is loaded)
        const Rect top = queueBrowserRow(0, nt > 0);
        const float trackH = vis * layout::QB_ROW_H - 6.0f, tx = top.right() + 8.0f;
        fg->AddRectFilled(ImVec2(tx, top.y), ImVec2(tx + 4, top.y + trackH), col::PANEL_HI, 2.0f);
        const int all = std::max(n, b.total);
        const float thumb = std::max(20.0f, trackH * vis / all);
        const float ty = top.y + (trackH - thumb) * (float)w.start / (float)std::max(1, all - vis);
        fg->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + 4, ty + thumb), col::MUTED, 2.0f);
    }
    if (!b.toast.empty()) { // feedback pill at the bottom of the list
        const ImVec2 s = textSize(TEXT_S, b.toast);
        const float pw = s.x + 36.0f, px = card.right() - 24.0f - pw, py = card.bottom() - 46.0f;
        fg->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 34.0f), g_m3e ? accentMain() : accentDeep(), 17.0f);
        fgText(fg, TEXT_S, px + 18.0f, py + 17.0f - s.y * 0.5f, g_m3e ? onMain() : col::WHITE, b.toast);
    }
    fgHints(fg, b.hints, card.x, card.bottom() + 18);
}


// ---- the movie remote's gear button and settings flyout --------------------------------------------------------

static float g_vsAnim = 0.0f;

static void vsChoiceRow(ImDrawList* fg, const Rect& r, bool selected, bool focus, const std::string& label, bool arrows, bool dim) {
    if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(10.0f), (selected || focus) ? GK::RowFocus : GK::Row, true);
    else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), (selected || focus) ? accentDeep() : col::PANEL_HI, 10.0f);
    const ImU32 tc = dim ? col::DIM : ((selected || focus) ? onFocus() : col::TEXT);
    if (arrows) {
        const float cy = r.y + r.h * 0.5f;
        for (int side = 0; side < 2; side++) {
            const Rect a = vsArrow(r, side == 1);
            const float cx = a.x + a.w * 0.5f, d = side == 0 ? -1.0f : 1.0f;
            fg->AddLine(ImVec2(cx - 4 * d, cy - 8), ImVec2(cx + 4 * d, cy), tc, 2.6f);
            fg->AddLine(ImVec2(cx + 4 * d, cy), ImVec2(cx - 4 * d, cy + 8), tc, 2.6f);
        }
        const ImVec2 ts = textSize(TEXT_S, label);
        fgText(fg, TEXT_S, r.x + (r.w - std::min(ts.x, r.w - 100.0f)) * 0.5f, r.y + (r.h - ts.y) * 0.5f, tc, label, r.w - 100.0f);
    } else {
        fgText(fg, TEXT_S, r.x + 16.0f, r.y + (r.h - TEXT_S) * 0.5f, tc, label, r.w - 56.0f);
        if (selected) { // the one in use: a tick
            const float cx = r.right() - 22.0f, cy = r.y + r.h * 0.5f;
            fg->AddLine(ImVec2(cx - 6, cy), ImVec2(cx - 2, cy + 5), tc, 2.8f);
            fg->AddLine(ImVec2(cx - 2, cy + 5), ImVec2(cx + 7, cy - 5), tc, 2.8f);
        }
    }
}

void drawVideoSettings(const NowPlayingModel& m) {
    const VideoSettingsModel& v = m.settings;
    if (!v.gear) return;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const Rect g = npGearRect();
    { // the button: round (cookie in M3E, glass in Liquid Glass)
        const float cx = g.x + g.w * 0.5f, cy = g.y + g.h * 0.5f, rad = g.w * 0.5f;
        if (g_m3e) m3eFillShape(fg, cx, cy, rad, rad, SH_COOKIE, rad, 0.0f, v.open ? accentMain() : g_m3c.secondaryContainer, 1.0f, 1.0f);
        else if (g_glass) {
            glassCircle(fg, cx, cy, rad, v.open ? GK::ButtonHot : GK::Button, true);
            if (v.open) fg->AddCircleFilled(ImVec2(cx, cy), rad - 3.0f, (accentMain() & 0x00FFFFFFu) | 0x80000000u, 40);
        } else {
            fg->AddCircleFilled(ImVec2(cx, cy), rad, v.open ? accentMain() : IM_COL32(0x2C, 0x31, 0x3A, 0xC0), 40);
            fg->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0x3A, 0x3F, 0x48, 0xC0), 40, 1.5f);
        }
        const ImU32 ic = g_m3e ? (v.open ? onMain() : g_m3c.onSecondaryContainer) : col::WHITE;
        // a cog: a ring with eight teeth and a hole
        const float r1 = rad * 0.34f, r2 = rad * 0.50f;
        for (int k = 0; k < 8; k++) {
            const float a = (float)k * 0.7853982f;
            fg->AddLine(ImVec2(cx + std::cos(a) * r1, cy + std::sin(a) * r1), ImVec2(cx + std::cos(a) * r2, cy + std::sin(a) * r2), ic, rad * 0.22f);
        }
        fg->AddCircle(ImVec2(cx, cy), r1, ic, 24, rad * 0.14f);
    }
    const float step = std::min(0.05f, ImGui::GetIO().DeltaTime) / 0.16f;
    g_vsAnim = v.open ? std::min(1.0f, g_vsAnim + step) : std::max(0.0f, g_vsAnim - step);
    if (g_vsAnim <= 0.0f) return;
    const int nq = (int)v.qualities.size();
    const Rect card = vsCard(nq);
    int ev = 0; float es = 1.0f;
    if (g_m3e) m3eEntranceBegin(fg, m3e::hashRect(card.x, card.y, card.w, card.h), ev, es);
    // wipes down out of the button (clipped), like the queue flyout wipes up
    bool clipped = false;
    if (!g_m3e && g_vsAnim < 0.995f) {
        fg->PushClipRect(ImVec2(card.x - 8.0f, card.y - 4.0f), ImVec2(card.right() + 8.0f, card.y + card.h * easeOut(g_vsAnim) + 4.0f), true);
        clipped = true;
    }
    if (themed()) themedSurface(fg, card.x, card.y, card.right(), card.bottom(), gr(18.0f), g_m3e ? GK::Dialog : GK::Panel, true);
    else {
        fg->AddRectFilled(ImVec2(card.x, card.y), ImVec2(card.right(), card.bottom()), col::PANEL, 18.0f);
        fg->AddRect(ImVec2(card.x, card.y), ImVec2(card.right(), card.bottom()), col::PANEL_HI, 18.0f, 0, 1.5f);
    }
    fgText(fg, TEXT_M, card.x + 20, card.y + 12, col::TEXT, "Playback settings", card.w - 40);
    fgText(fg, TEXT_S - 4.0f, card.x + 20, card.y + 42, col::MUTED, "For what is playing now only", card.w - 40);
    fgText(fg, TEXT_S - 3.0f, card.x + 20, card.y + 62, accentMain(), v.toast.empty() ? "VIDEO QUALITY" : v.toast, card.w - 40);
    for (int i = 0; i < nq; i++)
        vsChoiceRow(fg, vsQualityRow(i), i == v.quality, v.cursor == i, v.qualities[(size_t)i], false, false);
    for (int k = 0; k < 2; k++) {
        const bool has = k == 0 ? v.hasAudio : v.hasSubtitles;
        const std::string label = std::string(k == 0 ? "Audio: " : "Subtitles: ") + (k == 0 ? v.audioLabel : v.subtitleLabel);
        vsChoiceRow(fg, vsTrackRow(k, nq), false, v.cursor == nq + k, has ? label : label + " (no choice)", true, !has);
    }
    if (g_m3e) m3eEntranceEnd(fg, ev, es, card.right() - 30.0f, card.y);
    if (clipped) fg->PopClipRect();
}

} // namespace ui
