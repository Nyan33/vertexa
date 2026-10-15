#include "brush/brush.h"

// Fritsch-Carlson monotone cubic: smooth through every point, never above or below its neighbours
float curve_eval(const Curve *c, float p) {
    int n = c->n;
    if (n <= 0) return 1;
    if (n == 1) return clampf(c->y[0], 0, 1);
    p = clampf(p, 0, 1);
    if (p <= c->x[0]) return clampf(c->y[0], 0, 1);
    if (p >= c->x[n - 1]) return clampf(c->y[n - 1], 0, 1);
    float d[CURVE_MAX], m[CURVE_MAX];
    for (int i = 0; i + 1 < n; i++) { float h = c->x[i + 1] - c->x[i]; d[i] = h > 1e-6f ? (c->y[i + 1] - c->y[i]) / h : 0; }
    m[0] = d[0]; m[n - 1] = d[n - 2];
    for (int i = 1; i + 1 < n; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (int i = 0; i + 1 < n; i++) {
        if (fabsf(d[i]) < 1e-9f) { m[i] = m[i + 1] = 0; continue; }
        float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
        if (s > 9) { float t = 3 / sqrtf(s); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i]; }
    }
    int k = 0;
    while (k + 2 < n && p > c->x[k + 1]) k++;
    float h = c->x[k + 1] - c->x[k], t = h > 1e-6f ? (p - c->x[k]) / h : 0, t2 = t * t, t3 = t2 * t;
    float y = (2 * t3 - 3 * t2 + 1) * c->y[k] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * c->y[k + 1] + (t3 - t2) * h * m[k + 1];
    return clampf(y, 0, 1);
}

bool curve_insert(Curve *c, float x, float y, int *at) {
    if (c->n >= CURVE_MAX || c->n < 2) return false;
    int i = 1;
    while (i < c->n - 1 && c->x[i] < x) i++;
    if (x - c->x[i - 1] < 0.03f || c->x[i] - x < 0.03f) return false;   // too close to a neighbour
    for (int k = c->n; k > i; k--) { c->x[k] = c->x[k - 1]; c->y[k] = c->y[k - 1]; }
    c->x[i] = x; c->y[i] = clampf(y, 0, 1);
    c->n++;
    if (at) *at = i;
    return true;
}

void curve_remove(Curve *c, int i) {
    if (i <= 0 || i >= c->n - 1) return;
    for (int k = i; k + 1 < c->n; k++) { c->x[k] = c->x[k + 1]; c->y[k] = c->y[k + 1]; }
    c->n--;
}

void bb_begin(Builder *b, BrushOpts o) {
    memset(b, 0, sizeof(*b));
    b->o = o;
    b->k = 1 - (o.smoothing / 100.0f) * 0.9f;
    b->min_step = MAX(0.2f, 0.8f * o.unit);
}
void bb_free(Builder *b) { arrfree(b->pts); }

static float radius_for(Builder *b, float p, float tilt) {
    float f = b->o.pressure ? curve_eval(&b->o.curve, p) : 1;
    if (b->o.tilt_on && b->o.tilt_curve.n >= 2) f *= curve_eval(&b->o.tilt_curve, MIN(1, MAX(0, tilt) / 90));
    else if (b->o.tilt && tilt > 0) f *= 1 + 0.7f * MIN(1, tilt / 70);
    return MAX(0.05f, b->o.size / 2 * f);
}

static void emit(Builder *b, float x, float y, float p, float tilt) {
    float r = radius_for(b, p, tilt);
    ptrdiff_t n = arrlen(b->pts);
    if (n) {
        SPt *l = &b->pts[n - 1];
        if (hypotf(x - l->x, y - l->y) < b->min_step) { l->r = MAX(l->r * 0.8f, r); return; }
    }
    arrput(b->pts, ((SPt){x, y, r}));
}

void bb_add(Builder *b, float x, float y, float p, float tilt) {
    b->lx = x; b->ly = y; b->lp = p; b->lt = tilt;
    if (!b->started) { b->sx = x; b->sy = y; b->sp = p; b->started = true; }
    else { b->sx += (x - b->sx) * b->k; b->sy += (y - b->sy) * b->k; b->sp += (p - b->sp) * 0.45f; }
    emit(b, b->sx, b->sy, b->sp, tilt);
}

// thins the ends over ls (start) and le (end) document units, at most 45 % of the stroke each
static void apply_taper(SPt *pts, int n, float ls, float le) {
    if ((ls <= 0 && le <= 0) || n < 2) return;
    float *s = malloc(sizeof(float) * n);
    s[0] = 0;
    for (int i = 1; i < n; i++) s[i] = s[i - 1] + hypotf(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    float total = s[n - 1], Ls = MIN(ls, total * 0.45f), Le = MIN(le, total * 0.45f);
    for (int i = 0; i < n; i++) {
        float k = 1;
        if (Ls > 0) { float t = MIN(1, s[i] / Ls); k = MIN(k, t * t * (3 - 2 * t) * 0.92f + 0.08f); }
        if (Le > 0) { float t = MIN(1, (total - s[i]) / Le); k = MIN(k, t * t * (3 - 2 * t) * 0.95f + 0.05f); }
        pts[i].r *= k;
    }
    free(s);
}

void stroke_set_image(Stroke *s, const BrushOpts *o) {
    s->img = o->img_mode && o->img ? o->img : 0;
    s->img_mode = s->img ? o->img_mode : IMG_NONE;
    s->img_flags = o->img_flags;
    s->img_gap = MAX(0.5f, o->img_spacing * o->size);
    s->img_r0 = o->size / 2 * (o->pressure ? curve_eval(&o->curve, 0) : 1);
    s->img_r1 = o->size / 2 * (o->pressure ? curve_eval(&o->curve, 1) : 1);
    s->img_w0 = o->pressure ? o->img_w0 : o->img_w1;
    s->img_w1 = o->img_w1;
    s->dual_img = o->dual;
    s->dual_scale = o->dual_scale > 0 ? o->dual_scale : 1;
    s->dual_gap = MAX(0.5f, o->dual_spacing * o->size * s->dual_scale);
    s->dual_angle = o->dual_angle;
    s->dual_flags = o->dual_flags;
}

// how wide the image is across at a point of radius r (pressure gives both its size and its width)
static float img_width(const Stroke *st, float r) {
    if (st->img_w0 <= 0 && st->img_w1 <= 0) return 1;
    float d = st->img_r1 - st->img_r0;
    float t = fabsf(d) < 1e-5f ? 1 : clampf((r - st->img_r0) / d, 0, 1);
    return MAX(0.02f, lerpf(st->img_w0, st->img_w1, t));
}

void stroke_set_texture(Stroke *s, int bitmap, float density, float scale) {
    s->texture = bitmap > 0;
    s->sym = bitmap > 0 ? bitmap : 0;
    s->alpha = density;                          // strokes keep the grain density here
    s->m = mat_scale(scale > 0 ? scale : 1, scale > 0 ? scale : 1);   // texture pixels -> drawing, anchored to the canvas
}

static SPt *scratch;
void bb_live(Builder *b, Stroke *out) {
    arrsetlen(scratch, 0);
    if (arrlen(b->pts)) memcpy(arraddnptr(scratch, arrlen(b->pts)), b->pts, sizeof(SPt) * arrlen(b->pts));
    apply_taper(scratch, (int)arrlen(scratch), b->o.taper_s * b->o.size, 0);
    memset(out, 0, sizeof(*out));
    out->pts = scratch;
    out->color = b->o.color; out->tip = b->o.tip; out->angle = b->o.angle; out->aspect = b->o.aspect; out->erase = b->o.erase;
    if (!b->o.erase) { stroke_set_texture(out, b->o.tex, b->o.tex_density, b->o.tex_scale); stroke_set_image(out, &b->o); }
    stroke_bounds(out);
}

// Douglas-Peucker over (x, y, r): a sparse, clean curve that keeps width changes
static SPt *simplify(const SPt *p, int n, float eps) {
    SPt *out = NULL;
    if (n <= 2) { for (int i = 0; i < n; i++) arrput(out, p[i]); return out; }
    u8 *keep = calloc(n, 1);
    int *stack = malloc(sizeof(int) * 2 * n), sp = 0;
    keep[0] = keep[n - 1] = 1;
    stack[sp++] = 0; stack[sp++] = n - 1;
    while (sp) {
        int bi = stack[--sp], ai = stack[--sp];
        SPt a = p[ai], b = p[bi];
        float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
        if (L2 < 1e-12f) L2 = 1e-12f;
        float md = 0; int mi = -1;
        for (int i = ai + 1; i < bi; i++) {
            float t = clampf(((p[i].x - a.x) * dx + (p[i].y - a.y) * dy) / L2, 0, 1);
            float d = hypotf(p[i].x - (a.x + dx * t), p[i].y - (a.y + dy * t)) + fabsf(p[i].r - (a.r + (b.r - a.r) * t)) * 1.5f;
            if (d > md) { md = d; mi = i; }
        }
        if (md > eps && mi > 0) { keep[mi] = 1; stack[sp++] = ai; stack[sp++] = mi; stack[sp++] = mi; stack[sp++] = bi; }
    }
    for (int i = 0; i < n; i++) if (keep[i]) arrput(out, p[i]);
    free(keep); free(stack);
    return out;
}

Stroke bb_finish(Builder *b) {
    if (b->started) {
        // catch up from the stabilized point to where the pen really lifted
        for (int i = 1; i <= 6; i++) {
            float t = i / 6.0f;
            emit(b, b->sx + (b->lx - b->sx) * t, b->sy + (b->ly - b->sy) * t, b->sp + (b->lp - b->sp) * t, b->lt);
        }
    }
    int n = (int)arrlen(b->pts);
    apply_taper(b->pts, n, b->o.taper_s * b->o.size, b->o.taper_e * b->o.size);
    float eps = 0.35f * b->o.unit * (1 + b->o.fit / 40.0f);
    Stroke s = {0};
    s.pts = simplify(b->pts, n, eps);
    s.color = b->o.color; s.tip = b->o.tip; s.angle = b->o.angle; s.aspect = b->o.aspect; s.erase = b->o.erase;
    if (!b->o.erase) { stroke_set_texture(&s, b->o.tex, b->o.tex_density, b->o.tex_scale); stroke_set_image(&s, &b->o); }
    stroke_bounds(&s);
    bb_free(b);
    return s;
}

/* ── geometry ── */

static void support(const Stroke *st, float nx, float ny, float r, float *ox, float *oy) {
    if (st->tip == TIP_ROUND) { *ox = nx * r; *oy = ny * r; return; }
    float c = cosf(st->angle), s = sinf(st->angle);
    float du = nx * c + ny * s, dv = -nx * s + ny * c, pu, pv;
    if (st->tip == TIP_OVAL) {
        float a = r, bb = r * st->aspect, h = sqrtf(a * a * du * du + bb * bb * dv * dv);
        if (h < 1e-9f) h = 1e-9f;
        pu = a * a * du / h; pv = bb * bb * dv / h;
    } else { pu = du >= 0 ? r : -r; pv = dv >= 0 ? r : -r; }
    *ox = pu * c - pv * s; *oy = pu * s + pv * c;
}

typedef struct { DrawList *dl; Mat m; } Em;
static void tri(Em *e, float ax, float ay, float bx, float by, float cx, float cy) {
    V2 a = mat_ap(e->m, v2(ax, ay)), b = mat_ap(e->m, v2(bx, by)), c = mat_ap(e->m, v2(cx, cy));
    u32 i0 = dl_vtx_px(e->dl, a.x, a.y, 0), i1 = dl_vtx_px(e->dl, b.x, b.y, 0), i2 = dl_vtx_px(e->dl, c.x, c.y, 0);
    dl_idx(e->dl, i0, i1, i2);
}
static void tip_poly(Em *e, const Stroke *st, float x, float y, float r, float scale) {
    float rpx = r * scale;
    if (st->tip == TIP_SQUARE) {
        float c = cosf(st->angle), s = sinf(st->angle), q[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}, P[4][2];
        for (int i = 0; i < 4; i++) { P[i][0] = x + (q[i][0] * c - q[i][1] * s) * r; P[i][1] = y + (q[i][0] * s + q[i][1] * c) * r; }
        tri(e, P[0][0], P[0][1], P[1][0], P[1][1], P[2][0], P[2][1]);
        tri(e, P[0][0], P[0][1], P[2][0], P[2][1], P[3][0], P[3][1]);
        return;
    }
    int seg = (int)CLAMP(rpx * 0.9f, 8, 56);
    float b = st->tip == TIP_OVAL ? r * st->aspect : r, c = cosf(st->angle), s = sinf(st->angle);
    float px = 0, py = 0, fx = 0, fy = 0;
    for (int i = 0; i <= seg; i++) {
        float t = (float)i / seg * 2 * VX_PI, u = cosf(t) * r, v = sinf(t) * b;
        float qx = x + u * c - v * s, qy = y + u * s + v * c;
        if (i == 0) { fx = qx; fy = qy; } else if (i > 1) tri(e, fx, fy, px, py, qx, qy);
        px = qx; py = qy;
    }
}

// The centerline is Catmull-Rom through the nodes (straight segments for shapes). One node segment,
// sampled into k points (its end point excluded).
static int seg_count(const Stroke *st) {
    int n = (int)arrlen(st->pts);
    return n < 2 ? 0 : (st->flags & ITF_CLOSED) && n > 2 ? n : n - 1;
}
static void seg_controls(const Stroke *st, int i, SPt c[4]) {
    const SPt *p = st->pts;
    int n = (int)arrlen(st->pts);
    bool closed = (st->flags & ITF_CLOSED) && n > 2;
    for (int k = 0; k < 4; k++) {
        int j = i - 1 + k;
        c[k] = p[closed ? ((j % n) + n) % n : CLAMP(j, 0, n - 1)];
    }
}
static void sample_segment(const Stroke *st, int i, int k, SPt **out) {
    SPt c[4];
    seg_controls(st, i, c);
    SPt p0 = c[0], p1 = c[1], p2 = c[2], p3 = c[3];
    for (int j = 0; j < k; j++) {
        float t = (float)j / k, t2 = t * t, t3 = t2 * t;
        if (st->flags & ITF_LINEAR) { arrput(*out, ((SPt){p1.x + (p2.x - p1.x) * t, p1.y + (p2.y - p1.y) * t, p1.r + (p2.r - p1.r) * t})); continue; }
#define CR(a, b, c, e) (0.5f * (2 * (b) + (-(a) + (c)) * t + (2 * (a) - 5 * (b) + 4 * (c) - (e)) * t2 + (-(a) + 3 * (b) - 3 * (c) + (e)) * t3))
        arrput(*out, ((SPt){CR(p0.x, p1.x, p2.x, p3.x), CR(p0.y, p1.y, p2.y, p3.y), p1.r + (p2.r - p1.r) * t}));
#undef CR
    }
}
static SPt seg_end(const Stroke *st, int i) { SPt c[4]; seg_controls(st, i, c); return c[2]; }

// Centerline sampled densely (scratch array, valid until the next call)
static SPt *resampled;
SPt *brush_resample(const Stroke *st, float step) {
    arrsetlen(resampled, 0);
    int n = (int)arrlen(st->pts), segs = seg_count(st);
    if (!segs) { for (int i = 0; i < n; i++) arrput(resampled, st->pts[i]); return resampled; }
    for (int i = 0; i < segs; i++) {
        SPt c[4];
        seg_controls(st, i, c);
        sample_segment(st, i, MAX(1, (int)ceilf(hypotf(c[2].x - c[1].x, c[2].y - c[1].y) / step)), &resampled);
    }
    arrput(resampled, seg_end(st, segs - 1));
    return resampled;
}

static BrushTexFn tex_fn;
void brush_texture_source(BrushTexFn fn) { tex_fn = fn; }

static int cover_kind; static float cover_k1;
void brush_cover_style(int kind, float k1) { cover_kind = kind; cover_k1 = k1; }

// a union: several items stencilled, then covered once (overlaps are shaded a single time)
static bool cover_defer; static float ux0, uy0, ux1, uy1;
void brush_union_begin(void) { cover_defer = true; ux0 = uy0 = 1e30f; ux1 = uy1 = -1e30f; }
void brush_union_end(DrawList *dl, Color color) {
    cover_defer = false;
    if (ux1 < ux0) return;
    dl_set_pipe(dl, PIPE_COVER);
    u32 i0 = dl_vtx_k(dl, ux0, uy0, color, cover_kind, cover_k1), i1 = dl_vtx_k(dl, ux1, uy0, color, cover_kind, cover_k1);
    u32 i2 = dl_vtx_k(dl, ux1, uy1, color, cover_kind, cover_k1), i3 = dl_vtx_k(dl, ux0, uy1, color, cover_kind, cover_k1);
    dl_idx(dl, i0, i1, i2); dl_idx(dl, i0, i2, i3);
}

static void cover(DrawList *dl, const Item *it, Mat m, Color color, u8 pipe, float pad_doc) {
    V2 c[4] = {mat_ap(m, v2(it->bx0 - pad_doc, it->by0 - pad_doc)), mat_ap(m, v2(it->bx1 + pad_doc, it->by0 - pad_doc)),
               mat_ap(m, v2(it->bx1 + pad_doc, it->by1 + pad_doc)), mat_ap(m, v2(it->bx0 - pad_doc, it->by1 + pad_doc))};
    if (cover_defer) {
        for (int i = 0; i < 4; i++) { ux0 = MIN(ux0, c[i].x); uy0 = MIN(uy0, c[i].y); ux1 = MAX(ux1, c[i].x); uy1 = MAX(uy1, c[i].y); }
        return;
    }
    dl_set_pipe(dl, pipe);
    u32 i0 = dl_vtx_k(dl, c[0].x, c[0].y, color, cover_kind, cover_k1), i1 = dl_vtx_k(dl, c[1].x, c[1].y, color, cover_kind, cover_k1);
    u32 i2 = dl_vtx_k(dl, c[2].x, c[2].y, color, cover_kind, cover_k1), i3 = dl_vtx_k(dl, c[3].x, c[3].y, color, cover_kind, cover_k1);
    dl_idx(dl, i0, i1, i2); dl_idx(dl, i0, i2, i3);
}

// the stencilled area covered with a texture (uvm: drawing space -> texture coordinates)
static void tex_cover(DrawList *dl, const Item *it, Mat m, float pad, u32 tex, Mat uvm, Color tint, u8 smp, int kind, const float p[4]) {
    V2 q[4] = {v2(it->bx0 - pad, it->by0 - pad), v2(it->bx1 + pad, it->by0 - pad), v2(it->bx1 + pad, it->by1 + pad), v2(it->bx0 - pad, it->by1 + pad)};
    u32 old = dl->tex;
    dl_set_tex(dl, tex);
    dl_set_sampler(dl, smp);
    dl_set_pipe(dl, PIPE_COVER);
    u32 v0 = 0;
    static const float zero[4] = {0};
    const float *p4 = p ? p : zero;
    for (int i = 0; i < 4; i++) {
        V2 s = mat_ap(m, q[i]), uv = mat_ap(uvm, q[i]);
        u32 v = dl_vtx_uvp(dl, s.x, s.y, uv.x, uv.y, tint, kind, p4);
        if (!i) v0 = v;
    }
    dl_idx(dl, v0, v0 + 1, v0 + 2); dl_idx(dl, v0, v0 + 2, v0 + 3);
    dl_set_sampler(dl, SMP_LINEAR);
    dl_set_tex(dl, old);
}

/* ── visibility: geometry is only built for what can reach the target ── */

static Rect view_px(DrawList *dl) {
    Rect c = dl_clip(dl);
    float s = dl->scale, m = 4;
    return rect(c.x * s - m, c.y * s - m, c.w * s + 2 * m, c.h * s + 2 * m);
}
static bool box_visible(Mat m, float x0, float y0, float x1, float y1, Rect v) {
    V2 c[4] = {mat_ap(m, v2(x0, y0)), mat_ap(m, v2(x1, y0)), mat_ap(m, v2(x1, y1)), mat_ap(m, v2(x0, y1))};
    float ax = c[0].x, ay = c[0].y, bx = ax, by = ay;
    for (int i = 1; i < 4; i++) { ax = MIN(ax, c[i].x); ay = MIN(ay, c[i].y); bx = MAX(bx, c[i].x); by = MAX(by, c[i].y); }
    return bx >= v.x && ax <= v.x + v.w && by >= v.y && ay <= v.y + v.h;
}

/* ── swept area of the tip along a run of samples ──
   Quads between consecutive samples (tangent to both discs for round tips), small fans that close the
   wedge between neighbouring quads on each side, and full tips only at the ends of the run. Stamping the
   whole tip at every sample used to cost millions of pixels per stroke when zoomed in. */

static void join(Em *e, const Stroke *st, SPt c, V2 a, V2 b, float rpx) {
    if ((a.x == b.x && a.y == b.y) || c.r <= 0.001f) return;
    float ang = atan2f(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
    int steps = (int)CLAMP(ceilf(fabsf(ang) * sqrtf(MAX(rpx, 1.0f)) * 0.6f), 1, 32);
    float a0 = atan2f(a.y, a.x), ox, oy;
    support(st, a.x, a.y, c.r, &ox, &oy);
    float px = c.x + ox, py = c.y + oy;
    for (int s = 1; s <= steps; s++) {
        // the end points are the neighbouring pieces' own vertices, bit for bit, so the edges meet exactly
        if (s == steps) support(st, b.x, b.y, c.r, &ox, &oy);
        else { float t = a0 + ang * s / steps; support(st, cosf(t), sinf(t), c.r, &ox, &oy); }
        float qx = c.x + ox, qy = c.y + oy;
        tri(e, c.x, c.y, px, py, qx, qy);
        px = qx; py = qy;
    }
}

static void draw_run(Em *e, const Stroke *st, const SPt *p, int n, float scale) {
    if (n <= 0) return;
    if (p[0].r > 0.01f) tip_poly(e, st, p[0].x, p[0].y, p[0].r, scale);
    if (n > 1 && p[n - 1].r > 0.01f) tip_poly(e, st, p[n - 1].x, p[n - 1].y, p[n - 1].r, scale);
    V2 pa = {0}, pb = {0};
    bool have = false;
    for (int i = 0, j = 0; i + 1 < n; i++) {
        SPt s0 = p[j], s1 = p[i + 1];   // s0: end of the last piece drawn (a too-short piece is merged into the next)
        float dx = s1.x - s0.x, dy = s1.y - s0.y, d = hypotf(dx, dy);
        if (d < 1e-6f) continue;
        j = i + 1;
        float ux = dx / d, uy = dy / d, nx = -uy, ny = ux;
        V2 A, B;   // side directions: where the tip touches the envelope on each side
        if (st->tip == TIP_ROUND) {
            float sa = clampf((s0.r - s1.r) / d, -1, 1), ca = sqrtf(1 - sa * sa);
            A = v2(nx * ca + ux * sa, ny * ca + uy * sa); B = v2(-nx * ca + ux * sa, -ny * ca + uy * sa);
        } else { A = v2(nx, ny); B = v2(-nx, -ny); }
        float ax, ay, bx, by, cx, cy, ex, ey;
        support(st, A.x, A.y, s0.r, &ax, &ay); support(st, A.x, A.y, s1.r, &bx, &by);
        support(st, B.x, B.y, s1.r, &cx, &cy); support(st, B.x, B.y, s0.r, &ex, &ey);
        // fanned from the centres so the joins and the next piece share these exact vertices (no cracks)
        tri(e, s0.x, s0.y, s0.x + ax, s0.y + ay, s1.x + bx, s1.y + by);
        tri(e, s0.x, s0.y, s1.x + bx, s1.y + by, s1.x, s1.y);
        tri(e, s0.x, s0.y, s1.x, s1.y, s1.x + cx, s1.y + cy);
        tri(e, s0.x, s0.y, s1.x + cx, s1.y + cy, s0.x + ex, s0.y + ey);
        if (have) { float rpx = s0.r * scale; join(e, st, s0, pa, A, rpx); join(e, st, s0, pb, B, rpx); }
        // the radius changes faster than the centerline moves: no tangent, so cover the bigger tip
        if (fabsf(s0.r - s1.r) > 0.7f * d) { SPt big = s0.r > s1.r ? s0 : s1; tip_poly(e, st, big.x, big.y, big.r, scale); }
        pa = A; pb = B; have = true;
    }
}

/* ── image strokes: the tip image stamped along the centerline, or stretched over all of it ── */

static u8 img_pipe = PIPE_NORMAL;
void brush_image_pipe(u8 pipe) { img_pipe = pipe; }

static void img_vtx(DrawList *dl, Mat m, V2 p, float u, float v, Color c, u32 *first) {
    static const float zero[4] = {0};
    V2 s = mat_ap(m, p);
    u32 i = dl_vtx_uvp(dl, s.x, s.y, u, v, c, K_TIPMASK, zero);
    if (first) *first = i;
}

static void image_draw(DrawList *dl, const Stroke *st, Mat m, Color color, u32 view, int tw, int th, float rs) {
    float scale = mat_scalef(m);
    float rmax = 0;
    for (ptrdiff_t i = 0; i < arrlen(st->pts); i++) rmax = MAX(rmax, st->pts[i].r * rs);
    if (rmax <= 0) return;
    float aspect = (float)th / tw;   // the image lies with its width along the stroke
    bool fx = st->img_flags & IMGF_FLIP_X, fy = st->img_flags & IMGF_FLIP_Y;
    float u0 = fx ? 1 : 0, u1 = fx ? 0 : 1, v0 = fy ? 1 : 0, v1 = fy ? 0 : 1;
    u32 old = dl->tex;
    dl_set_tex(dl, view);
    dl_set_sampler(dl, SMP_MIP);
    dl_set_pipe(dl, img_pipe);
    float step = st->img_mode == IMG_STAMPS ? MAX(0.3f / scale, MIN(st->img_gap / 10, rmax / 2)) : MAX(0.25f, 1.5f / scale);
    SPt *p = brush_resample(st, step);
    int n = (int)arrlen(p);
    if (st->img_mode == IMG_STAMPS) {
        // one at the start, then one every img_gap along the line, turned with it (plus the tip angle)
        float ca = cosf(st->angle), sa = sinf(st->angle);
        float since = 1e30f;   // distance walked since the last stamp
        float R = MAX(MAX(st->img_r0, st->img_r1), 1e-3f);
        for (int i = 0; i < MAX(n, 1); i++) {
            SPt a = n ? p[i] : st->pts[0];
            V2 d = {1, 0};
            if (n > 1) {
                SPt b = p[MIN(i + 1, n - 1)], c = p[MAX(i - 1, 0)];
                float dx = b.x - c.x, dy = b.y - c.y, L = hypotf(dx, dy);
                if (L > 1e-6f) d = v2(dx / L, dy / L);
            }
            if (i) since += hypotf(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
            float gap = st->img_r1 > 0 || st->img_r0 > 0 ? st->img_gap * clampf(a.r / R, 0.03f, 1) : st->img_gap;
            if (since < MAX(gap, 0.3f / scale)) continue;
            since = 0;
            if (st->img_flags & IMGF_FIXED) d = v2(1, 0);
            V2 ax = {d.x * ca - d.y * sa, d.x * sa + d.y * ca}, ay = {-ax.y, ax.x};
            float hl = a.r * rs, hs = a.r * rs * aspect * img_width(st, a.r);   // half length, half width
            V2 c = {a.x, a.y};
            u32 q;
            img_vtx(dl, m, v2(c.x - ax.x * hl - ay.x * hs, c.y - ax.y * hl - ay.y * hs), u0, v0, color, &q);
            img_vtx(dl, m, v2(c.x + ax.x * hl - ay.x * hs, c.y + ax.y * hl - ay.y * hs), u1, v0, color, NULL);
            img_vtx(dl, m, v2(c.x + ax.x * hl + ay.x * hs, c.y + ax.y * hl + ay.y * hs), u1, v1, color, NULL);
            img_vtx(dl, m, v2(c.x - ax.x * hl + ay.x * hs, c.y - ax.y * hl + ay.y * hs), u0, v1, color, NULL);
            dl_idx(dl, q, q + 1, q + 2); dl_idx(dl, q, q + 2, q + 3);
        }
    } else if (n > 1) {
        // a strip: the image's width over the whole length, its height across the stroke
        float L = 0;
        for (int i = 1; i < n; i++) L += hypotf(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
        if (L <= 1e-6f) goto done;
        float s = 0;
        u32 q0 = 0;
        for (int i = 0; i < n; i++) {
            if (i) s += hypotf(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
            SPt b = p[MIN(i + 1, n - 1)], c = p[MAX(i - 1, 0)];
            float dx = b.x - c.x, dy = b.y - c.y, dl2 = MAX(1e-6f, hypotf(dx, dy));
            V2 nrm = {-dy / dl2, dx / dl2};
            float u = lerpf(u0, u1, s / L), w = p[i].r * img_width(st, p[i].r);
            u32 q;
            img_vtx(dl, m, v2(p[i].x - nrm.x * w, p[i].y - nrm.y * w), u, v0, color, &q);
            img_vtx(dl, m, v2(p[i].x + nrm.x * w, p[i].y + nrm.y * w), u, v1, color, NULL);
            if (i) dl_idx(dl, q0, q0 + 1, q + 1), dl_idx(dl, q0, q + 1, q);
            q0 = q;
        }
    }
done:
    dl_set_sampler(dl, SMP_LINEAR);
    dl_set_tex(dl, old);
}

bool brush_draw_dual(DrawList *dl, const Stroke *st, Mat m) {
    int tw = 0, th = 0;
    u32 view = st->dual_img && tex_fn ? tex_fn(st->dual_img, &tw, &th) : 0;
    if (!view || tw <= 0 || th <= 0 || !arrlen(st->pts)) return false;
    Stroke d = *st;   // the same line, stamped with the second image
    d.img_mode = IMG_STAMPS; d.img_gap = st->dual_gap; d.angle = st->dual_angle; d.img_flags = st->dual_flags;
    d.img_w0 = d.img_w1 = 0;
    image_draw(dl, &d, m, 0xffffffffu, view, tw, th, st->dual_scale > 0 ? st->dual_scale : 1);
    return true;
}

void brush_draw(DrawList *dl, const Stroke *st, Mat m, Color color, bool erase) {
    int n = (int)arrlen(st->pts);
    if (!n) return;
    float scale = mat_scalef(m);
    if (scale < 1e-6f) return;
    Rect v = view_px(dl);
    float ipad = 0;
    if (st->img_mode && st->img) for (int i = 0; i < n; i++) ipad = MAX(ipad, st->pts[i].r);   // stamps reach past the swept outline
    if (!box_visible(m, st->bx0 - ipad, st->by0 - ipad, st->bx1 + ipad, st->by1 + ipad, v)) return;
    if (st->img_mode && st->img && !erase && cover_kind == K_SOLID && !cover_defer && tex_fn) {
        int tw = 0, th = 0;
        u32 view = tex_fn(st->img, &tw, &th);
        if (view && tw > 0 && th > 0) { image_draw(dl, st, m, color, view, tw, th, 1); return; }
    }
    Em e = {dl, m};
    dl_set_pipe(dl, PIPE_STENCIL);
    static SPt *run;
    arrsetlen(run, 0);
    int segs = seg_count(st);
    if (!segs) { if (st->pts[0].r > 0.01f) tip_poly(&e, st, st->pts[0].x, st->pts[0].y, st->pts[0].r, scale); }
    for (int i = 0; i < segs; i++) {
        SPt c[4];
        seg_controls(st, i, c);
        // conservative box of this piece: its Catmull-Rom controls, the widest tip, some overshoot
        float x0 = c[0].x, y0 = c[0].y, x1 = x0, y1 = y0, r = 0;
        for (int k = 0; k < 4; k++) { x0 = MIN(x0, c[k].x); y0 = MIN(y0, c[k].y); x1 = MAX(x1, c[k].x); y1 = MAX(y1, c[k].y); r = MAX(r, c[k].r); }
        float pad = r * 1.42f + 0.2f * MAX(x1 - x0, y1 - y0);
        if (box_visible(m, x0 - pad, y0 - pad, x1 + pad, y1 + pad, v)) {
            float d = hypotf(c[2].x - c[1].x, c[2].y - c[1].y) * scale;
            bool flat = (st->flags & ITF_LINEAR) && fabsf(c[1].r - c[2].r) < 1e-4f;
            sample_segment(st, i, flat ? 1 : (int)CLAMP(ceilf(d / 0.75f), 1, 128), &run);
        } else if (arrlen(run)) {
            arrput(run, c[1]);
            draw_run(&e, st, run, (int)arrlen(run), scale);
            arrsetlen(run, 0);
        }
    }
    if (arrlen(run)) {
        arrput(run, seg_end(st, segs - 1));
        draw_run(&e, st, run, (int)arrlen(run), scale);
    }
    int tw = 0, th = 0;
    u32 view = st->texture && st->sym && !erase && cover_kind == K_SOLID && !cover_defer && tex_fn ? tex_fn(st->sym, &tw, &th) : 0;
    if (view && tw > 0 && th > 0) {   // the ink with the texture's grain
        float p[4] = {clampf(st->alpha, 0, 1), 0, 0, 0};
        Mat uvm = mat_mul(mat_scale(1.0f / tw, 1.0f / th), mat_inv(st->m));
        tex_cover(dl, st, m, 2.0f / scale, view, uvm, color, SMP_REPEAT, K_GRAIN, p);
        return;
    }
    cover(dl, st, m, color, erase ? PIPE_ERASE : PIPE_COVER, 2.0f / scale);
}

static void fill_stencil(DrawList *dl, const Item *it, Mat m) {
    int n = (int)arrlen(it->poly);
    Em e = {dl, m};
    dl_set_pipe(dl, PIPE_STENCIL_EO);
    int nc = (int)arrlen(it->starts);
    for (int c = 0; c < nc; c++) {
        int a = it->starts[c], b = c + 1 < nc ? it->starts[c + 1] : n;
        if (b - a < 3) continue;
        V2 o = it->poly[a];
        for (int i = a + 1; i + 1 < b; i++) tri(&e, o.x, o.y, it->poly[i].x, it->poly[i].y, it->poly[i + 1].x, it->poly[i + 1].y);
    }
}
void fill_draw_tex(DrawList *dl, const Item *it, Mat m, u32 tex, Mat uvm, Color tint, u8 smp, int kind, const float p[4]) {
    if (arrlen(it->poly) < 3 || !box_visible(m, it->bx0, it->by0, it->bx1, it->by1, view_px(dl))) return;
    fill_stencil(dl, it, m);
    tex_cover(dl, it, m, 2.0f / MAX(1e-6f, mat_scalef(m)), tex, uvm, tint, smp, kind, p);
}

// Fill: contours drawn as triangle fans into the stencil with even-odd parity, then covered.
void fill_draw(DrawList *dl, const Item *it, Mat m, Color color, bool erase) {
    if (arrlen(it->poly) < 3 || !box_visible(m, it->bx0, it->by0, it->bx1, it->by1, view_px(dl))) return;
    fill_stencil(dl, it, m);
    cover(dl, it, m, color, erase ? PIPE_ERASE : PIPE_COVER, 2.0f / MAX(1e-6f, mat_scalef(m)));
}
