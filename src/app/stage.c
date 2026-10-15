// Stage: view (zoom / pan), offscreen MSAA rendering, tool input, edit-in-place context.
#include "app/app.h"
#include "gfx/cursors.h"
#include "render/scene.h"
#include "geom/geom.h"
#include "platform/platform.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_time.h"
#include "geom/geom.h"

typedef struct {
    int rw, rh; float z, ox, oy;
    u32 version; int frame, sframe, nedit; bool onion; int op, on; u32 active;
    bool drawing, playing, cam_view; u32 live_n, force, sel;
} RenderKey;

static struct {
    Rect r;
    float z, ox, oy;          // view: stage-local logical px = scene * z + (ox, oy)
    bool fitted;
    int samples;
    float res, px;            // stage resolution (fraction of the display) and target px per logical px
    float cpu_ms; int tris;   // last re-render: CPU time and triangles (performance overlay)
    Target tgt; Pool pool;
    PipeSet pipes;
    Frame F;
    RenderKey last; bool valid; u32 force;
    Builder b; bool drawing; char draw_tool;
    bool panning; V2 pan_m; float pan_ox, pan_oy;
    bool space;
    bool cam_view;            // look through the camera: the stage shows exactly what exports
    PenSample pen[4096]; int npen;
} S;

static struct { int mode; V2 m0, c0; float cam0[4]; } CAM;   // dragging the camera frame
enum { CAM_NONE, CAM_MOVE, CAM_ZOOM, CAM_TURN };

static int wanted_samples(void) {
    if (app.gpu_low || !sg_query_pixelformat(SG_PIXELFORMAT_RGBA8).msaa) return 1;
    return CLAMP(app.msaa, 1, 8);
}
void stage_init(void) {
    S.samples = wanted_samples();
    S.pipes = gfx_make_pipes(S.samples, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL);
    S.res = 1;
    S.fitted = true;
}
// Settings -> Performance: anti-aliasing and resolution of the stage image, applied on the fly
static void apply_quality(void) {
    int s = wanted_samples();
    if (s != S.samples) {
        gfx_free_pipes(&S.pipes);
        S.pipes = gfx_make_pipes(s, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL);
        S.samples = s;
        S.tgt.w = -1;   // targets are remade below
    }
    S.res = app.gpu_low ? 0.75f : clampf(app.stage_res / 100.0f, 0.25f, 1);
}
void stage_stats(float *cpu_ms, int *tris, int *samples, float *res) { *cpu_ms = S.cpu_ms; *tris = S.tris; *samples = S.samples; *res = S.res; }

static void ensure_targets(int w, int h) {
    if (w == S.tgt.w && h == S.tgt.h && S.tgt.v_tex.id && S.tgt.samples == S.samples) return;
    target_make(&S.tgt, w, h, S.samples);
    S.valid = false;
}

/* ── view & editing space ── */

void stage_fit(void) {
    Doc *d = app.doc;
    if (S.r.w <= 0) return;
    S.z = MAX(0.02f, MIN((S.r.w - 96) / d->w, (S.r.h - 72) / d->h));
    S.ox = roundf((S.r.w - d->w * S.z) / 2); S.oy = roundf((S.r.h - d->h * S.z) / 2);
    S.fitted = true;
}
static void zoom_at(float z, float sx, float sy) {
    z = clampf(z, 0.05f, 32);
    S.ox = sx - (sx - S.ox) * z / S.z; S.oy = sy - (sy - S.oy) * z / S.z; S.z = z;
    S.fitted = false;
}
void stage_zoom_by(float k) { zoom_at(S.z * k, S.r.w / 2, S.r.h / 2); }
void stage_zoom_to(float z, V2 scene) {   // zoom so the scene point lands in the middle of the view
    S.z = clampf(z, 0.05f, 32);
    S.ox = roundf(S.r.w / 2 - scene.x * S.z); S.oy = roundf(S.r.h / 2 - scene.y * S.z);
    S.fitted = false;
}
void stage_pan(float dx, float dy) { S.ox += dx; S.oy += dy; S.fitted = false; }
void stage_zoom_100(void) { zoom_at(1, S.r.w / 2, S.r.h / 2); }
void stage_view_get(float v[3], bool *fitted) { v[0] = S.z; v[1] = S.ox; v[2] = S.oy; *fitted = S.fitted; }
void stage_view_set(const float v[3], bool fitted) {
    if (v[0] > 0 && !fitted) { S.z = v[0]; S.ox = v[1]; S.oy = v[2]; S.fitted = false; }
    else { S.fitted = true; stage_fit(); }
    S.force++;
}
Rect stage_rect(void) { return S.r; }
V2 stage_to_screen(V2 p) { return v2(S.r.x + S.ox + p.x * S.z, S.r.y + S.oy + p.y * S.z); }
void st_invalidate(void) { S.force++; }

// looking through the camera puts its frame where the stage was
static Mat camera_view_matrix(void) {
    Doc *d = app.doc;
    float cam[4];
    if (!S.cam_view || d->nedit || !doc_camera_at(d, doc_tl(d)->frame, cam)) return mat_id();
    return doc_camera_matrix(d, cam);
}
bool *stage_camera_view(void) { return &S.cam_view; }

static Mat local_to_screen(void) {
    return mat_mul(mat_mul(mat_tr(S.r.x + S.ox, S.r.y + S.oy), mat_scale(S.z, S.z)), mat_mul(camera_view_matrix(), doc_edit_matrix(app.doc)));
}
V2 st_local(V2 screen) { return mat_ap(mat_inv(local_to_screen()), screen); }
V2 st_screen(V2 local) { return mat_ap(local_to_screen(), local); }
float st_unit(void) { return 1.0f / MAX(1e-6f, mat_scalef(local_to_screen())); }
// brush sizes are document pixels, so a stroke keeps its width in the drawing whatever the zoom
// (Settings > General can make them screen pixels instead)
float st_size_unit(void) { return app.size_fixed ? st_unit() : 1.0f / MAX(1e-6f, mat_scalef(doc_edit_matrix(app.doc))); }

Drawing *st_drawing(bool create) {
    Doc *d = app.doc;
    Layer *l = doc_active(d);
    if (!l || !doc_drawable(l)) return NULL;
    int f = doc_tl(d)->frame;
    if (create) return doc_drawing_for_edit(d, l, f);
    return doc_drawing_at(d, l, f);
}

const char *st_cannot_edit(void) {
    Doc *d = app.doc;
    Layer *l = doc_active(d);
    if (!l) return "Select a layer first";
    if (l->type == LT_FOLDER) return "A folder holds layers. Pick a layer inside it";
    if (!doc_drawable(l)) return "You can't draw on this kind of layer";
    if (doc_hidden(d, l)) return "This layer is hidden. Show it to draw";
    if (doc_locked(d, l)) return "This layer is locked";
    return NULL;
}

/* ── painting ── */

static BrushOpts opts_for(char tool) {
    float unit = st_unit(), su = st_size_unit();
    BrushOpts o = {0};
    o.unit = unit;
    o.curve = app.curve;
    o.aspect = 1;
    if (tool == 'E') {
        o.size = app.eraser_size * su; o.smoothing = o.fit = 25; o.tip = TIP_ROUND; o.erase = 1; o.color = 0xffffffff;
    } else if (tool == 'Y') {
        static const float sm[] = {75, 55, 10};
        o.size = app.pencil_size * su; o.smoothing = o.fit = sm[app.pencil_mode]; o.tip = TIP_ROUND; o.color = app.stroke;
    } else {
        o.size = app.brush_size * su; o.smoothing = app.brush_smooth;
        o.pressure = app.brush_pressure; o.tilt = app.brush_tilt;
        o.tip = app.tip; o.angle = app.tip_angle * VX_PI / 180; o.aspect = app.tip == TIP_OVAL ? app.tip_aspect : 1;
        o.taper_s = app.taper_s * 0.09f; o.taper_e = app.taper_e * 0.09f;
        o.fit = app.brush_fit;
        o.tex = app.texture[0] ? texture_bitmap(app.texture) : 0;
        o.tex_density = app.tex_density / 100; o.tex_scale = app.tex_scale / 100;
        o.img = app.img_mode && app.tip_image[0] ? tipimg_bitmap(app.tip_image) : 0;
        o.img_mode = o.img ? app.img_mode : IMG_NONE; o.img_flags = app.img_flags; o.img_spacing = app.img_spacing / 100;
        o.img_w0 = app.img_w0 / 100; o.img_w1 = app.img_w1 / 100;
        o.color = col_alpha(app.fill, app.alpha * app.brush_opacity / 100);
    }
    return o;
}

static void feed_samples(void) {
    if (!S.drawing) return;
    if (S.npen) {
        for (int i = 0; i < S.npen; i++) { V2 p = st_local(v2(S.pen[i].x, S.pen[i].y)); bb_add(&S.b, p.x, p.y, S.pen[i].pressure, S.pen[i].tilt); }
    } else if (!plat_pen_in_range()) {
        for (int i = 0; i < ui.nmsamples; i++) { V2 p = st_local(ui.msamples[i]); bb_add(&S.b, p.x, p.y, 1, 0); }
    }
}

static void finish_stroke(void) {
    if (!S.drawing) return;
    S.drawing = false;
    Stroke st = bb_finish(&S.b);
    if (!arrlen(st.pts)) { item_free(&st); return; }
    app_commit();
    Drawing *dr = st_drawing(true);
    if (!dr) { item_free(&st); return; }
    if (S.draw_tool == 'B' && app.brush_mode == 2) arrins(dr->items, 0, st);
    else arrput(dr->items, st);
    doc_touch(app.doc);
}

// self-tests: a brush stroke through drawing-space points with a pen pressure each, as if drawn
void st_test_stroke(const V2 *pts, const float *pressure, int n) {
    Builder b;
    bb_begin(&b, opts_for('B'));
    for (int i = 0; i < n; i++) bb_add(&b, pts[i].x, pts[i].y, pressure[i], 0);
    Stroke st = bb_finish(&b);
    Drawing *dr = st_drawing(true);
    if (!dr || !arrlen(st.pts)) { item_free(&st); return; }
    arrput(dr->items, st);
    doc_touch(app.doc);
}

static void pick_color(V2 screen) {
    Doc *d = app.doc;
    Timeline *t = doc_tl(d);
    V2 p = st_local(screen);
    float tol = 2 * st_unit();
    for (ptrdiff_t li = 0; li < arrlen(t->layers); li++) {
        Layer *l = &t->layers[li];
        if (l->type == LT_FOLDER || doc_hidden(d, l)) continue;
        Drawing *dr = doc_drawing_at(d, l, t->frame);
        if (!dr) continue;
        for (ptrdiff_t s = arrlen(dr->items) - 1; s >= 0; s--) {
            Item *it = &dr->items[s];
            if (item_placed(it) || !item_hit(it, p, tol)) continue;
            if (it->erase) break;
            app_set_color(it->color | 0xff000000u);
            return;
        }
    }
    V2 sp = mat_ap(doc_edit_matrix(d), p);
    if (sp.x >= 0 && sp.y >= 0 && sp.x <= d->w && sp.y <= d->h) app_set_color(hex(0xffffff));
}

/* ── camera ── */

// the camera frame on screen (false: no camera, editing a symbol, or looking through it)
static bool camera_quad(V2 q[4], float cam[4]) {
    Doc *d = app.doc;
    if (S.cam_view || d->nedit || !doc_camera_at(d, doc_tl(d)->frame, cam)) return false;
    Mat inv = mat_inv(doc_camera_matrix(d, cam));
    V2 c[4] = {{0, 0}, {(float)d->w, 0}, {(float)d->w, (float)d->h}, {0, (float)d->h}};
    for (int i = 0; i < 4; i++) q[i] = stage_to_screen(mat_ap(inv, c[i]));
    return true;
}

// the camera key of this frame; editing between keys adds one there first (with the camera as it is)
static Key *camera_edit_key(void) {
    Doc *d = app.doc;
    Layer *l = doc_camera_layer(d);
    if (!l) return NULL;
    int f = doc_tl(d)->frame;
    float cur[4];
    doc_camera_at(d, f, cur);
    int ki = doc_key_index_for(l, f);
    if (ki < 0 || l->keys[ki].f != f || f > l->len) {
        Layer *one[1] = {l};
        doc_insert_key(d, one, 1, f, false);
        l = doc_camera_layer(d);
        ki = doc_key_index_for(l, f);
        if (ki < 0) return NULL;
        memcpy(l->keys[ki].cam, cur, sizeof(cur));
    }
    Key *k = &l->keys[ki];
    if (k->cam[2] <= 0) memcpy(k->cam, cur, sizeof(cur));
    return k;
}

static void camera_tool(UiResp *re) {
    V2 q[4]; float cam[4];
    if (!camera_quad(q, cam)) { if (re->pressed && S.cam_view) app_toast("Stop looking through the camera to move it"); return; }
    V2 m = ui.in.mouse, c = {(q[0].x + q[2].x) / 2, (q[0].y + q[2].y) / 2};
    int corner = -1; bool turn = false;
    for (int i = 0; i < 4; i++) {
        float dd = hypotf(m.x - q[i].x, m.y - q[i].y);
        if (dd <= 7) corner = i;
        else if (dd < 24 && corner < 0) { corner = i; turn = true; }
    }
    static const int st0[1] = {0};
    bool inside = point_in_poly(q, st0, 4, 1, m);
    if (corner >= 0 && inside && turn) corner = -1;   // just inside a corner: that's moving
    if (re->pressed && (corner >= 0 || inside)) {
        app_commit();
        Key *k = camera_edit_key();
        if (!k) return;
        CAM.mode = corner < 0 ? CAM_MOVE : turn ? CAM_TURN : CAM_ZOOM;
        CAM.m0 = m; CAM.c0 = c;
        memcpy(CAM.cam0, k->cam, sizeof(CAM.cam0));
    }
    if (re->held && CAM.mode) {
        float nc[4];
        memcpy(nc, CAM.cam0, sizeof(nc));
        bool shift = ui.in.mods & MOD_SHIFT;
        if (CAM.mode == CAM_MOVE) {
            V2 dm = v2((m.x - CAM.m0.x) / S.z, (m.y - CAM.m0.y) / S.z);
            if (shift) { if (fabsf(dm.x) > fabsf(dm.y)) dm.y = 0; else dm.x = 0; }
            nc[0] += dm.x; nc[1] += dm.y;
        } else if (CAM.mode == CAM_ZOOM) {   // a bigger frame sees more: zoom out
            float d0 = hypotf(CAM.m0.x - CAM.c0.x, CAM.m0.y - CAM.c0.y), d1 = hypotf(m.x - CAM.c0.x, m.y - CAM.c0.y);
            nc[2] = clampf(CAM.cam0[2] * d0 / MAX(4.0f, d1), 0.05f, 40);
            if (shift) nc[2] = roundf(nc[2] * 20) / 20;
        } else {
            float a0 = atan2f(CAM.m0.y - CAM.c0.y, CAM.m0.x - CAM.c0.x), a1 = atan2f(m.y - CAM.c0.y, m.x - CAM.c0.x);
            nc[3] = CAM.cam0[3] + (a1 - a0);
            if (shift) nc[3] = roundf(nc[3] / (VX_PI / 12)) * (VX_PI / 12);
        }
        Key *k = camera_edit_key();
        if (k) { memcpy(k->cam, nc, sizeof(nc)); doc_touch(app.doc); }
    }
    if (!re->held) CAM.mode = CAM_NONE;
    if (re->hover || CAM.mode) {
        int mode = CAM.mode ? CAM.mode : corner < 0 ? (inside ? CAM_MOVE : CAM_NONE) : turn ? CAM_TURN : CAM_ZOOM;
        ui_cursor(mode == CAM_MOVE ? SAPP_MOUSECURSOR_RESIZE_ALL : mode == CAM_ZOOM ? SAPP_MOUSECURSOR_RESIZE_NWSE : mode == CAM_TURN ? CUR_ROTATE : SAPP_MOUSECURSOR_DEFAULT);
    }
}

// the frame: always outlined; while the camera layer is active, what it leaves out is dimmed and the
// corners can be grabbed
static void camera_overlay(Rect r, bool editing) {
    V2 q[4]; float cam[4];
    if (!camera_quad(q, cam)) return;
    if (editing) {
        // the ring between a big outer square and the frame, corner to nearest corner
        V2 c = {(q[0].x + q[2].x) / 2, (q[0].y + q[2].y) / 2};
        float big = 8000;
        V2 o[4] = {{c.x - big, c.y - big}, {c.x + big, c.y - big}, {c.x + big, c.y + big}, {c.x - big, c.y + big}};
        int s0 = 0; float best = 1e30f;
        for (int i = 0; i < 4; i++) { float dd = hypotf(q[i].x - o[0].x, q[i].y - o[0].y); if (dd < best) { best = dd; s0 = i; } }
        Color dim = hexa(0x000000, 0x70);
        for (int i = 0; i < 4; i++) {
            V2 a = o[i], b = o[(i + 1) % 4], qa = q[(s0 + i) % 4], qb = q[(s0 + i + 1) % 4];
            dl_tri(ui.dl, a, b, qb, dim);
            dl_tri(ui.dl, a, qb, qa, dim);
        }
    }
    Color line = editing ? hex(0xffffff) : hexa(0xffffff, 0xb0);
    for (int i = 0; i < 4; i++) dl_line(ui.dl, q[i], q[(i + 1) % 4], 3, hexa(0x000000, 0x90));
    for (int i = 0; i < 4; i++) dl_line(ui.dl, q[i], q[(i + 1) % 4], 1.4f, line);
    if (editing) for (int i = 0; i < 4; i++) {
        dl_rect(ui.dl, rect(q[i].x - 4, q[i].y - 4, 8, 8), hex(0xffffff));
        dl_rrect_border(ui.dl, rect(q[i].x - 4, q[i].y - 4, 8, 8), 0, 1, hexa(0x000000, 0xa0));
    }
    char lab[48];
    snprintf(lab, sizeof(lab), "Camera  %d%%%s", (int)roundf(cam[2] * 100), "");
    if (fabsf(cam[3]) > 0.001f) snprintf(lab + strlen(lab), sizeof(lab) - strlen(lab), "  %d\xC2\xB0", (int)roundf(cam[3] * 180 / VX_PI));
    float w = font_width(F_SMALL_SEMI, lab) + 12;
    Rect b = {q[0].x, q[0].y - 22, w, 18};
    dl_rrect(ui.dl, b, 4, editing ? hex(0xffffff) : hexa(0x000000, 0xa0));
    font_draw_in(ui.dl, F_SMALL_SEMI, b, ALIGN_CENTER, lab, editing ? hex(0x141414) : hexa(0xffffff, 0xd0));
    (void)r;
}

/* ── frame ── */

static void breadcrumbs(Rect r) {
    Doc *d = app.doc;
    if (!d->nedit) return;
    float x = r.x + 14, y = r.y + 12;
    for (int i = -1; i < d->nedit; i++) {
        const char *name = i < 0 ? "Scene" : doc_symbol(d, d->edit[i]) ? doc_symbol(d, d->edit[i])->name : "?";
        bool last = i == d->nedit - 1;
        float w = font_width(F_UI_MED, name) + 16;
        Rect c = {x, y, w, 24};
        UiResp re = ui_interact(ui_idi("crumb", i), c);
        dl_rrect(ui.dl, c, 6, last ? C_S3 : re.hover ? C_S2 : col_alpha(C_S1, 0.9f));
        font_draw_in(ui.dl, F_UI_MED, c, ALIGN_CENTER, name, last ? C_TX : re.hover ? C_TX : C_TX2);
        if (re.clicked && !last) { while (d->nedit > i + 1) doc_exit_symbol(d); select_clear(); }
        x += w + 4;
        if (!last) { icon_draw(ui.dl, IC_CARET_RIGHT, v2(x + 6, y + 12), ICON_S, false, C_TX3); x += 16; }
    }
}

void stage_ui(Rect r) {
    bool resized = r.w != S.r.w || r.h != S.r.h;
    S.r = r;
    apply_quality();
    S.px = ui.scale * S.res;
    ensure_targets((int)roundf(r.w * S.px), (int)roundf(r.h * S.px));   // before anything references the texture
    if (resized && S.fitted) stage_fit();
    S.npen = plat_pen_take(S.pen, COUNT(S.pen));

    // view pill first, so it wins over the stage
    char zs[16]; snprintf(zs, sizeof(zs), "%d%%", (int)roundf(S.z * 100));
    float zw = font_width(F_MONO_MED, zs) + 34, rw = font_width(F_MONO_MED, "0\xC2\xB0") + 16;
    Rect pill = {r.x + r.w - 14 - zw - rw - 4, r.y + r.h - 14 - 26, zw + rw + 4, 26};
    Rect zr = {pill.x + 2, pill.y + 2, zw, 22}, rr = {zr.x + zw, pill.y + 2, rw, 22};
    UiResp zre = ui_interact(ui_id("zoom-pill"), zr);
    if (zre.clicked) stage_fit();
    bool has_cam = doc_camera_layer(app.doc) && !app.doc->nedit;
    if (!has_cam) S.cam_view = false;
    Rect cb = {pill.x - 34, pill.y, 28, 26};
    UiResp cre = has_cam ? ui_interact(ui_id("cam-view"), cb) : (UiResp){0};
    if (cre.clicked) S.cam_view = !S.cam_view;

    UiResp re = zre.hover ? (UiResp){0} : ui_interact(ui_id("stage"), r);
    char t = app.tool;
    if (ui.in.key_pressed[SAPP_KEYCODE_SPACE] && !ui.focus) S.space = true;
    if (!ui.in.key_down[SAPP_KEYCODE_SPACE]) S.space = false;
    bool pan_mode = S.space || t == 'H';
    V2 local = v2(ui.in.mouse.x - r.x, ui.in.mouse.y - r.y);

    if (re.hover && (ui.in.wheel_y != 0 || ui.in.wheel_x != 0)) {
        if (ui.in.mods & MOD_CTRL) zoom_at(S.z * powf(1.1f, ui.in.wheel_y), local.x, local.y);
        else if (ui.in.mods & MOD_SHIFT) { S.ox += ui.in.wheel_y * 40; S.fitted = false; }
        else { S.ox += ui.in.wheel_x * 40; S.oy += ui.in.wheel_y * 40; S.fitted = false; }
    }
    if (re.hover && ui.in.pressed[MB_MIDDLE]) { S.panning = true; S.pan_m = ui.in.mouse; S.pan_ox = S.ox; S.pan_oy = S.oy; }
    if (re.pressed && pan_mode) { S.panning = true; S.pan_m = ui.in.mouse; S.pan_ox = S.ox; S.pan_oy = S.oy; }
    Layer *al = doc_active(app.doc);
    bool cam_edit = al && al->type == LT_CAMERA && !app.doc->nedit;   // the camera layer is active: the stage moves the camera
    if (!pan_mode && !S.panning && cam_edit && t != 'Z') camera_tool(&re);
    else if (!pan_mode && !S.panning) {
        switch (t) {
        case 'B': case 'Y': case 'E':
            if (re.pressed) {
                const char *why = st_cannot_edit();
                if (why) app_toast("%s", why);
                else {
                    if (app.playing) app_play(false);
                    char eff = (t == 'B' && plat_pen_eraser() && !app.pen_eraser_tool) ? 'E' : t;   // the pen's eraser end erases
                    S.draw_tool = eff;
                    bb_begin(&S.b, opts_for(eff));
                    S.drawing = true;
                    V2 p = st_local(ui.in.mouse);
                    if (!S.npen) bb_add(&S.b, p.x, p.y, plat_pen_in_range() ? 0.2f : 1, 0);
                }
            }
            break;
        case 'C': trim_tool(&re); break;
        case 'V': case 'A': case 'Q': case 'L': select_tool(&re, t); break;
        case 'N': case 'R': case 'O': shape_tool(&re, t); break;
        case 'K':
            if (re.pressed) { const char *why = st_cannot_edit(); if (why) app_toast("%s", why); else bucket_click(st_local(ui.in.mouse)); }
            break;
        case 'I': if (re.pressed) pick_color(ui.in.mouse); break;
        case 'Z': if (re.pressed) zoom_at(S.z * ((ui.in.mods & MOD_ALT) ? 0.5f : 2.0f), local.x, local.y); break;
        default:
            if (re.pressed) app_toast("%s isn't available yet", tool_name(t));
            break;
        }
    }
    if (S.panning) {
        S.ox = S.pan_ox + ui.in.mouse.x - S.pan_m.x; S.oy = S.pan_oy + ui.in.mouse.y - S.pan_m.y; S.fitted = false;
        if (!ui.in.down[MB_LEFT] && !ui.in.down[MB_MIDDLE]) S.panning = false;
    }
    feed_samples();
    if (S.drawing && !ui.in.down[MB_LEFT]) finish_stroke();

    // stage image and overlays
    if (S.tgt.v_tex.id) dl_image(ui.dl, r, S.tgt.v_tex.id, 0xffffffff, 0, 0, 1, 1);
    else dl_rect(ui.dl, r, C_PASTE);
    if (S.cam_view) {   // looking through the camera: only what it sees
        V2 a = stage_to_screen(v2(0, 0)), b = stage_to_screen(v2((float)app.doc->w, (float)app.doc->h));
        Color c = col_a(C_PASTE, 0xf0);
        dl_push_clip(ui.dl, r);
        dl_rect(ui.dl, rect(r.x, r.y, r.w, a.y - r.y), c);
        dl_rect(ui.dl, rect(r.x, b.y, r.w, r.y + r.h - b.y), c);
        dl_rect(ui.dl, rect(r.x, a.y, a.x - r.x, b.y - a.y), c);
        dl_rect(ui.dl, rect(b.x, a.y, r.x + r.w - b.x, b.y - a.y), c);
        dl_pop_clip(ui.dl);
    }
    dl_push_clip(ui.dl, r);
    camera_overlay(r, cam_edit);
    if (cam_edit) {}
    else if (t == 'V' || t == 'A' || t == 'Q' || t == 'L') select_overlay();
    if (t == 'C' && !cam_edit) trim_overlay();
    breadcrumbs(r);
    dl_pop_clip(ui.dl);

    // cursor
    if (re.hover || S.drawing) {
        if (pan_mode || S.panning) ui_cursor(S.panning ? CUR_GRAB : CUR_HAND);
        else if (t == 'Z') ui_cursor(ui.in.mods & MOD_ALT ? CUR_ZOOM_OUT : CUR_ZOOM_IN);
        else if (t == 'I') ui_cursor(CUR_EYEDROPPER);
        else if (t == 'K') ui_cursor(CUR_BUCKET);
        else if (t == 'N' || t == 'R' || t == 'O') ui_cursor(SAPP_MOUSECURSOR_CROSSHAIR);
        else if (t == 'A' && ui.cursor == SAPP_MOUSECURSOR_DEFAULT) ui_cursor(CUR_SUBSELECT);
        else if (app_tool_paints(t)) {
            ui.hide_cursor = true;
            float sz = t == 'E' ? app.eraser_size : t == 'Y' ? app.pencil_size : app.brush_size;
            V2 m = ui.in.mouse;
            ui_overlay(true);
            dl_push_clip(ui.dl, r);
            float rad = MAX(2.0f, sz / 2 * st_size_unit() / st_unit());   // the size as it will land on the stage
            dl_circle_border(ui.dl, m, rad + 1, 1, hexa(0xffffff, 0x99));
            dl_circle_border(ui.dl, m, rad, 1, hexa(0x000000, 0xcc));
            if (rad > 5) dl_rect(ui.dl, rect(m.x - 0.5f, m.y - 0.5f, 1, 1), hex(0x000000));
            dl_pop_clip(ui.dl);
            ui_overlay(false);
        }
    }

    if (has_cam) {
        dl_rrect(ui.dl, cb, 7, S.cam_view ? C_AC : cre.hover ? C_S3 : C_S1);
        dl_rrect_border(ui.dl, cb, 7, 1, S.cam_view ? C_AC : C_LINE);
        icon_draw(ui.dl, IC_CAMERA, v2(cb.x + 14, cb.y + 13), ICON_M, false, S.cam_view ? hex(0xffffff) : cre.hover ? C_TX : C_TX2);
        ui_tooltip(ui_id("cam-view"), cb, S.cam_view ? "Back to the whole stage" : "Look through the camera");
    }
    dl_rrect(ui.dl, pill, 7, C_S1);
    dl_rrect_border(ui.dl, pill, 7, 1, C_LINE);
    if (zre.hover) dl_rrect(ui.dl, zr, 5, C_S3);
    icon_draw(ui.dl, IC_ZOOM, v2(zr.x + 13, zr.y + 11), ICON_S, false, zre.hover ? C_TX : C_TX3);
    font_draw_in(ui.dl, F_MONO_MED, rect(zr.x + 24, zr.y, zr.w - 30, zr.h), ALIGN_LEFT, zs, zre.hover ? C_TX : C_TX2);
    font_draw_in(ui.dl, F_MONO_MED, rr, ALIGN_CENTER, "0\xC2\xB0", C_TX3);
    ui_tooltip(ui_id("zoom-pill"), zr, "Fit to window");

    // Settings > Performance > overlay
    if (app.stats) {
        static float ms = 16;
        if (ui.dt > 0) ms = ms * 0.92f + 0.08f * clampf((float)ui.dt * 1000, 0.1f, 1000);
        float fps = 1000 / ms;
        char aa[8], s[160];
        if (S.samples > 1) snprintf(aa, sizeof(aa), "%d\xC3\x97", S.samples); else snprintf(aa, sizeof(aa), "off");
        snprintf(s, sizeof(s), "%.0f fps, %.1f ms. Stage %.1f ms, %.1fk triangles. AA %s, %d%%",
                 fps, ms, S.cpu_ms, S.tris / 1000.0f, aa, (int)roundf(S.res * 100));
        float w = font_width(F_MONO, s) + 20;
        Rect b = {r.x + r.w - w - 14, r.y + 14, w, 24};
        dl_rrect(ui.dl, b, 6, col_a(C_S1, 0xe6));
        dl_rrect_border(ui.dl, b, 6, 1, C_LINE);
        font_draw_in(ui.dl, F_MONO, rect_inset(b, 10, 0), ALIGN_LEFT, s, C_TX2);
    }
}

/* ── rendering ── */

static void paper(DrawList *dl, Mat view, Doc *d) {
    V2 a = mat_ap(view, v2(0, 0)), b = mat_ap(view, v2((float)d->w, (float)d->h));
    dl_set_pipe(dl, PIPE_NORMAL);
    dl_shadow(dl, rect(a.x, a.y + 18, b.x - a.x, b.y - a.y), 2, 40, hexa(0x000000, 0xb0));
    dl_rect(dl, rect(a.x, a.y, b.x - a.x, b.y - a.y), d->bg | 0xff000000u);
}

int stage_lists(DrawList **out, int cap) {
    Doc *d = app.doc;
    float sc = S.px;
    if (!S.tgt.v_tex.id) return 0;
    Timeline *t = doc_tl(d);
    Item *prev = NULL; int nprev = 0;
    bool preview = shape_preview(&prev, &nprev);
    RenderKey key;
    memset(&key, 0, sizeof(key));
    key.rw = S.tgt.w; key.rh = S.tgt.h; key.z = S.z; key.ox = S.ox; key.oy = S.oy;
    key.version = d->version; key.frame = t->frame; key.sframe = d->scene.frame; key.nedit = d->nedit;
    key.onion = d->onion_on; key.op = d->onion_prev; key.on = d->onion_next;
    key.active = (u32)t->active; key.drawing = S.drawing; key.playing = app.playing; key.cam_view = S.cam_view;
    key.live_n = S.drawing ? (u32)arrlen(S.b.pts) : 0; key.force = S.force; key.sel = select_signature();
    if (S.valid && !S.drawing && !preview && memcmp(&key, &S.last, sizeof(key)) == 0) { S.F.nsegs = 0; return 0; }
    S.last = key; S.valid = true;
    u64 t0 = stm_now();

    Mat view = mat_mul(mat_tr(S.ox * sc, S.oy * sc), mat_scale(S.z * sc, S.z * sc));
    Mat em = mat_mul(view, mat_mul(camera_view_matrix(), doc_edit_matrix(d)));
    frame_begin(&S.F, &S.tgt, &S.pool, font_view());
    paper(frame_dl(&S.F), view, d);

    // editing a symbol: the scene stays visible, dimmed, for context
    if (d->nedit) render_timeline(&S.F, d, &d->scene, d->scene.frame, view, 0.28f, 0, 0, false);

    // onion skin: neighbouring keyframes of every visible layer, under the current frame
    if (d->onion_on && !app.playing) {
        for (ptrdiff_t li = arrlen(t->layers) - 1; li >= 0; li--) {
            Layer *l = &t->layers[li];
            if (l->type == LT_FOLDER || doc_hidden(d, l)) continue;
            Key *ck = doc_key_for(l, t->frame);
            for (ptrdiff_t i = 0; i < arrlen(l->keys); i++) {
                Key *k = &l->keys[i];
                if ((ck && k->f == ck->f) || k->d < 0) continue;
                bool before = k->f < t->frame;
                int dist = before ? t->frame - k->f : k->f - t->frame;
                if (dist > (before ? d->onion_prev : d->onion_next)) continue;
                float a = before ? 0.42f - 0.18f * (dist - 1) / MAX(1, d->onion_prev) : 0.42f;
                render_items(&S.F, d, &d->drawings[k->d], 0, em, 1, col_alpha(before ? C_PREV : C_NEXT, a), 0);
            }
        }
    }

    render_timeline(&S.F, d, t, t->frame, em, 1, 0, 0, false);

    DrawList *dl = frame_dl(&S.F);
    if (!app.playing) select_render(dl, em);
    if (S.drawing) {
        Stroke live;
        bb_live(&S.b, &live);
        if (live.erase) brush_draw(dl, &live, em, col_a(C_AC, 0x60), false);
        else render_stroke(&S.F, &live, em, live.color);
        dl = frame_dl(&S.F);   // an image stroke ends in a new segment
    }
    for (int i = 0; i < nprev; i++) {
        if (prev[i].kind == IT_FILL) fill_draw(dl, &prev[i], em, prev[i].color, false);
        else brush_draw(dl, &prev[i], em, prev[i].color, false);
    }
    S.tris = 0;
    for (int i = 0; i < S.F.nsegs; i++) S.tris += (int)arrlen(S.F.segs[i]->dl.idx) / 3;
    S.cpu_ms = (float)stm_ms(stm_since(t0));
    return frame_lists(&S.F, out, cap);
}

void stage_render(void) {
    if (!S.F.nsegs) return;
    static const float clear[4] = {0.039f, 0.039f, 0.047f, 1};
    frame_passes(&S.F, &S.pipes, clear);
}

/* ── demo content (used for screenshots and as a quick self-test) ── */

static void demo_stroke(Doc *d, Layer *l, int f, const float (*pts)[3], int n, float size, Color c, u8 taper, u8 tip, float angle) {
    BrushOpts o = {.size = size, .smoothing = 45, .pressure = true, .curve = {3, {0, 0.46f, 1}, {0.12f, 0.48f, 1}}, .tip = tip, .angle = angle, .aspect = 0.38f,
                   .taper_s = taper & TAPER_START ? 4.5f : 0, .taper_e = taper & TAPER_END ? 4.5f : 0, .fit = 45, .color = c, .unit = 1};
    Builder b;
    bb_begin(&b, o);
    for (int i = 0; i < n - 1; i++) {
        const float *p0 = pts[MAX(0, i - 1)], *p1 = pts[i], *p2 = pts[i + 1], *p3 = pts[MIN(n - 1, i + 2)];
        for (int k = 0; k < 24; k++) {
            float t = k / 24.0f, t2 = t * t, t3 = t2 * t;
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = 0.5f * (2 * p1[j] + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2 + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3);
            bb_add(&b, v[0], v[1], v[2], 0);
        }
    }
    bb_add(&b, pts[n - 1][0], pts[n - 1][1], pts[n - 1][2], 0);
    Stroke st = bb_finish(&b);
    Drawing *dr = doc_drawing_for_edit(d, l, f);
    arrput(dr->items, st);
}

void stage_demo(void) {
    Doc *d = app.doc;
    Layer *base = doc_active(d);
    snprintf(base->name, sizeof(base->name), "Ink");
    Layer *one[1] = {base};
    doc_insert_frames(d, one, 1, 24, 1);
    for (int f = 1; f <= 23; f += 2) {
        if (f > 1) doc_insert_key(d, one, 1, f, true);
        float ph = (f - 1) / 24.0f * 2 * VX_PI, cx = 640 + sinf(ph) * 40, cy = 360 + cosf(ph * 2) * 18;
        float s1[][3] = {{cx - 240, cy + 60, 0.1f}, {cx - 120, cy - 70, 0.7f}, {cx, cy - 100, 1}, {cx + 130, cy - 60, 0.8f}, {cx + 230, cy + 50, 0.1f}};
        demo_stroke(d, base, f, s1, 5, 22, hex(0x141414), TAPER_BOTH, TIP_ROUND, 0);
        float s2[][3] = {{cx - 180, cy + 130, 0.2f}, {cx - 40, cy + 70, 1}, {cx + 120, cy + 140, 0.4f}};
        demo_stroke(d, base, f, s2, 3, 40, hex(0xff6f5e), TAPER_END, TIP_OVAL, 0.6f);
        float s3[][3] = {{cx + 260, cy - 120, 0.3f}, {cx + 300, cy - 20, 1}, {cx + 270, cy + 90, 0.3f}};
        demo_stroke(d, base, f, s3, 3, 16, hex(0x2f6fdb), TAPER_BOTH, TIP_SQUARE, 0.4f);
    }
    doc_tl(d)->frame = 11;
    d->onion_on = true; d->onion_prev = 4; d->onion_next = 2;
    d->loop_on = true; d->loop_a = 5; d->loop_b = 20;
    doc_touch(d);
}
