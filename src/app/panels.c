// Tool options bar, color panel and the right panel (Brushes / Library / Properties).
#include "app/app.h"
#include "sokol/sokol_app.h"
#include "render/scene.h"

/* ── tool options (context bar) ── */

static float cx_;          // layout cursor
static Rect bar_;

static void c_sep(void) { dl_rect(ui.dl, rect(cx_, bar_.y + 12, 1, 20), C_LINE2); cx_ += 1 + 18; }
// a slider for sizes: the track squared, so small sizes get most of it
static float slider_sq(UiId id, Rect r, float v, float lo, float hi) {
    float t = sqrtf(clampf((v - lo) / (hi - lo), 0, 1));
    float nt = ui_slider(id, r, t * 1000, 0, 1000) / 1000;
    return nt == t ? v : lo + nt * nt * (hi - lo);
}

static void c_slider(const char *label, float *v, float lo, float hi, const char *fmt) {
    float lw = font_width(F_UI, label);
    font_draw_in(ui.dl, F_UI, rect(cx_, bar_.y, lw, bar_.h), ALIGN_LEFT, label, C_TX2);
    cx_ += lw + 10;
    Rect sr = rect(cx_, bar_.y + 12, 96, 20);
    *v = roundf(hi > 100 ? slider_sq(ui_id(label), sr, *v, lo, hi) : ui_slider(ui_id(label), sr, *v, lo, hi));
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
        c_slider("Size", &app.brush_size, 1, 400, "%d");
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

// with vectors selected, a new color goes onto them as it changes (Umoupen); one undo step per drag
static bool recolor_open;
static void recolor(void) {
    if (!strchr("VAQL", app.tool) || app.tool == 0) return;
    if (select_recolor(col_alpha(app_color(), app.alpha), app.color_target, !recolor_open)) recolor_open = true;
}

void color_panel(Rect r) {
    if (!ui.in.down[0]) recolor_open = false;
    sync_hsv();
    dl_rect(ui.dl, rect(r.x, r.y + r.h - 1, r.w, 1), C_LINE);
    Rect in = rect_inset(r, 16, 16);
    V2 c = {roundf(in.x + in.w / 2), in.y + 4 + 102};
    if (ui_color_wheel(ui_id("wheel"), c, 102, 85, &app.hue, &app.sat, &app.val)) { set_from_hsv(); recolor(); }

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
        if (strlen(s) == 3) { char e[7] = {s[0], s[0], s[1], s[1], s[2], s[2], 0}; if (sscanf(e, "%x", &v) == 1) { app_set_color(hex(v)); recolor(); } }
        else if (sscanf(s, "%x", &v) == 1) { app_set_color(hex(v)); recolor(); }
    }
    if (ui.focus != ui_id("alpha")) snprintf(abuf, sizeof(abuf), "%d%%", (int)roundf(app.alpha * 100));
    if (ui_text_field(ui_id("alpha"), rect(in.x + in.w - 74, y, 74, 30), abuf, sizeof(abuf), "A")) {
        int a; if (sscanf(abuf, "%d", &a) == 1) { app.alpha = clampf(a / 100.0f, 0, 1); recolor(); }
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
            if (a.clicked) { app_set_color(app.swatches[i]); recolor(); }
            if (a.right) { memmove(&app.swatches[i], &app.swatches[i + 1], sizeof(Color) * (app.nswatches - i - 1)); app.nswatches--; }
        }
        sx += 27;
    }
}

/* ── brushes ── */

// an image tip on the ribbon: stamps along it, or the image stretched over it (as the stage draws them)
static void ribbon_image(const V2 *q, const float *hw, const float *wf, int N, const BrushPreset *p, Color c, u32 view, int tw, int th) {
    static const float zero[4] = {0};
    float s = ui.dl->scale, aspect = (float)th / tw, hmax = 0;
    for (int i = 0; i <= N; i++) hmax = MAX(hmax, hw[i]);
    bool fx = p->img_flags & IMGF_FLIP_X, fy = p->img_flags & IMGF_FLIP_Y;
    float u0 = fx ? 1 : 0, u1 = fx ? 0 : 1, v0 = fy ? 1 : 0, v1 = fy ? 0 : 1;
    u32 old = ui.dl->tex;
    dl_set_tex(ui.dl, view);
    dl_set_sampler(ui.dl, SMP_MIP);
#define IV(P, U, V) dl_vtx_uvp(ui.dl, (P).x * s, (P).y * s, U, V, c, K_TIPMASK, zero)
    if (p->img_mode == IMG_STAMPS) {
        float gap = MAX(2.0f, p->img_spacing / 100 * 2 * hmax), since = gap;
        float a = p->angle * VX_PI / 180, ca = cosf(a), sa = sinf(a);
        for (int i = 0; i <= N; i++) {
            if (i) since += hypotf(q[i].x - q[i - 1].x, q[i].y - q[i - 1].y);
            if (since < gap) continue;
            since = 0;
            V2 d = v2(q[MIN(i + 1, N)].x - q[MAX(i - 1, 0)].x, q[MIN(i + 1, N)].y - q[MAX(i - 1, 0)].y);
            float L = hypotf(d.x, d.y);
            d = (p->img_flags & IMGF_FIXED) || L < 1e-4f ? v2(1, 0) : v2(d.x / L, d.y / L);
            V2 ax = {d.x * ca - d.y * sa, d.x * sa + d.y * ca}, ay = {-ax.y, ax.x};
            float hl = hw[i], hs = hw[i] * aspect * wf[i];
            V2 c0 = q[i];
            u32 v = IV(v2(c0.x - ax.x * hl - ay.x * hs, c0.y - ax.y * hl - ay.y * hs), u0, v0);
            IV(v2(c0.x + ax.x * hl - ay.x * hs, c0.y + ax.y * hl - ay.y * hs), u1, v0);
            IV(v2(c0.x + ax.x * hl + ay.x * hs, c0.y + ax.y * hl + ay.y * hs), u1, v1);
            IV(v2(c0.x - ax.x * hl + ay.x * hs, c0.y - ax.y * hl + ay.y * hs), u0, v1);
            dl_idx(ui.dl, v, v + 1, v + 2); dl_idx(ui.dl, v, v + 2, v + 3);
        }
    } else {
        u32 prev = 0;
        for (int i = 0; i <= N; i++) {
            V2 d = v2(q[MIN(i + 1, N)].x - q[MAX(i - 1, 0)].x, q[MIN(i + 1, N)].y - q[MAX(i - 1, 0)].y);
            float L = MAX(1e-4f, hypotf(d.x, d.y));
            V2 n = v2(-d.y / L, d.x / L);
            float u = lerpf(u0, u1, (float)i / N);
            float h = hw[i] * wf[i];
            u32 v = IV(v2(q[i].x - n.x * h, q[i].y - n.y * h), u, v0);
            IV(v2(q[i].x + n.x * h, q[i].y + n.y * h), u, v1);
            if (i) { dl_idx(ui.dl, prev, prev + 1, v + 1); dl_idx(ui.dl, prev, v + 1, v); }
            prev = v;
        }
    }
#undef IV
    dl_set_sampler(ui.dl, SMP_LINEAR);
    dl_set_tex(ui.dl, old);
}

// a stroke drawn with the preset, light, heavy, light: its outline from the size, pressure curve, tip and
// tapers; with a texture, the grain shows through (a strip with soft one-pixel edges)
static void ribbon(Rect r, const BrushPreset *p, Color c) {
    enum { N = 40 };
    V2 q[N + 1]; float hw[N + 1], wf[N + 1];
    float ls = p->taper_s / 200, le = p->taper_e / 200;   // taper lengths as parts of the ribbon (100 % = half)
    for (int i = 0; i <= N; i++) {
        float t = (float)i / N;
        float pr = sinf(t * VX_PI);
        float w = p->size * 0.22f * MAX(0.06f, curve_eval(&p->curve, pr));
        if (ls > 0) w *= MIN(1, t / ls);
        if (le > 0) w *= MIN(1, (1 - t) / le);
        if (p->tip == TIP_OVAL) w *= 0.55f + 0.45f * fabsf(cosf(t * 2.2f + p->angle));
        hw[i] = clampf(w, 0.3f, r.h * 0.42f);
        wf[i] = MAX(0.02f, lerpf(p->img_w0, p->img_w1, pr) / 100);   // image tips: width across by pressure
        q[i] = v2(r.x + 4 + t * (r.w - 8), r.y + r.h / 2 + sinf(t * VX_PI * 1.4f + p->angle) * 3);
    }
    c = col_alpha(c, p->opacity > 0 ? p->opacity / 100 : 1);
    int ib = p->img_mode && p->tip_image[0] ? tipimg_bitmap(p->tip_image) : 0;
    Bitmap *ibm = ib ? bitmap_get(ib) : NULL;
    u32 iview = ibm ? bitmap_texture(ib) : 0;
    if (iview && ibm->w > 0 && ibm->h > 0) { ribbon_image(q, hw, wf, N, p, c, iview, ibm->w, ibm->h); return; }
    int bm = p->texture[0] ? texture_bitmap(p->texture) : 0;
    Bitmap *b = bm ? bitmap_get(bm) : NULL;
    u32 view = b ? bitmap_texture(bm) : 0;
    if (!view) {
        for (int i = 1; i <= N; i++) dl_line(ui.dl, q[i - 1], q[i], hw[i - 1] + hw[i], c);
        return;
    }
    float s = ui.dl->scale, ts = MAX(0.1f, p->tex_scale / 100);
    float prm[4] = {p->tex_density / 100, 0, 0, 0};
    u32 old = ui.dl->tex;
    dl_set_tex(ui.dl, view);
    dl_set_sampler(ui.dl, SMP_REPEAT);
    u32 v0 = 0;
    for (int i = 0; i <= N; i++) {
        V2 d = v2(q[MIN(i + 1, N)].x - q[MAX(i - 1, 0)].x, q[MIN(i + 1, N)].y - q[MAX(i - 1, 0)].y);
        float L = MAX(1e-4f, hypotf(d.x, d.y));
        V2 n = v2(-d.y / L, d.x / L);
        static const float off[4] = {-1, -1, 1, 1}, grow[4] = {0.7f, -0.7f, -0.7f, 0.7f}, al[4] = {0, 1, 1, 0};
        for (int k = 0; k < 4; k++) {
            float o = off[k] * MAX(0, hw[i] + grow[k] / s);
            V2 pp = v2(q[i].x + n.x * o, q[i].y + n.y * o);
            u32 v = dl_vtx_uvp(ui.dl, pp.x * s, pp.y * s, pp.x / (b->w * ts), pp.y / (b->h * ts), col_alpha(c, al[k]), K_GRAIN, prm);
            if (!i && !k) v0 = v;
        }
    }
    for (int i = 0; i < N; i++)
        for (int k = 0; k < 3; k++) {
            u32 a = v0 + i * 4 + k, b2 = a + 1, c2 = a + 4, d2 = a + 5;
            dl_idx(ui.dl, a, b2, d2); dl_idx(ui.dl, a, d2, c2);
        }
    dl_set_sampler(ui.dl, SMP_LINEAR);
    dl_set_tex(ui.dl, old);
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

// the brush list: a click picks a brush, Ctrl / Shift pick several, dragging a row moves it. Each row
// has Duplicate and Edit (built-ins can't be edited: they are duplicated first). Edit swaps the list
// for the brush's settings
static int B_EDIT = -1;     // the brush whose settings are open, -1 = the list
static int *B_DEL;          // brushes waiting for the delete confirmation

static bool small_icon(UiId id, Rect b, IconId ic, bool enabled, const char *tip) {
    UiResp re = ui_interact(id, b);
    if (re.hover && enabled) dl_rrect(ui.dl, b, 6, C_S3);
    icon_draw(ui.dl, ic, v2(b.x + b.w / 2, b.y + b.h / 2), ICON_S, false, !enabled ? col_a(C_TX3, 0x50) : re.hover ? C_TX : C_TX3);
    ui_tooltip(id, b, tip);
    return enabled && re.clicked;
}

static void brush_duplicate(int i) {
    BrushPreset p = app.presets[i];
    char base[48];
    snprintf(base, sizeof(base), "%.42s copy", p.name);
    app_brush_name(p.name, sizeof(p.name), base);
    app_apply_preset(app_insert_preset(i + 1, &p));
}

// the brush in use (app.*) written into a preset
static void preset_from_app(BrushPreset *q) {
    q->size = app.brush_size; q->smoothing = app.brush_smooth; q->curve = app.curve;
    q->tip = app.tip; q->angle = app.tip_angle; q->aspect = app.tip_aspect;
    q->taper_s = app.taper_s; q->taper_e = app.taper_e; q->opacity = app.brush_opacity; q->fit = app.brush_fit;
    snprintf(q->texture, sizeof(q->texture), "%s", app.texture);
    q->tex_density = app.tex_density; q->tex_scale = app.tex_scale;
    snprintf(q->tip_image, sizeof(q->tip_image), "%s", app.tip_image);
    q->img_mode = app.img_mode; q->img_flags = app.img_flags; q->img_spacing = app.img_spacing;
    q->img_w0 = app.img_w0; q->img_w1 = app.img_w1;
}

void brush_new(void) {   // the brush in use as it is now, at the end of the list, opened for editing
    BrushPreset p = {0};
    preset_from_app(&p);
    app_brush_name(p.name, sizeof(p.name), "New Brush");
    int at = app_insert_preset((int)arrlen(app.presets), &p);
    app_apply_preset(at);
    app.right_tab = 0;
    B_EDIT = at;
}
bool brush_editing(void) { return B_EDIT >= 0; }

static void ask_delete(const int *idx, int n, Rect at) {
    arrsetlen(B_DEL, 0);
    for (int k = 0; k < n; k++) if (idx[k] >= 0 && idx[k] < arrlen(app.presets) && app.presets[idx[k]].user) arrput(B_DEL, idx[k]);
    if (arrlen(B_DEL)) ui_open_popup(ui_id("preset-delete"), at);
}
static void delete_popup(void) {
    if (!ui_popup_begin(ui_id("preset-delete"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 250, 10 + 24 + 9 + 28 * 2))) return;
    int n = (int)arrlen(B_DEL);
    char t[96];
    if (n == 1 && B_DEL[0] < arrlen(app.presets)) snprintf(t, sizeof(t), "Delete \"%s\"?", app.presets[B_DEL[0]].name);
    else snprintf(t, sizeof(t), "Delete %d brushes?", n);
    ui_menu_header(t);
    ui_menu_sep();
    if (ui_menu_item_ic(IC_TRASH, n == 1 ? "Delete Brush" : "Delete Brushes", NULL, true, false)) {
        for (int q = (int)arrlen(app.presets) - 1; q >= 0; q--) {   // highest first, so the other indices stay right
            bool want = false;
            for (int k = 0; k < n; k++) want |= B_DEL[k] == q;
            if (!want) continue;
            if (B_EDIT == q) B_EDIT = -1;
            else if (B_EDIT > q) B_EDIT--;
            app_remove_preset(q);
        }
        arrsetlen(B_DEL, 0);
    }
    if (ui_menu_item("Cancel", NULL, true, false)) arrsetlen(B_DEL, 0);
    ui_popup_end();
}

/* ── the brush's settings, in blocks that fold ── */

static void slider_row(const char *id, const char *label, float x, float w, float *y, float *v, float lo, float hi, const char *fmt) {
    font_draw_in(ui.dl, F_UI, rect(x, *y, 76, 20), ALIGN_LEFT, label, C_TX3);
    *v = roundf(ui_slider(ui_id(id), rect(x + 84, *y, w - 84 - 48, 20), *v, lo, hi));
    char s[16]; snprintf(s, sizeof(s), fmt, (int)*v);
    font_draw_in(ui.dl, F_MONO_MED, rect(x + w - 44, *y, 44, 20), ALIGN_RIGHT, s, C_TX);
    *y += 20;
}

// a block's title: a click folds it
static bool block(const char *id, const char *title, float x, float w, float *y, bool *open) {
    *y += 6;
    dl_rect(ui.dl, rect(x - 16, *y, w + 32, 1), C_LINE);
    Rect h = {x - 8, *y + 4, w + 16, 30};
    UiResp re = ui_interact(ui_id(id), h);
    if (re.hover) dl_rrect(ui.dl, h, 6, C_S2);
    icon_draw(ui.dl, *open ? IC_CARET_DOWN : IC_CARET_RIGHT, v2(x + 5, h.y + 15), ICON_S, true, re.hover ? C_TX2 : C_TX3);
    font_draw_in(ui.dl, F_UI_MED, rect(x + 18, h.y, w - 18, h.h), ALIGN_LEFT, title, *open || re.hover ? C_TX : C_TX2);
    if (re.clicked) *open = !*open;
    *y = h.y + h.h + (*open ? 10 : 4);
    return *open;
}

static struct { bool on; Rect r; } IMG_DD;   // the tip image dropdown, drawn after the blocks
static void image_dropdown(void) {
    if (!IMG_DD.on) return;
    int n = tipimg_count(), sel = 0;
    const char *items[64];
    int m = 0;
    for (int i = 0; i < n && m < 64; i++) { items[m++] = tipimg_name(i); if (!strcmp(tipimg_name(i), app.tip_image)) sel = m - 1; }
    if (!m) return;
    int ns = ui_dropdown(ui_id("tip-pick"), IMG_DD.r, items, m, sel);
    if (ns != sel) snprintf(app.tip_image, sizeof(app.tip_image), "%s", items[ns]);
}

static void shape_block(float x, float w, float *py) {
    float y = *py;
    font_draw_in(ui.dl, F_UI, rect(x, y, 76, 20), ALIGN_LEFT, "Size", C_TX3);
    app.brush_size = roundf(slider_sq(ui_id("brush-size"), rect(x + 84, y, w - 84 - 48, 20), app.brush_size, 1, 400));
    char sz[16]; snprintf(sz, sizeof(sz), "%d", (int)app.brush_size);
    font_draw_in(ui.dl, F_MONO_MED, rect(x + w - 44, y, 44, 20), ALIGN_RIGHT, sz, C_TX);
    y += 20;
    y += 14;
    // the tip: a vector shape, or an image stamped along the line or laid over all of it
    static const char *tips[] = {"Round", "Oval", "Square", "Image"};
    font_draw_in(ui.dl, F_UI, rect(x, y + 5, 76, 20), ALIGN_LEFT, "Tip", C_TX3);
    int cur = app.img_mode ? 3 : app.tip;
    int tip = ui_segmented(ui_id("tip"), rect(x + 84, y, w - 84, 30), tips, 4, cur); y += 30;
    if (tip != cur && tip == 3) {
        if (!app.tip_image[0] && tipimg_count()) snprintf(app.tip_image, sizeof(app.tip_image), "%s", tipimg_name(0));
        if (!app.tip_image[0]) {
            char path[512], name[48];
            if (plat_open_dialog(path, sizeof(path), "Tip image", "*.png;*.jpg;*.jpeg;*.bmp") && tipimg_import(path, name, sizeof(name)))
                snprintf(app.tip_image, sizeof(app.tip_image), "%s", name);
        }
        if (app.tip_image[0]) { app.img_mode = IMG_STAMPS; app.tip = TIP_ROUND; }
    } else if (tip != cur) {
        app.img_mode = IMG_NONE;
        if (tip == TIP_OVAL && app.tip != TIP_OVAL && app.tip_aspect > 0.95f) app.tip_aspect = 0.38f;
        app.tip = (u8)tip;
    }
    IMG_DD.on = false;
    if (app.img_mode) {
        y += 12;
        font_draw_in(ui.dl, F_UI, rect(x, y + 5, 76, 20), ALIGN_LEFT, "Image", C_TX3);
        if (small_icon(ui_id("tip-import"), rect(x + w - 30, y, 30, 30), IC_IMPORT, true, "Add a tip image (dark and opaque is ink)")) {
            char path[512], name[48];
            if (plat_open_dialog(path, sizeof(path), "Tip image", "*.png;*.jpg;*.jpeg;*.bmp")) {
                if (tipimg_import(path, name, sizeof(name))) snprintf(app.tip_image, sizeof(app.tip_image), "%s", name);
                else app_toast("Couldn't read that image");
            }
        }
        IMG_DD.on = true; IMG_DD.r = rect(x + 84, y, w - 84 - 36, 30);   // drawn last, over what follows
        y += 30 + 12;
        static const char *place[] = {"Stamps", "Along the line"};
        font_draw_in(ui.dl, F_UI, rect(x, y + 5, 76, 20), ALIGN_LEFT, "Place", C_TX3);
        app.img_mode = (u8)(1 + ui_segmented(ui_id("tip-place"), rect(x + 84, y, w - 84, 30), place, 2, app.img_mode - 1)); y += 30;
        if (app.img_mode == IMG_STAMPS) {
            y += 12;
            slider_row("tip-spacing", "Spacing", x, w, &y, &app.img_spacing, 1, 300, "%d%%");
            y += 10;
            slider_row("tip-angle", "Angle", x, w, &y, &app.tip_angle, 0, 359, "%d\xC2\xB0");
        }
        y += 12;
        slider_row("tip-w0", "Width light", x, w, &y, &app.img_w0, 2, 300, "%d%%");
        y += 10;
        slider_row("tip-w1", "Width heavy", x, w, &y, &app.img_w1, 2, 300, "%d%%");
        y += 12;
        float cw = (w - 84 - 8) / 2;
        Rect c1 = {x + 84, y, cw, 28}, c2 = {x + 84 + cw + 8, y, cw, 28};
        font_draw_in(ui.dl, F_UI, rect(x, y + 4, 76, 20), ALIGN_LEFT, "Flip", C_TX3);
        if (ui_chip("Along", c1, app.img_flags & IMGF_FLIP_X)) app.img_flags ^= IMGF_FLIP_X;
        if (ui_chip("Across", c2, app.img_flags & IMGF_FLIP_Y)) app.img_flags ^= IMGF_FLIP_Y;
        y += 28;
        if (app.img_mode == IMG_STAMPS) {
            y += 8;
            Rect c3 = {x + 84, y, w - 84, 28};
            if (ui_chip("Turn with the line", c3, !(app.img_flags & IMGF_FIXED))) app.img_flags ^= IMGF_FIXED;
            y += 28;
        }
    } else if (app.tip != TIP_ROUND) {
        y += 12;
        slider_row("tip-angle", "Angle", x, w, &y, &app.tip_angle, 0, 180, "%d\xC2\xB0");
        if (app.tip == TIP_OVAL) {
            y += 10;
            float pc = app.tip_aspect * 100;
            slider_row("tip-aspect", "Roundness", x, w, &y, &pc, 5, 100, "%d%%");
            app.tip_aspect = pc / 100;
        }
    }
    *py = y + 4;
}

static void texture_block(float x, float w, float *py) {
    float y = *py;
    // the texture: none, the built-ins, the user's; the button brings in an image of one's own
    int n = texture_count(), sel = 0;
    const char *items[66];
    items[0] = "None";
    int m = 1;
    for (int i = 0; i < n && m < 66; i++) { items[m++] = texture_name(i); if (!strcmp(texture_name(i), app.texture)) sel = m - 1; }
    font_draw_in(ui.dl, F_UI, rect(x, y + 5, 76, 20), ALIGN_LEFT, "Grain", C_TX3);
    Rect ib = {x + w - 30, y, 30, 30};
    if (small_icon(ui_id("tex-import"), ib, IC_IMPORT, true, "Add a texture from an image (grey: white lets the ink through)")) {
        char path[512], name[48];
        if (plat_open_dialog(path, sizeof(path), "Images", "*.png;*.jpg;*.jpeg;*.bmp")) {
            if (texture_import(path, name, sizeof(name))) snprintf(app.texture, sizeof(app.texture), "%s", name);
            else app_toast("Couldn't read that image");
        }
    }
    Rect dd = {x + 84, y, w - 84 - 36, 30};
    y += 30;
    if (app.texture[0] && texture_find(app.texture) >= 0) {
        y += 12;
        slider_row("tex-density", "Density", x, w, &y, &app.tex_density, 0, 100, "%d%%");
        y += 10;
        slider_row("tex-scale", "Scale", x, w, &y, &app.tex_scale, 10, 400, "%d%%");
    }
    *py = y + 4;
    // the dropdown last: its list is drawn over the rows below
    int ns = ui_dropdown(ui_id("tex-pick"), dd, items, m, sel);
    if (ns != sel) snprintf(app.texture, sizeof(app.texture), "%s", ns ? items[ns] : "");
}

static void delete_button(int i, float x, float w, float *py) {
    Rect del = {x, *py, w, 30};
    UiId did = ui_id("brush-delete");
    UiResp dre = ui_interact(did, del);
    dl_rrect_border(ui.dl, del, 7, 1, dre.hover ? C_LINE2 : C_LINE);
    if (dre.hover) dl_rrect(ui.dl, rect_inset(del, 1, 1), 6, C_S2);
    float tw = font_width(F_UI, "Delete Brush");
    icon_draw(ui.dl, IC_TRASH, v2(del.x + del.w / 2 - tw / 2 - 6, del.y + 15), ICON_S, false, dre.hover ? C_TX : C_TX3);
    font_draw_in(ui.dl, F_UI, rect(del.x + del.w / 2 - tw / 2 + 6, del.y, tw + 4, del.h), ALIGN_LEFT, "Delete Brush", dre.hover ? C_TX : C_TX2);
    if (dre.clicked) ask_delete(&i, 1, rect(del.x, del.y + del.h + 4, del.w, 1));
    *py += 30;
}

// the settings of one of the user's brushes: they are the brush in use while open, and every change is
// kept in the brush (saved once the mouse is let go)
static void brush_editor(Rect r) {
    int i = B_EDIT;
    BrushPreset *p = &app.presets[i];
    static float scroll, content;
    static bool dirty;
    static bool open[6] = {true, true, true, true, true, true};
    if (rect_has(r, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y != 0) scroll -= ui.in.wheel_y * 40;
    scroll = clampf(scroll, 0, MAX(0, content - r.h));
    float x = r.x + 16, w = r.w - 32, y = r.y + 12 - scroll;

    // back, name, export
    if (small_icon(ui_id("brush-back"), rect(x - 6, y, 30, 30), IC_BACK, true, "Back to the brushes") ||
        (ui.in.key_pressed[SAPP_KEYCODE_ESCAPE] && !ui.focus && !ui.popup)) { B_EDIT = -1; scroll = 0; return; }
    static char name[48];
    UiId nid = ui_id("brush-name");
    if (ui.focus != nid) snprintf(name, sizeof(name), "%s", p->name);
    if (ui_text_field(nid, rect(x + 30, y, w - 30 - 36, 30), name, sizeof(name), NULL)) {
        for (char *c = name; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
        if (name[0] && strcmp(name, p->name)) {
            p->name[0] = 0;   // not in the way of its own new name
            app_brush_name(p->name, sizeof(p->name), name);
            app_presets_save();
        }
    }
    if (small_icon(ui_id("brush-export"), rect(x + w - 30, y, 30, 30), IC_EXPORT, true, "Export this brush (.vxbrush)")) app_export_preset(i);
    y += 30 + 14;

    // a stroke with it, light, heavy, light
    Rect pv = {x, y, w, 60};
    dl_rrect(ui.dl, pv, 8, C_S2);
    BrushPreset big = *p;
    big.size *= 2;   // the list's ribbons are small; here the size shows
    ribbon(rect_inset(pv, 12, 4), &big, C_TX);
    y += 60 + 8;

    if (block("blk-shape", "Shape", x, w, &y, &open[0])) shape_block(x, w, &y);
    if (block("blk-pressure", "Pressure", x, w, &y, &open[1])) {
        font_draw_in(ui.dl, F_UI, rect(x, y, w, 16), ALIGN_LEFT, "Pressure \xE2\x86\x92 size", C_TX3); y += 22;
        curve_editor(rect(x, y, w, 104));
        y += 104 + 4;
    }
    if (block("blk-taper", "Taper", x, w, &y, &open[2])) {
        slider_row("taper-start", "Start", x, w, &y, &app.taper_s, 0, 100, "%d%%");
        y += 10;
        slider_row("taper-end", "End", x, w, &y, &app.taper_e, 0, 100, "%d%%");
        y += 4;
    }
    if (block("blk-ink", "Ink", x, w, &y, &open[3])) {
        slider_row("brush-opacity", "Opacity", x, w, &y, &app.brush_opacity, 1, 100, "%d%%");
        y += 4;
    }
    float ty = y;
    bool tex_open = block("blk-texture", "Texture", x, w, &y, &open[4]);
    if (tex_open) { ty = y; y += 30 + 4; if (app.texture[0] && texture_find(app.texture) >= 0) y += 12 + 20 + 10 + 20; }
    if (block("blk-stab", "Stabilization", x, w, &y, &open[5])) {
        slider_row("brush-smooth", "Smoothing", x, w, &y, &app.brush_smooth, 0, 100, "%d");
        y += 10;
        slider_row("brush-fit", "Curve fit", x, w, &y, &app.brush_fit, 0, 100, "%d");
        y += 4;
    }
    y += 6;
    dl_rect(ui.dl, rect(r.x, y, r.w, 1), C_LINE);
    y += 16;
    delete_button(i, x, w, &y);
    if (tex_open) texture_block(x, w, &ty);   // after the blocks below it: its dropdown list goes over them
    if (open[0]) image_dropdown(); else IMG_DD.on = false;
    y += 16;
    content = y + scroll - r.y;
    if (content > r.h) {
        float th = r.h * r.h / content, sy = r.y + (r.h - th) * scroll / (content - r.h);
        dl_rrect(ui.dl, rect(r.x + r.w - 5, sy, 3, th), 1.5f, hexa(0xffffff, 0x22));
    }

    // what the controls (and the context bar) changed goes into the brush
    if (B_EDIT != i) return;
    BrushPreset q = *p;
    preset_from_app(&q);
    if (memcmp(&q, p, sizeof(q))) { *p = q; dirty = true; }
    if (dirty && !ui.in.down[0]) { app_presets_save(); dirty = false; }
}

static void brushes_tab(Rect r) {
    int n = (int)arrlen(app.presets);
    if (B_EDIT >= n || (B_EDIT >= 0 && (!app.presets[B_EDIT].user || app.preset != B_EDIT))) B_EDIT = -1;   // another brush was picked (an import)
    if (B_EDIT >= 0) { brush_editor(r); delete_popup(); return; }

    static float scroll;
    float list_h = r.h - 8, content = n * 36.0f + 4;
    Rect list = {r.x, r.y + 4, r.w, list_h};
    if (rect_has(list, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y != 0) scroll -= ui.in.wheel_y * 36;
    static int shown = -1;   // keep a newly chosen preset (e.g. just imported) in view
    if (app.preset != shown) {
        float py = app.preset * 36.0f;
        if (py < scroll) scroll = py;
        if (py + 40 > scroll + list_h) scroll = py + 40 - list_h;
        shown = app.preset;
    }
    static int press = -1, drag = -1;   // the row pressed; the row being moved once the mouse goes 5 px
    static float press_y;
    if (drag >= 0) {   // near the edges the list scrolls by itself
        if (ui.in.mouse.y < list.y + 24) scroll -= 6;
        if (ui.in.mouse.y > list.y + list.h - 24) scroll += 6;
    }
    scroll = clampf(scroll, 0, MAX(0, content - list_h));
    float top = list.y + 2 - scroll;
    dl_push_clip(ui.dl, list);
    static int *PICK;   // brushes picked with Ctrl / Shift (to delete several at once)
    for (ptrdiff_t k = arrlen(PICK) - 1; k >= 0; k--) if (PICK[k] >= n) arrdel(PICK, k);
    for (int i = 0; i < n; i++) {
        Rect row = {r.x + 8, top + i * 36.0f, r.w - 16, 34};
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        BrushPreset *p = &app.presets[i];
        // the row buttons first, so the row doesn't take their clicks
        Rect eb = {row.x + row.w - 28, row.y + 5, 24, 24}, cb = {eb.x - 26, eb.y, 24, 24};
        bool over = drag < 0 && rect_has(row, ui.in.mouse) && ui_mouse_free();
        bool on_btn = over && (rect_has(eb, ui.in.mouse) || rect_has(cb, ui.in.mouse));
        UiResp re = on_btn ? (UiResp){0} : ui_interact(ui_idi("preset", i), row);
        bool on = app.preset == i, picked = false;
        for (ptrdiff_t k = 0; k < arrlen(PICK); k++) picked |= PICK[k] == i;
        if (drag == i) dl_rrect(ui.dl, row, 7, C_S3);
        else if (on) { dl_rrect(ui.dl, row, 7, C_AC2); dl_rrect_border(ui.dl, row, 7, 1, C_AC3); }
        else if (picked) { dl_rrect(ui.dl, row, 7, C_S3); dl_rrect_border(ui.dl, row, 7, 1, C_AC3); }
        else if (re.hover || over) dl_rrect(ui.dl, row, 7, C_S2);
        Color tc = on || picked || over || drag == i ? C_TX : C_TX2;
        font_draw_in(ui.dl, F_UI, rect(row.x + 10, row.y, row.w - 10 - 158, row.h), ALIGN_LEFT, p->name, tc);
        ribbon(rect(cb.x - 98, row.y + 7, 92, 20), p, tc);
        if (small_icon(ui_idi("preset-dup", i), cb, IC_COPY, true, "Duplicate")) brush_duplicate(i);
        if (small_icon(ui_idi("preset-edit", i), eb, IC_EDIT, p->user,
                       p->user ? "Edit brush" : "Built-in brushes can't be changed: duplicate it and edit the copy")) {
            app_apply_preset(i);
            B_EDIT = i;
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
            } else { arrsetlen(PICK, 0); app_apply_preset(i); press = i; press_y = ui.in.mouse.y; }
        }
        if (re.dbl && p->user) B_EDIT = i;
        if (re.right) {
            if (!picked) { arrsetlen(PICK, 0); arrput(PICK, i); }
            ui_open_popup(ui_id("preset-menu"), rect(ui.in.mouse.x, ui.in.mouse.y, 1, 1));
        }
    }
    // moving a row: where it would land
    int slot = CLAMP((int)floorf((ui.in.mouse.y - top) / 36 + 0.5f), 0, n);
    if (press >= 0 && ui.in.down[0] && drag < 0 && fabsf(ui.in.mouse.y - press_y) > 5) drag = press;
    if (drag >= 0) {
        ui_cursor(SAPP_MOUSECURSOR_RESIZE_NS);
        if (slot != drag && slot != drag + 1) dl_rrect(ui.dl, rect(r.x + 10, top + slot * 36.0f - 2, r.w - 20, 2), 1, C_AC);
    }
    if (!ui.in.down[0]) {
        if (drag >= 0 && slot != drag && slot != drag + 1) { app_move_preset(drag, slot > drag ? slot - 1 : slot); arrsetlen(PICK, 0); shown = app.preset; }
        press = drag = -1;
    }
    dl_pop_clip(ui.dl);

    if (ui_popup_begin(ui_id("preset-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 240, 10 + 28 * 5 + 9 * 2))) {
        int one = arrlen(PICK) == 1 ? PICK[0] : -1, users = 0;
        for (ptrdiff_t k = 0; k < arrlen(PICK); k++) users += PICK[k] < n && app.presets[PICK[k]].user;
        if (ui_menu_item("Use This Brush", NULL, one >= 0, false)) app_apply_preset(one);
        if (ui_menu_item("Duplicate", NULL, one >= 0, false)) brush_duplicate(one);
        if (ui_menu_item("Edit Brush", NULL, one >= 0 && app.presets[one].user, false)) { app_apply_preset(one); B_EDIT = one; }
        ui_menu_sep();
        if (ui_menu_item("Export", NULL, one >= 0, false)) app_export_preset(one);
        ui_menu_sep();
        char del[64];
        snprintf(del, sizeof(del), users > 1 ? "Delete %d Brushes" : "Delete Brush", users);
        if (ui_menu_item(del, NULL, users > 0, false)) { ask_delete(PICK, (int)arrlen(PICK), rect(ui.popup_anchor.x, ui.popup_anchor.y, 1, 1)); arrsetlen(PICK, 0); }
        ui_popup_end();
    }
    delete_popup();
    if (content > list_h) {   // thin scroll thumb
        float th = list_h * list_h / content, ty = list.y + (list_h - th) * scroll / (content - list_h);
        dl_rrect(ui.dl, rect(r.x + r.w - 5, ty, 3, th), 1.5f, hexa(0xffffff, 0x22));
    }
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

// the ease of a tween as a graph (time across, progress up); a custom curve can be shaped: drag its points,
// double-click the graph to add a point, right-click one to take it away
static void ease_graph(Rect g, Key *k) {
    Doc *d = app.doc;
    dl_rrect(ui.dl, g, 8, C_S2);
    dl_rrect_border(ui.dl, g, 8, 1, C_LINE);
    Rect a = rect_inset(g, 14, 12);
    const float y0 = -0.3f, y1 = 1.3f;   // room for back and elastic overshoot
    #define GX(v) (a.x + (v) * a.w)
    #define GY(v) (a.y + a.h - ((v) - y0) / (y1 - y0) * a.h)
    dl_rect(ui.dl, rect(a.x, roundf(GY(0)), a.w, 1), C_LINE2);
    dl_rect(ui.dl, rect(a.x, roundf(GY(1)), a.w, 1), C_LINE2);
    dl_line(ui.dl, v2(GX(0), GY(0)), v2(GX(1), GY(1)), 1, col_a(C_TX3, 0x60));
    V2 prev = {GX(0), GY(key_ease(k, 0))};
    for (int i = 1; i <= 80; i++) {
        float t = i / 80.0f;
        V2 q = {GX(t), GY(key_ease(k, t))};
        dl_line(ui.dl, prev, q, 2, C_AC);
        prev = q;
    }
    if (k->ease != EASE_CUSTOM || k->nez < 4) {
        font_draw_in(ui.dl, F_SMALL, rect(g.x + 10, g.y + g.h - 20, g.w - 20, 16), ALIGN_RIGHT, "time", C_TX3);
        return;
    }
    static int drag = -1;
    int hover = -1, remove = -1;
    for (int i = 0; i < k->nez; i++) {   // handles: lines from the anchors to their controls
        if (i % 3 == 1) dl_line(ui.dl, v2(GX(k->ez[i - 1].x), GY(k->ez[i - 1].y)), v2(GX(k->ez[i].x), GY(k->ez[i].y)), 1, C_TX3);
        if (i % 3 == 2) dl_line(ui.dl, v2(GX(k->ez[i + 1].x), GY(k->ez[i + 1].y)), v2(GX(k->ez[i].x), GY(k->ez[i].y)), 1, C_TX3);
    }
    for (int i = 0; i < k->nez; i++) {
        V2 c = {GX(k->ez[i].x), GY(k->ez[i].y)};
        bool anchor = i % 3 == 0, fixed = i == 0 || i == k->nez - 1;
        UiResp re = ui_interact(ui_idi("ease-pt", i), rect(c.x - 7, c.y - 7, 14, 14));
        if (re.hover) hover = i;
        if (re.pressed && !fixed) { app_commit(); drag = i; }
        if (re.right && anchor && !fixed) remove = i;
        if (drag == i && re.held) {
            V2 q = {clampf((ui.in.mouse.x - a.x) / a.w, 0, 1), clampf(y0 + (a.y + a.h - ui.in.mouse.y) / a.h * (y1 - y0), y0, y1)};
            int lo = anchor ? i - 3 : (i % 3 == 1 ? i - 1 : i - 2), hi = anchor ? i + 3 : (i % 3 == 1 ? i + 2 : i + 1);
            q.x = clampf(q.x, k->ez[lo].x, k->ez[hi].x);   // time only goes forward
            V2 dlt = {q.x - k->ez[i].x, q.y - k->ez[i].y};
            k->ez[i] = q;
            if (anchor) { k->ez[i - 1].x += dlt.x; k->ez[i - 1].y += dlt.y; k->ez[i + 1].x += dlt.x; k->ez[i + 1].y += dlt.y; }   // its handles come along
            doc_touch(d);
        }
        if (fixed && !anchor) {}
        dl_circle(ui.dl, c, anchor ? 4.5f : 3.5f, anchor ? C_TX : C_S4);
        dl_circle_border(ui.dl, c, anchor ? 4.5f : 3.5f, 1.5f, re.hover || drag == i ? C_AC : C_TX2);
    }
    if (!ui.in.down[0]) drag = -1;
    if (remove > 0) { app_commit(); for (int i = remove - 1; i + 3 < k->nez; i++) k->ez[i] = k->ez[i + 3]; k->nez -= 3; doc_touch(d); }
    UiResp gr = ui_interact(ui_id("ease-graph"), a);
    if (gr.dbl && hover < 0 && k->nez + 3 <= EASE_PTS) {   // a new point on the curve where it was clicked
        float t = clampf((ui.in.mouse.x - a.x) / a.w, 0.02f, 0.98f), y = key_ease(k, t);
        int s = 0;
        while (s + 1 < (k->nez - 1) / 3 && t > k->ez[s * 3 + 3].x) s++;
        int at = s * 3 + 2;   // after the segment's second control
        app_commit();
        for (int i = k->nez - 1; i >= at; i--) k->ez[i + 3] = k->ez[i];
        k->ez[at] = v2(t - 0.08f, y); k->ez[at + 1] = v2(t, y); k->ez[at + 2] = v2(t + 0.08f, y);
        k->ez[at].x = MAX(k->ez[at].x, k->ez[at - 2 > 0 ? at - 2 : 0].x);
        k->nez += 3;
        doc_touch(d);
    }
    ui_tooltip(ui_id("ease-graph"), a, "Drag the points. Double-click adds a point, right-click takes it away");
    #undef GX
    #undef GY
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
        // the ease of the tween this frame is in
        Key *tk = doc_key_for(l, doc_tl(d)->frame);
        if (tk && tk->tw != TW_NONE) {
            prow(&y, r, "Ease");
            int e = ui_dropdown(ui_id("ease"), rect(fx, y, fw, 30), EASE_NAMES, EASE_COUNT, tk->ease);
            if (e != tk->ease) {
                app_commit();
                tk->ease = (u8)e;
                if (e == EASE_CUSTOM && tk->nez < 4) key_ease_custom_default(tk);
                if (e == EASE_CLASSIC && tk->ease_amt == 0) tk->ease_amt = 50;
                doc_touch(d);
            }
            y += 40;
            if (tk->ease == EASE_CLASSIC) {
                if (srow(&y, r, "Amount", ui_id("ease-amt"), &tk->ease_amt, -100, 100, 1, 1, "%.0f")) doc_touch(d);
            } else if (tk->ease >= EASE_QUAD && tk->ease <= EASE_ELASTIC) {
                static const char *dirs[] = {"In", "Out", "In-Out"};
                prow(&y, r, "Direction");
                int dsel = ui_segmented(ui_id("ease-dir"), rect(fx, y, fw, 28), dirs, 3, tk->ease_dir);
                if (dsel != tk->ease_dir) { app_commit(); tk->ease_dir = (u8)dsel; doc_touch(d); }
                y += 36;
            }
            if (tk->ease != EASE_NONE) { ease_graph(rect(r.x + 16, y, r.w - 32, 150), tk); y += 160; }
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
    if (swatch_row(&y, r, "Background", ui_id("doc-bg"), d->bg)) { app_commit(); app.doc->bg = app.fill | 0xff000000u; doc_touch(app.doc); }
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
    if (app.right_tab == 0 && !brush_editing()) {   // new brush, import
        static const struct { const char *id, *tip; IconId ic; } B[] = {
            {"brush-new", "New brush from the current settings", IC_PLUS},
            {"import-sut", "Import brushes (.vxbrush, Clip Studio .sut)", IC_IMPORT},
        };
        for (int i = 0; i < 2; i++) {
            Rect b = {tb.x + tb.w - 38 - (1 - i) * 30, tb.y + 6, 28, 28};
            UiId id = ui_id(B[i].id);
            UiResp re = ui_interact(id, b);
            if (re.hover) dl_rrect(ui.dl, b, 7, C_S2);
            icon_draw(ui.dl, B[i].ic, v2(b.x + 14, b.y + 14), ICON_M, false, re.hover ? C_TX : C_TX2);
            ui_tooltip(id, b, B[i].tip);
            if (re.clicked) { if (i == 0) brush_new(); else app_import_brushes(); }
        }
    }
    dl_push_clip(ui.dl, r);
    if (app.right_tab == 0) brushes_tab(r);
    else if (app.right_tab == 1) library_tab(r);
    else props_tab(r);
    dl_pop_clip(ui.dl);
    library_drag();
}
