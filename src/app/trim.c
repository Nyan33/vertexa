// Trim Edge: removes the part of a stroke between its nearest intersections with other strokes
// (or with itself). Click a segment, or drag across several. A stroke that crosses nothing is
// removed whole. Intersections are taken on the centerlines, so the cut ends tuck under the
// crossing line.
#include "app/app.h"
#include "geom/geom.h"
#include "render/scene.h"

typedef struct {
    bool valid;
    int item;            // index in the active drawing
    float a, b;          // arc-length range that would be removed
    Path path;
    bool closed;
} TrimHit;

static TrimHit hover;
static bool dragging;
static V2 last_local;
static int trimmed_count;

static float path_step(void) { return MAX(0.4f, 1.5f * st_unit()); }

static void add_cut(float **cuts, float s) { arrput(*cuts, s); }

static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

// all places where the centerline of `self` is crossed: by other strokes and by itself
static float *collect_cuts(const Path *P, const Item *self, bool all_layers) {
    float *cuts = NULL;
    int n = (int)arrlen(P->p);
    // itself (non-adjacent segments)
    for (int i = 0; i + 1 < n; i++) {
        V2 a = v2(P->p[i].x, P->p[i].y), b = v2(P->p[i + 1].x, P->p[i + 1].y);
        for (int j = i + 2; j + 1 < n; j++) {
            if (self->flags & ITF_CLOSED && i == 0 && j == n - 2) continue;
            V2 c = v2(P->p[j].x, P->p[j].y), dd = v2(P->p[j + 1].x, P->p[j + 1].y);
            if (MAX(a.x, b.x) < MIN(c.x, dd.x) || MAX(c.x, dd.x) < MIN(a.x, b.x) || MAX(a.y, b.y) < MIN(c.y, dd.y) || MAX(c.y, dd.y) < MIN(a.y, b.y)) continue;
            float t, u;
            if (seg_isect(a, b, c, dd, &t, &u)) {
                add_cut(&cuts, P->s[i] + (P->s[i + 1] - P->s[i]) * t);
                add_cut(&cuts, P->s[j] + (P->s[j + 1] - P->s[j]) * u);
            }
        }
    }
    // other strokes: the active drawing, or every visible drawing at this frame
    Doc *d = app.doc;
    Timeline *tl = doc_tl(d);
    Drawing *mine = st_drawing(false);
    for (ptrdiff_t li = 0; li < arrlen(tl->layers); li++) {
        Layer *l = &tl->layers[li];
        Drawing *dr = doc_drawing_at(d, l, tl->frame);
        if (!dr || (!all_layers && dr != mine) || doc_hidden(d, l)) continue;
        for (ptrdiff_t k = 0; k < arrlen(dr->items); k++) {
            Item *o = &dr->items[k];
            if (o == self || o->kind != IT_STROKE || o->erase) continue;
            if (o->bx1 < self->bx0 || o->bx0 > self->bx1 || o->by1 < self->by0 || o->by0 > self->by1) continue;
            Path Q = path_of(o, path_step());
            int m = (int)arrlen(Q.p);
            for (int i = 0; i + 1 < n; i++) {
                V2 a = v2(P->p[i].x, P->p[i].y), b = v2(P->p[i + 1].x, P->p[i + 1].y);
                float x0 = MIN(a.x, b.x), x1 = MAX(a.x, b.x), y0 = MIN(a.y, b.y), y1 = MAX(a.y, b.y);
                if (x1 < o->bx0 || x0 > o->bx1 || y1 < o->by0 || y0 > o->by1) continue;
                for (int j = 0; j + 1 < m; j++) {
                    V2 c = v2(Q.p[j].x, Q.p[j].y), dd = v2(Q.p[j + 1].x, Q.p[j + 1].y);
                    if (x1 < MIN(c.x, dd.x) || MAX(c.x, dd.x) < x0 || y1 < MIN(c.y, dd.y) || MAX(c.y, dd.y) < y0) continue;
                    float t;
                    if (seg_isect(a, b, c, dd, &t, NULL)) add_cut(&cuts, P->s[i] + (P->s[i + 1] - P->s[i]) * t);
                }
            }
            path_free(&Q);
        }
    }
    if (arrlen(cuts)) qsort(cuts, arrlen(cuts), sizeof(float), cmp_float);
    return cuts;
}

// the piece around arc length u: between the nearest cuts on either side
static void range_around(const Path *P, const float *cuts, bool closed, float u, float *a, float *b) {
    int n = (int)arrlen(cuts);
    float eps = 0.01f;
    *a = closed && n ? cuts[n - 1] - P->len : 0;   // wraps around for closed strokes
    *b = closed && n ? cuts[0] + P->len : P->len;
    for (int i = 0; i < n; i++) {
        if (cuts[i] < u - eps) *a = cuts[i];
        else if (cuts[i] > u + eps) { *b = cuts[i]; break; }
    }
}

static void hit_item(Item *it, int k, V2 p, TrimHit *h) {
    h->path = path_of(it, path_step());
    h->closed = (it->flags & ITF_CLOSED) != 0;
    float u = path_project(&h->path, p, NULL);
    h->item = k;
    if (app.trim_mode == TRIM_WHOLE) { h->a = 0; h->b = h->path.len; }
    else {
        float *cuts = collect_cuts(&h->path, it, app.trim_all_layers);
        range_around(&h->path, cuts, h->closed, u, &h->a, &h->b);
        if (h->closed && !arrlen(cuts)) { h->a = 0; h->b = h->path.len; }
        arrfree(cuts);
    }
    h->valid = true;
}

static bool find_hit(V2 p, float tol, TrimHit *h) {
    Drawing *dr = st_drawing(false);
    if (!dr) return false;
    for (ptrdiff_t k = arrlen(dr->items) - 1; k >= 0; k--) {
        Item *it = &dr->items[k];
        if (it->kind != IT_STROKE || it->erase || !item_hit(it, p, tol)) continue;
        hit_item(it, (int)k, p, h);
        return true;
    }
    return false;
}

static void hit_free(TrimHit *h) { if (h->valid) path_free(&h->path); h->valid = false; }

// dense points of the path between arc lengths s0 and s1
static void piece_points(const Path *P, float s0, float s1, SPt **out) {
    arrput(*out, path_at(P, s0));
    for (ptrdiff_t i = 0; i < arrlen(P->p); i++) if (P->s[i] > s0 && P->s[i] < s1) arrput(*out, P->p[i]);
    arrput(*out, path_at(P, s1));
}

static void apply(TrimHit *h) {
    Drawing *dr = st_drawing(false);
    if (!dr || h->item >= arrlen(dr->items)) return;
    Item style = dr->items[h->item];
    float L = h->path.len, minlen = 0.5f * st_unit();
    Item pieces[2]; int np = 0;
    float eps = 0.12f * st_unit();
    if (h->b - h->a < L - 1e-3f) {
        if (h->closed) {
            // a closed outline opens up: one piece from b around to a
            SPt *pts = NULL;
            float a = h->a < 0 ? h->a + L : h->a, b = h->b > L ? h->b - L : h->b;
            if (b <= a) { piece_points(&h->path, b, a, &pts); }
            else { piece_points(&h->path, b, L, &pts); SPt *tail = NULL; piece_points(&h->path, 0, a, &tail); for (ptrdiff_t i = 1; i < arrlen(tail); i++) arrput(pts, tail[i]); arrfree(tail); }
            if (arrlen(pts) > 1) pieces[np++] = stroke_from_dense(pts, (int)arrlen(pts), &style, eps);
            arrfree(pts);
        } else {
            if (h->a > minlen) { SPt *pts = NULL; piece_points(&h->path, 0, h->a, &pts); pieces[np++] = stroke_from_dense(pts, (int)arrlen(pts), &style, eps); arrfree(pts); }
            if (L - h->b > minlen) { SPt *pts = NULL; piece_points(&h->path, h->b, L, &pts); pieces[np++] = stroke_from_dense(pts, (int)arrlen(pts), &style, eps); arrfree(pts); }
        }
    }
    item_free(&dr->items[h->item]);
    arrdel(dr->items, h->item);
    for (int i = np - 1; i >= 0; i--) arrins(dr->items, h->item, pieces[i]);
    doc_touch(app.doc);
    trimmed_count++;
}

// segment the pointer moved along crosses a stroke centerline -> trim that piece
static void trim_crossings(V2 a, V2 b) {
    Drawing *dr = st_drawing(false);
    if (!dr) return;
    for (int guard = 0; guard < 64; guard++) {
        bool any = false;
        for (ptrdiff_t k = arrlen(dr->items) - 1; k >= 0 && !any; k--) {
            Item *it = &dr->items[k];
            if (it->kind != IT_STROKE || it->erase) continue;
            if (MAX(a.x, b.x) < it->bx0 || MIN(a.x, b.x) > it->bx1 || MAX(a.y, b.y) < it->by0 || MIN(a.y, b.y) > it->by1) continue;
            Path P = path_of(it, path_step());
            for (ptrdiff_t i = 0; i + 1 < arrlen(P.p) && !any; i++) {
                float t;
                if (seg_isect(a, b, v2(P.p[i].x, P.p[i].y), v2(P.p[i + 1].x, P.p[i + 1].y), NULL, &t)) {
                    V2 x = v2(lerpf(P.p[i].x, P.p[i + 1].x, t), lerpf(P.p[i].y, P.p[i + 1].y, t));
                    TrimHit h = {0};
                    hit_item(it, (int)k, x, &h);
                    path_free(&P);
                    apply(&h);
                    hit_free(&h);
                    any = true;
                    break;
                }
            }
            if (!any) path_free(&P);
        }
        if (!any) break;
        dr = st_drawing(false);
        if (!dr) break;
    }
}

void trim_tool(UiResp *re) {
    hit_free(&hover);
    V2 p = st_local(ui.in.mouse);
    if (re->pressed) {
        const char *why = st_cannot_edit();
        if (why) { app_toast("%s", why); return; }
        app_commit();
        trimmed_count = 0;
        dragging = true;
        last_local = p;
        TrimHit h = {0};
        if (find_hit(p, 3 * st_unit(), &h)) apply(&h);
        hit_free(&h);
        return;
    }
    if (dragging) {
        if (re->held) {
            if (hypotf(p.x - last_local.x, p.y - last_local.y) > 0.5f * st_unit()) { trim_crossings(last_local, p); last_local = p; }
        } else {
            dragging = false;
            if (!trimmed_count) app_toast("Nothing to trim here. Click or drag across a stroke");
        }
        return;
    }
    if (re->hover && !st_cannot_edit()) find_hit(p, 3 * st_unit(), &hover);
}

void trim_overlay(void) {
    if (!hover.valid || dragging) return;
    Drawing *dr = st_drawing(false);
    if (!dr || hover.item >= arrlen(dr->items)) return;
    float z = 1.0f / st_unit();
    Path *P = &hover.path;
    float a = hover.a, b = hover.b, L = P->len;
    // draw [a, b], wrapping for closed strokes
    float ranges[2][2]; int nr = 0;
    if (a < 0) { ranges[nr][0] = a + L; ranges[nr][1] = L; nr++; a = 0; }
    if (b > L) { ranges[nr][0] = 0; ranges[nr][1] = b - L; nr++; b = L; }
    ranges[nr][0] = a; ranges[nr][1] = b; nr++;
    for (int r = 0; r < nr; r++) {
        float s0 = ranges[r][0], s1 = ranges[r][1];
        int steps = MAX(2, (int)((s1 - s0) * z / 3));
        V2 prev = st_screen(v2(path_at(P, s0).x, path_at(P, s0).y));
        for (int i = 1; i <= steps; i++) {
            SPt q = path_at(P, s0 + (s1 - s0) * i / steps);
            V2 cur = st_screen(v2(q.x, q.y));
            dl_line(ui.dl, prev, cur, MAX(3.0f, q.r * 2 * z * 0.7f), col_a(C_AC, 0x9c));
            prev = cur;
        }
    }
    if (hover.b - hover.a < L - 1e-3f) {
        for (int e = 0; e < 2; e++) {
            float s = e ? hover.b : hover.a;
            if (s < 0) s += L;
            if (s > L) s -= L;
            SPt q = path_at(P, s);
            V2 c = st_screen(v2(q.x, q.y));
            dl_circle(ui.dl, c, 4, hex(0xffffff));
            dl_circle_border(ui.dl, c, 4, 1.5f, C_AC);
        }
    }
}

/* ── shared with the Selection tool: sections of strokes between their intersections ── */

float cut_step(void) { return path_step(); }

bool stroke_section_at(const Path *P, const Item *it, V2 p, float *a, float *b) {
    bool closed = (it->flags & ITF_CLOSED) != 0;
    float u = path_project(P, p, NULL);
    float *cuts = collect_cuts(P, it, false);
    range_around(P, cuts, closed, u, a, b);
    bool part = arrlen(cuts) > 0 && *b - *a < P->len - 1e-3f;
    arrfree(cuts);
    return part;
}

Item stroke_piece(const Path *P, const Item *it, float a, float b) {
    float L = P->len;
    if (L <= 0) return item_copy(it);
    float shift = floorf(a / L) * L;   // closed strokes: ranges may wrap past the start
    a -= shift; b -= shift;
    SPt *pts = NULL;
    if (b <= L + 1e-4f) piece_points(P, a, MIN(b, L), &pts);
    else {
        piece_points(P, a, L, &pts);
        SPt *tail = NULL;
        piece_points(P, 0, MIN(b - L, L), &tail);
        for (ptrdiff_t i = 1; i < arrlen(tail); i++) arrput(pts, tail[i]);
        arrfree(tail);
    }
    Item o = stroke_from_dense(pts, (int)arrlen(pts), it, 0.12f * st_unit());
    arrfree(pts);
    return o;
}
