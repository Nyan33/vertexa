#include "ui/ui.h"
#include "sokol/sokol_app.h"

Ui ui;

// the original Graphite palette; Settings can replace it at run time
Color THEME[TC_COUNT] = {
    [TC_BG] = 0xff100e0e, [TC_S1] = 0xff181515, [TC_S2] = 0xff1f1b1b, [TC_S3] = 0xff282323, [TC_S4] = 0xff332c2c,
    [TC_PASTE] = 0xff0c0a0a, [TC_LINE] = 0xff292323, [TC_LINE2] = 0xff373030,
    [TC_TX] = 0xfff1ecec, [TC_TX2] = 0xffada2a2, [TC_TX3] = 0xff776b6b,
    [TC_AC] = 0xff365aff, [TC_LOOP] = 0xff44b5f2, [TC_PREV] = 0xffff8d4c, [TC_NEXT] = 0xff7ac435,
    [TC_TWEEN_C] = 0xff68343a, [TC_TWEEN_S] = 0xff3c4a1d, [TC_SEL] = 0xffff8d4c,
};

UiId ui_id(const char *s) {
    u64 h = 1469598103934665603ull;
    while (*s) { h ^= (u8)*s++; h *= 1099511628211ull; }
    return h ? h : 1;
}
UiId ui_idi(const char *s, int i) { return ui_id(s) * 31 + (u64)(i + 1) * 0x9E3779B97F4A7C15ull; }

static bool in_blockers(V2 p) {
    for (int i = 0; i < ui.nblockers; i++) if (rect_has(ui.blockers[i], p)) return true;
    return false;
}

void ui_begin(float w, float h, float scale, double dt) {
    ui.w = w; ui.h = h; ui.scale = scale; ui.dt = dt; ui.time += dt;
    dl_reset(&ui.base, scale, rect(0, 0, w, h));
    dl_reset(&ui.over, scale, rect(0, 0, w, h));
    dl_set_tex(&ui.base, font_view());
    dl_set_tex(&ui.over, font_view());
    ui.dl = &ui.base;
    ui.in_overlay = false;
    memcpy(ui.blockers, ui.next_blockers, sizeof(ui.blockers));
    ui.nblockers = ui.next_nblockers;
    ui.next_nblockers = 0;
    ui.hot_next = 0;
    ui.cursor = SAPP_MOUSECURSOR_DEFAULT;
    ui.hide_cursor = false;
    ui.tip = NULL;
    ui.popup_opened_now = false;
    // a click outside an open popup closes it and is swallowed
    if (ui.popup && (ui.in.pressed[0] || ui.in.pressed[1]) && !in_blockers(ui.in.mouse) && !rect_has(ui.popup_anchor, ui.in.mouse)) {
        ui.popup = 0;
        ui.in.pressed[0] = ui.in.pressed[1] = false;
    }
    if (!ui.popup) ui.nblockers = 0;
}

void ui_end(void) {
    // tooltip
    if (ui.tip && ui.tip_id == ui.hot_next && !ui.active) {
        if (ui.time - ui.tip_since > 0.45) {
            ui_overlay(true);
            float tw = font_width(F_UI_MED, ui.tip) + 18;
            Rect r = {ui.tip_rect.x + ui.tip_rect.w + 8, ui.tip_rect.y + ui.tip_rect.h / 2 - 13, tw, 26};
            if (r.x + r.w > ui.w - 4) r = (Rect){ui.tip_rect.x + ui.tip_rect.w / 2 - tw / 2, ui.tip_rect.y + ui.tip_rect.h + 6, tw, 26};
            if (r.y + r.h > ui.h - 4) r.y = ui.tip_rect.y - 32;
            dl_shadow(ui.dl, rect(r.x, r.y + 4, r.w, r.h), 6, 10, hexa(0x000000, 0x80));
            dl_rrect(ui.dl, r, 6, C_S4);
            font_draw_in(ui.dl, F_UI_MED, rect_inset(r, 9, 0), ALIGN_LEFT, ui.tip, C_TX);
            ui_overlay(false);
        }
    }
    if (ui.hot != ui.hot_next) ui.tip_since = ui.time;
    ui.hot = ui.hot_next;
    if (ui.in.released[0] && ui.active) ui.active = 0;
}

void ui_overlay(bool on) { ui.in_overlay = on; ui.dl = on ? &ui.over : &ui.base; }
bool ui_mouse_free(void) {
    if (!ui.in.mouse_inside) return false;
    if (ui.in_popup) return true;                  // the open popup is on top of everything
    if (in_blockers(ui.in.mouse)) return false;    // and covers the rest, overlay included
    return ui.in_overlay || !ui.modal;             // a modal window covers the base layer
}

UiResp ui_interact(UiId id, Rect r) {
    UiResp o = {0};
    bool inside = rect_has(r, ui.in.mouse) && rect_has(dl_clip(ui.dl), ui.in.mouse) && ui_mouse_free();
    o.hover = inside && (!ui.active || ui.active == id);
    if (o.hover) ui.hot_next = id;
    if (o.hover && ui.in.pressed[0]) { ui.active = id; ui.press_pos = ui.in.mouse; o.pressed = true; ui.focus = 0; }
    if (ui.active == id) {
        o.held = ui.in.down[0];
        o.delta = v2(ui.in.mouse.x - ui.in.prev.x, ui.in.mouse.y - ui.in.prev.y);
        if (ui.in.released[0]) { o.released = true; o.clicked = inside; }
    }
    o.right = inside && ui.in.pressed[1];
    o.dbl = inside && ui.in.dbl;
    return o;
}

void ui_tooltip(UiId id, Rect r, const char *text) {
    if (ui.hot_next == id) { ui.tip = text; ui.tip_rect = r; ui.tip_id = id; }
}
void ui_cursor(int c) { ui.cursor = c; }

/* ── popups ── */

void ui_open_popup(UiId id, Rect anchor) { if (ui.popup != id) ui.popup_h = 0; ui.popup = id; ui.popup_anchor = anchor; ui.popup_opened_now = true; }
bool ui_popup_is_open(UiId id) { return ui.popup == id; }
void ui_close_popup(void) { ui.popup = 0; }

static Rect menu_r; static float menu_y;

bool ui_popup_begin(UiId id, Rect r) {
    if (ui.popup != id) return false;
    if (ui.popup_h > 0) r.h = ui.popup_h;
    if (r.x + r.w > ui.w - 6) r.x = ui.w - 6 - r.w;
    if (r.y + r.h > ui.h - 6) r.y = MAX(6, ui.popup_anchor.y - r.h - 4);
    ui_overlay(true);
    ui.in_popup = true;
    if (ui.next_nblockers < COUNT(ui.next_blockers)) ui.next_blockers[ui.next_nblockers++] = r;
    if (ui.nblockers < COUNT(ui.blockers)) ui.blockers[ui.nblockers++] = r;
    dl_shadow(ui.dl, rect(r.x, r.y + 8, r.w, r.h), 10, 18, hexa(0x000000, 0x90));
    dl_rrect(ui.dl, r, 10, C_S3);
    dl_rrect_border(ui.dl, r, 10, 1, C_LINE2);
    dl_push_clip(ui.dl, r);
    menu_r = r; menu_y = r.y + 5;
    return true;
}
void ui_popup_end(void) { ui.popup_h = menu_y - menu_r.y + 5; dl_pop_clip(ui.dl); ui.in_popup = false; ui_overlay(ui.modal); }

void ui_menu_header(const char *label) {
    Rect r = {menu_r.x + 5, menu_y, menu_r.w - 10, 24};
    font_draw_in(ui.dl, F_SMALL, rect_inset(r, 9, 0), ALIGN_LEFT, label, C_TX3);
    menu_y += 24;
}
void ui_menu_sep(void) {
    dl_rect(ui.dl, rect(menu_r.x + 9, menu_y + 4, menu_r.w - 18, 1), C_LINE2);
    menu_y += 9;
}
bool ui_menu_item(const char *label, const char *shortcut, bool enabled, bool checked) { return ui_menu_item_ic(-1, label, shortcut, enabled, checked); }
bool ui_menu_item_ic(int icon, const char *label, const char *shortcut, bool enabled, bool checked) {
    Rect r = {menu_r.x + 5, menu_y, menu_r.w - 10, 28};
    menu_y += 28;
    UiResp re = ui_interact(ui_idi(label, (int)menu_y), r);
    bool hl = re.hover && enabled;
    if (hl) dl_rrect(ui.dl, r, 6, C_AC);
    Color tc = !enabled ? C_TX3 : hl ? hex(0xffffff) : C_TX;
    Rect tr = rect_inset(r, 10, 0);
    if (icon >= 0) { icon_draw(ui.dl, (IconId)icon, v2(r.x + 18, r.y + 14), ICON_M, false, hl ? hex(0xffffff) : enabled ? C_TX2 : C_TX3); tr.x += 22; tr.w -= 22; }
    if (checked) { dl_circle(ui.dl, v2(r.x + 12, r.y + 14), 3, hl ? hex(0xffffff) : C_AC); tr.x += 12; tr.w -= 12; }
    font_draw_in(ui.dl, F_UI, tr, ALIGN_LEFT, label, tc);
    if (shortcut) font_draw_in(ui.dl, F_MONO, tr, ALIGN_RIGHT, shortcut, hl ? hexa(0xffffff, 0xb0) : C_TX3);
    if (re.clicked && enabled) { ui.popup = 0; return true; }
    return false;
}

/* ── widgets ── */

float ui_label(Rect r, FontId f, const char *s, Color c, int align) {
    font_draw_in(ui.dl, f, r, align, s, c);
    return font_width(f, s);
}

bool ui_chip(const char *label, Rect r, bool on) {
    UiResp re = ui_interact(ui_idi(label, (int)r.x * 7 + (int)r.y), r);
    if (on) dl_rrect(ui.dl, r, 6, C_S4);
    else dl_rrect_border(ui.dl, r, 6, 1, C_LINE2);
    font_draw_in(ui.dl, F_UI, r, ALIGN_CENTER, label, on || re.hover ? C_TX : C_TX2);
    return re.clicked;
}

bool ui_text_button(UiId id, Rect r, const char *label, bool on) {
    UiResp re = ui_interact(id, r);
    if (on) dl_rrect(ui.dl, r, 6, C_S4);
    else if (re.hover) dl_rrect(ui.dl, r, 6, C_S3);
    font_draw_in(ui.dl, F_UI, r, ALIGN_CENTER, label, on || re.hover ? C_TX : C_TX2);
    return re.clicked;
}

float ui_slider(UiId id, Rect r, float v, float lo, float hi) {
    UiResp re = ui_interact(id, r);
    float t = (v - lo) / (hi - lo);
    if (re.held) { t = clampf((ui.in.mouse.x - r.x) / r.w, 0, 1); v = lo + t * (hi - lo); }
    if (re.hover && ui.in.wheel_y != 0) { v = clampf(v + (ui.in.wheel_y > 0 ? 1 : -1) * (hi - lo) / 100.0f, lo, hi); t = (v - lo) / (hi - lo); }
    float cy = r.y + r.h / 2;
    dl_rrect(ui.dl, rect(r.x, cy - 2, r.w, 4), 2, C_S4);
    dl_rrect(ui.dl, rect(r.x, cy - 2, r.w * t, 4), 2, re.held || re.hover ? C_TX : C_TX2);
    dl_circle(ui.dl, v2(r.x + r.w * t, cy), re.held ? 6.5f : 5.5f, C_TX);
    if (re.hover || re.held) ui_cursor(SAPP_MOUSECURSOR_RESIZE_EW);
    return v;
}

int ui_segmented(UiId id, Rect r, const char **items, int n, int sel) {
    dl_rrect(ui.dl, r, 7, C_S2);
    dl_rrect_border(ui.dl, r, 7, 1, C_LINE);
    float w = (r.w - 4) / n;
    for (int i = 0; i < n; i++) {
        Rect c = {roundf(r.x + 2 + w * i), r.y + 2, roundf(w), r.h - 4};
        UiResp re = ui_interact(id + (UiId)i + 1, c);
        if (re.clicked) sel = i;
        if (i == sel) dl_rrect(ui.dl, c, 5, C_S4);
        font_draw_in(ui.dl, F_UI, c, ALIGN_CENTER, items[i], i == sel || re.hover ? C_TX : C_TX2);
    }
    return sel;
}

int ui_dropdown(UiId id, Rect r, const char **items, int n, int sel) {
    UiResp re = ui_interact(id, r);
    bool open = ui_popup_is_open(id);
    dl_rrect(ui.dl, r, 6, open ? C_S4 : C_S3);
    font_draw_in(ui.dl, F_UI, rect(r.x + 10, r.y, r.w - 30, r.h), ALIGN_LEFT, items[sel], C_TX);
    float cx = r.x + r.w - 13, cy = r.y + r.h / 2;
    dl_tri(ui.dl, v2(cx - 4, cy - 2), v2(cx + 4, cy - 2), v2(cx, cy + 3), re.hover ? C_TX2 : C_TX3);
    if (re.clicked) { if (open) ui_close_popup(); else ui_open_popup(id, r); }
    if (ui_popup_begin(id, rect(r.x, r.y + r.h + 4, MAX(r.w, 180), 10 + 28 * n))) {
        for (int i = 0; i < n; i++) if (ui_menu_item(items[i], NULL, true, i == sel)) sel = i;
        ui_popup_end();
    }
    return sel;
}

bool ui_text_field(UiId id, Rect r, char *buf, int cap, const char *prefix) {
    UiResp re = ui_interact(id, r);
    bool committed = false;
    if (re.pressed) { ui.focus = id; snprintf(ui.edit, sizeof(ui.edit), "%s", buf); ui.edit_len = (int)strlen(ui.edit); }
    bool focused = ui.focus == id;
    if (focused) {
        for (int i = 0; i < ui.in.nchars; i++) {
            int c = ui.in.chars[i];
            if (c >= 32 && c < 127 && ui.edit_len < (int)sizeof(ui.edit) - 1 && ui.edit_len < cap - 1) { ui.edit[ui.edit_len++] = (char)c; ui.edit[ui.edit_len] = 0; }
        }
        if (ui.in.key_pressed[SAPP_KEYCODE_BACKSPACE] && ui.edit_len > 0) ui.edit[--ui.edit_len] = 0;
        if (ui.in.key_pressed[SAPP_KEYCODE_ENTER] || ui.in.key_pressed[SAPP_KEYCODE_KP_ENTER] || (ui.in.pressed[0] && !re.hover)) {
            snprintf(buf, cap, "%s", ui.edit); ui.focus = 0; committed = true;
        }
        if (ui.in.key_pressed[SAPP_KEYCODE_ESCAPE]) ui.focus = 0;
        ui.in.nchars = 0;
    }
    dl_rrect(ui.dl, r, 6, C_S2);
    dl_rrect_border(ui.dl, r, 6, 1, focused ? C_AC3 : C_LINE2);
    float x = r.x + 9;
    if (prefix) { font_draw_in(ui.dl, F_MONO_MED, rect(x, r.y, 20, r.h), ALIGN_LEFT, prefix, C_TX3); x += font_width(F_MONO_MED, prefix) + 8; }
    const char *s = focused ? ui.edit : buf;
    float w = font_width(F_MONO_MED, s);
    font_draw_in(ui.dl, F_MONO_MED, rect(x, r.y, r.w, r.h), ALIGN_LEFT, s, C_TX);
    if (focused && fmod(ui.time, 1.0) < 0.6) dl_rect(ui.dl, rect(x + w + 1, r.y + 7, 1, r.h - 14), C_TX);
    if (re.hover) ui_cursor(SAPP_MOUSECURSOR_IBEAM);
    return committed;
}

void ui_focus_text(UiId id, const char *text) {
    ui.focus = id;
    snprintf(ui.edit, sizeof(ui.edit), "%s", text);
    ui.edit_len = (int)strlen(ui.edit);
}

/* ── color ── */

void color_to_hsv(Color c, float *h, float *s, float *v) {
    float r = (c & 255) / 255.f, g = ((c >> 8) & 255) / 255.f, b = ((c >> 16) & 255) / 255.f;
    float mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn;
    *v = mx; *s = mx > 0 ? d / mx : 0;
    if (d > 1e-6f) {
        float hh = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        *h = hh / 6;
    }
}

Color color_from_hsv(float h, float s, float v) {
    h -= floorf(h);
    float hh = h * 6, f = hh - floorf(hh), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f)), r, g, b;
    switch ((int)hh % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return rgba((u8)roundf(r * 255), (u8)roundf(g * 255), (u8)roundf(b * 255), 255);
}

bool ui_color_wheel(UiId id, V2 c, float ro, float ri, float *h, float *s, float *v) {
    static UiId grab_id; static int grab;   // 1 = ring, 2 = square
    float half = roundf(ri * 0.647f);
    Rect sv = {c.x - half, c.y - half, 2 * half, 2 * half};
    bool changed = false;
    UiResp sre = ui_interact(id + 1, sv);
    UiResp rre = sre.hover ? (UiResp){0} : ui_interact(id + 2, rect(c.x - ro, c.y - ro, 2 * ro, 2 * ro));
    if (sre.pressed) { grab_id = id; grab = 2; }
    if (rre.pressed) { float dd = hypotf(ui.in.mouse.x - c.x, ui.in.mouse.y - c.y); grab_id = id; grab = dd >= ri - 6 && dd <= ro + 4 ? 1 : 0; }
    if (sre.held && grab_id == id && grab == 2) {
        *s = clampf((ui.in.mouse.x - sv.x) / sv.w, 0, 1); *v = clampf(1 - (ui.in.mouse.y - sv.y) / sv.h, 0, 1);
        changed = true;
    }
    if (rre.held && grab_id == id && grab == 1) {
        float a = atan2f(ui.in.mouse.y - c.y, ui.in.mouse.x - c.x) / (2 * VX_PI);
        *h = a < 0 ? a + 1 : a;
        changed = true;
    }
    dl_hue_ring(ui.dl, c, ro, ri);
    dl_sv_square(ui.dl, sv, 6, *h);
    float ha = *h * 2 * VX_PI, hm = (ro + ri) / 2;
    V2 hp = {c.x + cosf(ha) * hm, c.y + sinf(ha) * hm};
    dl_circle(ui.dl, v2(hp.x, hp.y + 1), 9, hexa(0x000000, 0x55));
    dl_circle(ui.dl, hp, 8, hex(0xffffff));
    dl_circle(ui.dl, hp, 5.5f, color_from_hsv(*h, 1, 1));
    V2 sp = {sv.x + *s * sv.w, sv.y + (1 - *v) * sv.h};
    dl_circle_border(ui.dl, sp, 7.5f, 1, hexa(0x000000, 0x70));
    dl_circle_border(ui.dl, sp, 6.5f, 2, hex(0xffffff));
    return changed;
}
