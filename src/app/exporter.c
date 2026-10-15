// Export Symbols: the character editor in front of an export, after Umoupen's. The picked symbols (or,
// without symbols, the scene's labels) are the animations; each gets its engine name, XML prefix, frame
// rate, loop and offsets. The preview is drawn the way the game places the character: the gold cross is
// its origin, the bottom middle of the frame box every animation shares; offsets push the sprite against
// their sign and the game scale grows it around the feet; the faint idle shows where every animation
// would sit with no offsets of its own.
#include "app/app.h"
#include "render/scene.h"
#include "platform/platform.h"
#include "sokol/sokol_app.h"
#include <ctype.h>

static struct {
    bool open;
    ClipSrc clips[64]; int n, cur;
    int frame; bool playing; double t;
    float zoom; bool fitted;
    bool idle_ref, whole, quarter;
    float box[4];                 // clip space: the shared frame box (all clips, offsets aside)
    Target tgt; Pool pool; PipeSet pipes; bool pipes_ready; Frame F; bool drew;
    Rect view;                    // the preview, logical px
    bool anim_menu;
} X = {.idle_ref = true};

static const char *FNF_NAMES[] = {"idle", "danceLeft", "danceRight", "singLEFT", "singDOWN", "singUP", "singRIGHT",
                                  "singLEFTmiss", "singDOWNmiss", "singUPmiss", "singRIGHTmiss", "hey", "cheer", "idle-hold"};

bool exporter_is_open(void) { return X.open; }

void exporter_open(const int *syms, int n) {
    Doc *d = app.doc;
    X.n = export_clip_sources(d, syms, n, X.clips, 64);
    if (!X.n) { app_toast("Nothing to export yet. Draw something, or make symbols with F8"); return; }
    for (int i = 0; i < X.n; i++) export_clip_settings(d, X.clips[i].key, X.clips[i].name);   // settings exist from now on
    if (!export_clip_bounds(d, X.clips, X.n, X.box)) { X.box[0] = X.box[1] = 0; X.box[2] = d->w; X.box[3] = d->h; }
    if (!d->xp.id[0]) {   // the character id: the document's name, lower case, no spaces
        int o = 0;
        for (const char *s = d->name; *s && o < 47; s++) d->xp.id[o++] = *s == ' ' ? '-' : (char)tolower((u8)*s);
        d->xp.id[o] = 0;
    }
    X.open = true; X.cur = 0; X.frame = 1; X.playing = false; X.fitted = false; X.anim_menu = false;
}

static ExportClip *cur_set(void) { return export_clip_settings(app.doc, X.clips[X.cur].key, X.clips[X.cur].name); }
static int idle_clip(void) {   // the animation the faint reference shows
    for (int i = 0; i < X.n; i++) { const char *a = export_clip_settings(app.doc, X.clips[i].key, X.clips[i].name)->anim; if (!strcmp(a, "idle")) return i; }
    for (int i = 0; i < X.n; i++) { const char *a = export_clip_settings(app.doc, X.clips[i].key, X.clips[i].name)->anim; if (!strncmp(a, "dance", 5)) return i; }
    return 0;
}

// clip space -> preview target pixels, for a clip with these offsets
static Mat placement(const float off[2], float px_scale) {
    Doc *d = app.doc;
    float s = (d->xp.scale > 0 ? d->xp.scale : 1) * X.zoom * px_scale;
    V2 o = {(X.view.w / 2) * px_scale, (X.view.h * 0.82f) * px_scale};               // the origin on screen
    V2 feet = {(X.box[0] + X.box[2]) / 2, X.box[3]};                                  // bottom middle of the box
    return mat_mul(mat_mul(mat_tr(o.x, o.y), mat_scale(s, s)), mat_tr(d->xp.pos[0] - off[0] - feet.x, d->xp.pos[1] - off[1] - feet.y));
}

/* ── preview rendering (called by the main loop, like the stage) ── */

int exporter_lists(DrawList **out, int cap) {
    X.drew = false;
    if (!X.open || !X.tgt.v_tex.id) return 0;
    Doc *d = app.doc;
    frame_begin(&X.F, &X.tgt, &X.pool, font_view());
    int ic = idle_clip();
    static const float zero[2] = {0, 0};
    if (X.idle_ref && ic != X.cur) export_render_clip(&X.F, d, &X.clips[ic], 1, placement(zero, ui.scale), 0.28f);
    export_render_clip(&X.F, d, &X.clips[X.cur], X.frame, placement(cur_set()->off, ui.scale), 1);
    X.drew = true;
    return frame_lists(&X.F, out, cap);
}

void exporter_render(void) {
    if (!X.drew) return;
    static const float clear[4] = {0, 0, 0, 0};
    if (!X.pipes_ready) { X.pipes = gfx_make_pipes(8, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL); X.pipes_ready = true; }
    frame_passes(&X.F, &X.pipes, clear);
}

/* ── small widgets ── */

static bool check(const char *id, Rect r, bool *v, const char *label, const char *tip) {
    UiId uid = ui_id(id);
    UiResp re = ui_interact(uid, r);
    Rect b = {r.x, r.y + (r.h - 14) / 2, 14, 14};
    if (*v) { dl_rrect(ui.dl, b, 3, C_AC); icon_draw(ui.dl, IC_CHECK, v2(b.x + 7, b.y + 7), ICON_S, false, hex(0xffffff)); }
    else dl_rrect_border(ui.dl, b, 3, 1.2f, re.hover ? C_TX2 : C_TX3);
    font_draw_in(ui.dl, F_UI, rect(r.x + 22, r.y, r.w - 22, r.h), ALIGN_LEFT, label, re.hover ? C_TX : C_TX2);
    if (tip) ui_tooltip(uid, r, tip);
    if (re.clicked) *v = !*v;
    return re.clicked;
}

static bool num_field(const char *id, Rect r, float *v, const char *prefix, bool integer) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), integer ? "%.0f" : "%g", *v);
    if (!ui_text_field(ui_id(id), r, tmp, sizeof(tmp), prefix)) return false;
    float nv;
    if (sscanf(tmp, "%f", &nv) != 1) return false;
    *v = integer ? roundf(nv) : nv;
    return true;
}

static void label(Rect r, const char *s) { font_draw_in(ui.dl, F_UI, r, ALIGN_LEFT, s, C_TX3); }
static void heading(float x, float *y, float w, const char *s) {
    font_draw_in(ui.dl, F_UI_SEMI, rect(x, *y, w, 16), ALIGN_LEFT, s, C_TX2);
    *y += 16 + 10;
}

/* ── checks before export ── */

static const char *problem(void) {
    Doc *d = app.doc;
    if (d->xp.format == XF_SPARROW) {
        for (int i = 0; i < X.n; i++) if (!export_clip_settings(d, X.clips[i].key, X.clips[i].name)->prefix[0]) return "Give every clip an XML prefix.";
        return NULL;
    }
    if (!d->xp.id[0]) return "The character needs an ID: it names its files.";
    for (int i = 0; i < X.n; i++) {
        ExportClip *a = export_clip_settings(d, X.clips[i].key, X.clips[i].name);
        if (!a->anim[0] || !a->prefix[0]) return "Give every clip an engine animation name and XML prefix.";
        for (int k = 0; k < i; k++) {
            ExportClip *b = export_clip_settings(d, X.clips[k].key, X.clips[k].name);
            if (!strcmp(a->anim, b->anim)) return "Two clips have the same animation name.";
            if (!strcmp(a->prefix, b->prefix)) return "Two clips have the same XML prefix.";
        }
    }
    return NULL;
}

static void nudge(float dx, float dy) {   // artwork moves by (dx, dy) game pixels: offsets move against it
    Doc *d = app.doc;
    for (int i = 0; i < X.n; i++) {
        if (!X.whole && i != X.cur) continue;
        ExportClip *c = export_clip_settings(d, X.clips[i].key, X.clips[i].name);
        c->off[0] -= dx; c->off[1] -= dy;
    }
    doc_touch(d);
}

/* ── the window ── */

void exporter_window(void) {
    if (!X.open) return;
    Doc *d = app.doc;
    if (X.cur >= X.n) X.cur = 0;
    ExportClip *cs = cur_set();
    bool esc = ui.in.key_pressed[SAPP_KEYCODE_ESCAPE] && !ui.focus && !ui.popup;
    ui_overlay(true);
    Rect r = {0, M_TITLE_H, ui.w, ui.h - M_TITLE_H};
    dl_rect(ui.dl, r, C_BG);
    const float SIDE = 344;
    Rect side = {r.x + r.w - SIDE, r.y, SIDE, r.h};
    dl_rect(ui.dl, side, C_S1);
    dl_rect(ui.dl, rect(side.x, side.y, 1, side.h), C_LINE);
    font_draw(ui.dl, F_H2, r.x + 20, r.y + 30, "Export Symbols", C_TX);

    // ── preview ──
    X.view = (Rect){r.x + 16, r.y + 48, side.x - r.x - 32, r.h - 48 - 104};
    Rect v = X.view;
    int tw = (int)roundf(v.w * ui.scale), th = (int)roundf(v.h * ui.scale);
    if (tw != X.tgt.w || th != X.tgt.h || !X.tgt.v_tex.id) { target_make(&X.tgt, tw, th, 8); pool_free(&X.pool); }
    if (!X.fitted) {   // the whole character fits, with room around it
        float bw = MAX(1, X.box[2] - X.box[0]), bh = MAX(1, X.box[3] - X.box[1]);
        X.zoom = clampf(MIN(v.w * 0.7f / bw, v.h * 0.72f / bh), 0.05f, 8);
        X.fitted = true;
    }
    dl_rrect(ui.dl, v, 8, hex(0x56565c));   // mid grey: black line art and white highlights both read on it
    dl_push_clip(ui.dl, v);
    V2 o = {v.x + v.w / 2, v.y + v.h * 0.82f};
    float step = 50 * X.zoom;   // grid every 50 game pixels from the origin
    while (step < 16) step *= 2;
    for (float x = fmodf(o.x - v.x, step) + v.x; x < v.x + v.w; x += step) dl_rect(ui.dl, rect(roundf(x), v.y, 1, v.h), hexa(0xffffff, 0x14));
    for (float y = fmodf(o.y - v.y, step) + v.y; y < v.y + v.h; y += step) dl_rect(ui.dl, rect(v.x, roundf(y), v.w, 1), hexa(0xffffff, 0x14));
    if (X.tgt.v_tex.id) dl_image(ui.dl, v, X.tgt.v_tex.id, 0xffffffff, 0, 0, 1, 1);
    dl_rect(ui.dl, rect(v.x, roundf(o.y), v.w, 1), hexa(0xffffff, 0x70));            // the ground
    Color gold = hex(0xf2c94c);
    dl_rect(ui.dl, rect(o.x - 10, roundf(o.y) - 1, 20, 2), gold);                    // the origin (0, 0)
    dl_rect(ui.dl, rect(roundf(o.x) - 1, o.y - 10, 2, 20), gold);
    dl_pop_clip(ui.dl);
    UiResp pv = ui_interact(ui_id("xp-preview"), v);
    ui_tooltip(ui_id("xp-preview"), v, "Gold cross: where the game puts the character's feet. The wheel zooms");
    if (pv.hover && ui.in.wheel_y) X.zoom = clampf(X.zoom * powf(1.15f, ui.in.wheel_y), 0.05f, 16);

    // playback
    int len = export_clip_len(&X.clips[X.cur]);
    if (X.playing) {
        X.t += ui.dt;
        double spf = 1.0 / MAX(1, cs->fps);
        while (X.t >= spf) { X.t -= spf; X.frame = X.frame >= len ? 1 : X.frame + 1; }
    }
    X.frame = CLAMP(X.frame, 1, len);
    float cy = v.y + v.h + 12;
    Rect pb = {v.x, cy, 96, 30};
    if (ui_text_button(ui_id("xp-play"), pb, X.playing ? "Pause" : "Play", X.playing)) { X.playing = !X.playing; X.t = 0; }
    char fs[48]; snprintf(fs, sizeof(fs), "Frame %d / %d", X.frame, len);
    font_draw_in(ui.dl, F_MONO_MED, rect(pb.x + 108, cy, 140, 30), ALIGN_LEFT, fs, C_TX2);
    Rect fsl = {pb.x + 250, cy + 5, 220, 20};
    if (len > 1) X.frame = (int)roundf(ui_slider(ui_id("xp-frame"), fsl, (float)X.frame, 1, (float)len));
    char zs[16]; snprintf(zs, sizeof(zs), "%d%%", (int)roundf(X.zoom * 100));
    Rect zm = {v.x + v.w - 120, cy, 30, 30}, zp = {v.x + v.w - 30, cy, 30, 30};
    if (ui_text_button(ui_id("xp-zoom-out"), zm, "\xE2\x88\x92", false)) X.zoom = MAX(0.05f, X.zoom / 1.25f);
    font_draw_in(ui.dl, F_MONO_MED, rect(zm.x + 30, cy, 60, 30), ALIGN_CENTER, zs, C_TX2);
    if (ui_text_button(ui_id("xp-zoom-in"), zp, "+", false)) X.zoom = MIN(16, X.zoom * 1.25f);
    cy += 30 + 14;
    check("xp-idle", rect(v.x, cy, 150, 24), &X.idle_ref, "Idle Reference", "The idle, faint, where it sits with no offsets of its own");
    check("xp-whole", rect(v.x + 170, cy, 200, 24), &X.whole, "Move Whole Character", "Off: arrows move this animation. On: every animation together");
    check("xp-quarter", rect(v.x + 390, cy, 190, 24), &X.quarter, "Quarter-pixel Steps", "Arrows move by 1 game pixel, or 0.25 with this on");
    float st = X.quarter ? 0.25f : 1;
    const char *tips[4] = {"Move left  (\xE2\x86\x90)", "Move right  (\xE2\x86\x92)", "Move up  (\xE2\x86\x91)", "Move down  (\xE2\x86\x93)"};
    const char *glyph[4] = {"\xE2\x86\x90", "\xE2\x86\x92", "\xE2\x86\x91", "\xE2\x86\x93"};
    const float dir[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        Rect b = {v.x + v.w - 4 * 34 + i * 34, cy - 3, 30, 30};
        UiId bid = ui_idi("xp-arrow", i);
        if (ui_text_button(bid, b, glyph[i], false)) nudge(dir[i][0] * st, dir[i][1] * st);
        ui_tooltip(bid, b, tips[i]);
    }
    if (!ui.focus) {   // the keyboard arrows nudge too
        static const int keys[4] = {SAPP_KEYCODE_LEFT, SAPP_KEYCODE_RIGHT, SAPP_KEYCODE_UP, SAPP_KEYCODE_DOWN};
        for (int i = 0; i < 4; i++) if (ui.in.key_pressed[keys[i]]) nudge(dir[i][0] * st, dir[i][1] * st);
    }

    // ── side panel ──
    float x = side.x + 20, w = SIDE - 40, y = side.y + 20;
    heading(x, &y, w, "Format");
    static const char *formats[] = {"Sparrow spritesheet", "Friday Night Funkin' V-Slice", "Friday Night Funkin' Psych"};
    static const int forder[] = {XF_SPARROW, XF_VSLICE, XF_PSYCH};
    for (int i = 0; i < 3; i++) {
        Rect row = {x - 6, y, w + 12, 28};
        UiResp re = ui_interact(ui_idi("xp-format", i), row);
        bool on = d->xp.format == forder[i];
        if (re.hover) dl_rrect(ui.dl, row, 6, C_S2);
        V2 c = {row.x + 16, row.y + 14};
        dl_circle_border(ui.dl, c, 6, 1.5f, on ? C_AC : C_TX3);
        if (on) dl_circle(ui.dl, c, 3, C_AC);
        font_draw_in(ui.dl, F_UI, rect(row.x + 30, row.y, row.w - 30, row.h), ALIGN_LEFT, formats[i], on || re.hover ? C_TX : C_TX2);
        if (re.clicked && !on) { d->xp.format = forder[i]; doc_touch(d); }
        y += 30;
    }
    y += 12;
    bool fnf = d->xp.format != XF_SPARROW;
    if (fnf) {
        heading(x, &y, w, "Character");
        label(rect(x, y, 100, 30), "Character ID");
        if (ui_text_field(ui_id("xp-id"), rect(x + 104, y, w - 104, 30), d->xp.id, sizeof(d->xp.id), NULL)) doc_touch(d);
        y += 36;
        label(rect(x, y, 100, 30), "Game Scale");
        if (num_field("xp-scale", rect(x + 104, y, w - 104, 30), &d->xp.scale, NULL, false)) { d->xp.scale = clampf(d->xp.scale, 0.05f, 20); doc_touch(d); }
        y += 36;
        float hw = (w - 8) / 2;
        if (num_field("xp-posx", rect(x, y, hw, 30), &d->xp.pos[0], "Position X", false)) doc_touch(d);
        if (num_field("xp-posy", rect(x + hw + 8, y, hw, 30), &d->xp.pos[1], "Position Y", false)) doc_touch(d);
        ui_tooltip(ui_id("xp-posx"), rect(x, y, w, 30), fnf && d->xp.format == XF_VSLICE ? "Position: the character's offsets in its JSON" : "Position in the stage");
        y += 30 + 16;
    }
    heading(x, &y, w, "Animation");
    // pager
    Rect pl = {x - 4, y, 26, 26}, pr = {x + w - 22, y, 26, 26};
    if (ui_text_button(ui_id("xp-prev"), pl, "\xE2\x80\xB9", false) && X.n > 1) { X.cur = (X.cur + X.n - 1) % X.n; X.frame = 1; }
    if (ui_text_button(ui_id("xp-next"), pr, "\xE2\x80\xBA", false) && X.n > 1) { X.cur = (X.cur + 1) % X.n; X.frame = 1; }
    char pg[96]; snprintf(pg, sizeof(pg), "%d / %d   %s", X.cur + 1, X.n, X.clips[X.cur].name);
    font_draw_in(ui.dl, F_UI_MED, rect(x + 26, y, w - 52, 26), ALIGN_CENTER, pg, C_TX);
    y += 34;
    cs = cur_set();
    label(rect(x, y, 100, 30), fnf ? "Animation" : "Name");
    Rect af = {x + 104, y, w - 104 - 34, 30};
    if (ui_text_field(ui_id("xp-anim"), af, cs->anim, sizeof(cs->anim), NULL)) doc_touch(d);
    if (!cs->anim[0] && ui.focus != ui_id("xp-anim")) font_draw_in(ui.dl, F_UI, rect(af.x + 10, af.y, af.w - 10, 30), ALIGN_LEFT, "idle, singLEFT, danceLeft\xE2\x80\xA6", C_TX3);
    Rect ab = {af.x + af.w + 4, y, 30, 30};
    UiId abid = ui_id("xp-anim-pick");
    UiResp abr = ui_interact(abid, ab);
    if (abr.hover || ui_popup_is_open(abid)) dl_rrect(ui.dl, ab, 6, C_S3);
    icon_draw(ui.dl, IC_CARET_DOWN, v2(ab.x + 15, ab.y + 15), ICON_S, true, abr.hover ? C_TX : C_TX2);
    if (abr.clicked) ui_open_popup(abid, ab);
    ui_tooltip(abid, ab, "FNF animation names");
    y += 36;
    label(rect(x, y, 100, 30), "XML Prefix");
    if (ui_text_field(ui_id("xp-prefix"), rect(x + 104, y, w - 104, 30), cs->prefix, sizeof(cs->prefix), NULL)) doc_touch(d);
    y += 36;
    float fpsv = (float)cs->fps;
    label(rect(x, y, 100, 30), "FPS");
    if (num_field("xp-fps", rect(x + 104, y, 80, 30), &fpsv, NULL, true)) { cs->fps = CLAMP((int)fpsv, 1, 120); doc_touch(d); }
    if (check("xp-loop", rect(x + 200, y + 3, w - 200, 24), &cs->loop, "Loop", "The game repeats this animation")) doc_touch(d);
    y += 36;
    float hw = (w - 8) / 2;
    if (num_field("xp-offx", rect(x, y, hw, 30), &cs->off[0], "Offset X", false)) doc_touch(d);
    if (num_field("xp-offy", rect(x + hw + 8, y, hw, 30), &cs->off[1], "Offset Y", false)) doc_touch(d);
    y += 30 + 16;

    // every clip at a glance
    heading(x, &y, w, "Clips");
    Rect list = {x - 6, y, w + 12, side.y + side.h - 96 - y};
    static float scroll;
    float content = X.n * 28.0f;
    if (rect_has(list, ui.in.mouse) && ui.in.wheel_y) scroll -= ui.in.wheel_y * 28;
    scroll = clampf(scroll, 0, MAX(0, content - list.h));
    dl_push_clip(ui.dl, list);
    for (int i = 0; i < X.n; i++) {
        Rect row = {list.x, list.y + i * 28 - scroll, list.w, 26};
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        ExportClip *c = export_clip_settings(d, X.clips[i].key, X.clips[i].name);
        UiResp re = ui_interact(ui_idi("xp-clip", i), row);
        if (i == X.cur) dl_rrect(ui.dl, row, 6, C_S3); else if (re.hover) dl_rrect(ui.dl, row, 6, C_S2);
        font_draw_in(ui.dl, F_UI, rect(row.x + 10, row.y, row.w * 0.55f, row.h), ALIGN_LEFT, X.clips[i].name, i == X.cur || re.hover ? C_TX : C_TX2);
        bool missing = fnf && !c->anim[0];
        font_draw_in(ui.dl, F_MONO, rect(row.x, row.y, row.w - 10, row.h), ALIGN_RIGHT, missing ? "no name" : c->anim[0] ? c->anim : "", missing ? hex(0xf2c94c) : C_TX3);
        if (re.clicked) { X.cur = i; X.frame = 1; }
    }
    dl_pop_clip(ui.dl);

    // export
    const char *why = problem();
    if (why) font_draw_in(ui.dl, F_UI, rect(x, side.y + side.h - 88, w, 18), ALIGN_LEFT, why, hex(0xf2c94c));
    Rect eb = {x + w - 160, side.y + side.h - 56, 160, 38}, cb = {x, side.y + side.h - 56, 100, 38};
    bool close = ui_text_button(ui_id("xp-close"), cb, "Close", false);
    UiResp er = ui_interact(ui_id("xp-export"), eb);
    dl_rrect(ui.dl, eb, 8, why ? col_a(C_AC, 0x50) : er.hover ? col_mix(C_AC, hex(0xffffff), 0.12f) : C_AC);
    icon_draw(ui.dl, IC_EXPORT, v2(eb.x + 26, eb.y + 19), ICON_M, false, hex(0xffffff));
    font_draw_in(ui.dl, F_UI_SEMI, rect(eb.x + 44, eb.y, eb.w - 50, eb.h), ALIGN_LEFT, "Export", hex(0xffffff));
    if (er.clicked && !why) {
        char dest[512], msg[300];
        bool picked = d->xp.format == XF_SPARROW ? plat_save_dialog(dest, sizeof(dest), "Sparrow spritesheet", "png", d->xp.id[0] ? d->xp.id : d->name)
                                                  : plat_pick_folder(dest, sizeof(dest), d->xp.format == XF_VSLICE ? "Your V-Slice mod folder" : "Your Psych Engine mod folder");
        if (picked) {
            if (export_clips(X.clips, X.n, d->xp.format, dest, msg, sizeof(msg))) X.open = false;
            app_toast("%s", msg);
        }
    }

    // FNF animation names (drawn last, over the panel)
    if (ui_popup_begin(abid, rect(ab.x + ab.w - 200, ab.y + ab.h + 4, 200, 10 + 28 * COUNT(FNF_NAMES)))) {
        for (int i = 0; i < COUNT(FNF_NAMES); i++)
            if (ui_menu_item(FNF_NAMES[i], NULL, true, !strcmp(cs->anim, FNF_NAMES[i]))) { snprintf(cs->anim, sizeof(cs->anim), "%s", FNF_NAMES[i]); doc_touch(d); }
        ui_popup_end();
    }
    ui_overlay(false);
    if (close || esc) X.open = false;
}

// self-tests: the window driven directly
ClipSrc *exporter_clips(int *n) { *n = X.n; return X.clips; }
void exporter_select(int i) { if (i >= 0 && i < X.n) { X.cur = i; X.frame = 1; } }
void exporter_nudge(float dx, float dy, bool whole) { bool w = X.whole; X.whole = whole; nudge(dx, dy); X.whole = w; }
