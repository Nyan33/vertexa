// Timeline: layer list + frame grid drawn on the pixel grid
// (a cell = 1 px boundary + odd interior, so keys, numbers and the playhead sit on a true center pixel).
#include "app/app.h"
#include "sokol/sokol_app.h"
#include "audio/audio.h"

enum { DRAG_NONE, DRAG_SCRUB, DRAG_LOOP_A, DRAG_LOOP_B, DRAG_LOOP_MOVE, DRAG_END, DRAG_SEL, DRAG_SPLIT,
       DRAG_SEL_EXTRA, DRAG_MOVE, DRAG_PAN, DRAG_HBAR, DRAG_VBAR };

static struct {
    float sx, sy;          // scroll in px
    int cw;                // cell width (even: 1 px boundary + odd interior)
    int drag, anchor_r, anchor_f, grab_f, la0, lb0;
    int rename_id; char rename[48];
    int menu_r, menu_f;
    // frames: Ctrl adds ranges next to the main one (doc->sel); a selection can be dragged along
    Sel extra[32]; int nextra, xanchor_r, xanchor_f;
    int move_df;
    // layers: Ctrl / Shift selection, drag and drop
    int *lsel;
    int lpress; float lpress_y; bool ldragging; int drop_ref, drop_where, drop_ok;
    // navigation
    int last_frame; V2 pan_m; float pan_sx, pan_sy, bar_grab;
} T = {.cw = 18, .last_frame = -1};

static int lsel_index(int id) { for (ptrdiff_t i = 0; i < arrlen(T.lsel); i++) if (T.lsel[i] == id) return (int)i; return -1; }
static void lsel_only(int id) { arrsetlen(T.lsel, 0); arrput(T.lsel, id); }
static void clear_frames(void) { app.doc->sel.on = false; T.nextra = 0; }

static float FX0;          // frames area left edge
static float X(int f) { return FX0 + (f - 1) * T.cw - T.sx; }
static float MID(int f) { return X(f) + T.cw / 2; }

static void R(Rect r, Color c) { dl_rect(ui.dl, r, c); }
static void RR(float x, float y, float w, float h, float rad, Color c) { if (w > 0 && h > 0) dl_rrect(ui.dl, rect(x, y, w, h), rad, c); }

static Color with_alpha(Color c, float a) { return col_alpha(c, ((c >> 24) / 255.0f) * a); }
static Color light(Color c, float s, float v) { float h = 0, cs, cv; color_to_hsv(c, &h, &cs, &cv); return color_from_hsv(h, MIN(cs, s), v); }

// without a frame selection an edit goes to the current frame of the selected layers (folders opened up)
static int layer_targets(Layer **out, int cap) {
    Doc *d = app.doc;
    Timeline *t = doc_tl(d);
    Sel keep = d->sel;
    int rows[512], nr = doc_rows(d, rows, 512), n = 0;
    for (int r = 0; r < nr; r++) {
        Layer *l = &t->layers[rows[r]];
        if (l->id != t->active && lsel_index(l->id) < 0) continue;
        d->sel = (Sel){true, r, r, 0, 0};
        Layer *tmp[256];
        int k = doc_targets(d, tmp, 256);
        for (int j = 0; j < k && n < cap; j++) {
            bool dup = false;
            for (int q = 0; q < n; q++) dup |= out[q] == tmp[j];
            if (!dup) out[n++] = tmp[j];
        }
    }
    d->sel = keep;
    return n;
}

static void run_op(int which, Layer **ls, int n, int f0, int f1) {
    Doc *d = app.doc;
    switch (which) {
    case 0: doc_insert_frames(d, ls, n, f0, f1 - f0 + 1); break;
    case 1: doc_remove_frames(d, ls, n, f0, f1); break;
    case 2: doc_insert_key(d, ls, n, f0, false); break;
    case 3: doc_insert_key(d, ls, n, f0, true); break;
    case 4: doc_clear_key(d, ls, n, f0); break;
    case 5: doc_set_tween(d, ls, n, f0, f1, TW_CLASSIC); break;
    case 6: doc_set_tween(d, ls, n, f0, f1, TW_SHAPE); break;
    case 7: doc_set_tween(d, ls, n, f0, f1, TW_NONE); break;
    }
}

// every selected range (right-most first, so earlier ranges keep their frame numbers), or the
// current frame of the selected layers
static void op(int which) {
    app_commit();
    Doc *d = app.doc;
    Sel rs[33]; int nr = 0;
    if (d->sel.on) rs[nr++] = d->sel;
    for (int i = 0; i < T.nextra; i++) rs[nr++] = T.extra[i];
    if (!nr) {
        Layer *ls[256];
        int n = layer_targets(ls, 256);
        if (!n) { app_toast("The layer is locked"); return; }
        run_op(which, ls, n, doc_tl(d)->frame, doc_tl(d)->frame);
        return;
    }
    for (int i = 1; i < nr; i++) for (int j = i; j > 0 && rs[j - 1].f0 < rs[j].f0; j--) { Sel x = rs[j]; rs[j] = rs[j - 1]; rs[j - 1] = x; }
    Sel keep = d->sel;
    int done = 0;
    for (int i = 0; i < nr; i++) {
        d->sel = rs[i]; d->sel.on = true;
        Layer *ls[256];
        int n = doc_targets(d, ls, 256);
        if (n) { run_op(which, ls, n, rs[i].f0, rs[i].f1); done++; }
    }
    d->sel = keep;
    if (!done) app_toast("The layer is locked");
}
void timeline_op(int which) { op(which); }

void timeline_shortcuts(void) {
    if (act(ACT_PREV)) app_step(-1);
    if (act(ACT_NEXT)) app_step(1);
    if (act(ACT_FIRST)) doc_tl(app.doc)->frame = 1;
    if (act(ACT_LAST)) doc_tl(app.doc)->frame = doc_tl(app.doc)->len;
    if (act(ACT_PLAY)) app_play(!app.playing);
    if (act(ACT_FRAME)) op(0);
    if (act(ACT_REMOVE_FRAMES)) op(1);
    if (act(ACT_KEYFRAME)) op(2);
    if (act(ACT_BLANK_KEY)) op(3);
    if (act(ACT_CLEAR_KEY)) op(4);
}

static void frame_menu(void) {
    if (!ui_popup_begin(ui_id("frame-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 230, 10 + 28 * 8 + 9 * 2))) return;
    if (ui_menu_item("Insert Frame", act_keys(ACT_FRAME), true, false)) op(0);
    if (ui_menu_item("Remove Frames", act_keys(ACT_REMOVE_FRAMES), true, false)) op(1);
    ui_menu_sep();
    if (ui_menu_item("Insert Keyframe", act_keys(ACT_KEYFRAME), true, false)) op(2);
    if (ui_menu_item("Insert Blank Keyframe", act_keys(ACT_BLANK_KEY), true, false)) op(3);
    if (ui_menu_item("Clear Keyframe", act_keys(ACT_CLEAR_KEY), true, false)) op(4);
    ui_menu_sep();
    if (ui_menu_item("Create Classic Tween", NULL, true, false)) op(5);
    if (ui_menu_item("Create Shape Tween", NULL, true, false)) op(6);
    if (ui_menu_item("Remove Tween", NULL, true, false)) op(7);
    ui_popup_end();
}

/* ── toolbar ── */

static void transport_icon(Rect c, int kind, Color col) {
    static const IconId ic[] = {IC_SKIP_BACK, IC_STEP_BACK, IC_PLAY, IC_PAUSE, IC_STEP_FWD, IC_SKIP_FWD};
    icon_draw(ui.dl, ic[kind], v2(c.x + c.w / 2, c.y + c.h / 2), ICON_M, true, col);
}

static const IconId TYPE_ICON[LT_COUNT] = {IC_VECTOR, IC_RASTER, IC_CAMERA, IC_AUDIO, IC_FOLDER, IC_MASK, IC_GUIDE, IC_REFERENCE};

// square icon toggle with an accent when on
static bool icon_toggle(const char *id, Rect r, IconId ic, bool on, Color accent, const char *tip) {
    UiResp re = ui_interact(ui_id(id), r);
    if (on) { dl_rrect(ui.dl, r, 6, col_alpha(accent, 0.12f)); dl_rrect_border(ui.dl, r, 6, 1, col_alpha(accent, 0.35f)); }
    else if (re.hover) dl_rrect(ui.dl, r, 6, C_S3);
    icon_draw(ui.dl, ic, v2(r.x + r.w / 2, r.y + r.h / 2), ICON_M, false, on ? accent : re.hover ? C_TX : C_TX2);
    ui_tooltip(ui_id(id), r, tip);
    return re.clicked;
}

static void toolbar(Rect bar) {
    Doc *d = app.doc;
    R(rect(bar.x, bar.y + bar.h - 1, bar.w, 1), C_LINE);
    Rect left = cut_left(&bar, M_NAMES_W);
    R(rect(left.x + left.w - 1, left.y, 1, left.h - 1), C_LINE);
    // new layer (with types)
    Rect nb = {left.x + 8, left.y + 5, font_width(F_UI, "New layer") + 52, 26};
    UiId nid = ui_id("new-layer");
    bool open = ui_popup_is_open(nid);
    UiResp re = ui_interact(nid, nb);
    if (open) dl_rrect(ui.dl, nb, 6, C_S4); else if (re.hover) dl_rrect(ui.dl, nb, 6, C_S3);
    icon_draw(ui.dl, IC_PLUS, v2(nb.x + 15, nb.y + nb.h / 2), ICON_M, false, open || re.hover ? C_TX : C_TX2);
    font_draw_in(ui.dl, F_UI, rect(nb.x + 28, nb.y, nb.w, nb.h), ALIGN_LEFT, "New layer", open || re.hover ? C_TX : C_TX2);
    icon_draw(ui.dl, IC_CARET_DOWN, v2(nb.x + nb.w - 13, nb.y + nb.h / 2), ICON_S, false, C_TX3);
    if (re.clicked) { if (open) ui_close_popup(); else ui_open_popup(nid, nb); }
    if (ui_popup_begin(nid, rect(nb.x, nb.y + nb.h + 4, 210, 10 + 24 + 28 * 8 + 9))) {
        static const u8 order[] = {LT_VECTOR, LT_RASTER, LT_CAMERA, LT_AUDIO, LT_FOLDER, LT_MASK, LT_GUIDE, LT_REFERENCE};
        ui_menu_header("New layer");
        for (int i = 0; i < COUNT(order); i++) {
            if (i == 5) ui_menu_sep();
            if (ui_menu_item_ic(TYPE_ICON[order[i]], LAYER_TYPE_NAMES[order[i]], i == 0 ? act_keys(ACT_NEW_LAYER) : NULL, true, false)) {
                app_commit();
                Layer *l = doc_add_layer(app.doc, order[i], NULL);
                doc_tl(app.doc)->active = l->id;
            }
        }
        ui_popup_end();
    }

    // transport
    float x = bar.x + 12;
    Rect tp = {x, bar.y + 6, 5 * 30 + 6, 24};
    dl_rrect(ui.dl, tp, 7, C_S2);
    dl_rrect_border(ui.dl, tp, 7, 1, C_LINE);
    static const char *tips[] = {"First frame  Home", "Previous frame  ,", "Play  Enter", "Next frame  .", "Last frame  End"};
    for (int i = 0; i < 5; i++) {
        Rect c = {tp.x + 3 + i * 30, tp.y + 2, 30, 20};
        UiId id = ui_idi("transport", i);
        UiResp b = ui_interact(id, c);
        bool on = i == 2;
        if (on) dl_rrect(ui.dl, c, 5, C_S4); else if (b.hover) dl_rrect(ui.dl, c, 5, C_S3);
        transport_icon(c, i == 2 ? (app.playing ? 3 : 2) : i == 3 ? 4 : i == 4 ? 5 : i, on || b.hover ? C_TX : C_TX2);
        ui_tooltip(id, c, tips[i]);
        if (b.clicked) {
            if (i == 0) doc_tl(d)->frame = 1;
            else if (i == 1) app_step(-1);
            else if (i == 2) app_play(!app.playing);
            else if (i == 3) app_step(1);
            else doc_tl(d)->frame = doc_tl(d)->len;
        }
    }
    x = tp.x + tp.w + 12;
    // loop & onion toggles
    Rect lr = {x, bar.y + 6, 30, 24}, orr = {x + 34, bar.y + 6, 30, 24};
    if (icon_toggle("loop", lr, IC_LOOP, d->loop_on, C_LOOP, "Loop playback")) d->loop_on = !d->loop_on;
    if (icon_toggle("onion", orr, IC_ONION, d->onion_on, light(C_PREV, 0.44f, 1), "Onion skin")) d->onion_on = !d->onion_on;
    x = orr.x + orr.w + 14;
    // fps
    static const char *fps_items[] = {"12 fps", "24 fps", "25 fps", "30 fps", "60 fps"};
    static const int fps_vals[] = {12, 24, 25, 30, 60};
    int sel = 1;
    for (int i = 0; i < 5; i++) if (fps_vals[i] == d->fps) sel = i;
    Rect fr = {x, bar.y + 6, 74, 24};
    UiId fid = ui_id("fps");
    UiResp fre = ui_interact(fid, fr);
    if (fre.hover || ui_popup_is_open(fid)) dl_rrect(ui.dl, fr, 6, C_S3);
    font_draw_in(ui.dl, F_MONO_MED, rect(fr.x + 8, fr.y, fr.w, fr.h), ALIGN_LEFT, fps_items[sel], C_TX2);
    if (fre.clicked) { if (ui_popup_is_open(fid)) ui_close_popup(); else ui_open_popup(fid, fr); }
    if (ui_popup_begin(fid, rect(fr.x, fr.y + fr.h + 4, 120, 10 + 28 * 5))) {
        for (int i = 0; i < 5; i++) if (ui_menu_item(fps_items[i], NULL, true, i == sel)) { app_commit(); app.doc->fps = fps_vals[i]; }
        ui_popup_end();
    }
    // timeline zoom
    Rect zr = {bar.x + bar.w - 12 - 84, bar.y + 6, 84, 24};
    font_draw_in(ui.dl, F_UI, rect(zr.x - 46, zr.y, 40, zr.h), ALIGN_RIGHT, "Zoom", C_TX3);
    float z = ui_slider(ui_id("tl-zoom"), zr, (float)T.cw, 10, 34);
    int cw = ((int)roundf(z)) & ~1;
    if (cw != T.cw) T.cw = CLAMP(cw, 10, 34);
}

/* ── layer list ── */

static const char *type_tag(u8 t) {
    switch (t) { case LT_RASTER: return "RAS"; case LT_CAMERA: return "CAM"; case LT_AUDIO: return "AUD"; case LT_MASK: return "MASK"; case LT_GUIDE: return "GUIDE"; case LT_REFERENCE: return "REF"; default: return NULL; }
}

// dragging layers: above / below a layer, or into a folder (its middle); Ctrl / Shift picks move together
static void layer_drag(Rect r, int *rows, int nrows) {
    Doc *d = app.doc;
    if (!T.lpress) return;
    if (ui.in.down[0]) {
        if (!T.ldragging && fabsf(ui.in.mouse.y - T.lpress_y) > 4) T.ldragging = true;
        if (!T.ldragging) return;
        float top = r.y + M_RULER_H - T.sy, fy = (ui.in.mouse.y - top) / M_ROW_H;
        int i = (int)floorf(fy);
        float frac = fy - i;
        T.drop_ok = true;
        if (i < 0) { i = 0; frac = 0; }
        if (i >= nrows) { T.drop_ref = 0; T.drop_where = 2; }
        else {
            Layer *l = &doc_tl(d)->layers[rows[i]];
            T.drop_ref = l->id;
            if (l->type == LT_FOLDER) T.drop_where = frac < 0.3f ? 0 : frac > 0.7f && l->collapsed ? 2 : frac > 0.7f ? 1 : 1;
            else T.drop_where = frac < 0.5f ? 0 : 2;
            if (lsel_index(l->id) >= 0) T.drop_ok = false;   // onto itself
        }
        // the mark: a line between layers, or the folder lit up
        ui_cursor(SAPP_MOUSECURSOR_RESIZE_NS);
        if (!T.drop_ok) return;
        float y; int depth = 0;
        if (!T.drop_ref) y = top + nrows * M_ROW_H;
        else {
            int ri = 0;
            for (int j = 0; j < nrows; j++) if (doc_tl(d)->layers[rows[j]].id == T.drop_ref) ri = j;
            Layer *l = &doc_tl(d)->layers[rows[ri]];
            depth = doc_depth(d, l);
            if (T.drop_where == 1) {
                Rect fr = {r.x + 2, top + ri * M_ROW_H + 1, r.w - 5, M_ROW_H - 2};
                dl_rrect_border(ui.dl, fr, 4, 1.5f, C_AC);
                return;
            }
            int end = ri;
            if (T.drop_where == 2) while (end + 1 < nrows && doc_depth(d, &doc_tl(d)->layers[rows[end + 1]]) > depth) end++;
            y = top + (T.drop_where == 0 ? ri : end + 1) * M_ROW_H;
        }
        float x = r.x + 10 + depth * 16;
        R(rect(x, y - 1, r.x + r.w - 8 - x, 2), C_AC);
        dl_circle(ui.dl, v2(x, y), 3, C_AC);
        return;
    }
    // released
    if (T.ldragging && T.drop_ok) {
        int ids[256], n = 0;
        if (lsel_index(T.lpress) >= 0) for (ptrdiff_t k = 0; k < arrlen(T.lsel) && n < 256; k++) ids[n++] = T.lsel[k];
        else ids[n++] = T.lpress;
        app_commit();
        if (!doc_move_layers(app.doc, ids, n, T.drop_ref, T.drop_where)) app_toast("A folder can't go into itself");
    } else if (!T.ldragging && !(ui.in.mods & (MOD_CTRL | MOD_SHIFT))) lsel_only(T.lpress);   // a plain click picks one layer
    T.lpress = 0; T.ldragging = false;
}

static void layer_menu(void) {
    if (!ui_popup_begin(ui_id("layer-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 240, 10 + 28 * 8 + 9 * 3))) return;
    Doc *d = app.doc;
    int n = (int)arrlen(T.lsel);
    if (ui_menu_item("New Layer", act_keys(ACT_NEW_LAYER), true, false)) { app_commit(); Layer *l = doc_add_layer(app.doc, LT_VECTOR, NULL); doc_tl(app.doc)->active = l->id; lsel_only(l->id); }
    if (ui_menu_item("New Folder", NULL, true, false)) { app_commit(); Layer *l = doc_add_layer(app.doc, LT_FOLDER, NULL); doc_tl(app.doc)->active = l->id; lsel_only(l->id); }
    ui_menu_sep();
    if (ui_menu_item(n > 1 ? "Put Layers in a New Folder" : "Put in a New Folder", NULL, n > 0, false)) {
        int ids[256], k = 0;
        for (ptrdiff_t i = 0; i < arrlen(T.lsel) && k < 256; i++) ids[k++] = T.lsel[i];
        // the folder goes where the top-most of them is
        Timeline *t = doc_tl(d);
        for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) if (lsel_index(t->layers[i].id) >= 0) { t->active = t->layers[i].id; break; }
        app_commit();
        d = app.doc;
        Layer *ref = doc_active(d);
        int above = ref ? ref->id : 0;
        Layer *f = doc_add_layer(d, LT_FOLDER, NULL);
        int fid = f->id;
        if (ref && ref->type == LT_FOLDER) { int one[1] = {fid}; doc_move_layers(d, one, 1, above, 0); }   // beside, not inside
        doc_move_layers(d, ids, k, fid, 1);
        doc_tl(d)->active = fid;
        lsel_only(fid);
    }
    if (ui_menu_item("Rename", NULL, n == 1, false)) T.rename_id = doc_tl(d)->active;
    ui_menu_sep();
    Layer *al = doc_active(d);
    // see through: shown at half strength while editing, exports ignore it
    if (ui_menu_item("See Through", NULL, al && al->type != LT_FOLDER, al && al->transparent)) {
        bool on = !al->transparent;
        for (ptrdiff_t i = 0; i < arrlen(T.lsel); i++) { Layer *q = doc_layer(d, T.lsel[i]); if (q) q->transparent = on; }
        al->transparent = on;
        doc_touch(d);
    }
    // mask: the layer becomes a mask for the one under it (Animate's Mask); again turns it back
    bool is_mask = al && al->type == LT_MASK;
    if (ui_menu_item("Mask", NULL, al && (al->type == LT_VECTOR || is_mask), is_mask)) {
        app_commit();
        d = app.doc;
        al = doc_active(d);
        Timeline *t = doc_tl(d);
        if (is_mask) {
            for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) if (t->layers[i].parent == al->id) t->layers[i].parent = al->parent;
            al->type = LT_VECTOR;
        } else {
            int i = doc_layer_index(d, al->id);
            al->type = LT_MASK;
            if (i + 1 < arrlen(t->layers) && t->layers[i + 1].parent == al->parent && t->layers[i + 1].type != LT_FOLDER) t->layers[i + 1].parent = al->id;
        }
        doc_touch(d);
    }
    ui_menu_sep();
    char del[48];
    snprintf(del, sizeof(del), n > 1 ? "Delete %d Layers" : "Delete Layer", n);
    if (ui_menu_item(del, NULL, n > 0 && n < (int)arrlen(doc_tl(d)->layers), false)) {
        int ids[256], k = 0;
        for (ptrdiff_t i = 0; i < arrlen(T.lsel) && k < 256; i++) ids[k++] = T.lsel[i];
        app_commit();
        for (int i = 0; i < k; i++) doc_delete_layer(app.doc, ids[i]);
        lsel_only(doc_tl(app.doc)->active);
    }
    ui_popup_end();
}

static void names(Rect r, int *rows, int nrows) {
    Doc *d = app.doc;
    R(rect(r.x + r.w - 1, r.y, 1, r.h), C_LINE);
    R(rect(r.x, r.y + M_RULER_H - 1, r.w - 1, 1), C_LINE);
    dl_push_clip(ui.dl, rect(r.x, r.y + M_RULER_H, r.w - 1, r.h - M_RULER_H));
    for (int i = 0; i < nrows; i++) {
        Layer *l = &doc_tl(d)->layers[rows[i]];
        float y = r.y + M_RULER_H + i * M_ROW_H - T.sy;
        if (y + M_ROW_H < r.y + M_RULER_H || y > r.y + r.h) continue;
        Rect row = {r.x, y, r.w - 1, M_ROW_H};
        bool active = l->id == doc_tl(d)->active;
        bool hidden = doc_hidden(d, l);
        float x = r.x + 10 + doc_depth(d, l) * 16;
        // small controls first, so they win over the row underneath
        Rect lk = {r.x + r.w - 26, y + 4, 16, 16}, ey = {lk.x - 20, y + 4, 16, 16};
        Rect cr = {x + 9, y + 4, 14, 16};
        UiResp ere = ui_interact(ui_idi("eye", l->id), ey), lre = ui_interact(ui_idi("lock", l->id), lk);
        UiResp cre = {0};
        if (l->type == LT_FOLDER) cre = ui_interact(ui_idi("fold", l->id), cr);
        bool on_ctl = ere.hover || lre.hover || cre.hover;
        UiResp re = on_ctl ? (UiResp){0} : ui_interact(ui_idi("layer-row", l->id), row);
        bool picked = lsel_index(l->id) >= 0;
        if (active) R(row, C_AC2); else if (picked) R(row, col_a(C_AC, 0x14)); else if (re.hover || on_ctl) R(row, hexa(0xffffff, 0x06));
        R(rect(row.x, y + M_ROW_H - 1, row.w, 1), hexa(0xffffff, 0x06));
        RR(x, y + 5, 3, 13, 1.5f, l->color);
        x += 11;
        if (l->type == LT_FOLDER) {
            Color cc = cre.hover ? C_TX : C_TX3;
            icon_draw(ui.dl, l->collapsed ? IC_CARET_RIGHT : IC_CARET_DOWN, v2(x + 4, y + 12), ICON_S, true, cc);
            if (cre.clicked) l->collapsed = !l->collapsed;
            x += 14;
        }
        // name (double click renames)
        UiId fid = ui_idi("rename", l->id);
        if (T.rename_id == l->id) {
            Rect fr = {x - 4, y + 2, r.w - (x - r.x) - 60, 20};
            if (ui.focus != fid) { ui.focus = fid; snprintf(ui.edit, sizeof(ui.edit), "%s", l->name); ui.edit_len = (int)strlen(ui.edit); }
            if (ui_text_field(fid, fr, l->name, sizeof(l->name), NULL)) { T.rename_id = 0; doc_touch(d); }
            if (ui.focus != fid) T.rename_id = 0;
        } else {
            icon_draw(ui.dl, TYPE_ICON[l->type], v2(x + 7, y + 12), ICON_S, false, hidden ? C_TX3 : active ? C_TX2 : C_TX3);
            x += 20;
            font_draw_in(ui.dl, active ? F_UI_MED : F_UI, rect(x, y, r.w - (x - r.x) - 60, M_ROW_H - 1), ALIGN_LEFT, l->name, hidden ? C_TX3 : active ? C_TX : C_TX2);
        }
        // toggles: visible / locked (shown on hover, or when not default)
        bool show = re.hover || on_ctl || active;
        V2 ec = {ey.x + 8, ey.y + 8}, lc = {lk.x + 8, lk.y + 8};
        if (l->hidden) icon_draw(ui.dl, IC_EYE_OFF, ec, ICON_S, false, ere.hover ? C_TX : C_TX2);
        else if (l->transparent) icon_draw(ui.dl, IC_EYE, ec, ICON_S, false, col_a(C_AC, ere.hover ? 0xff : 0xa0));   // see-through
        else if (show) icon_draw(ui.dl, IC_EYE, ec, ICON_S, false, ere.hover ? C_TX : C_TX3);
        if (l->locked) icon_draw(ui.dl, IC_LOCK, lc, ICON_S, true, lre.hover ? C_TX : C_TX2);
        else if (show) icon_draw(ui.dl, IC_UNLOCK, lc, ICON_S, false, lre.hover ? C_TX : C_TX3);
        ui_tooltip(ui_idi("eye", l->id), ey, l->hidden ? "Show layer" : l->transparent ? "See-through layer. Shift-click shows it fully" : "Hide layer. Shift-click: see through it");
        ui_tooltip(ui_idi("lock", l->id), lk, l->locked ? "Unlock layer" : "Lock layer");
        if (ere.clicked) { if (ui.in.mods & MOD_SHIFT) { l->transparent = !l->transparent; l->hidden = false; } else l->hidden = !l->hidden; doc_touch(d); }
        if (lre.clicked) { l->locked = !l->locked; doc_touch(d); }
        if (l->opacity < 0.999f) {
            char op[8]; snprintf(op, sizeof(op), "%d%%", (int)roundf(l->opacity * 100));
            font_draw_in(ui.dl, F_MONO, rect(ey.x - 44, y, 40, M_ROW_H - 1), ALIGN_RIGHT, op, C_TX3);
        }
        if (re.pressed) {
            Timeline *t = doc_tl(d);
            u32 mods = ui.in.mods;
            if (mods & MOD_CTRL) {             // add / take away one layer
                int k = lsel_index(l->id);
                if (k >= 0 && arrlen(T.lsel) > 1) { arrdel(T.lsel, k); if (t->active == l->id) t->active = T.lsel[0]; }
                else if (k < 0) { arrput(T.lsel, l->id); t->active = l->id; }
            } else if (mods & MOD_SHIFT) {     // everything between the active layer and this one
                int ai = -1;
                for (int j = 0; j < nrows; j++) if (doc_tl(d)->layers[rows[j]].id == t->active) ai = j;
                if (ai < 0) ai = i;
                arrsetlen(T.lsel, 0);
                for (int j = MIN(ai, i); j <= MAX(ai, i); j++) arrput(T.lsel, doc_tl(d)->layers[rows[j]].id);
                t->active = l->id;
            } else {
                if (lsel_index(l->id) < 0) lsel_only(l->id);
                t->active = l->id;
                T.lpress = l->id; T.lpress_y = ui.in.mouse.y; T.ldragging = false;
            }
            clear_frames();
        }
        if (re.right) {
            if (lsel_index(l->id) < 0) lsel_only(l->id);
            doc_tl(d)->active = l->id;
            ui_open_popup(ui_id("layer-menu"), rect(ui.in.mouse.x, ui.in.mouse.y, 1, 1));
        }
        if (re.dbl) T.rename_id = l->id;
    }
    layer_drag(r, rows, nrows);
    dl_pop_clip(ui.dl);
    if (rect_has(r, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y != 0) T.sy = MAX(0, T.sy - ui.in.wheel_y * 24);
    layer_menu();
}

/* ── frame grid ── */

static void grid(Rect fr, int *rows, int nrows) {
    Doc *d = app.doc;
    FX0 = fr.x;
    int len = doc_tl(d)->len, f = doc_tl(d)->frame, cw = T.cw;
    Rect ruler = {fr.x, fr.y, fr.w, M_RULER_H};
    float rowsTop = fr.y + M_RULER_H, rowsH = fr.h - M_RULER_H;
    float contentH = MIN(rowsH, nrows * M_ROW_H - T.sy);
    int f_first = MAX(1, (int)floorf(T.sx / cw) + 1), f_last = f_first + (int)ceilf(fr.w / cw) + 1;
    dl_push_clip(ui.dl, fr);

    /* rows */
    R(rect(fr.x, rowsTop, fr.w, rowsH), C_S1);
    for (int i = 0; i < nrows; i++) if (doc_tl(d)->layers[rows[i]].id == doc_tl(d)->active) R(rect(fr.x, rowsTop + i * M_ROW_H - T.sy, fr.w, M_ROW_H - 1), col_a(C_AC, 0x0b));
    dl_push_clip(ui.dl, rect(fr.x, rowsTop, fr.w, rowsH));
    for (int c = MAX(4, (f_first / 4) * 4); c <= MIN(len, f_last); c += 4) R(rect(X(c) + 1, rowsTop, cw - 1, contentH), hexa(0xffffff, 0x05));
    for (int c = f_first; c <= f_last + 1; c++) R(rect(X(c), rowsTop, 1, contentH), c <= len + 1 ? hexa(0xffffff, 0x0a) : hexa(0xffffff, 0x05));
    if (X(len + 1) + 1 < fr.x + fr.w) R(rect(X(len + 1) + 1, rowsTop, fr.x + fr.w - X(len + 1) - 1, contentH), hexa(0x000000, 0x4d));
    for (int s = d->fps + 1; s <= f_last + 1; s += d->fps) R(rect(X(s), rowsTop, 1, contentH), hexa(0xffffff, 0x1f));

    for (int i = 0; i < nrows; i++) {
        Layer *l = &doc_tl(d)->layers[rows[i]];
        float y = rowsTop + i * M_ROW_H - T.sy;
        if (y + M_ROW_H < rowsTop || y > fr.y + fr.h) continue;
        R(rect(fr.x, y + M_ROW_H - 1, fr.w, 1), hexa(0xffffff, 0x06));
        float al = doc_hidden(d, l) ? 0.4f : 1;
        bool active = l->id == doc_tl(d)->active;
        if (l->type == LT_FOLDER) {
            int end = 0;
            Layer *kids[256]; int nk = 0;
            for (ptrdiff_t c = 0; c < arrlen(doc_tl(d)->layers); c++) if (doc_tl(d)->layers[c].parent == l->id && doc_tl(d)->layers[c].type != LT_FOLDER && nk < 256) kids[nk++] = &doc_tl(d)->layers[c];
            for (int k = 0; k < nk; k++) end = MAX(end, kids[k]->len);
            if (end) RR(X(1) + 1, y + 9, X(end + 1) - X(1) - 1, 5, 2.5f, with_alpha(C_S4, al));
            for (int k = 0; k < nk; k++) for (ptrdiff_t q = 0; q < arrlen(kids[k]->keys); q++) {
                int kf = kids[k]->keys[q].f;
                if (kf >= f_first - 1 && kf <= f_last) R(rect(MID(kf), y + 7, 1, 9), with_alpha(col_mix(C_TX3, C_TX2, 0.75f), al));
            }
            continue;
        }
        for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) {
            Key *key = &l->keys[k];
            int a = key->f, b = doc_span_end(l, (int)k);
            if (b < f_first - 1 || a > f_last + 1) continue;
            Color col = key->tw == TW_CLASSIC && k + 1 < arrlen(l->keys) ? C_TWEEN_C : key->tw == TW_SHAPE && k + 1 < arrlen(l->keys) ? C_TWEEN_S
                      : key->d < 0 && doc_drawable(l) ? C_S3 : active ? col_mix(C_S4, C_AC, 0.08f) : col_mix(C_S4, C_TX3, 0.05f);
            RR(X(a) + 1, y + 3, X(b + 1) - X(a) - 1, 17, 2.5f, with_alpha(col, al));
            for (int c = MAX(a + 1, f_first); c <= MIN(b, f_last); c++) R(rect(X(c), y + 3, 1, 17), hexa(0x000000, 0x38));
            if (key->tw && k + 1 < arrlen(l->keys) && b - a >= 2) {
                Color ac = light(key->tw == TW_CLASSIC ? C_TWEEN_C : C_TWEEN_S, 0.42f, 0.95f);
                float x0 = MID(a) + 6, x1 = X(b + 1) - 4;
                R(rect(x0, y + 11, x1 - x0 - 4, 1), with_alpha(ac, al));
                dl_tri(ui.dl, v2(x1, y + 11.5f), v2(x1 - 5, y + 8.5f), v2(x1 - 5, y + 14.5f), with_alpha(ac, al));
            }
            bool content = key->d >= 0 && arrlen(d->drawings[key->d].items);
            V2 kc = {MID(a) + 0.5f, y + 11.5f};
            if (content || !doc_drawable(l)) dl_circle(ui.dl, kc, 3.25f, with_alpha(col_mix(C_TX2, C_TX, 0.85f), al));
            else dl_circle_border(ui.dl, kc, 3.25f, 1.5f, with_alpha(col_mix(C_TX2, C_TX, 0.85f), al));
            if (l->type == LT_AUDIO && l->sound && k == 0) {   // the waveform, from the first key on
                Sound *so = sound_get(l->sound);
                float x0 = MAX(X(a) + 1, fr.x), x1 = MIN(X(l->len + 1), fr.x + fr.w);
                Color wc = with_alpha(col_mix(C_NEXT, C_TX, 0.25f), al);
                for (float px = x0; so && px < x1; px += 1) {
                    double t0 = (px - X(a)) / cw / d->fps, t1 = t0 + 1.0 / (cw * d->fps);
                    float h = roundf(sound_peak(so, t0, t1) * 8);
                    R(rect(px, y + 11 - h, 1, 2 * h + 1), wc);
                }
            }
            if (key->label[0] && X(b + 1) - X(a) > 40) {
                dl_push_clip(ui.dl, rect(X(a) + 1, y, X(b + 1) - X(a) - 2, M_ROW_H));
                font_draw_in(ui.dl, F_SMALL, rect(MID(a) + 8, y, 200, M_ROW_H - 1), ALIGN_LEFT, key->label, with_alpha(C_TX2, al));
                dl_pop_clip(ui.dl);
            }
        }
    }
    for (int i = -1; i < T.nextra; i++) {
        Sel s = i < 0 ? d->sel : T.extra[i];
        if (!s.on) continue;
        for (int ghost = 0; ghost < 1 + (T.drag == DRAG_MOVE && T.move_df != 0); ghost++) {
            int off = ghost ? T.move_df : 0;
            float y0 = rowsTop + s.r0 * M_ROW_H - T.sy, y1 = rowsTop + (s.r1 + 1) * M_ROW_H - T.sy - 1;
            Rect sr = {X(s.f0 + off) + 1, y0 + 1, X(s.f1 + 1 + off) - X(s.f0 + off) - 1, y1 - y0 - 1};
            R(sr, ghost ? col_a(C_AC, 0x22) : hexa(0xffffff, 0x10));
            dl_rrect_border(ui.dl, rect(sr.x - 0.5f, sr.y - 0.5f, sr.w + 1, sr.h + 1), 2, 1, ghost ? C_AC : hexa(0xffffff, 0xd0));
        }
    }
    if (d->loop_on) { R(rect(X(d->loop_a), rowsTop, 1, contentH), col_a(C_LOOP, 0x4d)); R(rect(X(d->loop_b + 1), rowsTop, 1, contentH), col_a(C_LOOP, 0x4d)); }
    R(rect(X(f) + 1, rowsTop, cw - 1, rowsH), col_a(C_AC, 0x14));
    R(rect(MID(f), rowsTop, 1, rowsH), C_AC);
    dl_pop_clip(ui.dl);

    /* ruler */
    R(ruler, col_mix(C_BG, C_S1, 0.6f));
    R(rect(ruler.x, ruler.y + M_RULER_H - 1, ruler.w, 1), col_mix(C_LINE, C_LINE2, 0.25f));
    if (X(len + 1) + 2 < ruler.x + ruler.w) R(rect(X(len + 1) + 2, ruler.y, ruler.x + ruler.w - X(len + 1) - 2, M_RULER_H - 1), hexa(0x000000, 0x40));
    for (int c = f_first; c <= f_last + 1; c++) R(rect(X(c), ruler.y + M_RULER_H - 4, 1, 3), c <= len + 1 ? col_mix(C_LINE2, C_TX3, 0.2f) : col_mix(C_LINE, C_LINE2, 0.5f));
    int step = cw >= 14 ? 4 : 8;
    for (int c = f_first; c <= f_last; c++) {
        if ((c != 1 && c % step) || c == f) continue;
        char s[16]; snprintf(s, sizeof(s), "%d", c);
        float w = font_width(F_MONO, s);
        font_draw(ui.dl, F_MONO, MID(c) + 0.5f - w / 2, ruler.y + 25, s, c <= len ? col_mix(C_TX3, C_TX2, 0.25f) : col_mix(C_LINE2, C_TX3, 0.5f));
    }
    for (int s = d->fps + 1; s <= f_last + 1; s += d->fps) {
        R(rect(X(s), ruler.y + 6, 1, M_RULER_H - 7), hexa(0xffffff, 0x33));
        char t[8]; snprintf(t, sizeof(t), "%ds", (s - 1) / d->fps);
        float w = font_width(F_SMALL_SEMI, t);
        font_draw(ui.dl, F_SMALL_SEMI, X(s) - 4 - w, ruler.y + 14, t, C_TX2);
    }
    if (d->loop_on) {
        RR(X(d->loop_a) + 1, ruler.y + 1, X(d->loop_b + 1) - X(d->loop_a) - 1, 4, 2, col_a(C_LOOP, 0x55));
        RR(X(d->loop_a) + 1, ruler.y + 1, 3, 9, 1.5f, C_LOOP);
        RR(X(d->loop_b + 1) - 3, ruler.y + 1, 3, 9, 1.5f, C_LOOP);
    }
    if (d->onion_on) {
        int p = MAX(1, f - d->onion_prev);
        if (p < f) R(rect(X(p) + 1, ruler.y + M_RULER_H - 3, X(f) - X(p) - 1, 2), C_PREV);
        R(rect(X(f + 1) + 1, ruler.y + M_RULER_H - 3, X(f + 1 + d->onion_next) - X(f + 1) - 1, 2), C_NEXT);
    }
    RR(X(len + 1) - 1, ruler.y + 4, 3, M_RULER_H - 9, 1.5f, T.drag == DRAG_END ? hex(0xffffff) : col_mix(C_TX2, C_TX, 0.6f));
    char fs[16]; snprintf(fs, sizeof(fs), "%d", f);
    float bw = MAX(cw - 1, font_width(F_MONO_SEMI, fs) + 7);
    RR(MID(f) + 0.5f - bw / 2, ruler.y + 15, bw, 15, 3, C_AC);
    font_draw(ui.dl, F_MONO_SEMI, MID(f) + 0.5f - font_width(F_MONO_SEMI, fs) / 2, ruler.y + 26, fs, hex(0xffffff));
    dl_pop_clip(ui.dl);

    /* interaction */
    UiId gid = ui_id("tl-grid");
    float maxsx = MAX(0, (MAX(len, f) + 60) * cw - fr.w), maxsy = MAX(0, nrows * M_ROW_H - rowsH + 8);
    // scroll bars (on top of the grid, so they go first)
    Rect hb = {fr.x + 2, fr.y + fr.h - 9, fr.w - 14, 7}, vb = {fr.x + fr.w - 9, rowsTop + 2, 7, rowsH - 14};
    float hth = MAX(30, hb.w * fr.w / (fr.w + maxsx)), htx = hb.x + (hb.w - hth) * (maxsx > 0 ? T.sx / maxsx : 0);
    float vth = MAX(24, vb.h * rowsH / (rowsH + maxsy)), vty = vb.y + (vb.h - vth) * (maxsy > 0 ? T.sy / maxsy : 0);
    UiResp hre = maxsx > 0 ? ui_interact(ui_id("tl-hbar"), hb) : (UiResp){0};
    UiResp vre = maxsy > 0 ? ui_interact(ui_id("tl-vbar"), vb) : (UiResp){0};
    if (hre.pressed) { T.drag = DRAG_HBAR; T.bar_grab = rect_has(rect(htx, hb.y, hth, hb.h), ui.in.mouse) ? ui.in.mouse.x - htx : hth / 2; }
    if (vre.pressed) { T.drag = DRAG_VBAR; T.bar_grab = rect_has(rect(vb.x, vty, vb.w, vth), ui.in.mouse) ? ui.in.mouse.y - vty : vth / 2; }
    if (T.drag == DRAG_HBAR && ui.in.down[0]) T.sx = clampf((ui.in.mouse.x - T.bar_grab - hb.x) / MAX(1, hb.w - hth) * maxsx, 0, maxsx);
    if (T.drag == DRAG_VBAR && ui.in.down[0]) T.sy = clampf((ui.in.mouse.y - T.bar_grab - vb.y) / MAX(1, vb.h - vth) * maxsy, 0, maxsy);
    if ((T.drag == DRAG_HBAR || T.drag == DRAG_VBAR) && !ui.in.down[0]) T.drag = DRAG_NONE;
    if (maxsx > 0) dl_rrect(ui.dl, rect(htx, hb.y, hth, hb.h), 3.5f, hre.hover || T.drag == DRAG_HBAR ? hexa(0xffffff, 0x50) : hexa(0xffffff, 0x26));
    if (maxsy > 0) dl_rrect(ui.dl, rect(vb.x, vty, vb.w, vth), 3.5f, vre.hover || T.drag == DRAG_VBAR ? hexa(0xffffff, 0x50) : hexa(0xffffff, 0x26));

    UiResp re = (hre.hover || vre.hover) ? (UiResp){0} : ui_interact(gid, fr);
    V2 m = ui.in.mouse;
    int mf = (int)floorf((m.x - FX0 + T.sx) / cw) + 1;
    mf = MAX(1, mf);
    int mr = (int)floorf((m.y - rowsTop + T.sy) / M_ROW_H);
    bool in_ruler = m.y < rowsTop;
    bool ctrl = ui.in.mods & MOD_CTRL, shift = ui.in.mods & MOD_SHIFT;
    if (re.hover && in_ruler) {
        if (fabsf(m.x - X(len + 1)) <= 5 || (d->loop_on && m.y < ruler.y + 12 && (fabsf(m.x - X(d->loop_a)) <= 6 || fabsf(m.x - X(d->loop_b + 1)) <= 6)))
            ui_cursor(SAPP_MOUSECURSOR_RESIZE_EW);
    }
    int in_rect = -2;   // -1 = the main selection, 0.. = an extra range, -2 = none
    if (!in_ruler && mr >= 0 && mr < nrows) {
        if (d->sel.on && mr >= d->sel.r0 && mr <= d->sel.r1 && mf >= d->sel.f0 && mf <= d->sel.f1) in_rect = -1;
        for (int i = 0; i < T.nextra && in_rect == -2; i++) { Sel x = T.extra[i]; if (mr >= x.r0 && mr <= x.r1 && mf >= x.f0 && mf <= x.f1) in_rect = i; }
    }
    if (re.hover && !in_ruler && in_rect != -2 && !ctrl && !shift && T.drag == DRAG_NONE) ui_cursor(SAPP_MOUSECURSOR_RESIZE_EW);
    if (re.pressed) {
        if (app.playing) app_play(false);
        if (in_ruler) {
            if (fabsf(m.x - X(len + 1)) <= 5) T.drag = DRAG_END;
            else if (d->loop_on && m.y < ruler.y + 12 && fabsf(m.x - X(d->loop_a)) <= 6) T.drag = DRAG_LOOP_A;
            else if (d->loop_on && m.y < ruler.y + 12 && fabsf(m.x - X(d->loop_b + 1)) <= 6) T.drag = DRAG_LOOP_B;
            else if (d->loop_on && m.y < ruler.y + 8 && mf >= d->loop_a && mf <= d->loop_b) { T.drag = DRAG_LOOP_MOVE; T.grab_f = mf; T.la0 = d->loop_a; T.lb0 = d->loop_b; }
            else T.drag = DRAG_SCRUB;
        } else if (mr >= 0 && mr < nrows) {
            Layer *l = &doc_tl(d)->layers[rows[mr]];
            if (shift) {                       // stretch the main range from where it started
                if (!d->sel.on) { T.anchor_r = mr; T.anchor_f = doc_tl(d)->frame; }
                d->sel = (Sel){true, MIN(T.anchor_r, mr), MAX(T.anchor_r, mr), MIN(T.anchor_f, mf), MAX(T.anchor_f, mf)};
                doc_tl(d)->frame = mf;
                T.drag = DRAG_SEL;
            } else if (ctrl) {                 // add a range, or take away the one clicked
                if (in_rect == -1) { if (T.nextra) { d->sel = T.extra[T.nextra - 1]; T.nextra--; } else d->sel.on = false; T.drag = DRAG_NONE; }
                else if (in_rect >= 0) { T.extra[in_rect] = T.extra[--T.nextra]; T.drag = DRAG_NONE; }
                else {
                    Sel x = {true, mr, mr, mf, mf};
                    if (!d->sel.on) { d->sel = x; T.anchor_r = mr; T.anchor_f = mf; T.drag = DRAG_SEL; }
                    else if (T.nextra < 32) { T.extra[T.nextra++] = x; T.xanchor_r = mr; T.xanchor_f = mf; T.drag = DRAG_SEL_EXTRA; }
                    doc_tl(d)->frame = mf;
                }
            } else if (in_rect != -2) {        // grab the selection: dragging moves those frames
                T.drag = DRAG_MOVE; T.grab_f = mf; T.move_df = 0;
            } else {
                doc_tl(d)->active = l->id;
                lsel_only(l->id);
                doc_tl(d)->frame = mf;
                T.nextra = 0;
                d->sel = (Sel){true, mr, mr, mf, mf};
                T.drag = DRAG_SEL; T.anchor_r = mr; T.anchor_f = mf;
            }
        } else T.drag = DRAG_SCRUB;
    }
    // middle button pans the timeline
    if (re.hover && ui.in.pressed[MB_MIDDLE]) { T.drag = DRAG_PAN; T.pan_m = m; T.pan_sx = T.sx; T.pan_sy = T.sy; }
    if (T.drag == DRAG_PAN) {
        T.sx = clampf(T.pan_sx - (m.x - T.pan_m.x), 0, maxsx); T.sy = clampf(T.pan_sy - (m.y - T.pan_m.y), 0, maxsy);
        ui_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
        if (!ui.in.down[MB_MIDDLE]) T.drag = DRAG_NONE;
    }
    if (re.held) {
        // dragging past the edge scrolls
        if (T.drag == DRAG_SCRUB || T.drag == DRAG_SEL || T.drag == DRAG_SEL_EXTRA || T.drag == DRAG_MOVE) {
            if (m.x > fr.x + fr.w - 16) T.sx = MIN(maxsx + cw * 4, T.sx + cw * 0.5f);
            if (m.x < fr.x + 16) T.sx = MAX(0, T.sx - cw * 0.5f);
        }
        switch (T.drag) {
        case DRAG_SCRUB: doc_tl(d)->frame = mf; if (app.playing) app_play(false); break;
        case DRAG_END: doc_tl(d)->len = MAX(1, (int)roundf((m.x - FX0 + T.sx) / cw)); d->loop_b = MIN(d->loop_b, doc_tl(d)->len); d->loop_a = MIN(d->loop_a, d->loop_b); break;
        case DRAG_LOOP_A: d->loop_a = CLAMP(mf, 1, d->loop_b); break;
        case DRAG_LOOP_B: d->loop_b = MAX(d->loop_a, (int)roundf((m.x - FX0 + T.sx) / cw)); break;
        case DRAG_LOOP_MOVE: { int df = mf - T.grab_f; df = MAX(df, 1 - T.la0); d->loop_a = T.la0 + df; d->loop_b = T.lb0 + df; break; }
        case DRAG_SEL: {
            int r1 = CLAMP(mr, 0, nrows - 1);
            d->sel = (Sel){true, MIN(T.anchor_r, r1), MAX(T.anchor_r, r1), MIN(T.anchor_f, mf), MAX(T.anchor_f, mf)};
            doc_tl(d)->frame = mf;
            break;
        }
        case DRAG_SEL_EXTRA: {
            int r1 = CLAMP(mr, 0, nrows - 1);
            if (T.nextra) T.extra[T.nextra - 1] = (Sel){true, MIN(T.xanchor_r, r1), MAX(T.xanchor_r, r1), MIN(T.xanchor_f, mf), MAX(T.xanchor_f, mf)};
            break;
        }
        case DRAG_MOVE: {
            int lo = d->sel.on ? d->sel.f0 : 1 << 30;
            for (int i = 0; i < T.nextra; i++) lo = MIN(lo, T.extra[i].f0);
            T.move_df = MAX(mf - T.grab_f, 1 - lo);
            ui_cursor(SAPP_MOUSECURSOR_RESIZE_EW);
            break;
        }
        }
    }
    if (re.released) {
        if (T.drag == DRAG_MOVE) {
            if (T.move_df) {
                // each range moves; ranges further along the move go first so none lands on one not moved yet
                app_commit();
                d = app.doc;
                Sel rs[33]; int nr = 0;
                if (d->sel.on) rs[nr++] = d->sel;
                for (int i = 0; i < T.nextra; i++) rs[nr++] = T.extra[i];
                for (int i = 1; i < nr; i++) for (int j = i; j > 0 && (T.move_df > 0 ? rs[j - 1].f0 < rs[j].f0 : rs[j - 1].f0 > rs[j].f0); j--) { Sel x = rs[j]; rs[j] = rs[j - 1]; rs[j - 1] = x; }
                for (int i = 0; i < nr; i++) {
                    d->sel = rs[i]; d->sel.on = true;
                    Layer *ls[256];
                    int n = doc_targets(d, ls, 256);
                    doc_move_frames(d, ls, n, rs[i].f0, rs[i].f1, T.move_df);
                }
                if (nr) { d->sel = rs[0]; }
                // the ranges follow their frames
                bool main_on = false;
                Sel moved[33]; int nm = 0;
                for (int i = 0; i < nr; i++) { Sel x = rs[i]; x.f0 += T.move_df; x.f1 += T.move_df; moved[nm++] = x; }
                d->sel = nm ? moved[0] : (Sel){0}; main_on = nm > 0;
                T.nextra = 0;
                for (int i = 1; i < nm && T.nextra < 32; i++) T.extra[T.nextra++] = moved[i];
                d->sel.on = main_on;
                doc_tl(d)->frame = MAX(1, doc_tl(d)->frame + T.move_df);
            } else if (mr >= 0 && mr < nrows) {   // a click inside the selection picks that one frame
                Layer *l = &doc_tl(d)->layers[rows[mr]];
                doc_tl(d)->active = l->id; lsel_only(l->id);
                doc_tl(d)->frame = mf; T.nextra = 0;
                d->sel = (Sel){true, mr, mr, mf, mf};
                T.anchor_r = mr; T.anchor_f = mf;
            }
            T.move_df = 0;
        }
        if (T.drag != DRAG_PAN && T.drag != DRAG_HBAR && T.drag != DRAG_VBAR) T.drag = DRAG_NONE;
    }
    if (re.dbl && !in_ruler && mr >= 0 && mr < nrows) {
        Layer *l = &doc_tl(d)->layers[rows[mr]];
        int ki = doc_key_index_for(l, mf);
        if (ki >= 0 && mf <= l->len) { d->sel = (Sel){true, mr, mr, l->keys[ki].f, doc_span_end(l, ki)}; T.nextra = 0; }
    }
    if (re.right && !in_ruler) {
        if (mr >= 0 && mr < nrows && in_rect == -2) {
            Layer *l = &doc_tl(d)->layers[rows[mr]];
            doc_tl(d)->active = l->id; lsel_only(l->id); doc_tl(d)->frame = mf;
            d->sel = (Sel){true, mr, mr, mf, mf}; T.nextra = 0;
        }
        ui_open_popup(ui_id("frame-menu"), rect(m.x, m.y, 1, 1));
    }
    // wheel: frames sideways; Shift: layers up and down; Ctrl: frame width
    if (re.hover && (ui.in.wheel_y || ui.in.wheel_x)) {
        if (ctrl) T.cw = CLAMP(T.cw + (ui.in.wheel_y > 0 ? 2 : -2), 10, 34);
        else if (shift && !ui.in.wheel_x) T.sy = T.sy - ui.in.wheel_y * 24;
        else T.sx = T.sx - (ui.in.wheel_x ? -ui.in.wheel_x : ui.in.wheel_y) * cw * 2;
    }
    T.sx = clampf(T.sx, 0, maxsx + (T.drag == DRAG_SCRUB || T.drag == DRAG_SEL || T.drag == DRAG_MOVE ? cw * 4 : 0));
    T.sy = clampf(T.sy, 0, maxsy);
    // the playhead stays in view: while playing, and when the frame changes from the keyboard or buttons
    f = doc_tl(d)->frame;
    if (f != T.last_frame && T.drag == DRAG_NONE) {
        if (X(f) + cw > fr.x + fr.w - 10) T.sx += X(f) + cw - (fr.x + fr.w - 10) + (app.playing ? fr.w * 0.25f : cw * 4);
        if (X(f) < fr.x) T.sx = MAX(0, (f - 1) * cw - (app.playing ? 40 : cw * 4));
    }
    T.last_frame = f;
    frame_menu();
}

void timeline_ui(Rect r) {
    Doc *d = app.doc;
    for (ptrdiff_t i = arrlen(T.lsel) - 1; i >= 0; i--) if (!doc_layer(d, T.lsel[i])) arrdel(T.lsel, i);
    if (lsel_index(doc_tl(d)->active) < 0) lsel_only(doc_tl(d)->active);
    if (!d->sel.on && !T.nextra) T.nextra = 0;
    R(r, C_S1);
    R(rect(r.x, r.y, r.w, 1), C_LINE);
    r.y += 1; r.h -= 1;
    toolbar(cut_top(&r, M_TLBAR_H));
    int rows[512];
    int nrows = doc_rows(d, rows, 512);
    Rect nm = cut_left(&r, M_NAMES_W);
    names(nm, rows, nrows);
    grid(r, rows, nrows);
}
