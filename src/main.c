// Entry point: sokol_app callbacks, input collection and the per-frame render order.
#include "app/app.h"
#include "platform/platform.h"
#include "gfx/font.h"
#include "render/scene.h"
#include "gfx/logo.h"
#include "gfx/cursors.h"
#include "audio/audio.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_time.h"
#include "sokol/sokol_app.h"
#include <stdarg.h>

static struct {
    float scale;
    u64 last;
    double last_press; V2 last_press_pos;
    const char *capture; int capture_frame; bool demo, selftest;
    int page; bool save_test;    // --selftest=settings-*
    bool home, splash;           // start screen / splash (a plain launch, or --home / --splash)
    double splash_t;
    float bench;                 // --bench=ZOOM: re-render the stage every frame at that zoom and report timings
    double bench_cpu, bench_dt; int bench_n;
    const char *open;
    int goto_frame;              // --frame=N: show this frame of the opened document
    int goto_layer;              // --layer=N: make the Nth layer (from the top) active and show Properties
    int key_release;
    int variant;   // --selftest=menu | --selftest=layers
    int frames;
    bool cursor_hidden;
} M;

void vx_log(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
}


/* ── scripted input (--selftest): drives the real UI input path, then captures ── */

static void in_move(V2 p) { ui.in.mouse = p; ui.in.mouse_inside = true; if (ui.nmsamples < COUNT(ui.msamples)) ui.msamples[ui.nmsamples++] = p; }
static void in_key(int k, u32 mods) { ui.in.key_down[k] = true; ui.in.key_pressed[k] = true; ui.in.mods = mods; M.key_release = k; }
static V2 curve_pt(int which, float t) {
    float x = 260 + t * 760, y;
    if (which == 0) y = 360 + sinf(t * 2 * VX_PI) * 120;
    else if (which == 1) y = 420 + sinf(t * 2 * VX_PI + 0.8f) * 90;
    else { x = 560 + t * 260; y = 250 + t * 300; }
    return stage_to_screen(v2(x, y));
}
static void stroke_frames(int f, int start, int len, int which) {
    if (f < start || f > start + len) return;
    if (f == start) { in_move(curve_pt(which, 0)); ui.in.down[0] = true; ui.in.pressed[0] = true; return; }
    if (f == start + len) { ui.in.down[0] = false; ui.in.released[0] = true; return; }
    for (int i = 1; i <= 6; i++) in_move(curve_pt(which, ((f - start - 1) * 6 + i) / (6.0f * (len - 1))));
}
static void selftest(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (M.variant) {
        V2 p = M.variant == 1 ? v2(118, 18) : v2(50, ui.h - app.tl_h + 19);
        if (f == 5) { in_move(p); ui.in.down[0] = true; ui.in.pressed[0] = true; }
        if (f == 6) { ui.in.down[0] = false; ui.in.released[0] = true; }
        if (f == 8) in_move(v2(p.x + 40, p.y + (M.variant == 1 ? 62 : 70)));
        return;
    }
    if (f == 5) in_key(SAPP_KEYCODE_B, 0);
    stroke_frames(f, 10, 30, 0);                 // ink on frame 1
    if (f == 44) in_key(SAPP_KEYCODE_PERIOD, 0); // frame 2
    if (f == 45) in_key(SAPP_KEYCODE_F7, 0);     // blank keyframe
    if (f == 47) app_set_color(hex(0xff6f5e));
    stroke_frames(f, 50, 24, 1);                 // red on frame 2
    if (f == 78) in_key(SAPP_KEYCODE_O, MOD_ALT | MOD_SHIFT);  // onion skin
    if (f == 80) in_key(SAPP_KEYCODE_E, 0);
    stroke_frames(f, 82, 14, 2);                 // erase across the red stroke
    if (f == 98) in_key(SAPP_KEYCODE_B, 0);
    if (f == 100) in_key(SAPP_KEYCODE_N, MOD_CTRL | MOD_SHIFT);  // new layer
    if (f == 103) app_set_color(hex(0x2f6fdb));
    stroke_frames(f, 105, 16, 2);                // blue on the new layer
}

static void click_at(int f, int at, V2 screen) {
    if (f == at) { in_move(screen); ui.in.down[0] = true; ui.in.pressed[0] = true; }
    if (f == at + 1) { ui.in.down[0] = false; ui.in.released[0] = true; }
}
static void drag_line(int f, int start, int len, V2 a, V2 b) {
    if (f < start || f > start + len) return;
    if (f == start) { in_move(stage_to_screen(a)); ui.in.down[0] = true; ui.in.pressed[0] = true; return; }
    if (f == start + len) { ui.in.down[0] = false; ui.in.released[0] = true; return; }
    for (int i = 1; i <= 4; i++) {
        float t = ((f - start - 1) * 4 + i) / (4.0f * (len - 1));
        in_move(stage_to_screen(v2(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t))));
    }
}

// --selftest=edit: draw a '#', trim the overshoots, fill, make a symbol, save, reopen, export
static void selftest_edit(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 3) app.brush_size = 10;
    if (f == 4) in_key(SAPP_KEYCODE_B, 0);
    drag_line(f, 6, 10, v2(400, 250), v2(880, 250));
    drag_line(f, 18, 10, v2(400, 470), v2(880, 470));
    drag_line(f, 30, 10, v2(500, 170), v2(500, 550));
    drag_line(f, 42, 10, v2(780, 170), v2(780, 550));
    if (f == 54) in_key(SAPP_KEYCODE_C, 0);
    static const float cuts[8][2] = {{450, 250}, {830, 250}, {450, 470}, {830, 470}, {500, 205}, {500, 515}, {780, 205}, {780, 515}};
    for (int i = 0; i < 8; i++) click_at(f, 56 + i * 3, stage_to_screen(v2(cuts[i][0], cuts[i][1])));
    if (f == 82) { vx_log("selftest: items after trim = %d", st_drawing(false) ? (int)arrlen(st_drawing(false)->items) : -1); app_set_color(hex(0xff6f5e)); }
    if (f == 83) in_key(SAPP_KEYCODE_K, 0);
    click_at(f, 85, stage_to_screen(v2(640, 360)));
    if (f == 90) vx_log("selftest: items after fill = %d (first kind %d)", (int)arrlen(st_drawing(false)->items), st_drawing(false)->items[0].kind);
    if (f == 92) in_key(SAPP_KEYCODE_V, 0);
    if (f == 94) in_key(SAPP_KEYCODE_A, MOD_CTRL);
    if (f == 96) in_key(SAPP_KEYCODE_F8, 0);
    if (f == 100) {
        vx_log("selftest: symbols = %d, items = %d", (int)arrlen(app.doc->symbols), (int)arrlen(st_drawing(false)->items));
        bool ok = doc_save(app.doc, "D:/Programming/Vertexa/research/shots/v2/selftest.vxa");
        vx_log("selftest: save %s", ok ? "ok" : "FAILED");
    }
    if (f == 102) {
        bool ok = app_open_path("D:/Programming/Vertexa/research/shots/v2/selftest.vxa");
        vx_log("selftest: reopen %s, symbols = %d, scene items = %d", ok ? "ok" : "FAILED", (int)arrlen(app.doc->symbols),
               st_drawing(false) ? (int)arrlen(st_drawing(false)->items) : -1);
    }
    if (f == 104) { in_key(SAPP_KEYCODE_Q, 0); }
    if (f == 106) { app.right_tab = 1; }
    if (f == 108) {
        char msg[200];
        ClipSrc cs[64];
        int n = export_clip_sources(app.doc, NULL, 0, cs, 64);
        export_clips(cs, n, XF_SPARROW, "D:/Programming/Vertexa/research/shots/v2/selftest_sheet.png", msg, sizeof(msg));
        vx_log("selftest: %s", msg);
    }
    // select the instance again for the screenshot
    click_at(f, 140, stage_to_screen(v2(640, 300)));
}

// --selftest=select: a '#', a section between intersections picked and dragged (the document only changes
// on the drag), a box in Sections mode, then Free Transform with the transformation point moved
static void drag_screen(int f, int start, int len, V2 a, V2 b) {
    if (f < start || f > start + len) return;
    if (f == start) { in_move(a); ui.in.down[0] = true; ui.in.pressed[0] = true; return; }
    if (f == start + len) { in_move(b); ui.in.down[0] = false; ui.in.released[0] = true; return; }
    float t = (float)(f - start) / len;
    in_move(v2(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t)));
}
// --selftest=shape[-edit]: a rectangle; a box over its right part picks only that part, which is dragged off.
// shape-edit: the right edge bent outward, a Ctrl-drag on the top edge makes a spike, a brush stroke bent
static void selftest_shape(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 2) { app.stroke = hex(0xff3b30); app.fill = hex(0x8b7bff); app.shape_width = 6; }   // outline and fill told apart
    if (f == 3) in_key(SAPP_KEYCODE_R, 0);
    drag_line(f, 5, 8, v2(400, 200), v2(800, 500));
    if (M.page) {
        if (f == 15) in_key(SAPP_KEYCODE_B, 0);
        drag_line(f, 17, 8, v2(300, 620), v2(1000, 620));
    }
    if (f == 28) in_key(SAPP_KEYCODE_V, 0);
    if (!M.page) {
        drag_screen(f, 30, 8, stage_to_screen(v2(650, 150)), stage_to_screen(v2(900, 560)));
        float b[4]; V2 pv;
        if (f == 41 && select_info(b, &pv)) vx_log("selftest: box: picked %d, items %d, picked bounds %.0f %.0f %.0f %.0f", select_count(), (int)arrlen(st_drawing(false)->items), b[0], b[1], b[2], b[3]);
        drag_screen(f, 43, 8, stage_to_screen(v2(720, 350)), stage_to_screen(v2(820, 350)));
        if (f == 55) vx_log("selftest: after the drag: items %d", (int)arrlen(st_drawing(false)->items));
        return;
    }
    drag_screen(f, 30, 8, stage_to_screen(v2(800, 350)), stage_to_screen(v2(920, 350)));   // bend the right edge
    if (f == 41) { Drawing *dr = st_drawing(false); int pts = 0; for (ptrdiff_t i = 0; i < arrlen(dr->items); i++) pts += (int)arrlen(dr->items[i].poly) + (int)arrlen(dr->items[i].pts); vx_log("selftest: bend: items %d, points %d", (int)arrlen(dr->items), pts); }
    if (f == 43) ui.in.mods = MOD_CTRL;
    drag_screen(f, 43, 8, stage_to_screen(v2(560, 200)), stage_to_screen(v2(600, 90)));      // a spike up from the top edge
    if (f == 52) ui.in.mods = 0;
    drag_screen(f, 55, 8, stage_to_screen(v2(650, 620)), stage_to_screen(v2(650, 690)));     // bend the brush stroke
    if (f == 66) in_move(stage_to_screen(v2(200, 100)));
}

// --selftest=brushes: a row dragged two places down. brush-edit: a new brush, its settings open.
// recolor: a stroke selected, a swatch clicked: the stroke takes the color, one undo puts it back
static void selftest_brushes(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (M.page == 0) {
        drag_screen(f, 4, 6, v2(ui.w - 260, 446), v2(ui.w - 260, 540));
        if (f == 12) vx_log("selftest: order %s, %s, %s; current %d", app.presets[0].name, app.presets[1].name, app.presets[2].name, app.preset);
        if (f == 13) in_move(v2(ui.w - 200, 482));
    } else if (M.page == 1) {
        if (f == 3) brush_new();
        if (f == 6) vx_log("selftest: new brush \"%s\" (%d brushes), editing %d", app.presets[app.preset].name, (int)arrlen(app.presets), brush_editing());
    } else if (M.page == 5) {   // --selftest=csp-tip: a Clip Studio feather brush (image tip), a short and a long stroke
        if (f == 3) {
            bool ok = app_open_path("D:/Programming/Vertexa/research/sut_feather/short 2 detail.sut");
            BrushPreset *p = &app.presets[app.preset];
            vx_log("selftest: import %s: \"%s\" size %.0f, tip image \"%s\" mode %d spacing %.0f angle %.0f; %s", ok ? "ok" : "FAILED",
                   p->name, p->size, p->tip_image, p->img_mode, p->img_spacing, p->angle, app.toast);
            app.fill = hex(0x22e6c8);
            in_key(SAPP_KEYCODE_B, 0);
        }
        if (f == 6) {   // short pen strokes, light to heavy, as on the brush's page; then a long one
            for (int k = 0; k < 4; k++) {
                V2 pts[6]; float pr[6];
                for (int i = 0; i < 6; i++) { pts[i] = v2(160 + k * 250 + i * 8, 470 + i * 2); pr[i] = 0.05f + k * 0.3f; }
                st_test_stroke(pts, pr, 6);
            }
            V2 pts[40]; float pr[40];
            for (int i = 0; i < 40; i++) { float t = i / 39.0f; pts[i] = v2(150 + t * 980, 640 - sinf(t * 3.1f) * 60); pr[i] = 0.2f + 0.6f * sinf(t * 3.1f); }
            st_test_stroke(pts, pr, 40);
        }
        if (f == 20) {   // the same image laid along a line
            app.img_mode = IMG_ALONG;
            V2 pts[30]; float pr[30];
            for (int i = 0; i < 30; i++) { float t = i / 29.0f; pts[i] = v2(300 + t * 700, 130 + sinf(t * 3.1f) * 50); pr[i] = 0.1f; }
            st_test_stroke(pts, pr, 30);
        }
        if (f == 26) {
            static const char *vxa = "D:/Programming/Vertexa/research/tmp/image_stroke.vxa";
            bool sv = doc_save(app.doc, vxa), ok = sv && app_open_path(vxa);
            Drawing *dr = st_drawing(false);
            int n = 0;
            for (ptrdiff_t q = 0; dr && q < arrlen(dr->items); q++) n += dr->items[q].img_mode && bitmap_get(dr->items[q].img) != NULL;
            vx_log("selftest: saved and reopened %s, image strokes %d", ok ? "ok" : "FAILED", n);
        }
        if (f == 32) { Drawing *dr = st_drawing(false); vx_log("selftest: strokes %d, first image %d mode %d", dr ? (int)arrlen(dr->items) : 0, dr && arrlen(dr->items) ? dr->items[0].img : 0, dr && arrlen(dr->items) ? dr->items[0].img_mode : 0); }
        if (f == 33) brush_new();
        if (f == 35) in_move(v2(700, 700));
    } else if (M.page == 4) {   // --selftest=texture: Pencil, Chalk and Wash strokes, saved and read back
        static const char *names[] = {"Pencil", "Chalk", "Wash"};
        static const char *vxa = "D:/Programming/Vertexa/research/tmp/texture_test.vxa";
        for (int k = 0; k < 3; k++) {
            if (f == 3 + k * 12) {
                for (ptrdiff_t q = 0; q < arrlen(app.presets); q++) if (!strcmp(app.presets[q].name, names[k])) app_apply_preset((int)q);
                app.fill = hex(k == 2 ? 0x2f6fdb : 0x141414);
                if (k == 0) in_key(SAPP_KEYCODE_B, 0);
                if (k == 0) app.brush_size = 14;
            }
            drag_line(f, 5 + k * 12, 8, v2(260, 200 + k * 160), v2(1000, 260 + k * 160));
        }
        if (f == 40) {
            Drawing *dr = st_drawing(false);
            for (ptrdiff_t q = 0; dr && q < arrlen(dr->items); q++) { Item *it = &dr->items[q]; vx_log("selftest: stroke %d: texture %d bitmap %d density %.2f", (int)q, it->texture, it->sym, it->alpha); }
            vx_log("selftest: save %s", doc_save(app.doc, vxa) ? "ok" : "FAILED");
        }
        if (f == 42) {
            bool ok = app_open_path(vxa);
            Drawing *dr = st_drawing(false);
            int tex = 0;
            for (ptrdiff_t q = 0; dr && q < arrlen(dr->items); q++) tex += dr->items[q].texture && bitmap_get(dr->items[q].sym) != NULL;
            vx_log("selftest: reopen %s, textured strokes %d", ok ? "ok" : "FAILED", tex);
        }
        if (f == 44) brush_new();
        if (f == 46) { in_move(v2(ui.w - 150, 700)); ui.in.wheel_y = -12; }   // the editor scrolled to its lower blocks
    } else {
        if (f == 3) { app.fill = hex(0x141414); app.brush_size = 12; in_key(SAPP_KEYCODE_B, 0); }
        drag_line(f, 5, 8, v2(300, 300), v2(900, 360));
        if (f == 16) in_key(SAPP_KEYCODE_V, 0);
        if (f == 18) in_key(SAPP_KEYCODE_A, MOD_CTRL);
        click_at(f, 20, v2(ui.w - 57, 330));   // the green swatch
        if (f == 23) { Item *it = select_first(); vx_log("selftest: recolor: selected %d, color %08X", select_count(), it ? it->color : 0); }
        if (f == 24) app_undo();
        if (f == 26) { Drawing *dr = st_drawing(false); vx_log("selftest: after undo: color %08X", dr && arrlen(dr->items) ? dr->items[0].color : 0); }
        if (f == 27) app_redo();
        if (f == 28) in_move(v2(700, 600));
    }
}

static void selftest_select(int f) {
    static u32 v0; static float b[4]; static V2 pv;
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 3) app.brush_size = 10;
    if (f == 4) in_key(SAPP_KEYCODE_B, 0);
    drag_line(f, 6, 10, v2(400, 250), v2(880, 250));
    drag_line(f, 18, 10, v2(400, 470), v2(880, 470));
    drag_line(f, 30, 10, v2(500, 170), v2(500, 550));
    drag_line(f, 42, 10, v2(780, 170), v2(780, 550));
    if (f == 54) { in_key(SAPP_KEYCODE_V, 0); app.sel_mode = SELM_SECTIONS; }
    if (f == 56) v0 = app.doc->version;
    click_at(f, 57, stage_to_screen(v2(640, 250)));
    if (f == 60) vx_log("selftest: section click: selected %d, items %d, document changed %d", select_count(), (int)arrlen(st_drawing(false)->items), app.doc->version != v0);
    drag_screen(f, 62, 10, stage_to_screen(v2(640, 250)), stage_to_screen(v2(640, 300)));
    if (f == 75) vx_log("selftest: after dragging the section: items %d, selected %d", (int)arrlen(st_drawing(false)->items), select_count());
    // a box over the right part: cuts the horizontals at its edge
    drag_screen(f, 78, 8, stage_to_screen(v2(730, 140)), stage_to_screen(v2(900, 520)));
    if (f == 90) vx_log("selftest: box in Sections mode: selected %d, items %d, document changed %d", select_count(), (int)arrlen(st_drawing(false)->items), app.doc->version != v0);
    // Free Transform: everything, pivot dragged to the top-left corner, then a rotation around it
    if (f == 92) in_key(SAPP_KEYCODE_Q, 0);
    if (f == 93) in_key(SAPP_KEYCODE_A, MOD_CTRL);
    if (f == 95 && select_info(b, &pv)) vx_log("selftest: Q: bounds %.0f %.0f %.0f %.0f, pivot %.0f %.0f", b[0], b[1], b[2], b[3], pv.x, pv.y);
    if (f >= 96 && f <= 106) drag_screen(f, 96, 10, stage_to_screen(pv), stage_to_screen(v2(b[0] + 3, b[1] + 2)));
    if (f == 108 && select_info(b, &pv)) vx_log("selftest: pivot after drag %.0f %.0f (snapped to the corner)", pv.x, pv.y);
    if (f >= 110 && f <= 122) {
        V2 c = stage_to_screen(v2(b[2], b[3])), a = v2(c.x + 12, c.y + 12), e = v2(c.x - 60, c.y + 80);
        drag_screen(f, 110, 12, a, e);
    }
    if (f == 126 && select_info(b, &pv)) vx_log("selftest: after rotating: bounds %.0f %.0f %.0f %.0f, pivot %.0f %.0f", b[0], b[1], b[2], b[3], pv.x, pv.y);
    if (f == 128) in_move(stage_to_screen(v2(640, 360)));
}


// --selftest=settings[-perf|-keys|-theme|-save]: the Settings pages (same geometry as settings.c)
static V2 settings_content(void) {   // top-left of the page content, below its title
    float H = MIN(690, ui.h - 24);
    float rx = roundf((ui.w - 900) / 2), ry = roundf(MAX(12, (ui.h - H) / 2));
    return v2(rx + 200 + 32, ry + 26 + 44);
}
static void selftest_settings(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 3) settings_open(M.page);
    V2 c = settings_content();
    if (M.page == SET_THEME && !M.save_test) {   // the preset list open
        click_at(f, 8, v2(c.x + 120, c.y + 15));
        if (f == 11) in_move(v2(c.x + 120, c.y + 15 + 30 + 4 + 5 + 28 + 14));
    }
    if (M.page == SET_THEME && M.save_test) {    // Save as... "Mint"
        click_at(f, 6, v2(c.x + 248 + 46, c.y + 15));
        if (f == 12) { const char *t = "Mint"; ui.in.nchars = 0; for (const char *k = t; *k; k++) ui.in.chars[ui.in.nchars++] = *k; }
        if (f == 14) in_key(SAPP_KEYCODE_ENTER, 0);
        if (f == 18) vx_log("selftest: toast: %s", app.toast);
    }
    if (M.page == SET_KEYS) {                     // rebind "New document" to F2, then check the action fires
        float y0 = c.y + 30 + 10 + 16 + 12;      // list top: first group header, then the first row
        float cw = 900 - 200 - 64;
        click_at(f, 8, v2(c.x + cw - 260 + 63, y0 + 30 + 15));
        if (f == 11) in_key(SAPP_KEYCODE_F2, 0);
        if (f == 13) { char t[32]; vx_log("selftest: New document is now %s (%s)", act_keys(ACT_NEW), chord_text(KEYMAP[ACT_NEW][0], t, 32)); }
        if (f == 15) in_move(v2(c.x + 100, y0 + 30 + 15));
    }
}


// --selftest=tween: a rectangle on frame 1, an oval and a stroke on frame 12, shape tween, frame 6 shown
static void selftest_tween(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 3) in_key(SAPP_KEYCODE_R, 0);
    drag_line(f, 5, 8, v2(300, 200), v2(520, 420));
    if (f == 16) { doc_tl(app.doc)->frame = 12; in_key(SAPP_KEYCODE_F7, 0); }
    if (f == 18) in_key(SAPP_KEYCODE_O, 0);
    drag_line(f, 20, 8, v2(700, 260), v2(1020, 560));
    if (f == 31) in_key(SAPP_KEYCODE_B, 0);
    drag_line(f, 33, 8, v2(650, 150), v2(1050, 180));
    if (f == 45) {
        Layer *l = doc_active(app.doc);
        Layer *one[1] = {l};
        doc_set_tween(app.doc, one, 1, 1, 1, TW_SHAPE);
        doc_tl(app.doc)->frame = 6;
        app.doc->onion_on = true; app.doc->onion_prev = 5; app.doc->onion_next = 6;
        vx_log("selftest: keys %d, frame 1 items %d, frame 12 items %d", (int)arrlen(l->keys),
               (int)arrlen(app.doc->drawings[l->keys[0].d].items), (int)arrlen(app.doc->drawings[l->keys[1].d].items));
    }
}


// --selftest=timeline: Ctrl-picked layers dragged into a folder, two Ctrl-picked frame ranges get F6,
// the selection dragged along, the wheel scrolls the frames
static V2 tl_cell(int row, int frame) {   // centre of a timeline cell (no scrolling)
    float top = ui.h - app.tl_h + 1 + M_TLBAR_H + M_RULER_H;
    return v2(M_NAMES_W + (frame - 1) * 18 + 9, top + row * M_ROW_H + 12);
}
static void log_layers(const char *when) {
    Timeline *t = doc_tl(app.doc);
    char b[400] = ""; int o = 0;
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) {
        Layer *l = &t->layers[i];
        o += snprintf(b + o, sizeof(b) - o, "%s%s(%s)", i ? ", " : "", l->name, l->parent ? doc_layer(app.doc, l->parent)->name : "-");
        for (ptrdiff_t k = 0; k < arrlen(l->keys) && o < 360; k++) o += snprintf(b + o, sizeof(b) - o, " k%d", l->keys[k].f);
    }
    vx_log("selftest: %s: %s", when, b);
}
static void selftest_timeline(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    Doc *d = app.doc;
    if (f == 3) {
        const char *nm[] = {"Layer 2", "Layer 3"};
        for (int i = 0; i < 2; i++) { Layer *l = doc_add_layer(d, LT_VECTOR, nm[i]); doc_tl(d)->active = l->id; }
        Layer *fo = doc_add_layer(d, LT_FOLDER, "Folder"); doc_tl(d)->active = fo->id;
        log_layers("start");
    }
    float nx = 120;
    click_at(f, 6, v2(nx, tl_cell(1, 1).y));                       // Layer 3
    if (f == 8) { ui.in.mods = MOD_CTRL; M.key_release = SAPP_KEYCODE_LEFT_CONTROL; }
    if (f == 8) { in_move(v2(nx, tl_cell(3, 1).y)); ui.in.down[0] = true; ui.in.pressed[0] = true; ui.in.mods = MOD_CTRL; }
    if (f == 9) { ui.in.down[0] = false; ui.in.released[0] = true; }
    drag_screen(f, 12, 8, v2(nx, tl_cell(1, 1).y), v2(nx, tl_cell(0, 1).y));   // onto the folder's middle
    if (f == 22) log_layers("after the drag into the folder");
    // frames on Layer 2 (now row 3): frame 5, Ctrl frame 10, then F6
    click_at(f, 24, tl_cell(3, 5));
    if (f == 27) { in_move(tl_cell(3, 10)); ui.in.down[0] = true; ui.in.pressed[0] = true; ui.in.mods = MOD_CTRL; }
    if (f == 28) { ui.in.down[0] = false; ui.in.released[0] = true; }
    if (f == 30) in_key(SAPP_KEYCODE_F6, 0);
    if (f == 33) log_layers("F6 on two ranges");
    drag_screen(f, 35, 8, tl_cell(3, 5), tl_cell(3, 8));          // both ranges move by 3
    if (f == 46) log_layers("after moving the frames");
    if (f == 48) { in_move(tl_cell(2, 20)); ui.in.wheel_y = -6; }
}


// --selftest=camera[-view]: a camera layer, its frame dragged and zoomed, a PNG exported through it
static V2 cam_corner(int i) {
    float cam[4];
    doc_camera_at(app.doc, doc_tl(app.doc)->frame, cam);
    Mat inv = mat_inv(doc_camera_matrix(app.doc, cam));
    V2 c[4] = {{0, 0}, {(float)app.doc->w, 0}, {(float)app.doc->w, (float)app.doc->h}, {0, (float)app.doc->h}};
    return stage_to_screen(mat_ap(inv, c[i]));
}
static void selftest_camera(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 3) app.brush_size = 30;
    drag_line(f, 5, 10, v2(200, 200), v2(1080, 520));
    drag_line(f, 17, 10, v2(200, 520), v2(1080, 200));
    if (f == 30) { Layer *l = doc_add_layer(app.doc, LT_CAMERA, "Camera"); doc_tl(app.doc)->active = l->id; }
    static V2 a, b;
    if (f == 32) { a = stage_to_screen(v2(640, 360)); b = stage_to_screen(v2(760, 420)); }
    drag_screen(f, 33, 8, a, b);                                  // move the frame
    if (f == 44) { a = cam_corner(0); b = v2(a.x + 180, a.y + 101); }
    drag_screen(f, 45, 8, a, b);                                  // a corner inward: zoom in
    if (f == 56) {
        float cam[4];
        doc_camera_at(app.doc, 1, cam);
        vx_log("selftest: camera %.0f %.0f zoom %.2f turn %.2f", cam[0], cam[1], cam[2], cam[3]);
        char msg[200];
        export_png_sequence("D:/Programming/Vertexa/research/tmp/camera_png", msg, sizeof(msg));
    }
    if (f == 64 && M.page) *stage_camera_view() = true;
}


// --selftest=export-symbols[-window]: three strokes become symbols (Idle, Left, Left Miss), the export
// window opens with them, an offset is nudged, and they go out as a V-Slice character
static void selftest_export(int f) {
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    static const float ys[3] = {200, 360, 520};
    static const char *names[3] = {"Idle", "Left", "Left Miss"};
    if (f == 3) app.brush_size = 24;
    for (int i = 0; i < 3; i++) {
        int b = 5 + i * 22;
        if (f == b) in_key(SAPP_KEYCODE_B, 0);
        drag_line(f, b + 2, 8, v2(400 + i * 40, ys[i]), v2(800 + i * 60, ys[i] + 60));
        if (f == b + 12) in_key(SAPP_KEYCODE_V, 0);
        click_at(f, b + 14, stage_to_screen(v2(600 + i * 50, ys[i] + 30)));
        if (f == b + 17) in_key(SAPP_KEYCODE_F8, 0);
        if (f == b + 19 && arrlen(app.doc->symbols) > i) snprintf(app.doc->symbols[i].name, 64, "%s", names[i]);
    }
    if (M.page == 2) {   // --selftest=library: the Library tab, the first symbol, then Ctrl the third
        if (f == 72) { app.right_tab = 1; }
        click_at(f, 74, v2(ui.w - 160, 477));
        if (f == 77) { in_move(v2(ui.w - 160, 549)); ui.in.down[0] = true; ui.in.pressed[0] = true; ui.in.mods = MOD_CTRL; }
        if (f == 78) { ui.in.down[0] = false; ui.in.released[0] = true; }
        if (f == 80) { int ids[8], n = library_selection(ids, 8); vx_log("selftest: library picks %d", n); }
        return;
    }
    if (f == 72) {
        int ids[3], n = 0;
        for (ptrdiff_t i = 0; i < arrlen(app.doc->symbols) && n < 3; i++) ids[n++] = app.doc->symbols[i].id;
        exporter_open(ids, n);
        app.doc->xp.format = XF_VSLICE;
        snprintf(app.doc->xp.id, sizeof(app.doc->xp.id), "testchar");
        exporter_nudge(3, -2, false);          // the idle: 3 px right, 2 px up
        int nc; ClipSrc *cs = exporter_clips(&nc);
        vx_log("selftest: %d symbols, %d clips", (int)arrlen(app.doc->symbols), nc);
        for (int i = 0; i < nc; i++) {
            ExportClip *c = export_clip_settings(app.doc, cs[i].key, cs[i].name);
            vx_log("selftest: clip %s -> anim \"%s\" prefix \"%s\" offsets %g %g", cs[i].name, c->anim, c->prefix, c->off[0], c->off[1]);
        }
        exporter_select(1);
    }
    if (f == 76 && !M.page) {
        int nc; ClipSrc *cs = exporter_clips(&nc);
        char msg[300];
        export_clips(cs, nc, XF_VSLICE, "D:/Programming/Vertexa/research/tmp/vslice_symbols", msg, sizeof(msg));
    }
    if (f == 90 && !M.page) vx_log("selftest: %s", app.toast);
    if (f == 91 && !M.page) {   // the export settings travel with the document
        const char *p = "D:/Programming/Vertexa/research/tmp/export_settings.vxa";
        bool ok = doc_save(app.doc, p) && app_open_path(p);
        Doc *d = app.doc;
        ExportClip *c = arrlen(d->xp.clips) ? &d->xp.clips[0] : NULL;
        vx_log("selftest: reopened %s: id \"%s\", format %d, %d clip settings, first %s offsets %g %g", ok ? "ok" : "FAILED", d->xp.id, d->xp.format,
               (int)arrlen(d->xp.clips), c ? c->anim : "-", c ? c->off[0] : 0, c ? c->off[1] : 0);
    }
}

// --selftest=fnf: four labelled frames, FNF (V-Slice + Psych) and PNG sequence exports, a Free Transform
// handle drag, then the Trim Edge hover preview for the screenshot
#define FNF_OUT "D:/Programming/Vertexa/research/tmp/"
static bool sel_bounds_now(float b[4]) {
    Drawing *dr = st_drawing(false);
    b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f;
    bool any = false;
    for (ptrdiff_t i = 0; dr && i < arrlen(dr->items); i++) any |= item_bounds_m(app.doc, &dr->items[i], 0, mat_id(), b, 0);
    return any;
}
static void selftest_fnf(int f) {
    static float b[4];
    static V2 h0, h1;
    if (M.key_release) { ui.in.key_down[M.key_release] = false; ui.in.mods = 0; M.key_release = 0; }
    if (f == 3) app.brush_size = 10;
    if (f == 4) in_key(SAPP_KEYCODE_B, 0);
    drag_line(f, 6, 10, v2(400, 250), v2(880, 250));
    if (f == 18) in_key(SAPP_KEYCODE_PERIOD, 0);
    if (f == 19) in_key(SAPP_KEYCODE_F7, 0);
    drag_line(f, 21, 10, v2(400, 300), v2(880, 500));
    if (f == 33) in_key(SAPP_KEYCODE_PERIOD, 0);
    if (f == 34) in_key(SAPP_KEYCODE_F7, 0);
    drag_line(f, 36, 10, v2(640, 170), v2(640, 550));
    if (f == 48) in_key(SAPP_KEYCODE_PERIOD, 0);
    if (f == 49) in_key(SAPP_KEYCODE_F7, 0);
    drag_line(f, 51, 10, v2(400, 500), v2(880, 200));
    drag_line(f, 63, 10, v2(400, 200), v2(880, 500));
    if (f == 75) {
        Layer *l = doc_active(app.doc);
        vx_log("selftest: keys = %d", (int)arrlen(l->keys));
        if (arrlen(l->keys) >= 4) {
            snprintf(l->keys[0].label, sizeof(l->keys[0].label), "idle");
            snprintf(l->keys[2].label, sizeof(l->keys[2].label), "singLEFT");
            snprintf(l->keys[3].label, sizeof(l->keys[3].label), "singLEFTmiss");
        }
        snprintf(app.doc->name, sizeof(app.doc->name), "test \"char\"");
    }
    char msg[300];
    static ClipSrc cs[64]; static int ncs;   // no symbols here: the labels are the animations
    if (f == 76) { ncs = export_clip_sources(app.doc, NULL, 0, cs, 64); export_clips(cs, ncs, XF_VSLICE, FNF_OUT "fnf_vslice", msg, sizeof(msg)); vx_log("selftest: %d clips, %s", ncs, msg); }
    if (f == 90) { vx_log("selftest: busy %d, toast: %s", export_busy(), app.toast); export_clips(cs, ncs, XF_PSYCH, FNF_OUT "fnf_psych", msg, sizeof(msg)); }
    if (f == 104) { vx_log("selftest: busy %d, toast: %s", export_busy(), app.toast); export_png_sequence(FNF_OUT "png_seq", msg, sizeof(msg)); }
    if (f == 116) vx_log("selftest: busy %d, toast: %s", export_busy(), app.toast);
    // Free Transform: drag the bottom-right handle of the X on frame 4
    if (f == 118) in_key(SAPP_KEYCODE_Q, 0);
    if (f == 120) in_key(SAPP_KEYCODE_A, MOD_CTRL);
    if (f == 122) {
        sel_bounds_now(b);
        vx_log("selftest: selected %d, bounds %.0f %.0f %.0f %.0f", select_count(), b[0], b[1], b[2], b[3]);
        h0 = stage_to_screen(v2(b[2], b[3])); h1 = v2(h0.x + 80, h0.y + 50);
    }
    if (f == 124) { in_move(h0); ui.in.down[0] = true; ui.in.pressed[0] = true; }
    if (f > 124 && f < 133) { float t = (f - 124) / 8.0f; in_move(v2(lerpf(h0.x, h1.x, t), lerpf(h0.y, h1.y, t))); }
    if (f == 133) { in_move(h1); ui.in.down[0] = false; ui.in.released[0] = true; }
    if (f == 136) { sel_bounds_now(b); vx_log("selftest: after scale %.0f %.0f %.0f %.0f", b[0], b[1], b[2], b[3]); }
    // Trim Edge hover: one arm of the X, up to the crossing
    if (f == 138) in_key(SAPP_KEYCODE_C, 0);
    if (f >= 140) in_move(stage_to_screen(v2(lerpf(b[0], b[2], 0.2f), lerpf(b[1], b[3], 0.2f))));
}

// --selftest=tabs[-close]: a drawing, a new document, an FLA, another new one; closing the first asks
static void tab_log(const char *when) {
    char b[400] = ""; int n = 0;
    for (int i = 0; i < arrlen(app.tabs); i++) n += snprintf(b + n, sizeof(b) - n, "%s%s%s%s", i ? ", " : "", app.tabs[i]->doc == app.doc || i == app.tab ? ">" : "", i == app.tab ? app.doc->name : app.tabs[i]->doc->name, app_tab_modified(i) ? "*" : "");
    vx_log("selftest: %s: %d tabs [%s], home %d", when, (int)arrlen(app.tabs), b, app.home);
}
static void selftest_tabs(int f) {
    if (f == 3) in_key(SAPP_KEYCODE_B, 0);
    drag_line(f, 4, 6, v2(300, 300), v2(700, 380));
    if (f == 12) app_new();
    if (f == 13) app_open_path("D:/Programming/Vertexa/research/testdata/rebecca/rebecca walk.fla");
    if (f == 15) app_new();
    if (f == 16) tab_log("opened");
    if (f == 18) app_close_tab(0);
    if (M.page && (f == 22 || f == 23)) { Rect w = {roundf((ui.w - 440) / 2), roundf((ui.h - 168) / 2), 440, 168}; click_at(f, 22, v2(w.x + 24 + 50, w.y + w.h - 24 - 17)); }   // Don't Save
    if (M.page && f == 26) tab_log("after Don't Save");
    if (M.page && f == 27) app_close_tab(app.tab);
    if (M.page && f == 29) tab_log("after closing the shown one");
}

// --selftest=blend: a test card. Bands of white, grey, black and orange; over them a column per blend
// mode (a gradient bar with a half-clear white disc), Alpha and Erase inside a Layer group; underneath a
// mask, blur, drop shadow, glow, adjust colour, brightness, tint and a see-through layer
static Item card_rect(float x, float y, float w, float h, Color c) {
    Item it = {.kind = IT_FILL, .color = c, .alpha = 1};
    arrput(it.starts, 0);
    arrput(it.poly, v2(x, y)); arrput(it.poly, v2(x + w, y)); arrput(it.poly, v2(x + w, y + h)); arrput(it.poly, v2(x, y + h));
    item_bounds(&it);
    return it;
}
static Item card_disc(float cx, float cy, float r, Color c) {
    Item it = {.kind = IT_FILL, .color = c, .alpha = 1};
    arrput(it.starts, 0);
    for (int i = 0; i < 64; i++) arrput(it.poly, v2(cx + r * cosf(i * VX_PI / 32), cy + r * sinf(i * VX_PI / 32)));
    item_bounds(&it);
    return it;
}
static Item card_inst(int sym, float x, float y, u8 blend) { return (Item){.kind = IT_INSTANCE, .sym = sym, .first = 1, .loop = LOOP_LOOP, .m = mat_tr(x, y), .alpha = 1, .blend = blend}; }
static Drawing *card_sym(Doc *d, int sid) { Symbol *sy = doc_symbol(d, sid); return &d->drawings[sy->tl.layers[0].keys[0].d]; }
static Drawing *card_layer(Doc *d, Layer *l) { return &d->drawings[l->keys[0].d]; }

static void selftest_blend(int f) {
    if (f == 2) {
        Doc *d = app.doc;
        d->w = 1280; d->h = 900;
        int bar = doc_add_symbol(d, "Bar", SYM_GRAPHIC), wrap_a = doc_add_symbol(d, "Alpha group", SYM_GRAPHIC), wrap_e = doc_add_symbol(d, "Erase group", SYM_GRAPHIC);
        int star = doc_add_symbol(d, "Star", SYM_GRAPHIC);
        Layer *back = &d->scene.layers[0];
        snprintf(back->name, sizeof(back->name), "Bands");
        int back_id = back->id;
        Layer *cols = doc_add_layer(d, LT_VECTOR, "Blend modes"); int cols_id = cols->id;
        Layer *fx = doc_add_layer(d, LT_VECTOR, "Effects"); int fx_id = fx->id;
        Layer *masked = doc_add_layer(d, LT_VECTOR, "Masked"); int masked_id = masked->id;
        d->scene.active = masked_id;
        Layer *mask = doc_add_layer(d, LT_MASK, "Mask"); int mask_id = mask->id;
        Layer *ghost = doc_add_layer(d, LT_VECTOR, "See-through"); int ghost_id = ghost->id;
        doc_layer(d, masked_id)->parent = mask_id;
        doc_layer(d, ghost_id)->transparent = true; doc_layer(d, ghost_id)->see_pct = 30;
        // the bar: a gradient (cyan -> magenta) and a half-clear white disc
        Item g = card_rect(-30, 0, 60, 700, 0);
        g.grad = GR_LINEAR; g.m = (Mat){0, 700 / 1638.4f, -60 / 1638.4f, 0, 0, 350};
        arrput(g.stops, ((GStop){0, hex(0x00c0ff)})); arrput(g.stops, ((GStop){1, hex(0xff3080)}));
        arrput(card_sym(d, bar)->items, g);
        arrput(card_sym(d, bar)->items, card_disc(0, 350, 26, hexa(0xffffff, 0x99)));
        // Alpha / Erase only work inside a group with Layer blending: green, then the bar with the mode
        arrput(card_sym(d, wrap_a)->items, card_rect(-30, 0, 60, 700, hex(0x30c060)));
        arrput(card_sym(d, wrap_a)->items, card_inst(bar, 0, 0, BLEND_ALPHA));
        arrput(card_sym(d, wrap_e)->items, card_rect(-30, 0, 60, 700, hex(0x30c060)));
        arrput(card_sym(d, wrap_e)->items, card_inst(bar, 0, 0, BLEND_ERASE));
        // a star for the filters
        Item st = {.kind = IT_FILL, .color = hex(0xffc040), .alpha = 1};
        arrput(st.starts, 0);
        for (int i = 0; i < 10; i++) { float r = i % 2 ? 22 : 50, a = -VX_PI / 2 + i * VX_PI / 5; arrput(st.poly, v2(r * cosf(a), r * sinf(a))); }
        item_bounds(&st);
        arrput(card_sym(d, star)->items, st);
        Drawing *bd = card_layer(d, doc_layer(d, back_id));
        static const u32 bands[4] = {0xffffff, 0x808080, 0x101010, 0xe08a30};
        for (int i = 0; i < 4; i++) arrput(bd->items, card_rect(0, 10 + i * 175, 1280, 175, hex(bands[i])));
        Drawing *cd = card_layer(d, doc_layer(d, cols_id));
        for (int i = 0; i < BLEND_COUNT; i++) {
            float x = 70 + i * 82;
            if (i == BLEND_ALPHA) arrput(cd->items, card_inst(wrap_a, x, 10, BLEND_LAYER));
            else if (i == BLEND_ERASE) arrput(cd->items, card_inst(wrap_e, x, 10, BLEND_LAYER));
            else arrput(cd->items, card_inst(bar, x, 10, (u8)i));
        }
        // effects row
        Drawing *ed = card_layer(d, doc_layer(d, fx_id));
        for (int i = 0; i < 7; i++) {
            Item it = card_inst(star, 330 + i * 130, 800, BLEND_NORMAL);
            Filter fl = {.quality = 3, .bx = 10, .by = 10, .strength = 1, .angle = 45, .dist = 8};
            if (i == 0) { fl.type = FX_BLUR; arrput(it.filters, fl); }
            if (i == 1) { fl.type = FX_SHADOW; fl.color = hexa(0x000000, 0xcc); arrput(it.filters, fl); }
            if (i == 2) { fl.type = FX_GLOW; fl.color = hex(0x40a0ff); fl.strength = 2; arrput(it.filters, fl); }
            if (i == 3) { fl.type = FX_GLOW; fl.color = hex(0xff2020); fl.flags = FXF_INNER; fl.strength = 2; arrput(it.filters, fl); }
            if (i == 4) { fl.type = FX_ADJUST; fl.adj[3] = 150; fl.adj[2] = 40; arrput(it.filters, fl); }
            if (i == 5) { it.cx.kind = CX_BRIGHTNESS; it.cx.amount = -0.6f; }
            if (i == 6) { it.cx.kind = CX_TINT; it.cx.amount = 0.7f; it.cx.tint = hex(0x30ff60); }
            arrput(ed->items, it);
        }
        // a mask: stripes, kept inside a disc
        Drawing *md = card_layer(d, doc_layer(d, masked_id));
        for (int i = 0; i < 12; i++) arrput(md->items, card_rect(60 + i * 18, 730, 9, 150, hex(i % 2 ? 0x2060ff : 0xff6020)));
        arrput(card_layer(d, doc_layer(d, mask_id))->items, card_disc(170, 805, 70, hex(0x000000)));
        // see-through: a black block at 30 % while editing
        arrput(card_layer(d, doc_layer(d, ghost_id))->items, card_rect(1210, 730, 60, 150, hex(0x000000)));
        doc_touch(d);
        stage_fit();
        vx_log("selftest: blend card, columns %d (%s ... %s), layers %d", BLEND_COUNT, BLEND_NAMES[0], BLEND_NAMES[BLEND_COUNT - 1], (int)arrlen(d->scene.layers));
    }
    if (f == 6) {   // an export ignores the see-through layer
        char msg[200] = "";
        bool ok = export_png_sequence("D:/Programming/Vertexa/research/tmp/blend_export", msg, sizeof(msg));
        vx_log("selftest: export %s: %s", ok ? "started" : "FAILED", msg);
    }
    if (f == 7) app.tool = 'V';
    if (f == 8 || f == 9) click_at(f, 8, stage_to_screen(v2(330, 800)));   // the blurred star
    if (f == 10) { app.right_tab = 2; vx_log("selftest: picked %d, filters on it %d", select_count(), select_first() ? (int)arrlen(select_first()->filters) : -1); }
    if (f == 14) {   // saved and read back: blend modes, filters, colour effects, gradients, mask, see-through
        bool ok = doc_save(app.doc, "D:/Programming/Vertexa/research/tmp/blend_card.vxa");
        char err[200] = "";
        Doc *r = ok ? doc_load("D:/Programming/Vertexa/research/tmp/blend_card.vxa", err, sizeof(err)) : NULL;
        int nfx = 0, ncx = 0, ngr = 0, nbl = 0;
        for (ptrdiff_t i = 0; r && i < arrlen(r->drawings); i++)
            for (ptrdiff_t k = 0; k < arrlen(r->drawings[i].items); k++) {
                Item *it = &r->drawings[i].items[k];
                nfx += (int)arrlen(it->filters); ncx += it->cx.kind != CX_NONE; ngr += it->grad != GR_NONE; nbl += it->blend != BLEND_NORMAL;
            }
        Layer *gl = NULL;
        for (ptrdiff_t i = 0; r && i < arrlen(r->scene.layers); i++) if (r->scene.layers[i].transparent) gl = &r->scene.layers[i];
        vx_log("selftest: reopened %s: filters %d, colour effects %d, gradients %d, blended %d, see-through %s %d%%", r ? "ok" : err,
               nfx, ncx, ngr, nbl, gl ? gl->name : "-", gl ? gl->see_pct : 0);
        doc_free(r);
    }
}

// --selftest=image: a PNG imported (Library + stage), saved into a .vxa and read back
static void selftest_image(int f) {
    static const char *png = "D:/Programming/Vertexa/research/testdata/dat_png/koala5.png", *vxa = "D:/Programming/Vertexa/research/tmp/image_test.vxa";
    if (f == 3) vx_log("selftest: import %s", app_import_image(png, v2(-1, -1)) ? "ok" : "FAILED");
    if (f == 5) app.right_tab = 1;
    if (f == 7) {
        bool ok = doc_save(app.doc, vxa);
        char err[200] = "";
        Doc *r = ok ? doc_load(vxa, err, sizeof(err)) : NULL;
        int placed = 0;
        for (ptrdiff_t i = 0; r && i < arrlen(r->drawings); i++)
            for (ptrdiff_t k = 0; k < arrlen(r->drawings[i].items); k++) placed += r->drawings[i].items[k].kind == IT_BITMAP;
        Bitmap *b = r && arrlen(r->bitmaps) ? bitmap_get(r->bitmaps[0]) : NULL;
        vx_log("selftest: reopened %s: library images %d, placed %d, first %s %dx%d", r ? "ok" : err, r ? (int)arrlen(r->bitmaps) : 0, placed,
               b ? b->name : "-", b ? b->w : 0, b ? b->h : 0);
        doc_free(r);
    }
}

static void init(void) {
    sg_setup(&(sg_desc){.environment = sglue_environment(), .logger.func = slog_func});
    stm_setup();
    M.scale = sapp_dpi_scale();
    font_init(M.scale);
    gfx_init();
    plat_init(M.scale, !M.capture);   // fixed size when capturing, so screenshots are comparable
    logo_window_icon();
    cursors_init(M.scale);
    app_init();
    stage_init();
    if (M.demo) stage_demo();
    if (M.open) app_open_path(M.open);
    if (M.goto_frame > 0) doc_tl(app.doc)->frame = M.goto_frame;
    if (M.goto_layer > 0 && M.goto_layer <= arrlen(doc_tl(app.doc)->layers)) { doc_tl(app.doc)->active = doc_tl(app.doc)->layers[M.goto_layer - 1].id; app.right_tab = 2; }
    if (M.home) {   // the start screen, without the empty document behind it
        if (arrlen(app.tabs) == 1 && !app_tab_modified(0) && !M.open) app_close_tab(0);
        app.home = true; home_prepare();
    }
    M.last = stm_now();
}

static void frame(void) {
    double dt = stm_sec(stm_laptime(&M.last));
    float s = sapp_dpi_scale() * clampf(app.ui_zoom, 0.75f, 2);   // Settings -> interface size
    if (s != M.scale) { M.scale = s; font_init(s); plat_set_scale(s); cursors_init(s); }
    int fw = sapp_width(), fh = sapp_height();

    if (M.selftest && M.variant == 3) selftest_edit(M.frames);
    else if (M.selftest && M.variant == 4) selftest_fnf(M.frames);
    else if (M.selftest && M.variant == 7) selftest_select(M.frames);
    else if (M.selftest && M.variant == 9) selftest_tween(M.frames);
    else if (M.selftest && M.variant == 10) selftest_timeline(M.frames);
    else if (M.selftest && M.variant == 12) selftest_camera(M.frames);
    else if (M.selftest && M.variant == 14) selftest_export(M.frames);
    else if (M.selftest && M.variant == 15) selftest_tabs(M.frames);
    else if (M.selftest && M.variant == 16) selftest_blend(M.frames);
    else if (M.selftest && M.variant == 17) selftest_image(M.frames);
    else if (M.selftest && M.variant == 18) selftest_shape(M.frames);
    else if (M.selftest && M.variant == 19) selftest_brushes(M.frames);
    else if (M.selftest && M.variant == 13) {   // --selftest=curve: a point added to the pressure curve, then removed
        V2 add = {ui.w - 80, 800};
        if (M.frames == 2) brush_new();   // the curve is in a brush's settings
        if (M.frames == 4) vx_log("selftest: curve points %d", app.curve.n);
        click_at(M.frames, 6, add);
        if (M.frames == 9) vx_log("selftest: after a click %d points", app.curve.n);
        if (M.frames == 11) { in_move(add); ui.in.pressed[1] = true; }
        if (M.frames == 13) vx_log("selftest: after a right-click %d points", app.curve.n);
    }
    else if (M.selftest && M.variant == 11) {   // --selftest=audio-save: a sound saved into a .vxa and read back
        static const char *vxa = "D:/Programming/Vertexa/research/tmp/audio_test.vxa";
        if (M.frames == 4) app_open_path("D:/Programming/Vertexa/research/testdata/audio/beat.wav");
        if (M.frames == 6) vx_log("selftest: save %s", doc_save(app.doc, vxa) ? "ok" : "FAILED");
        if (M.frames == 8) {
            bool ok = app_open_path(vxa);
            Layer *l = NULL;
            for (ptrdiff_t i = 0; i < arrlen(app.doc->scene.layers); i++) if (app.doc->scene.layers[i].type == LT_AUDIO) l = &app.doc->scene.layers[i];
            Sound *s = l ? sound_get(l->sound) : NULL;
            vx_log("selftest: reopen %s, audio layer %s, sound %s, %d frames at %d Hz, layer length %d", ok ? "ok" : "FAILED",
                   l ? l->name : "-", s ? s->name : "-", s ? s->nframes : 0, s ? s->rate : 0, l ? l->len : 0);
        }
    }
    else if (M.selftest && M.variant == 5) selftest_settings(M.frames);
    else if (M.selftest && M.variant == 8) {   // --selftest=recovery: a stroke, then the recovery copy is written
        if (M.frames == 4) in_key(SAPP_KEYCODE_B, 0);
        if (M.frames == 5) ui.in.key_down[SAPP_KEYCODE_B] = false;
        drag_line(M.frames, 6, 10, v2(400, 300), v2(880, 300));
        if (M.frames == 20) { app.recovery_min = 1; app_autosave_due(); }
        if (M.frames == 22) {
            char dir[1024];
            plat_config_path("recovery", dir, sizeof(dir));
            vx_log("selftest: recovery copies while running: %d", plat_count_files(dir, "*.vxa"));
        }
    }
    else if (M.selftest) selftest(M.frames);
    if (M.bench > 0) {
        Drawing *dr = st_drawing(false);
        if (M.frames == 4 && dr && arrlen(dr->items) && arrlen(dr->items[0].pts)) {
            const Item *it = &dr->items[0];
            SPt c = it->pts[arrlen(it->pts) / 2];
            stage_zoom_to(M.bench, v2(c.x, c.y));
        }
        if (M.frames > 4) { stage_pan(M.frames % 2 ? 1 : -1, 0); }   // a view change re-renders the stage each frame
        if (M.frames > 20) { M.bench_dt += dt; M.bench_n++; }
        if (M.frames == 140) {
            vx_log("bench zoom %.0f: frame %.2f ms (%.0f fps), stage CPU %.2f ms", M.bench, 1000 * M.bench_dt / M.bench_n,
                   M.bench_n / M.bench_dt, 1000 * M.bench_cpu / M.bench_n);
            sapp_request_quit();
        }
    }
    ui_begin(fw / M.scale, fh / M.scale, M.scale, dt);
    app_frame();
    if (M.splash) {   // the splash holds a moment, then fades into the start screen
        M.splash_t += dt;
        float a = M.splash_t < 1.1 ? 1 : 1 - (float)((M.splash_t - 1.1) / 0.35);
        if (ui.in.pressed[0] && M.splash_t > 0.3) M.splash_t = MAX(M.splash_t, 1.1);   // a click hurries it
        if (a <= 0) M.splash = false;
        else splash_draw(a);
    }
    ui_end();

    if (ui.hide_cursor != M.cursor_hidden) { sapp_show_mouse(!ui.hide_cursor); M.cursor_hidden = ui.hide_cursor; }
    sapp_mouse_cursor want = (sapp_mouse_cursor)cursor_for(ui.cursor);   // the app's own cursor set
    if (sapp_get_mouse_cursor() != want) sapp_set_mouse_cursor(want);

    static DrawList *lists[8192];   // a document with many blended groups has many passes
    scene_frame_start();
    u64 t_stage = stm_now();
    int n = stage_lists(lists, 4000);
    if (M.bench > 0 && M.frames > 20) M.bench_cpu += stm_sec(stm_since(t_stage));
    n += export_lists(lists + n, 2000);
    n += exporter_lists(lists + n, 2000);
    lists[n++] = &ui.base;
    lists[n++] = &ui.over;
    gfx_upload(lists, n);
    stage_render();
    export_render();
    exporter_render();
    export_after_render();
    sg_begin_pass(&(sg_pass){
        .action = {.colors[0] = {.load_action = SG_LOADACTION_CLEAR, .clear_value = {0.055f, 0.055f, 0.063f, 1}}},
        .swapchain = sglue_swapchain(),
    });
    gfx_draw(&ui.base, gfx_swapchain_pipes(), fw, fh);
    gfx_draw(&ui.over, gfx_swapchain_pipes(), fw, fh);
    sg_end_pass();
    sg_commit();

    // per-frame input reset
    Input *in = &ui.in;
    in->prev = in->mouse;
    memset(in->pressed, 0, sizeof(in->pressed));
    memset(in->released, 0, sizeof(in->released));
    memset(in->key_pressed, 0, sizeof(in->key_pressed));
    in->wheel_x = in->wheel_y = 0;
    in->nchars = 0;
    in->dbl = false;
    ui.nmsamples = 0;

    M.frames++;
    if (M.capture && M.frames == M.capture_frame) {
        bool ok = plat_capture_png(M.capture);
        vx_log(ok ? "captured %s" : "capture failed: %s", M.capture);
        sapp_request_quit();
    }
}

static void event(const sapp_event *e) {
    if (M.selftest && e->type != SAPP_EVENTTYPE_RESIZED) return;   // scripted input only
    Input *in = &ui.in;
    float s = M.scale;
    in->mods = ((e->modifiers & SAPP_MODIFIER_SHIFT) ? MOD_SHIFT : 0) | ((e->modifiers & SAPP_MODIFIER_CTRL) ? MOD_CTRL : 0) | ((e->modifiers & SAPP_MODIFIER_ALT) ? MOD_ALT : 0);
    switch (e->type) {
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        in->mouse = v2(e->mouse_x / s, e->mouse_y / s);
        in->mouse_inside = true;
        if (ui.nmsamples < COUNT(ui.msamples)) ui.msamples[ui.nmsamples++] = in->mouse;
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN: {
        int b = e->mouse_button == SAPP_MOUSEBUTTON_LEFT ? MB_LEFT : e->mouse_button == SAPP_MOUSEBUTTON_RIGHT ? MB_RIGHT : MB_MIDDLE;
        in->mouse = v2(e->mouse_x / s, e->mouse_y / s);
        in->down[b] = true; in->pressed[b] = true;
        if (b == MB_LEFT) {
            double t = stm_sec(stm_now());
            if (t - M.last_press < 0.35 && fabsf(in->mouse.x - M.last_press_pos.x) < 4 && fabsf(in->mouse.y - M.last_press_pos.y) < 4) in->dbl = true;
            M.last_press = t; M.last_press_pos = in->mouse;
        }
        break;
    }
    case SAPP_EVENTTYPE_MOUSE_UP: {
        int b = e->mouse_button == SAPP_MOUSEBUTTON_LEFT ? MB_LEFT : e->mouse_button == SAPP_MOUSEBUTTON_RIGHT ? MB_RIGHT : MB_MIDDLE;
        in->down[b] = false; in->released[b] = true;
        break;
    }
    case SAPP_EVENTTYPE_MOUSE_SCROLL: in->wheel_x += e->scroll_x; in->wheel_y += e->scroll_y; break;
    case SAPP_EVENTTYPE_MOUSE_ENTER: in->mouse_inside = true; break;
    case SAPP_EVENTTYPE_MOUSE_LEAVE: in->mouse_inside = false; break;
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (e->key_code >= 0 && e->key_code < 512) { in->key_down[e->key_code] = true; in->key_pressed[e->key_code] = true; }
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if (e->key_code >= 0 && e->key_code < 512) in->key_down[e->key_code] = false;
        break;
    case SAPP_EVENTTYPE_CHAR:
        if (in->nchars < COUNT(in->chars) && !(in->mods & (MOD_CTRL | MOD_ALT))) in->chars[in->nchars++] = (int)e->char_code;
        break;
    case SAPP_EVENTTYPE_FILES_DROPPED:
        for (int i = 0; i < sapp_get_num_dropped_files(); i++) {
            const char *f = sapp_get_dropped_file_path(i);
            // an image dropped on the stage lands where it was dropped
            if (app_is_image(f) && !app.home && rect_has(stage_rect(), in->mouse)) app_import_image(f, in->mouse);
            else app_open_path(f);
        }
        break;
    case SAPP_EVENTTYPE_QUIT_REQUESTED:
        if (!app_quit_requested()) sapp_cancel_quit();   // unsaved documents: ask first
        break;
    case SAPP_EVENTTYPE_UNFOCUSED:
        memset(in->down, 0, sizeof(in->down));
        memset(in->key_down, 0, sizeof(in->key_down));
        break;
    default: break;
    }
}

static void cleanup(void) {
    sapp_set_mouse_cursor(SAPP_MOUSECURSOR_DEFAULT);
    plat_reset_cursor();
    recovery_cleanup();
    audio_shutdown();
    font_shutdown();
    gfx_shutdown();
    sg_shutdown();
}

sapp_desc sokol_main(int argc, char *argv[]) {
    {   // file names with letters outside the ANSI code page (Cyrillic paths) need the wide command line
        int ac = 0; char **av = plat_argv_utf8(&ac);
        if (av) { argc = ac; argv = av; }
    }
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--capture=", 10)) { M.capture = argv[i] + 10; if (!M.capture_frame) M.capture_frame = 8; }
        else if (!strncmp(argv[i], "--capture-frame=", 16)) M.capture_frame = atoi(argv[i] + 16);
        else if (!strcmp(argv[i], "--home")) M.home = true;
        else if (!strcmp(argv[i], "--splash")) { M.splash = true; M.home = true; }
        else if (!strncmp(argv[i], "--bench=", 8)) { M.bench = (float)atof(argv[i] + 8); M.demo = true; }
        else if (!strcmp(argv[i], "--demo")) M.demo = true;
        else if (!strncmp(argv[i], "--open=", 7)) M.open = argv[i] + 7;
        else if (!strncmp(argv[i], "--frame=", 8)) M.goto_frame = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--layer=", 8)) M.goto_layer = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--cursor-sheet=", 15)) {   // [--cursor-scale=N before it] every cursor, enlarged
            float sc = 1.5f;
            for (int j = 1; j < argc; j++) if (!strncmp(argv[j], "--cursor-scale=", 15)) sc = (float)atof(argv[j] + 15);
            bool ok = cursors_sheet(argv[i] + 15, sc);
            vx_log("cursor sheet %s", ok ? "written" : "FAILED");
            exit(0);
        }
        else if (argv[i][0] != '-') M.open = argv[i];
        else if (!strcmp(argv[i], "--selftest=menu")) { M.selftest = true; M.variant = 1; M.capture_frame = 14; }
        else if (!strcmp(argv[i], "--selftest=edit")) { M.selftest = true; M.variant = 3; M.capture_frame = 150; }
        else if (!strcmp(argv[i], "--selftest=settings")) { M.selftest = true; M.variant = 5; M.page = SET_GENERAL; M.capture_frame = 20; }
        else if (!strcmp(argv[i], "--selftest=settings-perf")) { M.selftest = true; M.variant = 5; M.page = SET_PERFORMANCE; M.capture_frame = 20; }
        else if (!strcmp(argv[i], "--selftest=settings-keys")) { M.selftest = true; M.variant = 5; M.page = SET_KEYS; M.capture_frame = 20; }
        else if (!strcmp(argv[i], "--selftest=settings-layout")) { M.selftest = true; M.variant = 5; M.page = SET_LAYOUT; M.capture_frame = 20; }
        else if (!strcmp(argv[i], "--selftest=settings-theme")) { M.selftest = true; M.variant = 5; M.page = SET_THEME; M.capture_frame = 20; }
        else if (!strcmp(argv[i], "--selftest=settings-save")) { M.selftest = true; M.variant = 5; M.page = SET_THEME; M.save_test = true; M.capture_frame = 22; }
        else if (!strcmp(argv[i], "--selftest=recovery")) { M.selftest = true; M.variant = 8; M.capture_frame = 30; }
        else if (!strcmp(argv[i], "--selftest=tween")) { M.selftest = true; M.variant = 9; M.capture_frame = 50; }
        else if (!strcmp(argv[i], "--selftest=timeline")) { M.selftest = true; M.variant = 10; M.capture_frame = 54; }
        else if (!strcmp(argv[i], "--selftest=audio-save")) { M.selftest = true; M.variant = 11; M.capture_frame = 12; }
        else if (!strcmp(argv[i], "--selftest=camera")) { M.selftest = true; M.variant = 12; M.capture_frame = 62; }
        else if (!strcmp(argv[i], "--selftest=camera-view")) { M.selftest = true; M.variant = 12; M.page = 1; M.capture_frame = 68; }
        else if (!strcmp(argv[i], "--selftest=curve")) { M.selftest = true; M.variant = 13; M.capture_frame = 16; }
        else if (!strcmp(argv[i], "--selftest=export-symbols")) { M.selftest = true; M.variant = 14; M.capture_frame = 94; }
        else if (!strcmp(argv[i], "--selftest=library")) { M.selftest = true; M.variant = 14; M.page = 2; M.capture_frame = 82; }
        else if (!strcmp(argv[i], "--selftest=export-window")) { M.selftest = true; M.variant = 14; M.page = 1; M.capture_frame = 80; }
        else if (!strcmp(argv[i], "--selftest=brushes")) { M.selftest = true; M.variant = 19; M.capture_frame = 16; }
        else if (!strcmp(argv[i], "--selftest=brush-edit")) { M.selftest = true; M.variant = 19; M.page = 1; M.capture_frame = 10; }
        else if (!strcmp(argv[i], "--selftest=csp-tip")) { M.selftest = true; M.variant = 19; M.page = 5; M.capture_frame = 38; }
        else if (!strcmp(argv[i], "--selftest=texture")) { M.selftest = true; M.variant = 19; M.page = 4; M.capture_frame = 50; }
        else if (!strcmp(argv[i], "--selftest=recolor")) { M.selftest = true; M.variant = 19; M.page = 2; M.capture_frame = 30; }
        else if (!strcmp(argv[i], "--selftest=shape")) { M.selftest = true; M.variant = 18; M.capture_frame = 58; }
        else if (!strcmp(argv[i], "--selftest=shape-edit")) { M.selftest = true; M.variant = 18; M.page = 1; M.capture_frame = 70; }
        else if (!strcmp(argv[i], "--selftest=image")) { M.selftest = true; M.variant = 17; M.capture_frame = 12; }
        else if (!strcmp(argv[i], "--selftest=blend")) { M.selftest = true; M.variant = 16; M.capture_frame = 18; }
        else if (!strcmp(argv[i], "--selftest=tabs")) { M.selftest = true; M.variant = 15; M.capture_frame = 21; }
        else if (!strcmp(argv[i], "--selftest=tabs-close")) { M.selftest = true; M.variant = 15; M.page = 1; M.capture_frame = 31; }
        else if (!strcmp(argv[i], "--selftest=fnf")) { M.selftest = true; M.variant = 4; M.capture_frame = 146; }
        else if (!strcmp(argv[i], "--selftest=select")) { M.selftest = true; M.variant = 7; M.capture_frame = 132; }
        else if (!strcmp(argv[i], "--selftest=layers")) { M.selftest = true; M.variant = 2; M.capture_frame = 14; }
        else if (!strcmp(argv[i], "--selftest")) { M.selftest = true; if (!M.capture_frame || M.capture_frame < 130) M.capture_frame = 130; }
    }
    // a plain launch: the splash, then the start screen
    if (argc <= 1) { M.home = true; M.splash = true; }
    return (sapp_desc){
        .init_cb = init, .frame_cb = frame, .event_cb = event, .cleanup_cb = cleanup,
        .width = 1600, .height = 940,
        .window_title = "Vertexa",
        .high_dpi = true,
        .sample_count = 1,
        .depth_format = SAPP_PIXELFORMAT_DEPTH_STENCIL,
        .swap_interval = 1,
        .enable_clipboard = true,
        .enable_dragndrop = true,
        .max_dropped_files = 8,
        .max_dropped_file_path_length = 1024,
        .logger.func = slog_func,
        .win32 = {.console_utf8 = true},
    };
}
