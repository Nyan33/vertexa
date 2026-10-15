// Tool options bar, color panel and the right panel (Brushes / Library / Properties).
#include "app/app.h"
#include "sokol/sokol_app.h"
#include "render/scene.h"

/* ── tool options (context bar) ── */

static float cx_;          // layout cursor
static Rect bar_;

static void c_sep(void) { dl_rect(ui.dl, rect(cx_, bar_.y + 12, 1, 20), C_LINE2); cx_ += 1 + 18; }
static void c_slider(const char *label, float *v, float lo, float hi, const char *fmt) {
    float lw = font_width(F_UI, label);
    font_draw_in(ui.dl, F_UI, rect(cx_, bar_.y, lw, bar_.h), ALIGN_LEFT, label, C_TX2);
    cx_ += lw + 10;
    *v = roundf(ui_slider(ui_id(label), rect(cx_, bar_.y + 12, 96, 20), *v, lo, hi));
    cx_ += 96 + 10;
    char s[16]; snprintf(s, sizeof(s), fmt, (int)*v);
    font_draw_in(ui.dl, F_MONO_MED, rect(cx_, bar_.y, 30, bar_.h), ALIGN_LEFT, s, C_TX);
    cx_ += 30 + 16;
}
static void c_chip(const char *label, bool *v, const char *tip) {
    float w = font_width(F_UI, label) + 22;
    Rect r = {cx_, bar_.y + 8, w, 28};
    if (ui_chip(label, r, *v)) *v = !*v;
    if (tip) ui_tooltip(ui_idi(label, (int)r.x * 7 + (int)r.y), r, tip);
    cx_ += w + 8;
}
static void c_drop(const char *id, const char **items, int n, int *sel, float w) {
    *sel = ui_dropdown(ui_id(id), rect(cx_, bar_.y + 8, w, 28), items, n, *sel);
    cx_ += w + 8;
}
static void c_label(const char *s) {
    float w = font_width(F_UI, s);
    font_draw_in(ui.dl, F_UI, rect(cx_, bar_.y, w, bar_.h), ALIGN_LEFT, s, C_TX3);
    cx_ += w + 8;
}
static void c_hint(const char *s) { font_draw_in(ui.dl, F_UI, rect(cx_, bar_.y, 600, bar_.h), ALIGN_LEFT, s, C_TX3); }

void context_bar(Rect r) {
    dl_rect(ui.dl, r, C_S1);
    dl_rect(ui.dl, rect(r.x, r.y + r.h - 1, r.w, 1), C_LINE);
    bar_ = rect(r.x, r.y, r.w, r.h - 1);
    dl_push_clip(ui.dl, r);
    cx_ = r.x + 16;
    Layer *cl = doc_active(app.doc);
    bool cam = cl && cl->type == LT_CAMERA && !app.doc->nedit;   // the camera layer is active: the stage moves the camera
    icon_draw(ui.dl, cam ? IC_CAMERA : tool_icon(app.tool), v2(cx_ + 8, bar_.y + bar_.h / 2), ICON_M, app.tool == 'V' && !cam, C_TX2);
    font_draw_in(ui.dl, F_UI_SEMI, rect(cx_ + 24, bar_.y, 100, bar_.h), ALIGN_LEFT, cam ? "Camera" : tool_name(app.tool), C_TX);
    cx_ += 124;
    Layer *al = doc_active(app.doc);
    if (al && al->type == LT_CAMERA && !app.doc->nedit) {
        float cam[4];
        doc_camera_at(app.doc, doc_tl(app.doc)->frame, cam);
        char s[64];
        snprintf(s, sizeof(s), "Zoom %d%%, turn %d\xC2\xB0", (int)roundf(cam[2] * 100), (int)roundf(cam[3] * 180 / VX_PI));
        font_draw_in(ui.dl, F_MONO_MED, rect(cx_, bar_.y, 200, bar_.h), ALIGN_LEFT, s, C_TX);
        cx_ += font_width(F_MONO_MED, s) + 16;
        if (ui_text_button(ui_id("cam-reset"), rect(cx_, bar_.y + 8, 110, 28), "Reset camera", false)) {
            Layer *l = doc_camera_layer(app.doc);
            int ki = l ? doc_key_index_for(l, doc_tl(app.doc)->frame) : -1;
            if (ki >= 0) { app_commit(); l = doc_camera_layer(app.doc); doc_camera_default(app.doc, l->keys[ki].cam); doc_touch(app.doc); }
        }
        cx_ += 110 + 8;
        c_chip("Look through it", stage_camera_view(), "Show only what the camera sees (exports look like this)");
        c_sep();
        c_hint("Drag the frame to move the camera, a corner to zoom, just outside a corner to turn. Shift snaps");
        dl_pop_clip(ui.dl);
        return;
    }
    switch (app.tool) {
    case 'B': {
        c_slider("Size", &app.brush_size, 1, 100, "%d");
        c_slider("Smoothing", &app.brush_smooth, 0, 100, "%d");
        c_sep();
        c_chip("Pressure", &app.brush_pressure, "Pen pressure changes the size");
        c_chip("Tilt", &app.brush_tilt, "Pen tilt widens the stroke");
        c_sep();
        static const char *modes[] = {"Paint normal", "Paint behind"};
        int m = app.brush_mode == 2 ? 1 : 0;
        c_drop("brush-mode", modes, 2, &m, 140);
        app.brush_mode = m ? 2 : 0;
        c_chip("Object drawing", &app.object_draw, "Keep each stroke a separate object");
        break;
    }
    case 'Y': {
        static const char *modes[] = {"Smooth", "Straighten", "Ink"};
        int m = app.pencil_mode;
        c_drop("pencil-mode", modes, 3, &m, 120);
        app.pencil_mode = m;
        c_slider("Size", &app.pencil_size, 1, 20, "%d");
        break;
    }
    case 'E': c_slider("Size", &app.eraser_size, 2, 200, "%d"); break;
    case 'C': {
        static const char *modes[] = {"Up to intersection", "Whole stroke"};
        c_drop("trim-mode", modes, 2, &app.trim_mode, 170);
        c_chip("All layers", &app.trim_all_layers, "Lines on other layers also stop the cut");
        c_sep();
        c_hint("Click a segment, or drag across several");
        break;
    }
    case 'N': case 'R': case 'O': {
        c_slider("Outline", &app.shape_width, 1, 40, "%d");
        if (app.tool != 'N') { c_sep(); c_chip("Fill", &app.shape_fill, "Fill with the fill color"); c_chip("Outline", &app.shape_line, "Draw the outline with the stroke color"); }
        c_sep();
        c_hint(app.tool == 'N' ? "Shift: 45\xC2\xB0 steps" : "Shift: square or circle. Alt: from the center");
        break;
    }
    case 'K': {
        static const char *gaps[] = {"Don't close gaps", "Close small gaps", "Close medium gaps", "Close large gaps"};
        c_drop("bucket-gap", gaps, 4, &app.bucket_gap, 170);
        c_hint("Click inside an area enclosed by lines, or click a fill to recolor it");
        break;
    }
    case 'V': case 'Q': case 'L': {
        // what a click picks: the whole stroke or its part between intersections; which vectors count
        static const char *modes[] = {"Whole", "To crossings"}, *boxes[] = {"Cuts", "Touched"};
        c_label("Click");
        app.sel_mode = ui_segmented(ui_id("sel-mode"), rect(cx_, bar_.y + 8, 196, 28), modes, 2, app.sel_mode);
        cx_ += 196 + 14;
        c_label("Box");
        app.sel_box = ui_segmented(ui_id("sel-box"), rect(cx_, bar_.y + 8, 156, 28), boxes, 2, app.sel_box);
        cx_ += 156 + 14;
        static const char *targets[] = {"Lines and fills", "Lines only", "Fills only"};
        c_drop("sel-target", targets, 3, &app.sel_target, 150);
        c_sep();
        if (app.tool == 'V') c_hint("Shift adds. Double-click a symbol to edit it, F8 makes a symbol");
        else if (app.tool == 'Q') c_hint("Drag the white point to move the pivot, double-click it to center. Alt scales around it, outside a corner rotates");
        else c_hint("Draw around what you want to select");
        break;
    }
    case 'A': c_hint("Click a stroke to show its points, drag a point to reshape the stroke"); break;
    case 'I': c_hint("Click the stage to pick a color into the current swatch (fill or stroke)"); break;
    case 'H': c_hint("Drag to pan. Space works with any tool. Double-click the zoom badge to fit"); break;
    case 'Z': c_hint("Click to zoom in, Alt+click to zoom out. Ctrl+wheel zooms anywhere"); break;
    default: c_hint("This tool arrives in a later milestone"); break;
    }
    dl_pop_clip(ui.dl);
}

/* ── color ── */


static Color shown_color;   // detect outside changes (eyedropper, swatches, swaps)
static void sync_hsv(void) {
    Color c = app_color();
    if (c != shown_color) { float h = app.hue; color_to_hsv(c, &h, &app.sat, &app.val); if (app.sat > 0.001f && app.val > 0.001f) app.hue = h; shown_color = c; }
}
static void set_from_hsv(void) { Color c = color_from_hsv(app.hue, app.sat, app.val); app_set_color(c); shown_color = c; }

void color_panel(Rect r) {
    sync_hsv();
    dl_rect(ui.dl, rect(r.x, r.y + r.h - 1, r.w, 1), C_LINE);
    Rect in = rect_inset(r, 16, 16);
    V2 c = {roundf(in.x + in.w / 2), in.y + 4 + 102};
    if (ui_color_wheel(ui_id("wheel"), c, 102, 85, &app.hue, &app.sat, &app.val)) set_from_hsv();

    // stroke / fill, hex, alpha
    float y = in.y + 4 + 204 + 16;
    Rect sf = {in.x, y, 44, 30};
    Rect st = {sf.x, sf.y + 2, 26, 26}, fl = {sf.x + 18, sf.y + 2, 26, 26};
    UiResp fre = ui_interact(ui_id("fill-chip"), fl);
    UiResp tre = fre.hover ? (UiResp){0} : ui_interact(ui_id("stroke-chip"), st);
    if (fre.clicked) app.color_target = 0;
    if (tre.clicked) app.color_target = 1;
    ui_tooltip(ui_id("fill-chip"), fl, "Fill color (brush)");
    ui_tooltip(ui_id("stroke-chip"), st, "Stroke color (pencil). X swaps");
    V2 sc = {st.x + 13, st.y + 13}, fc = {fl.x + 13, fl.y + 13};
    if (app.color_target == 1) dl_circle(ui.dl, sc, 15, C_TX);
    dl_circle(ui.dl, sc, 13.5f, C_S1);
    dl_circle_border(ui.dl, sc, 13, 6, app.stroke);
    dl_circle_border(ui.dl, sc, 13, 1, hexa(0xffffff, 0x59));
    dl_circle_border(ui.dl, sc, 7, 1, hexa(0xffffff, 0x59));
    if (app.color_target == 0) dl_circle(ui.dl, fc, 15, C_TX);
    dl_circle(ui.dl, fc, 13.5f, C_S1);
    dl_circle(ui.dl, fc, 12.5f, app.fill);
    dl_circle_border(ui.dl, fc, 12.5f, 1, hexa(0xffffff, 0x22));
    static char hexbuf[16], abuf[16];
    if (ui.focus != ui_id("hex")) snprintf(hexbuf, sizeof(hexbuf), "%06X", col_rgb(app_color()));
    if (ui_text_field(ui_id("hex"), rect(in.x + 52, y, in.w - 52 - 82, 30), hexbuf, sizeof(hexbuf), "#")) {
        unsigned v; char *s = hexbuf; if (*s == '#') s++;
        if (strlen(s) == 3) { char e[7] = {s[0], s[0], s[1], s[1], s[2], s[2], 0}; if (sscanf(e, "%x", &v) == 1) app_set_color(hex(v)); }
        else if (sscanf(s, "%x", &v) == 1) app_set_color(hex(v));
    }
    if (ui.focus != ui_id("alpha")) snprintf(abuf, sizeof(abuf), "%d%%", (int)roundf(app.alpha * 100));
    if (ui_text_field(ui_id("alpha"), rect(in.x + in.w - 74, y, 74, 30), abuf, sizeof(abuf), "A")) {
        int a; if (sscanf(abuf, "%d", &a) == 1) app.alpha = clampf(a / 100.0f, 0, 1);
    }
    // swatches
    y += 30 + 14;
    float sx = in.x;
    for (int i = 0; i <= app.nswatches; i++) {
        if (sx + 20 > in.x + in.w + 0.5f) { sx = in.x; y += 27; }
        Rect wr = {sx, y, 20, 20};
        V2 wc = {sx + 10, y + 10};
        if (i == app.nswatches) {
            UiResp a = ui_interact(ui_id("swatch-add"), wr);
            dl_circle_border(ui.dl, wc, 10, 1, a.hover ? C_TX3 : C_LINE2);
            font_draw_in(ui.dl, F_UI, wr, ALIGN_CENTER, "+", a.hover ? C_TX : C_TX3);
            ui_tooltip(ui_id("swatch-add"), wr, "Add the current color");
            if (a.clicked && app.nswatches < COUNT(app.swatches)) app.swatches[app.nswatches++] = app_color();
        } else {
            UiResp a = ui_interact(ui_idi("swatch", i), wr);
            bool on = (app.swatches[i] | 0xff000000u) == (app_color() | 0xff000000u);
            if (on) { dl_circle(ui.dl, wc, 12.5f, C_TX); dl_circle(ui.dl, wc, 11, C_S1); }
            dl_circle(ui.dl, wc, 10, app.swatches[i]);
            dl_circle_border(ui.dl, wc, 10, 1, hexa(0xffffff, 0x18));
            if (a.clicked) app_set_color(app.swatches[i]);
            if (a.right) { memmove(&app.swatches[i], &app.swatches[i + 1], sizeof(Color) * (app.nswatches - i - 1)); app.nswatches--; }
        }
        sx += 27;
    }
}

/* ── brushes ── */

static void ribbon(Rect r, const BrushPreset *p, Color c) {
    V2 prev = {0}; float pw = 0;
    for (int i = 0; i <= 32; i++) {
        float t = i / 32.0f;
        float pr = sinf(t * VX_PI);                         // a pressure stroke: light, heavy, light
        float w = p->size * 0.22f * MAX(0.06f, curve_eval(&p->curve, pr));
        if (p->taper == TAPER_START || p->taper == TAPER_BOTH) w *= MIN(1, t * 4);
        if (p->taper == TAPER_END || p->taper == TAPER_BOTH) w *= MIN(1, (1 - t) * 4);
        if (p->tip == TIP_OVAL) w *= 0.55f + 0.45f * fabsf(cosf(t * 2.2f + p->angle));
        w = clampf(w, 0.6f, r.h * 0.42f);
        V2 q = {r.x + 4 + t * (r.w - 8), r.y + r.h / 2 + sinf(t * VX_PI * 1.4f + p->angle) * 3};
        if (i) dl_line(ui.dl, prev, q, (w + pw), c);
        prev = q; pw = w;
    }
}

// pressure -> size: click adds a point, drag moves it, right-click / double-click or dragging it
// out of the graph removes it; the end points move up and down only
static void curve_editor(Rect r) {
    dl_rrect(ui.dl, r, 8, C_S2);
    dl_rrect_border(ui.dl, r, 8, 1, C_LINE);
    Rect g = rect_inset(r, 10, 10);
    for (int i = 1; i < 4; i++) {
        dl_rect(ui.dl, rect(g.x, roundf(g.y + g.h * i / 4), g.w, 1), hexa(0xffffff, 0x0a));
        dl_rect(ui.dl, rect(roundf(g.x + g.w * i / 4), g.y, 1, g.h), hexa(0xffffff, 0x0a));
    }
    Curve *c = &app.curve;
    static int drag = -1;
    static bool out;
    V2 m = ui.in.mouse;
#define CPT(i) v2(g.x + g.w * c->x[i], g.y + g.h * (1 - c->y[i]))
    int hot = -1;
    for (int i = 0; i < c->n; i++) { V2 q = CPT(i); if (fabsf(m.x - q.x) <= 7 && fabsf(m.y - q.y) <= 7) hot = i; }
    UiId id = ui_id("pressure-curve");
    UiResp re = ui_interact(id, r);
    float mx = clampf((m.x - g.x) / g.w, 0, 1), my = clampf(1 - (m.y - g.y) / g.h, 0, 1);
    bool inner = hot > 0 && hot < c->n - 1;
    if ((re.right || re.dbl) && inner) { curve_remove(c, hot); hot = -1; drag = -1; }
    else if (re.pressed) {
        drag = hot;
        if (drag < 0 && !curve_insert(c, mx, my, &drag)) drag = -1;
        out = false;
    }
    if (re.held && drag >= 0 && drag < c->n) {
        bool end = drag == 0 || drag == c->n - 1;
        c->y[drag] = my;
        if (!end) c->x[drag] = clampf(mx, c->x[drag - 1] + 0.03f, c->x[drag + 1] - 0.03f);
        out = !end && (m.x < r.x - 28 || m.x > r.x + r.w + 28 || m.y < r.y - 28 || m.y > r.y + r.h + 28);
    }
    if (re.released) {
        if (out && drag > 0 && drag < c->n - 1) curve_remove(c, drag);
        drag = -1; out = false;
    }
    if (hot >= 0 || drag >= 0) ui_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
    ui_tooltip(id, r, "Click adds a point, right-click or drag it out to remove");

    // area + curve
    V2 prev = {g.x, g.y + g.h * (1 - curve_eval(c, 0))};
    for (int i = 1; i <= 64; i++) {
        float p = i / 64.0f;
        V2 q = {g.x + g.w * p, g.y + g.h * (1 - curve_eval(c, p))};
        float by = g.y + g.h;
        dl_tri(ui.dl, prev, q, v2(q.x, by), col_a(C_AC, 0x16));
        dl_tri(ui.dl, prev, v2(q.x, by), v2(prev.x, by), col_a(C_AC, 0x16));
        dl_line(ui.dl, prev, q, 2, C_AC);
        prev = q;
    }
    for (int i = 0; i < c->n; i++) {
        V2 q = CPT(i);
        bool on = i == drag || (drag < 0 && i == hot);
        Color col = i == drag && out ? col_a(C_TX, 0x50) : C_TX;
        if (on) dl_circle(ui.dl, q, 7, col_a(C_AC, 0x40));
        dl_circle(ui.dl, q, on ? 5 : 4, C_S1);
        dl_circle_border(ui.dl, q, on ? 5 : 4, 2, col);
    }
#undef CPT
    if (drag >= 0 && drag < c->n) {
        char s[32];
        snprintf(s, sizeof(s), out ? "remove" : "%d%% \xE2\x86\x92 %d%%", (int)roundf(c->x[drag] * 100), (int)roundf(c->y[drag] * 100));
        font_draw_in(ui.dl, F_MONO, rect(g.x, g.y, g.w - 2, 14), ALIGN_RIGHT, s, C_TX2);
    }
    font_draw(ui.dl, F_MONO, g.x + 2, g.y + g.h - 3, "pressure", hexa(0xffffff, 0x40));
}

static void brushes_tab(Rect r) {
    // preset list: scrolls once it outgrows six rows
    static float scroll;
    int n = (int)arrlen(app.presets);
    float list_h = MIN(n, 6) * 36.0f + 2, content = n * 36.0f + 2;
    Rect list = {r.x, r.y + 6, r.w, list_h};
    if (rect_has(list, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y != 0) scroll -= ui.in.wheel_y * 36;
    static int shown = -1;   // keep a newly chosen preset (e.g. just imported) in view
    if (app.preset != shown) {
        float py = app.preset * 36.0f;
        if (py < scroll) scroll = py;
        if (py + 38 > scroll + list_h) scroll = py + 38 - list_h;
        shown = app.preset;
    }
    scroll = clampf(scroll, 0, MAX(0, content - list_h));
    dl_push_clip(ui.dl, list);
    float y = list.y + 2 - scroll;
    int remove = -1;
    static int *PICK;   // presets picked with Ctrl / Shift (to delete several imported brushes at once)
    for (ptrdiff_t k = arrlen(PICK) - 1; k >= 0; k--) if (PICK[k] >= n) arrdel(PICK, k);
    for (int i = 0; i < n; i++, y += 36) {
        Rect row = {r.x + 8, y, r.w - 16, 34};
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        BrushPreset *p = &app.presets[i];
        UiId id = ui_idi("preset", i);
        // the trash button first, so the row doesn't take its click
        bool del = p->user && rect_has(row, ui.in.mouse) && ui_mouse_free();
        Rect x = {row.x + row.w - 30, row.y + 5, 24, 24};
        UiId xid = ui_idi("preset-del", i);
        UiResp xr = del ? ui_interact(xid, x) : (UiResp){0};
        UiResp re = xr.hover ? (UiResp){0} : ui_interact(id, row);
        bool on = app.preset == i, picked = false;
        for (ptrdiff_t k = 0; k < arrlen(PICK); k++) picked |= PICK[k] == i;
        if (on) { dl_rrect(ui.dl, row, 7, C_AC2); dl_rrect_border(ui.dl, row, 7, 1, C_AC3); }
        else if (picked) { dl_rrect(ui.dl, row, 7, C_S3); dl_rrect_border(ui.dl, row, 7, 1, C_AC3); }
        else if (re.hover) dl_rrect(ui.dl, row, 7, C_S2);
        Color tc = on || picked || re.hover ? C_TX : C_TX2;
        float nx = row.x + 10;
        if (p->user) { icon_draw(ui.dl, IC_IMPORT, v2(nx + 6, row.y + 17), ICON_S, false, C_TX3); nx += 18; }
        font_draw_in(ui.dl, F_UI, rect(nx, row.y, row.x + row.w - 140 - nx, row.h), ALIGN_LEFT, p->name, tc);
        Rect rib = rect(row.x + row.w - 136, row.y + 7, 128, 20);
        if (del) rib.w -= 28;
        ribbon(rib, p, tc);
        if (del) {
            if (xr.hover) dl_rrect(ui.dl, x, 6, C_S3);
            icon_draw(ui.dl, IC_TRASH, v2(x.x + 12, x.y + 12), ICON_S, false, xr.hover ? C_TX : C_TX3);
            ui_tooltip(xid, x, "Remove imported brush");
            if (xr.clicked) { remove = i; continue; }
        }
        if (re.pressed) {
            u32 mods = ui.in.mods;
            if (mods & MOD_CTRL) {          // pick more (the current one comes along)
                if (!arrlen(PICK)) arrput(PICK, app.preset);
                int k = -1;
                for (ptrdiff_t q = 0; q < arrlen(PICK); q++) if (PICK[q] == i) k = (int)q;
                if (k >= 0) arrdel(PICK, k); else arrput(PICK, i);
            } else if (mods & MOD_SHIFT) {   // a run, from the current one
                arrsetlen(PICK, 0);
                for (int q = MIN(app.preset, i); q <= MAX(app.preset, i); q++) arrput(PICK, q);
            } else { arrsetlen(PICK, 0); app_apply_preset(i); }
        }
        if (re.right) {
            if (!picked) { arrsetlen(PICK, 0); arrput(PICK, i); }
            ui_open_popup(ui_id("preset-menu"), rect(ui.in.mouse.x, ui.in.mouse.y, 1, 1));
        }
    }
    dl_pop_clip(ui.dl);
    if (remove >= 0) app_remove_preset(remove);
    if (ui_popup_begin(ui_id("preset-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 240, 10 + 28 * 2 + 9))) {
        int users = 0;
        for (ptrdiff_t k = 0; k < arrlen(PICK); k++) users += PICK[k] < n && app.presets[PICK[k]].user;
        if (ui_menu_item("Use This Brush", NULL, arrlen(PICK) == 1, false)) { app_apply_preset(PICK[0]); arrsetlen(PICK, 0); }
        ui_menu_sep();
        char del[64];
        snprintf(del, sizeof(del), users > 1 ? "Remove %d Imported Brushes" : "Remove Imported Brush", users);
        if (ui_menu_item(del, NULL, users > 0, false)) {
            // highest first, so the other indices stay right
            for (int q = n - 1; q >= 0; q--) {
                bool want = false;
                for (ptrdiff_t k = 0; k < arrlen(PICK); k++) want |= PICK[k] == q;
                if (want && app.presets[q].user) app_remove_preset(q);
            }
            arrsetlen(PICK, 0);
        }
        ui_popup_end();
    }
    if (content > list_h) {   // thin scroll thumb
        float th = list_h * list_h / content, ty = list.y + (list_h - th) * scroll / (content - list_h);
        dl_rrect(ui.dl, rect(r.x + r.w - 5, ty, 3, th), 1.5f, hexa(0xffffff, 0x22));
    }
    y = list.y + list_h + 2;
    y += 6;
    dl_rect(ui.dl, rect(r.x, y, r.w, 1), C_LINE);
    float x = r.x + 16, w = r.w - 32;
    y += 14;
    font_draw_in(ui.dl, F_UI, rect(x, y, w, 16), ALIGN_LEFT, "Tip", C_TX3); y += 22;
    static const char *tips[] = {"Round", "Oval", "Square"};
    u8 tip = (u8)ui_segmented(ui_id("tip"), rect(x, y, w, 30), tips, 3, app.tip); y += 30;
    if (tip == TIP_OVAL && app.tip != TIP_OVAL && app.tip_aspect > 0.95f) app.tip_aspect = 0.38f;
    app.tip = tip;
    if (app.tip != TIP_ROUND) {
        y += 12;
        font_draw_in(ui.dl, F_UI, rect(x, y, 60, 20), ALIGN_LEFT, "Angle", C_TX3);
        app.tip_angle = roundf(ui_slider(ui_id("tip-angle"), rect(x + 72, y, w - 72 - 44, 20), app.tip_angle, 0, 180));
        char s[16]; snprintf(s, sizeof(s), "%d\xC2\xB0", (int)app.tip_angle);
        font_draw_in(ui.dl, F_MONO_MED, rect(x + w - 40, y, 40, 20), ALIGN_RIGHT, s, C_TX);
        y += 20;
        if (app.tip == TIP_OVAL) {
            y += 8;
            font_draw_in(ui.dl, F_UI, rect(x, y, 70, 20), ALIGN_LEFT, "Roundness", C_TX3);
            float pc = roundf(ui_slider(ui_id("tip-aspect"), rect(x + 72, y, w - 72 - 44, 20), app.tip_aspect * 100, 5, 100));
            app.tip_aspect = pc / 100;
            char a[16]; snprintf(a, sizeof(a), "%d%%", (int)pc);
            font_draw_in(ui.dl, F_MONO_MED, rect(x + w - 40, y, 40, 20), ALIGN_RIGHT, a, C_TX);
            y += 20;
        }
    }
    y += 14;
    font_draw_in(ui.dl, F_UI, rect(x, y, w, 16), ALIGN_LEFT, "Taper", C_TX3); y += 22;
    static const char *tapers[] = {"None", "Start", "End", "Both"};
    app.taper = (u8)ui_segmented(ui_id("taper"), rect(x, y, w, 30), tapers, 4, app.taper); y += 30;
    y += 14;
    font_draw_in(ui.dl, F_UI, rect(x, y, w, 16), ALIGN_LEFT, "Pressure \xE2\x86\x92 size", C_TX3); y += 22;
    curve_editor(rect(x, y, w, 104));
}

static int lib_drag = 0;   // being dragged to the stage: a symbol id, or minus a bitmap id

static int uses_of(Doc *d, int key) {   // placements of a symbol (key > 0) or a bitmap (key < 0)
    int n = 0;
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++) {
        if (!d->drawings[i].used) continue;
        for (ptrdiff_t k = 0; k < arrlen(d->drawings[i].items); k++) {
            Item *it = &d->drawings[i].items[k];
            if (key > 0 ? it->kind == IT_INSTANCE && it->sym == key : (it->kind == IT_BITMAP || it->kind == IT_FILL) && it->sym == -key) n++;
        }
    }
    return n;
}

// the Library: symbols, then bitmaps. Ctrl adds or takes away one, Shift picks a run; the symbols
// picked are what Export Symbols exports, in the order they were picked
static int *LIB_SEL;   // symbol ids, and bitmap ids as negatives
static int lib_anchor, lib_rename, lib_menu;
static float lib_scroll;

void library_forget(void) { arrsetlen(LIB_SEL, 0); lib_rename = 0; lib_scroll = 0; lib_drag = 0; }
static int lib_index(int key) { for (ptrdiff_t i = 0; i < arrlen(LIB_SEL); i++) if (LIB_SEL[i] == key) return (int)i; return -1; }
static bool lib_exists(Doc *d, int key) {
    if (key > 0) return doc_symbol(d, key) != NULL;
    for (ptrdiff_t i = 0; i < arrlen(d->bitmaps); i++) if (d->bitmaps[i] == -key) return true;
    return false;
}
int library_selection(int *out, int cap) {
    Doc *d = app.doc;
    int n = 0;
    for (ptrdiff_t i = 0; i < arrlen(LIB_SEL) && n < cap; i++) if (LIB_SEL[i] > 0 && doc_symbol(d, LIB_SEL[i])) out[n++] = LIB_SEL[i];
    return n;
}
static int lib_key(Doc *d, int row) { int ns = (int)arrlen(d->symbols); return row < ns ? d->symbols[row].id : -d->bitmaps[row - ns]; }
static const char *lib_name(Doc *d, int key) {
    if (key > 0) { Symbol *s = doc_symbol(d, key); return s ? s->name : ""; }
    Bitmap *b = bitmap_get(-key);
    return b ? b->name : "";
}

static void bitmap_thumb(Rect r, int id, Color tint) {
    Bitmap *b = bitmap_get(id);
    u32 view = b ? bitmap_texture(id) : 0;
    if (!view) { icon_draw(ui.dl, IC_REFERENCE, v2(r.x + r.w / 2, r.y + r.h / 2), ICON_M, false, tint); return; }
    float k = MIN(r.w / b->w, r.h / b->h), w = MAX(1, b->w * k), h = MAX(1, b->h * k);
    Rect t = {roundf(r.x + (r.w - w) / 2), roundf(r.y + (r.h - h) / 2), roundf(w), roundf(h)};
    dl_checker(ui.dl, t, 0);
    dl_image(ui.dl, t, view, 0xffffffff, 0, 0, 1, 1);
}

static void library_tab(Rect r) {
    Doc *d = app.doc;
    for (ptrdiff_t i = arrlen(LIB_SEL) - 1; i >= 0; i--) if (!lib_exists(d, LIB_SEL[i])) arrdel(LIB_SEL, i);   // after undo / delete
    int ns = (int)arrlen(d->symbols), nb = (int)arrlen(d->bitmaps), n = ns + nb;
    if (!n) {
        Rect c = {r.x, r.y + 40, r.w, 60};
        font_draw_in(ui.dl, F_UI_MED, rect(c.x, c.y, c.w, 20), ALIGN_CENTER, "The library is empty", C_TX2);
        font_draw_in(ui.dl, F_UI, rect(c.x, c.y + 22, c.w, 20), ALIGN_CENTER, "F8 makes a symbol, Ctrl+R imports an image", C_TX3);
        return;
    }
    char head[64];
    int picked = (int)arrlen(LIB_SEL);
    if (picked) snprintf(head, sizeof(head), "%d of %d picked", picked, n);
    else if (nb && ns) snprintf(head, sizeof(head), "%d symbol%s, %d image%s", ns, ns == 1 ? "" : "s", nb, nb == 1 ? "" : "s");
    else if (nb) snprintf(head, sizeof(head), "%d image%s", nb, nb == 1 ? "" : "s");
    else snprintf(head, sizeof(head), "%d symbol%s", ns, ns == 1 ? "" : "s");
    font_draw_in(ui.dl, F_SMALL, rect(r.x + 16, r.y + 6, r.w - 140, 22), ALIGN_LEFT, head, C_TX3);
    Rect xb = {r.x + r.w - 8 - 112, r.y + 4, 112, 26};
    if (ns) {
        if (ui_text_button(ui_id("lib-export"), xb, "Export\xE2\x80\xA6", false)) { int ids[64], k = library_selection(ids, 64); exporter_open(ids, k); }
        ui_tooltip(ui_id("lib-export"), xb, library_selection((int[1]){0}, 1) ? "Export the picked symbols as animations" : "Export every symbol as an animation");
    }
    Rect list = {r.x, r.y + 36, r.w, r.h - 36};
    float content = n * 36.0f + 8;
    if (rect_has(list, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y) lib_scroll -= ui.in.wheel_y * 36;
    lib_scroll = clampf(lib_scroll, 0, MAX(0, content - list.h));
    dl_push_clip(ui.dl, list);
    float y = list.y + 2 - lib_scroll;
    for (int i = 0; i < n; i++, y += 36) {
        int key = lib_key(d, i);
        Rect row = {r.x + 8, y, r.w - 16, 34};
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        UiId id = ui_idi("lib", key);
        UiResp re = ui_interact(id, row);
        bool editing = key > 0 && doc_editing(d) == key, on = lib_index(key) >= 0;
        if (on) { dl_rrect(ui.dl, row, 7, C_AC2); dl_rrect_border(ui.dl, row, 7, 1, C_AC3); }
        else if (editing) dl_rrect_border(ui.dl, row, 7, 1, C_LINE2);
        else if (re.hover) dl_rrect(ui.dl, row, 7, C_S2);
        Color tc = re.hover || on || editing ? C_TX : C_TX2;
        if (key > 0) icon_draw(ui.dl, IC_SYMBOL, v2(row.x + 16, row.y + 17), ICON_M, false, tc);
        else bitmap_thumb(rect(row.x + 4, row.y + 5, 24, 24), -key, tc);
        UiId rid = ui_idi("lib-rename", key);
        char *nm = key > 0 ? doc_symbol(d, key)->name : bitmap_get(-key) ? bitmap_get(-key)->name : NULL;
        if (lib_rename == key && nm) {
            if (ui.focus != rid) { ui_focus_text(rid, nm); }
            if (ui_text_field(rid, rect(row.x + 30, row.y + 4, row.w - 80, 26), nm, 64, NULL)) { lib_rename = 0; doc_touch(d); }
            if (ui.focus != rid) lib_rename = 0;
        } else font_draw_in(ui.dl, F_UI, rect(row.x + 34, row.y, row.w - 100, row.h), ALIGN_LEFT, lib_name(d, key), tc);
        char u[16]; snprintf(u, sizeof(u), "\xC3\x97%d", uses_of(d, key));
        font_draw_in(ui.dl, F_MONO, rect(row.x, row.y, row.w - 10, row.h), ALIGN_RIGHT, u, C_TX3);
        if (key > 0) ui_tooltip(id, row, "Drag onto the stage, double-click to edit. Ctrl or Shift picks several");
        else if (re.hover) {
            static char tip[96];
            Bitmap *bm = bitmap_get(-key);
            snprintf(tip, sizeof(tip), "%d \xC3\x97 %d px. Drag onto the stage", bm ? bm->w : 0, bm ? bm->h : 0);
            ui_tooltip(id, row, tip);
        }
        if (re.dbl && key > 0) { symbol_edit(key); lib_drag = 0; continue; }
        if (re.pressed) {
            u32 mods = ui.in.mods;
            if (mods & MOD_CTRL) { int k = lib_index(key); if (k >= 0) arrdel(LIB_SEL, k); else arrput(LIB_SEL, key); lib_anchor = key; }
            else if (mods & MOD_SHIFT) {
                int a0 = i;
                for (int k = 0; k < n; k++) if (lib_key(d, k) == lib_anchor) a0 = k;
                for (int k = MIN(a0, i); k <= MAX(a0, i); k++) if (lib_index(lib_key(d, k)) < 0) arrput(LIB_SEL, lib_key(d, k));
            } else {
                if (lib_index(key) < 0) { arrsetlen(LIB_SEL, 0); arrput(LIB_SEL, key); }
                lib_anchor = key;
                lib_drag = key;   // a drag puts it on the stage
            }
        }
        if (re.released && !(ui.in.mods & (MOD_CTRL | MOD_SHIFT)) && !rect_has(stage_rect(), ui.in.mouse) && arrlen(LIB_SEL) > 1 && re.clicked) {
            arrsetlen(LIB_SEL, 0); arrput(LIB_SEL, key);   // a plain click in a multiple pick keeps just that one
        }
        if (re.right) {
            if (lib_index(key) < 0) { arrsetlen(LIB_SEL, 0); arrput(LIB_SEL, key); }
            lib_menu = key;
            ui_open_popup(ui_id("lib-menu"), rect(ui.in.mouse.x, ui.in.mouse.y, 1, 1));
        }
    }
    dl_pop_clip(ui.dl);
    if (content > list.h) {
        float th = list.h * list.h / content, ty = list.y + (list.h - th) * lib_scroll / (content - list.h);
        dl_rrect(ui.dl, rect(r.x + r.w - 5, ty, 3, th), 1.5f, hexa(0xffffff, 0x22));
    }
    if (ui_popup_begin(ui_id("lib-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 240, 10 + 28 * 5 + 9 * 2))) {
        int k = (int)arrlen(LIB_SEL), nsym = library_selection((int[64]){0}, 64);
        bool one_bitmap = k == 1 && lib_menu < 0;
        char ex[48]; snprintf(ex, sizeof(ex), nsym > 1 ? "Export %d Symbols\xE2\x80\xA6" : "Export Symbol\xE2\x80\xA6", nsym);
        if (ui_menu_item_ic(IC_EXPORT, ex, act_keys(ACT_EXPORT), nsym > 0, false)) { int ids[64], m = library_selection(ids, 64); exporter_open(ids, m); }
        ui_menu_sep();
        if (one_bitmap) {
            Bitmap *bm = bitmap_get(-lib_menu);
            if (ui_menu_item("Place on Stage", NULL, bm != NULL, false)) bitmap_place(-lib_menu, v2(-1, -1));
            if (ui_menu_item("Smoothing", NULL, bm != NULL, bm && bm->smooth)) { bm->smooth = !bm->smooth; doc_touch(d); }
        } else {
            if (ui_menu_item("Edit", NULL, k == 1 && lib_menu > 0, false)) symbol_edit(lib_menu);
            if (ui_menu_item("Place on Stage", NULL, k == 1 && lib_menu > 0, false)) library_place(lib_menu, v2(-1, -1));
        }
        if (ui_menu_item("Rename", NULL, k == 1, false)) lib_rename = lib_menu;
        ui_menu_sep();
        char del[48]; snprintf(del, sizeof(del), k > 1 ? "Delete %d Items" : "Delete", k);
        if (ui_menu_item(del, NULL, k > 0, false)) {
            int kept = 0;
            app_commit();
            int *keys = NULL;
            for (ptrdiff_t q = 0; q < arrlen(LIB_SEL); q++) arrput(keys, LIB_SEL[q]);
            for (ptrdiff_t q = 0; q < arrlen(keys); q++) if (!(keys[q] > 0 ? doc_delete_symbol(app.doc, keys[q]) : doc_delete_bitmap(app.doc, -keys[q]))) kept++;
            arrfree(keys);
            arrsetlen(LIB_SEL, 0);
            if (kept) app_toast("%d item%s still on the stage %s kept. Delete or break apart those first", kept, kept == 1 ? "" : "s", kept == 1 ? "was" : "were");
        }
        ui_popup_end();
    }
}

static void library_drag(void) {
    if (!lib_drag) return;
    if (ui.in.down[0]) {
        Doc *d = app.doc;
        if (!lib_exists(d, lib_drag)) { lib_drag = 0; return; }
        const char *name = lib_name(d, lib_drag);
        ui_overlay(true);
        float w = font_width(F_UI_MED, name) + 40;
        Rect r = {ui.in.mouse.x + 12, ui.in.mouse.y + 10, w, 28};
        dl_shadow(ui.dl, rect(r.x, r.y + 4, r.w, r.h), 7, 12, hexa(0x000000, 0x90));
        dl_rrect(ui.dl, r, 7, C_S4);
        if (lib_drag > 0) icon_draw(ui.dl, IC_SYMBOL, v2(r.x + 15, r.y + 14), ICON_S, false, C_TX);
        else bitmap_thumb(rect(r.x + 6, r.y + 5, 18, 18), -lib_drag, C_TX);
        font_draw_in(ui.dl, F_UI_MED, rect(r.x + 28, r.y, r.w, r.h), ALIGN_LEFT, name, C_TX);
        ui_overlay(false);
        ui_cursor(SAPP_MOUSECURSOR_POINTING_HAND);
    } else {
        if (rect_has(stage_rect(), ui.in.mouse)) { if (lib_drag > 0) library_place(lib_drag, ui.in.mouse); else bitmap_place(-lib_drag, ui.in.mouse); }
        lib_drag = 0;
    }
}

static void prow(float *y, Rect r, const char *label) { font_draw_in(ui.dl, F_UI, rect(r.x + 16, *y, 90, 30), ALIGN_LEFT, label, C_TX3); }
static void section(float *y, Rect r, const char *title) {
    font_draw_in(ui.dl, F_UI_MED, rect(r.x + 16, *y, r.w - 32, 18), ALIGN_LEFT, title, C_TX2);
    *y += 26;
}

// a labelled slider row; returns true when the value changed (rounded to step)
static bool srow(float *y, Rect r, const char *label, UiId id, float *v, float lo, float hi, float step, float shown, const char *fmt) {
    float fx = r.x + 16 + 92, fw = r.w - 32 - 92;
    prow(y, r, label);
    float nv = ui_slider(id, rect(fx, *y + 5, fw - 54, 20), *v, lo, hi);
    nv = roundf(nv / step) * step;
    bool ch = fabsf(nv - *v) > step * 0.25f;
    if (ch) *v = nv;
    char t[24]; snprintf(t, sizeof(t), fmt, *v * shown);
    font_draw_in(ui.dl, F_MONO_MED, rect(fx + fw - 50, *y, 50, 30), ALIGN_RIGHT, t, C_TX);
    *y += 34;
    return ch;
}
static bool swatch_row(float *y, Rect r, const char *label, UiId id, Color c) {   // click: take the fill colour
    float fx = r.x + 16 + 92;
    prow(y, r, label);
    Rect sw = {fx, *y + 4, 44, 22};
    UiResp re = ui_interact(id, sw);
    dl_checker(ui.dl, sw, 5);
    dl_rrect(ui.dl, sw, 5, c);
    dl_rrect_border(ui.dl, sw, 5, 1, re.hover ? C_TX2 : C_LINE2);
    char hx[12]; snprintf(hx, sizeof(hx), "#%02X%02X%02X", c & 255, (c >> 8) & 255, (c >> 16) & 255);
    font_draw_in(ui.dl, F_MONO, rect(sw.x + sw.w + 10, *y, 90, 30), ALIGN_LEFT, hx, C_TX2);
    ui_tooltip(id, sw, "Click to use the fill color");
    *y += 34;
    return re.clicked;
}

// Animate's colour effect and filters of a symbol instance
static void instance_effects(float *yy, Rect r, Item *it) {
    Doc *d = app.doc;
    float y = *yy, fx = r.x + 16 + 92, fw = r.w - 32 - 92;
    static const char *cxn[] = {"None", "Brightness", "Tint", "Advanced"};
    prow(&y, r, "Color");
    int ck = ui_dropdown(ui_id("inst-cx"), rect(fx, y, fw, 30), cxn, 4, it->cx.kind);
    if (ck != it->cx.kind) {
        app_commit(); it = select_first();
        if (ck == CX_ADVANCED) colorfx_update(&it->cx);   // starts from what it looked like
        if (ck == CX_TINT && it->cx.kind != CX_TINT) { it->cx.amount = 0.5f; it->cx.tint = app.fill; }
        if (ck == CX_BRIGHTNESS && it->cx.kind != CX_BRIGHTNESS) it->cx.amount = 0;
        it->cx.kind = (u8)ck;
        colorfx_update(&it->cx);
        doc_touch(d);
    }
    y += 40;
    if (it->cx.kind == CX_BRIGHTNESS) { if (srow(&y, r, "Brightness", ui_id("cx-b"), &it->cx.amount, -1, 1, 0.01f, 100, "%.0f%%")) doc_touch(d); }
    else if (it->cx.kind == CX_TINT) {
        if (swatch_row(&y, r, "Tint", ui_id("cx-tc"), it->cx.tint)) { app_commit(); it = select_first(); it->cx.tint = app.fill | 0xff000000u; doc_touch(d); }
        if (srow(&y, r, "Amount", ui_id("cx-ta"), &it->cx.amount, 0, 1, 0.01f, 100, "%.0f%%")) doc_touch(d);
    } else if (it->cx.kind == CX_ADVANCED) {
        static const char *mn[3] = {"Red \xC3\x97", "Green \xC3\x97", "Blue \xC3\x97"}, *on[3] = {"Red +", "Green +", "Blue +"};
        for (int i = 0; i < 3; i++) if (srow(&y, r, mn[i], ui_idi("cx-m", i), &it->cx.mul[i], -1, 1, 0.01f, 100, "%.0f%%")) doc_touch(d);
        for (int i = 0; i < 3; i++) if (srow(&y, r, on[i], ui_idi("cx-a", i), &it->cx.add[i], -1, 1, 1 / 255.0f, 255, "%.0f")) doc_touch(d);
    }
    y += 6;
    // filters
    font_draw_in(ui.dl, F_UI_MED, rect(r.x + 16, y, r.w - 32, 28), ALIGN_LEFT, "Filters", C_TX2);
    Rect add = {r.x + r.w - 16 - 28, y, 28, 28};
    UiResp ar = ui_interact(ui_id("fx-add"), add);
    if (ar.hover || ui_popup_is_open(ui_id("fx-add"))) dl_rrect(ui.dl, add, 6, C_S3);
    icon_draw(ui.dl, IC_PLUS, v2(add.x + 14, add.y + 14), ICON_S, false, ar.hover ? C_TX : C_TX2);
    ui_tooltip(ui_id("fx-add"), add, "Add a filter");
    if (ar.clicked) ui_open_popup(ui_id("fx-add"), add);
    if (ui_popup_begin(ui_id("fx-add"), rect(add.x + add.w - 200, add.y + add.h + 4, 200, 10 + 28 * FX_COUNT))) {
        for (int t = 0; t < FX_COUNT; t++) if (ui_menu_item(FILTER_NAMES[t], NULL, true, false)) {
            app_commit(); it = select_first();
            Filter f = {.type = (u8)t, .quality = 1, .bx = 5, .by = 5, .strength = 1, .angle = 45, .dist = 5,
                        .color = t == FX_SHADOW ? hexa(0x000000, 0xff) : hex(0xff0000)};
            arrput(it->filters, f);
            doc_touch(d);
        }
        ui_popup_end();
    }
    y += 34;
    if (!arrlen(it->filters)) { font_draw_in(ui.dl, F_SMALL, rect(r.x + 16, y - 6, r.w - 32, 20), ALIGN_LEFT, "None. Blur, glow, drop shadow and color adjustment", C_TX3); y += 18; }
    int remove = -1;
    for (ptrdiff_t i = 0; i < arrlen(it->filters); i++) {
        Filter *f = &it->filters[i];
        int base = (int)i * 32;
        Rect head = {r.x + 12, y, r.w - 24, 28};
        dl_rrect(ui.dl, head, 6, C_S2);
        bool off = f->flags & FXF_OFF;
        Rect eye = {head.x + 4, head.y + 2, 24, 24}, bin = {head.x + head.w - 28, head.y + 2, 24, 24};
        UiResp er = ui_interact(ui_idi("fx-eye", base), eye), br = ui_interact(ui_idi("fx-del", base), bin);
        icon_draw(ui.dl, off ? IC_EYE_OFF : IC_EYE, v2(eye.x + 12, eye.y + 12), ICON_S, false, er.hover ? C_TX : C_TX2);
        icon_draw(ui.dl, IC_TRASH, v2(bin.x + 12, bin.y + 12), ICON_S, false, br.hover ? C_TX : C_TX3);
        font_draw_in(ui.dl, F_UI_MED, rect(head.x + 34, head.y, head.w - 70, head.h), ALIGN_LEFT, FILTER_NAMES[f->type < FX_COUNT ? f->type : 0], off ? C_TX3 : C_TX);
        ui_tooltip(ui_idi("fx-eye", base), eye, off ? "Turn the filter on" : "Turn the filter off");
        if (er.clicked) { f->flags ^= FXF_OFF; doc_touch(d); }
        if (br.clicked) remove = (int)i;
        y += 36;
        bool ch = false;
        if (f->type == FX_ADJUST) {
            static const char *an[4] = {"Brightness", "Contrast", "Saturation", "Hue"};
            for (int k = 0; k < 4; k++) ch |= srow(&y, r, an[k], ui_idi("fx-adj", base + k), &f->adj[k], k == 3 ? -180 : -100, k == 3 ? 180 : 100, 1, 1, k == 3 ? "%.0f\xC2\xB0" : "%.0f");
        } else {
            ch |= srow(&y, r, "Blur X", ui_idi("fx-bx", base), &f->bx, 0, 100, 0.5f, 1, "%.1f");
            ch |= srow(&y, r, "Blur Y", ui_idi("fx-by", base), &f->by, 0, 100, 0.5f, 1, "%.1f");
            static const char *qn[3] = {"Low", "Medium", "High"};
            prow(&y, r, "Quality");
            int qs = ui_segmented(ui_idi("fx-q", base), rect(fx, y, fw, 28), qn, 3, CLAMP(f->quality, 1, 3) - 1);
            if (qs + 1 != f->quality) { f->quality = (u8)(qs + 1); ch = true; }
            y += 36;
            if (f->type != FX_BLUR) {
                ch |= srow(&y, r, "Strength", ui_idi("fx-st", base), &f->strength, 0, 10, 0.01f, 100, "%.0f%%");
                if (swatch_row(&y, r, "Color", ui_idi("fx-col", base), f->color)) { f->color = (app.fill & 0x00ffffffu) | (f->color & 0xff000000u); ch = true; }
                if (f->type == FX_SHADOW) {
                    ch |= srow(&y, r, "Angle", ui_idi("fx-an", base), &f->angle, 0, 360, 1, 1, "%.0f\xC2\xB0");
                    ch |= srow(&y, r, "Distance", ui_idi("fx-di", base), &f->dist, -100, 100, 0.5f, 1, "%.1f");
                }
                float cx = fx;
                if (ui_chip("Inner", rect(cx, y, 64, 26), f->flags & FXF_INNER)) { f->flags ^= FXF_INNER; ch = true; }
                cx += 70;
                if (ui_chip("Knockout", rect(cx, y, 84, 26), f->flags & FXF_KNOCKOUT)) { f->flags ^= FXF_KNOCKOUT; ch = true; }
                cx += 90;
                if (f->type == FX_SHADOW && ui_chip("Hide object", rect(cx, y, 100, 26), f->flags & FXF_HIDE)) { f->flags ^= FXF_HIDE; ch = true; }
                y += 34;
            }
        }
        if (ch) doc_touch(d);
        y += 6;
    }
    if (remove >= 0) { app_commit(); it = select_first(); arrdel(it->filters, remove); doc_touch(d); }
    *yy = y;
}

static void props_tab(Rect r) {
    Doc *d = app.doc;
    Layer *l = doc_active(d);
    // the panel scrolls; the mouse only counts inside it (unless a menu of it is open)
    static float scroll, content;
    Rect view = r;
    if (rect_has(view, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y) scroll -= ui.in.wheel_y * 40;
    scroll = clampf(scroll, 0, MAX(0, content - view.h));
    V2 mouse = ui.in.mouse;
    if (!ui.popup && !rect_has(view, mouse)) ui.in.mouse = v2(-1e6f, -1e6f);
    dl_push_clip(ui.dl, view);
    float y = r.y + 16 - scroll, fx = r.x + 16 + 92, fw = r.w - 32 - 92;
    Item *it = select_count() == 1 ? select_first() : NULL;
    if (it && it->kind == IT_BITMAP && bitmap_get(it->sym)) {
        Bitmap *bm = bitmap_get(it->sym);
        char t[96]; snprintf(t, sizeof(t), "%s, %d \xC3\x97 %d image", bm->name, bm->w, bm->h);
        section(&y, r, t);
        prow(&y, r, "Blending");
        int bl = ui_dropdown(ui_id("bmp-blend"), rect(fx, y, fw, 30), BLEND_NAMES, BLEND_COUNT, it->blend);
        if (bl != it->blend) { app_commit(); it = select_first(); it->blend = (u8)bl; doc_touch(d); }
        y += 40;
        prow(&y, r, "Alpha");
        float al = ui_slider(ui_id("bmp-alpha"), rect(fx, y + 5, fw - 48, 20), it->alpha * 100, 0, 100);
        if (fabsf(al - it->alpha * 100) > 0.01f) { it->alpha = roundf(al) / 100; doc_touch(d); }
        char s2[16]; snprintf(s2, sizeof(s2), "%d%%", (int)roundf(it->alpha * 100));
        font_draw_in(ui.dl, F_MONO_MED, rect(fx + fw - 42, y, 42, 30), ALIGN_RIGHT, s2, C_TX);
        y += 40;
        prow(&y, r, "Smoothing");
        if (ui_chip(bm->smooth ? "On" : "Off", rect(fx, y, 64, 30), bm->smooth)) { bm->smooth = !bm->smooth; doc_touch(d); }
        y += 50;
    }
    if (it && it->kind == IT_INSTANCE && doc_symbol(d, it->sym)) {
        Symbol *sy = doc_symbol(d, it->sym);
        char t[96]; snprintf(t, sizeof(t), "%s, %s", sy->name, sy->type == SYM_MOVIECLIP ? "movie clip" : "graphic symbol");
        section(&y, r, t);
        prow(&y, r, "Blending");
        int bl = ui_dropdown(ui_id("inst-blend"), rect(fx, y, fw, 30), BLEND_NAMES, BLEND_COUNT, it->blend);
        if (bl != it->blend) { app_commit(); it = select_first(); it->blend = (u8)bl; doc_touch(d); }
        y += 40;
        static const char *loops[] = {"Loop", "Play once", "Single frame"};
        prow(&y, r, "Looping");
        int lp = ui_dropdown(ui_id("inst-loop"), rect(fx, y, fw, 30), loops, 3, it->loop);
        if (lp != it->loop) { app_commit(); it = select_first(); it->loop = (u8)lp; doc_touch(d); }
        y += 40;
        prow(&y, r, "First frame");
        int len = doc_symbol_len(d, it->sym);
        float ff = ui_slider(ui_id("inst-first"), rect(fx, y + 5, fw - 48, 20), (float)it->first, 1, (float)MAX(2, len));
        if ((int)roundf(ff) != it->first) { it->first = CLAMP((int)roundf(ff), 1, len); doc_touch(d); }
        char s2[16]; snprintf(s2, sizeof(s2), "%d", it->first);
        font_draw_in(ui.dl, F_MONO_MED, rect(fx + fw - 42, y, 42, 30), ALIGN_RIGHT, s2, C_TX);
        y += 40;
        prow(&y, r, "Alpha");
        float al = ui_slider(ui_id("inst-alpha"), rect(fx, y + 5, fw - 48, 20), it->alpha * 100, 0, 100);
        if (fabsf(al - it->alpha * 100) > 0.01f) { it->alpha = roundf(al) / 100; doc_touch(d); }
        snprintf(s2, sizeof(s2), "%d%%", (int)roundf(it->alpha * 100));
        font_draw_in(ui.dl, F_MONO_MED, rect(fx + fw - 42, y, 42, 30), ALIGN_RIGHT, s2, C_TX);
        y += 40;
        instance_effects(&y, r, it);
        y += 14;
    }
    if (l && l->type != LT_FOLDER) {
        Key *k = doc_key_at(l, doc_tl(d)->frame);
        char t[64];
        if (k) snprintf(t, sizeof(t), "Frame %d: %s", doc_tl(d)->frame, k->d >= 0 || !doc_drawable(l) ? "keyframe" : "blank keyframe");
        else if (doc_key_for(l, doc_tl(d)->frame)) snprintf(t, sizeof(t), "Frame %d: held from %d", doc_tl(d)->frame, doc_key_for(l, doc_tl(d)->frame)->f);
        else snprintf(t, sizeof(t), "Frame %d: empty", doc_tl(d)->frame);
        section(&y, r, t);
        if (k) {
            prow(&y, r, "Label");
            char buf[32]; snprintf(buf, sizeof(buf), "%s", k->label);
            if (ui_text_field(ui_id("label"), rect(fx, y, fw, 30), buf, sizeof(buf), NULL)) { app_commit(); k = doc_key_at(doc_active(app.doc), doc_tl(app.doc)->frame); snprintf(k->label, sizeof(k->label), "%s", buf); doc_touch(app.doc); }
            y += 40;
            static const char *tw[] = {"None", "Classic", "Shape"};
            prow(&y, r, "Tween");
            int sel = ui_dropdown(ui_id("tween"), rect(fx, y, fw, 30), tw, 3, k->tw);
            if (sel != k->tw) { app_commit(); k = doc_key_at(doc_active(app.doc), doc_tl(app.doc)->frame); k->tw = (u8)sel; doc_touch(app.doc); }
            y += 40;
        }
        y += 10;
    }
    if (l) {
        char t[80]; snprintf(t, sizeof(t), "%s (%s)", l->name, LAYER_TYPE_NAMES[l->type]);
        section(&y, r, t);
        prow(&y, r, "Opacity");
        float op = ui_slider(ui_id("layer-opacity"), rect(fx, y + 5, fw - 48, 20), l->opacity * 100, 0, 100);
        if (fabsf(op - l->opacity * 100) > 0.01f) { l->opacity = roundf(op) / 100; doc_touch(d); }
        char s[8]; snprintf(s, sizeof(s), "%d%%", (int)roundf(l->opacity * 100));
        font_draw_in(ui.dl, F_MONO_MED, rect(fx + fw - 42, y, 42, 30), ALIGN_RIGHT, s, C_TX);
        y += 40;
        prow(&y, r, "Blending");
        int b = ui_dropdown(ui_id("blend"), rect(fx, y, fw, 30), BLEND_NAMES, BLEND_COUNT, l->blend);
        if (b != l->blend) { l->blend = (u8)b; doc_touch(d); }
        if (l->transparent) {   // Animate's layer transparency: while editing only
            y += 40;
            float pct = l->see_pct ? l->see_pct : 50;
            if (srow(&y, r, "See through", ui_id("layer-see"), &pct, 1, 100, 1, 1, "%.0f%%")) { l->see_pct = (u8)pct; doc_touch(d); }
            y -= 34;
        }
        y += 50;
    }
    section(&y, r, "Document");
    char s[48];
    prow(&y, r, "Size"); snprintf(s, sizeof(s), "%d \xC3\x97 %d", d->w, d->h);
    font_draw_in(ui.dl, F_MONO_MED, rect(fx, y, fw, 30), ALIGN_LEFT, s, C_TX); y += 32;
    prow(&y, r, "Frame rate"); snprintf(s, sizeof(s), "%d fps", d->fps);
    font_draw_in(ui.dl, F_MONO_MED, rect(fx, y, fw, 30), ALIGN_LEFT, s, C_TX); y += 32;
    prow(&y, r, "Length"); snprintf(s, sizeof(s), "%d frames, %.2f s", doc_tl(d)->len, doc_tl(d)->len / (float)d->fps);
    font_draw_in(ui.dl, F_MONO_MED, rect(fx, y, fw, 30), ALIGN_LEFT, s, C_TX);
    y += 46;
    dl_pop_clip(ui.dl);
    ui.in.mouse = mouse;
    content = y + scroll - r.y;
    if (content > view.h) {
        float th = view.h * view.h / content, ty = view.y + (view.h - th) * scroll / (content - view.h);
        dl_rrect(ui.dl, rect(r.x + r.w - 5, ty, 3, th), 1.5f, hexa(0xffffff, 0x22));
    }
}

void right_panel(Rect r) {
    static const char *tabs[] = {"Brushes", "Library", "Properties"};
    Rect tb = cut_top(&r, 40);
    dl_rect(ui.dl, rect(tb.x, tb.y + tb.h - 1, tb.w, 1), C_LINE);
    float x = tb.x + 10;
    for (int i = 0; i < 3; i++) {
        float w = font_width(F_UI_MED, tabs[i]) + 18;
        Rect t = {x, tb.y, w, tb.h - 1};
        UiResp re = ui_interact(ui_idi("tab", i), t);
        bool on = app.right_tab == i;
        font_draw_in(ui.dl, F_UI_MED, t, ALIGN_CENTER, tabs[i], on ? C_TX : re.hover ? C_TX2 : C_TX3);
        if (on) dl_rrect(ui.dl, rect(t.x + 9, tb.y + tb.h - 2, w - 18, 2), 1, C_AC);
        if (re.clicked) app.right_tab = i;
        x += w + 2;
    }
    if (app.right_tab == 0) {
        Rect b = {tb.x + tb.w - 38, tb.y + 6, 28, 28};
        UiId id = ui_id("import-sut");
        UiResp re = ui_interact(id, b);
        if (re.hover) dl_rrect(ui.dl, b, 7, C_S2);
        icon_draw(ui.dl, IC_IMPORT, v2(b.x + 14, b.y + 14), ICON_M, false, re.hover ? C_TX : C_TX2);
        ui_tooltip(id, b, "Import Clip Studio brush (.sut)");
        if (re.clicked) app_import_brushes();
    }
    dl_push_clip(ui.dl, r);
    if (app.right_tab == 0) brushes_tab(r);
    else if (app.right_tab == 1) library_tab(r);
    else props_tab(r);
    dl_pop_clip(ui.dl);
    library_drag();
}
