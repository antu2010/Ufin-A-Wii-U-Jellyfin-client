// library_ui.inc -- included at the end of app_ui.cpp (inside namespace ui, after m3e_draw.inc): the library
// chrome. Floating Sort / New playlist buttons, the sort flyout, the "+" on song rows, the New playlist dialog
// and the "Add to playlist" picker. One implementation for Plain, Liquid Glass and M3E (themedSurface and the
// M3E helpers do the styling), so behaviour is identical everywhere.

namespace ui {

// Plain draws popups on the foreground list: text there needs its own helper (text() writes to the background).
static void fgText(ImDrawList* fg, float size, float x, float y, ImU32 c, const std::string& s, float maxW = 0.0f) {
    const std::string t = maxW > 0.0f ? fitText(font(), size, s, maxW) : s;
    if (g_m3e && g_bold) fg->AddText(font(), size, ImVec2(x + std::max(0.7f, size * 0.022f), y), c, t.c_str());
    fg->AddText(font(), size, ImVec2(x, y), c, t.c_str());
}

// A glyph centred in a round button: "+" or the sort bars.
static void fabGlyph(ImDrawList* d, FabKind kind, float cx, float cy, ImU32 c) {
    if (kind == FabKind::NewPlaylist) {
        d->AddLine(ImVec2(cx - 10, cy), ImVec2(cx + 10, cy), c, 3.2f);
        d->AddLine(ImVec2(cx, cy - 10), ImVec2(cx, cy + 10), c, 3.2f);
    } else { // three bars, long to short: "sort"
        d->AddLine(ImVec2(cx - 11, cy - 8), ImVec2(cx + 11, cy - 8), c, 3.2f);
        d->AddLine(ImVec2(cx - 11, cy), ImVec2(cx + 5, cy), c, 3.2f);
        d->AddLine(ImVec2(cx - 11, cy + 8), ImVec2(cx - 1, cy + 8), c, 3.2f);
    }
}

// One floating button: a circle (Plain, Liquid Glass) or the established M3E cookie.
static void drawFab(ImDrawList* d, const Rect& r, FabKind kind, bool focus) {
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, rad = r.w * 0.5f;
    ImU32 glyph = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    if (g_m3e) {
        if (focus) m3eFillShape(d, cx, cy, rad + 4.0f, rad + 4.0f, SH_COOKIE, rad, 0.0f, col::TEXT, 1.0f, 1.0f);
        m3eFillShape(d, cx, cy, rad, rad, SH_COOKIE, rad, 0.0f, g_m3c.primaryContainer, 1.0f, 1.0f);
        glyph = g_m3c.onPrimaryContainer;
    } else if (g_glass) {
        glassCircle(d, cx, cy, rad, focus ? GK::ButtonHot : GK::Button, true);
        d->AddCircleFilled(ImVec2(cx, cy), rad - 3.0f, (accentMain() & 0x00FFFFFFu) | (focus ? 0xB0000000u : 0x80000000u), 40);
        if (focus) d->AddCircle(ImVec2(cx, cy), rad + 2.0f, col::TEXT, 40, 2.5f);
    } else {
        d->AddCircleFilled(ImVec2(cx, cy + 2.0f), rad, IM_COL32(0, 0, 0, 70), 40); // soft shadow
        d->AddCircleFilled(ImVec2(cx, cy), rad, focus ? accentMain() : accentDeep(), 40);
        if (focus) d->AddCircle(ImVec2(cx, cy), rad + 3.0f, col::TEXT, 40, 2.5f);
    }
    fabGlyph(d, kind, cx, cy, glyph);
}

static void drawFabs(const BrowserModel& m) {
    const int n = (int)m.fabs.size();
    for (int i = 0; i < n; i++) drawFab(dl(), layout::fabRect(i, n), m.fabs[(size_t)i], i == m.fabFocus && !m.sidebarFocused);
}

int hitTestFab(const BrowserModel& m, float x, float y) {
    const int n = (int)m.fabs.size();
    for (int i = 0; i < n; i++) {
        Rect r = layout::fabRect(i, n);
        r.x -= 6; r.y -= 6; r.w += 12; r.h += 12; // a little slack around a 56 px target
        if (r.contains(x, y)) return i;
    }
    return -1;
}

// The "+" on a song row.
static void drawRowAdd(const Rect& row, bool hot) {
    const Rect b = layout::rowAddButton(row);
    const float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
    const ImU32 fill = g_m3e ? g_m3c.secondaryContainer : (hot ? col::PANEL_HI : col::PANEL_HI);
    dl()->AddCircleFilled(ImVec2(cx, cy), b.w * 0.5f, fill, 28);
    const ImU32 c = hot && !g_m3e ? onFocus() : col::TEXT;
    dl()->AddLine(ImVec2(cx - 7, cy), ImVec2(cx + 7, cy), c, 2.4f);
    dl()->AddLine(ImVec2(cx, cy - 7), ImVec2(cx, cy + 7), c, 2.4f);
}

bool hitTestRowAdd(const BrowserModel& m, int itemIndex, int slot, float x, float y) {
    if (itemIndex < 0 || itemIndex >= (int)m.items.size() || !m.items[(size_t)itemIndex].addable) return false;
    Rect b = layout::rowAddButton(layout::listRow(slot));
    b.x -= 8; b.y -= 4; b.w += 16; b.h += 8;
    return b.contains(x, y);
}

// --- sort flyout ---

static void drawSortMenu(const BrowserModel& m) {
    const SortMenuModel& sm = m.sortMenu;
    if (!sm.open || sm.options.empty()) return;
    const int n = (int)sm.options.size();
    const Rect anchor = layout::fabRect(sm.anchor, (int)m.fabs.size());
    const Rect card = layout::sortMenuCard(n, anchor);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    int ev = 0; float es = 1.0f;
    m3eEntranceBegin(fg, m3e::hashRect(card.x, card.y, card.w, card.h), ev, es); // springs out of its button
    if (themed()) themedSurface(fg, card.x, card.y, card.right(), card.bottom(), gr(16.0f), g_m3e ? GK::Dialog : GK::Panel, true);
    else {
        fg->AddRectFilled(ImVec2(card.x, card.y), ImVec2(card.right(), card.bottom()), col::PANEL, 16.0f);
        fg->AddRect(ImVec2(card.x, card.y), ImVec2(card.right(), card.bottom()), col::PANEL_HI, 16.0f, 0, 1.5f);
    }
    fgText(fg, TEXT_S, card.x + 20, card.y + 12, col::MUTED, sm.title, card.w - 40);
    const std::string cur = sm.options[(size_t)std::max(0, std::min(n - 1, sm.current))] + " - " + sm.directionText;
    fgText(fg, TEXT_M, card.x + 20, card.y + 12 + TEXT_S + 8, col::TEXT, cur, card.w - 40);
    for (int i = 0; i <= n; i++) {
        const Rect r = layout::sortMenuRow(n, anchor, i);
        const bool sel = (i == sm.cursor);
        if (sel) {
            if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(12.0f), GK::RowFocus, true);
            else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentDeep(), 12.0f);
        }
        const ImU32 tc = sel ? onFocus() : col::TEXT;
        const float ty = r.y + (r.h - TEXT_M) * 0.5f;
        if (i < n) {
            if (i == sm.current) { // the active criterion: a dot
                fg->AddCircleFilled(ImVec2(r.x + 18, r.y + r.h * 0.5f), 5.0f, sel ? onFocus() : accentMain(), 16);
            }
            fgText(fg, TEXT_M, r.x + 36, ty, tc, sm.options[(size_t)i], r.w - 48);
        } else {
            fgText(fg, TEXT_M, r.x + 16, ty, tc, "Direction", r.w * 0.4f);
            // an arrow up (ascending) or down, then what it means
            const float ax = r.x + r.w * 0.42f, ay = r.y + r.h * 0.5f;
            if (sm.descending) fg->AddTriangleFilled(ImVec2(ax - 6, ay - 4), ImVec2(ax + 6, ay - 4), ImVec2(ax, ay + 6), tc);
            else fg->AddTriangleFilled(ImVec2(ax - 6, ay + 4), ImVec2(ax + 6, ay + 4), ImVec2(ax, ay - 6), tc);
            fgText(fg, TEXT_S, ax + 14, r.y + (r.h - TEXT_S) * 0.5f, tc, sm.directionText, r.right() - ax - 22);
        }
    }
    m3eEntranceEnd(fg, ev, es, card.right(), card.bottom());
}

int hitTestSortMenu(const BrowserModel& m, float x, float y) {
    const SortMenuModel& sm = m.sortMenu;
    if (!sm.open || sm.options.empty()) return -1;
    const int n = (int)sm.options.size();
    const Rect anchor = layout::fabRect(sm.anchor, (int)m.fabs.size());
    for (int i = 0; i <= n; i++)
        if (layout::sortMenuRow(n, anchor, i).contains(x, y)) return i;
    return layout::sortMenuCard(n, anchor).contains(x, y) ? -2 : -1;
}

// --- dialogs ---

// The card, scrim and entrance shared by the New playlist dialog and the picker.
struct DialogShell {
    ImDrawList* fg;
    int ev = 0; float es = 1.0f;
    Rect card;
    DialogShell(const Rect& c) : fg(ImGui::GetForegroundDrawList()), card(c) {
        if (!g_m3e) fg->AddRectFilled(ImVec2(0, 0), ImVec2(layout::SCREEN_W, layout::SCREEN_H), IM_COL32(0, 0, 0, 0xB0));
        m3eEntranceBegin(fg, m3e::hashRect(c.x, c.y, c.w, c.h), ev, es);
        if (g_m3e) m3eDialogScrim(fg, m3e::entranceAlpha(es), ev);
        if (themed()) themedSurface(fg, c.x, c.y, c.right(), c.bottom(), gr(18.0f), g_m3e ? GK::Dialog : GK::Panel, true);
        else fg->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.right(), c.bottom()), col::PANEL, 18.0f);
        if (g_m3e) {}
        else if (g_glass) glassBand(fg, c.x, c.y, c.right(), c.bottom(), gr(18.0f), c.y, c.y + 6.0f, accentMain());
        else fg->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.right(), c.y + 6), accentMain(), gr(18.0f), ImDrawFlags_RoundCornersTop);
    }
    ~DialogShell() { m3eEntranceEnd(fg, ev, es, card.x + card.w * 0.5f, card.y + card.h * 0.5f); }
};

static void fgHints(ImDrawList* fg, const std::vector<Hint>& hints, float x, float y) {
    const float rr = 14.0f;
    for (const Hint& hint : hints) {
        const float bw = std::max(2 * rr, textSize(TEXT_S, hint.button).x + 14.0f);
        if (themed()) themedSurface(fg, x, y, x + bw, y + 2 * rr, rr, GK::Button, true);
        else fg->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + 2 * rr), col::PANEL_HI, rr);
        const ImVec2 bs = textSize(TEXT_S, hint.button);
        fgText(fg, TEXT_S, x + (bw - bs.x) * 0.5f, y + rr - bs.y * 0.5f, col::TEXT, hint.button);
        x += bw + 8.0f;
        fgText(fg, TEXT_S, x, y + rr - bs.y * 0.5f, col::MUTED, hint.label);
        x += textSize(TEXT_S, hint.label).x + 26.0f;
    }
}

void drawPlaylistForm(const PlaylistFormModel& m) {
    const Rect card = layout::plFormCard(m.showVisibility);
    DialogShell sh(card);
    ImDrawList* fg = sh.fg;
    fgText(fg, TEXT_L, card.x + 30, card.y + 28, col::TEXT, m.title, card.w - 60);

    auto field = [&](int slot, bool focus) {
        const Rect r = layout::plFormSlot(slot, m.showVisibility);
        if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(14.0f), focus ? GK::RowFocus : GK::Row, true);
        else {
            fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), focus ? accentDeep() : col::PANEL_HI, 14.0f);
        }
        return r;
    };
    { // name
        const bool f = (m.focus == 0);
        const Rect r = field(0, f);
        fgText(fg, TEXT_S - 2, r.x + 20, r.y + 7, f ? onFocus() : col::MUTED, "Name");
        const bool empty = m.name.empty();
        fgText(fg, TEXT_M, r.x + 20, r.y + 28, empty ? (f ? onFocus() : col::DIM) : (f ? onFocus() : col::TEXT),
               empty ? "Tap or press A to type a name" : m.name, r.w - 40);
    }
    if (m.showVisibility) {
        const bool f = (m.focus == 1);
        const Rect r = layout::plFormSlot(1, true);
        if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(14.0f), GK::Row, true);
        else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), col::PANEL_HI, 14.0f);
        for (int w = 0; w < 2; w++) {
            const Rect h = layout::plFormVisChip(w, true);
            const bool on = (m.isPublic ? 1 : 0) == w;
            const Rect in{h.x + 5, h.y + 5, h.w - 10, h.h - 10};
            if (on) {
                if (themed()) themedSurface(fg, in.x, in.y, in.right(), in.bottom(), gr(11.0f), GK::RowFocus, true);
                else fg->AddRectFilled(ImVec2(in.x, in.y), ImVec2(in.right(), in.bottom()), accentDeep(), 11.0f);
            }
            const char* label = w == 0 ? "Private" : "Public";
            const ImVec2 ts = textSize(TEXT_M, label);
            fgText(fg, TEXT_M, in.x + (in.w - ts.x) * 0.5f, in.y + (in.h - ts.y) * 0.5f, on ? onFocus() : col::MUTED, label);
        }
        if (f) fg->AddRect(ImVec2(r.x - 2, r.y - 2), ImVec2(r.right() + 2, r.bottom() + 2), accentMain(), gr(16.0f), 0, 2.5f);
    }
    // message line
    if (!m.message.empty()) {
        const Rect nameSlot = layout::plFormSlot(m.showVisibility ? 1 : 0, m.showVisibility);
        fgText(fg, TEXT_S, nameSlot.x + 4, nameSlot.bottom() + 12, m.messageIsError ? rgb(0xFF6B6B) : col::MUTED, m.message,
               nameSlot.w - 8);
    }
    const char* labels[2] = {m.busy ? "Creating..." : "Create", "Cancel"};
    for (int i = 0; i < 2; i++) {
        const Rect r = layout::plFormSlot(2 + i, m.showVisibility);
        const bool focus = (m.focus == 2 + i), primary = (i == 0);
        if (g_m3e) {
            m3eButton(r, primary ? accentMain() : g_m3c.secondaryContainer, focus, fg);
        } else {
            ImU32 fill = primary ? (focus ? accentMain() : accentDeep()) : (focus ? col::PANEL_HI : col::PANEL);
            if (primary && m.busy) fill = col::PANEL_HI;
            if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(14.0f), (primary || focus) ? GK::ButtonHot : GK::Button, true);
            else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), fill, 14.0f);
            if (!themed() && focus && !primary) fg->AddRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), accentMain(), 14.0f, 0, 2.5f);
        }
        const ImU32 tc = g_m3e ? (primary ? g_m3c.onPrimary : g_m3c.onSecondaryContainer) : col::WHITE;
        const ImVec2 ts = textSize(TEXT_M, labels[i]);
        fgText(fg, TEXT_M, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - ts.y) * 0.5f, (primary && m.busy) ? col::MUTED : tc, labels[i]);
    }
    fgHints(fg, m.hints, card.x, card.bottom() + 18);
}

int hitTestPlaylistForm(const PlaylistFormModel& m, float x, float y) {
    if (layout::plFormSlot(0, m.showVisibility).contains(x, y)) return 0;
    if (m.showVisibility) {
        if (layout::plFormVisChip(0, true).contains(x, y)) return 10;
        if (layout::plFormVisChip(1, true).contains(x, y)) return 11;
    }
    if (layout::plFormSlot(2, m.showVisibility).contains(x, y)) return 2;
    if (layout::plFormSlot(3, m.showVisibility).contains(x, y)) return 3;
    return -1;
}

void drawPlaylistPicker(const PlaylistPickerModel& m) {
    const Rect card = layout::pickCard();
    DialogShell sh(card);
    ImDrawList* fg = sh.fg;
    fgText(fg, TEXT_L, card.x + 24, card.y + 22, col::TEXT, m.title, card.w - 48);
    const std::string sub = m.message.empty() ? m.subtitle : m.message;
    fgText(fg, TEXT_S, card.x + 24, card.y + 66, col::MUTED, sub, card.w - 48);
    const int n = (int)m.rows.size();
    const ListWindow w = computeListWindow(n, m.selected, layout::PICK_ROWS);
    for (int i = w.start; i < w.end; i++) {
        const Rect r = layout::pickRow(i - w.start);
        const bool sel = (i == m.selected);
        if (themed()) themedSurface(fg, r.x, r.y, r.right(), r.bottom(), gr(12.0f), sel ? GK::RowFocus : GK::Row, true);
        else fg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), sel ? accentDeep() : col::PANEL_HI, 12.0f);
        const ImU32 tc = sel ? onFocus() : col::TEXT;
        float tx = r.x + 18;
        if (i == 0) { // "New playlist": a plus
            const float cx = r.x + 28, cy = r.y + r.h * 0.5f;
            fg->AddLine(ImVec2(cx - 8, cy), ImVec2(cx + 8, cy), tc, 2.6f);
            fg->AddLine(ImVec2(cx, cy - 8), ImVec2(cx, cy + 8), tc, 2.6f);
            tx = r.x + 52;
        }
        fgText(fg, TEXT_M, tx, r.y + (r.h - TEXT_M) * 0.5f, m.busy ? col::MUTED : tc, m.rows[(size_t)i], r.right() - tx - 16);
    }
    if (n > layout::PICK_ROWS) { // scroll bar
        const Rect top = layout::pickRow(0);
        const float trackH = layout::PICK_ROWS * layout::PICK_ROW_H - 6.0f;
        const float tx = card.right() - 12.0f;
        fg->AddRectFilled(ImVec2(tx, top.y), ImVec2(tx + 4, top.y + trackH), col::PANEL_HI, 2.0f);
        const float thumb = std::max(20.0f, trackH * layout::PICK_ROWS / n);
        const float ty = top.y + (trackH - thumb) * w.start / (float)(n - layout::PICK_ROWS);
        fg->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + 4, ty + thumb), col::MUTED, 2.0f);
    }
    fgHints(fg, m.hints, card.x, card.bottom() + 18);
}

int hitTestPlaylistPicker(const PlaylistPickerModel& m, float x, float y) {
    const int n = (int)m.rows.size();
    const ListWindow w = computeListWindow(n, m.selected, layout::PICK_ROWS);
    for (int i = w.start; i < w.end; i++)
        if (layout::pickRow(i - w.start).contains(x, y)) return i;
    return -1;
}

} // namespace ui
