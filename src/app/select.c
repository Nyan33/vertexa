// Selection (V), Subselection (A), Free Transform (Q) and Lasso (L).
// As in Umoupen and Flash, the vectors themselves are selected and shown with a hatch, not boxed in:
//  * a click picks the stroke under the pointer, or in Sections mode only its part between intersections;
//  * a drag box or the lasso cuts strokes at its edge and picks the parts inside (or whole touched strokes);
//  * picked sections become strokes of their own only when they are moved, transformed or deleted,
//    so just clicking around never changes the document.
// Free Transform has a transformation point: drag it anywhere (it snaps to the box), double-click it to
// put it back in the middle; rotation and Alt-scaling turn around it. Subselection drags stroke nodes.
#include "app/app.h"
#include "gfx/cursors.h"
#include "geom/geom.h"
#include "render/scene.h"
#include "sokol/sokol_app.h"

enum { M_NONE, M_MOVE, M_MARQUEE, M_LASSO, M_SCALE, M_ROTATE, M_PIVOT, M_NODE, M_SHAPE };
typedef struct { int item; float a, b; } Section;   // arc-length range of a stroke's centerline

static struct {
    int drawing;           // drawings[] index the selection belongs to
    int *idx;              // whole items, ascending
    Section *secs;         // parts of strokes, not split off yet
    int mode;
    V2 start, last;        // local coords
    V2 sstart;             // screen coords at press
    V2 *lasso;             // local points
    Item *orig;            // what is being transformed, as it was when the drag started
    float ob[4];           // bounds of the selection at press
    int handle;            // 0..3 corners (tl, tr, br, bl), 4..7 edges (t, r, b, l)
    bool moved, committed;
    int hover; Section hsec; bool hsec_on;   // under the pointer
    V2 pivot, opivot; bool pivot_on; u32 pivot_sig;
    int node_item, node;   // Subselection: the stroke whose nodes are shown, the node under the pointer
} SL = {.drawing = -1, .hover = -1, .node_item = -1, .node = -1};

static Item *CLIP;

static int drawing_index(Drawing *dr) { return dr ? (int)(dr - app.doc->drawings) : -1; }
static void clear_items(void) { arrsetlen(SL.idx, 0); arrsetlen(SL.secs, 0); }

void select_clear(void) { clear_items(); SL.drawing = -1; SL.node_item = -1; }
int select_count(void) { return (int)(arrlen(SL.idx) + arrlen(SL.secs)); }

static Drawing *sel_drawing(void) {
    Drawing *dr = st_drawing(false);
    if (drawing_index(dr) != SL.drawing) { clear_items(); SL.node_item = -1; SL.drawing = drawing_index(dr); }
    if (dr) {
        for (ptrdiff_t i = arrlen(SL.idx) - 1; i >= 0; i--) if (SL.idx[i] >= arrlen(dr->items)) arrdel(SL.idx, i);
        for (ptrdiff_t i = arrlen(SL.secs) - 1; i >= 0; i--) if (SL.secs[i].item >= arrlen(dr->items)) arrdel(SL.secs, i);
        if (SL.node_item >= arrlen(dr->items)) SL.node_item = -1;
    }
    return dr;
}
Item *select_first(void) { Drawing *dr = sel_drawing(); return dr && arrlen(SL.idx) ? &dr->items[SL.idx[0]] : NULL; }

static bool is_sel(int i) { for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) if (SL.idx[k] == i) return true; return false; }
static void sel_add(int i) {
    if (is_sel(i)) return;
    for (ptrdiff_t k = arrlen(SL.secs) - 1; k >= 0; k--) if (SL.secs[k].item == i) arrdel(SL.secs, k);   // the whole wins
    ptrdiff_t at = 0;
    while (at < arrlen(SL.idx) && SL.idx[at] < i) at++;
    arrins(SL.idx, at, i);
}
static void sel_remove(int i) { for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) if (SL.idx[k] == i) { arrdel(SL.idx, k); return; } }

static bool target_ok(const Item *it) {
    if (it->erase) return false;
    if (item_placed(it)) return true;
    if (app.sel_target == SELT_LINES) return it->kind == IT_STROKE;
    if (app.sel_target == SELT_FILLS) return it->kind == IT_FILL;
    return true;
}

static bool bounds_of(const Item *it, float b[4]) { b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f; return item_bounds_m(app.doc, it, 0, mat_id(), b, 0); }
static void grow(float b[4], const float c[4]) { b[0] = MIN(b[0], c[0]); b[1] = MIN(b[1], c[1]); b[2] = MAX(b[2], c[2]); b[3] = MAX(b[3], c[3]); }

static Item section_piece(Drawing *dr, Section s) {
    Item *it = &dr->items[s.item];
    Path P = path_of(it, cut_step());
    Item o = stroke_piece(&P, it, s.a, s.b);
    path_free(&P);
    return o;
}

static bool sel_bounds(Drawing *dr, float b[4]) {
    b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f;
    bool any = false;
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) { float c[4]; if (bounds_of(&dr->items[SL.idx[k]], c)) { grow(b, c); any = true; } }
    for (ptrdiff_t k = 0; k < arrlen(SL.secs); k++) {
        Item pc = section_piece(dr, SL.secs[k]);
        float c[4];
        if (bounds_of(&pc, c)) { grow(b, c); any = true; }
        item_free(&pc);
    }
    return any;
}

static int hit_item(Drawing *dr, V2 p) {
    if (!dr) return -1;
    float tol = 3 * st_unit();
    for (ptrdiff_t k = arrlen(dr->items) - 1; k >= 0; k--) {
        Item *it = &dr->items[k];
        if (!target_ok(it)) continue;
        if (item_placed(it)) {
            float b[4];
            if (bounds_of(it, b) && p.x >= b[0] - tol && p.y >= b[1] - tol && p.x <= b[2] + tol && p.y <= b[3] + tol) return (int)k;
        } else if (item_hit(it, p, tol)) return (int)k;
    }
    return -1;
}

// look on other unlocked, visible layers too; switches the active layer when something is found there
static int hit_any_layer(V2 p) {
    Doc *d = app.doc;
    Timeline *t = doc_tl(d);
    int k = hit_item(st_drawing(false), p);
    if (k >= 0) return k;
    for (ptrdiff_t li = 0; li < arrlen(t->layers); li++) {
        Layer *l = &t->layers[li];
        if (l->id == t->active || l->type == LT_FOLDER || doc_hidden(d, l) || doc_locked(d, l)) continue;
        Drawing *dr = doc_drawing_at(d, l, t->frame);
        int h = hit_item(dr, p);
        if (h >= 0) { t->active = l->id; select_clear(); SL.drawing = drawing_index(dr); return h; }
    }
    return -1;
}

// Sections mode: the part of stroke k around p, between its intersections (false: take the whole item)
static bool section_at(Drawing *dr, int k, V2 p, Section *out) {
    Item *it = &dr->items[k];
    if (app.sel_mode != SELM_SECTIONS || it->kind != IT_STROKE) return false;
    Path P = path_of(it, cut_step());
    float a, b;
    bool part = stroke_section_at(&P, it, p, &a, &b);
    path_free(&P);
    if (part) *out = (Section){k, a, b};
    return part;
}
static bool in_range(float u, float a, float b, float L) { return (u >= a && u <= b) || (u + L >= a && u + L <= b) || (u - L >= a && u - L <= b); }
static int sec_at(Drawing *dr, int k, V2 p) {
    for (ptrdiff_t i = 0; i < arrlen(SL.secs); i++) {
        if (SL.secs[i].item != k) continue;
        Path P = path_of(&dr->items[k], cut_step());
        float u = path_project(&P, p, NULL), L = P.len;
        path_free(&P);
        if (in_range(u, SL.secs[i].a, SL.secs[i].b, L)) return (int)i;
    }
    return -1;
}
static bool point_selected(Drawing *dr, V2 p) {
    int k = hit_item(dr, p);
    return k >= 0 && (is_sel(k) || sec_at(dr, k, p) >= 0);
}

// picked sections become strokes of their own (in place, same z-order); the selection then holds them
static void materialize(void) {
    Drawing *dr = sel_drawing();
    if (!dr || !arrlen(SL.secs)) return;
    int n = (int)arrlen(dr->items);
    Item *out = NULL;
    int *sel = NULL;
    float minlen = 0.5f * st_unit();
    for (int k = 0; k < n; k++) {
        Item *it = &dr->items[k];
        Section *mine = NULL;
        for (ptrdiff_t i = 0; i < arrlen(SL.secs); i++) if (SL.secs[i].item == k) arrput(mine, SL.secs[i]);
        if (!arrlen(mine)) {
            arrput(out, *it);
            if (is_sel(k)) arrput(sel, (int)arrlen(out) - 1);
            continue;
        }
        for (ptrdiff_t i = 1; i < arrlen(mine); i++)   // by start
            for (ptrdiff_t j = i; j > 0 && mine[j - 1].a > mine[j].a; j--) { Section t = mine[j]; mine[j] = mine[j - 1]; mine[j - 1] = t; }
        Path P = path_of(it, cut_step());
        float L = P.len;
        int m = (int)arrlen(mine);
        if (it->flags & ITF_CLOSED) {
            for (int i = 0; i < m; i++) {
                arrput(out, stroke_piece(&P, it, mine[i].a, mine[i].b));
                arrput(sel, (int)arrlen(out) - 1);
                float oa = mine[i].b, ob = i + 1 < m ? mine[i + 1].a : mine[0].a + L;
                if (ob - oa > minlen) arrput(out, stroke_piece(&P, it, oa, ob));
            }
        } else {
            float cur = 0;
            for (int i = 0; i < m; i++) {
                if (mine[i].a - cur > minlen) arrput(out, stroke_piece(&P, it, cur, mine[i].a));
                arrput(out, stroke_piece(&P, it, mine[i].a, mine[i].b));
                arrput(sel, (int)arrlen(out) - 1);
                cur = mine[i].b;
            }
            if (L - cur > minlen) arrput(out, stroke_piece(&P, it, cur, L));
        }
        path_free(&P);
        arrfree(mine);
        item_free(it);
    }
    arrfree(dr->items);
    dr->items = out;
    arrfree(SL.idx);
    SL.idx = sel;
    arrsetlen(SL.secs, 0);
    doc_touch(app.doc);
}

static void transform_item(Item *dst, const Item *src, Mat T) {
    item_free(dst);
    *dst = item_copy(src);
    float s = sqrtf(fabsf(mat_det(T)));
    for (ptrdiff_t i = 0; i < arrlen(dst->pts); i++) { V2 q = mat_ap(T, v2(dst->pts[i].x, dst->pts[i].y)); dst->pts[i].x = q.x; dst->pts[i].y = q.y; dst->pts[i].r *= s; }
    for (ptrdiff_t i = 0; i < arrlen(dst->poly); i++) dst->poly[i] = mat_ap(T, dst->poly[i]);
    if (item_placed(dst) || (dst->kind == IT_FILL && dst->sym) || (dst->kind == IT_STROKE && dst->texture))
        dst->m = mat_mul(T, dst->m);   // bitmap fills and stroke textures move with their vector
    if (dst->kind == IT_STROKE) { dst->angle += atan2f(T.b, T.a); dst->img_gap *= s; dst->img_r0 *= s; dst->img_r1 *= s;
                                  dst->dual_gap *= s; dst->dual_angle += atan2f(T.b, T.a); }
    item_bounds(dst);
}

/* ── the selection signature: what the hatch and the transformation point depend on ── */

static u32 mix32(u32 h, u32 v) { h ^= v; h *= 16777619u; return h; }
static u32 mixf(u32 h, float f) { u32 v; memcpy(&v, &f, 4); return mix32(h, v); }
static u32 sel_sig(void) {
    u32 h = mix32(2166136261u, (u32)SL.drawing);
    for (ptrdiff_t i = 0; i < arrlen(SL.idx); i++) h = mix32(h, (u32)SL.idx[i]);
    for (ptrdiff_t i = 0; i < arrlen(SL.secs); i++) { h = mix32(h, (u32)SL.secs[i].item | 0x80000000u); h = mixf(h, SL.secs[i].a); h = mixf(h, SL.secs[i].b); }
    return h;
}
u32 select_signature(void) {
    u32 h = mix32(sel_sig(), (u32)app.tool);
    h = mix32(h, (u32)SL.hover);
    if (SL.hsec_on) { h = mixf(h, SL.hsec.a); h = mixf(h, SL.hsec.b); }
    h = mix32(h, (u32)SL.node_item); h = mix32(h, (u32)SL.node);
    return h;
}

/* ── transforms ── */

static V2 box_center(void) { return v2((SL.ob[0] + SL.ob[2]) / 2, (SL.ob[1] + SL.ob[3]) / 2); }
static V2 current_pivot(Drawing *dr) {
    if (SL.pivot_sig != sel_sig()) { SL.pivot_on = false; SL.pivot_sig = sel_sig(); }
    if (SL.pivot_on) return SL.pivot;
    float b[4];
    if (!sel_bounds(dr, b)) return v2(0, 0);
    return v2((b[0] + b[2]) / 2, (b[1] + b[3]) / 2);
}

// first real change of a drag: undo point, split off picked sections, remember the originals
static void begin_transform(void) {
    if (SL.committed) return;
    Drawing *dr = sel_drawing();
    V2 pv = current_pivot(dr);
    app_commit();
    materialize();
    dr = sel_drawing();
    for (ptrdiff_t i = 0; i < arrlen(SL.orig); i++) item_free(&SL.orig[i]);
    arrsetlen(SL.orig, 0);
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) arrput(SL.orig, item_copy(&dr->items[SL.idx[k]]));
    SL.opivot = pv;
    SL.pivot_sig = sel_sig();
    SL.committed = true;
}
static void apply_transform(Mat T) {
    begin_transform();
    Drawing *dr = sel_drawing();
    if (!dr) return;
    for (ptrdiff_t k = 0; k < arrlen(SL.idx) && k < arrlen(SL.orig); k++) transform_item(&dr->items[SL.idx[k]], &SL.orig[k], T);
    SL.pivot = mat_ap(T, SL.opivot); SL.pivot_on = true; SL.pivot_sig = sel_sig();
    doc_touch(app.doc);
}

/* ── handles and the transformation point (screen space) ── */

static void bbox_screen(const float b[4], V2 c[4]) {
    c[0] = st_screen(v2(b[0], b[1])); c[1] = st_screen(v2(b[2], b[1])); c[2] = st_screen(v2(b[2], b[3])); c[3] = st_screen(v2(b[0], b[3]));
}
static V2 handle_pos(const V2 c[4], int h) {
    if (h < 4) return c[h];
    V2 a = c[h - 4], b = c[(h - 3) % 4];
    return v2((a.x + b.x) / 2, (a.y + b.y) / 2);
}
static int handle_at(Drawing *dr, V2 m, bool *rotate) {
    *rotate = false;
    if (!select_count()) return -1;
    float b[4];
    if (!sel_bounds(dr, b)) return -1;
    V2 c[4];
    bbox_screen(b, c);
    for (int h = 0; h < 8; h++) { V2 p = handle_pos(c, h); if (fabsf(m.x - p.x) <= 6 && fabsf(m.y - p.y) <= 6) return h; }
    for (int h = 0; h < 4; h++) { float dd = hypotf(m.x - c[h].x, m.y - c[h].y); if (dd > 6 && dd < 22) { *rotate = true; return h; } }
    return -1;
}
static bool pivot_hit(Drawing *dr, V2 m) {
    if (!select_count()) return false;
    V2 pv = st_screen(current_pivot(dr));
    return hypotf(m.x - pv.x, m.y - pv.y) <= 7;
}
// the transformation point snaps to the box: centre, corners, middles of the edges
static V2 snap_pivot(V2 p) {
    float *b = SL.ob, bd = 8 * st_unit();
    V2 best = p;
    for (int i = 0; i < 9; i++) {
        float x = b[0] + (b[2] - b[0]) * (i % 3) / 2, y = b[1] + (b[3] - b[1]) * (i / 3) / 2, d = hypotf(p.x - x, p.y - y);
        if (d < bd) { bd = d; best = v2(x, y); }
    }
    return best;
}

static void enter_instance(Drawing *dr, int k) {
    Item *it = &dr->items[k];
    if (it->kind != IT_INSTANCE) return;
    Doc *d = app.doc;
    int sym = it->sym;
    Mat m = it->m;
    select_clear();
    doc_enter_symbol(d, sym, m);
    app_toast("Editing %s. Double-click empty space or use the crumbs to go back", doc_symbol(d, sym)->name);
}

/* ── drag box and lasso ── */

static bool region_has(const V2 *R, int n, V2 p) { static const int st0[1] = {0}; return point_in_poly(R, st0, n, 1, p); }
static bool region_crosses(const V2 *R, int n, V2 a, V2 b) {
    for (int j = 0; j < n; j++) if (seg_isect(a, b, R[j], R[(j + 1) % n], NULL, NULL)) return true;
    return false;
}
/* ── cutting fills with the selection region ── */

static float cross2(V2 o, V2 a, V2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
static float area2(const V2 *p, int n) { float a = 0; for (int i = 0; i < n; i++) { V2 q = p[(i + 1) % n]; a += p[i].x * q.y - q.x * p[i].y; } return a; }

// a contour clipped by a convex polygon (Sutherland-Hodgman); edges may run along the clip edge, which an
// even-odd fill doesn't mind
static V2 *clip_convex(const V2 *in, int n, const V2 *C, int nc) {
    V2 *cur = NULL, *nxt = NULL;
    for (int i = 0; i < n; i++) arrput(cur, in[i]);
    float o = area2(C, nc) > 0 ? 1.0f : -1.0f;
    for (int j = 0; j < nc && arrlen(cur); j++) {
        V2 a = C[j], b = C[(j + 1) % nc];
        arrsetlen(nxt, 0);
        int m = (int)arrlen(cur);
        for (int i = 0; i < m; i++) {
            V2 P = cur[i], Q = cur[(i + 1) % m];
            float dp = o * cross2(a, b, P), dq = o * cross2(a, b, Q);
            if (dq >= 0) {
                if (dp < 0) { float t = dp / (dp - dq); arrput(nxt, v2(P.x + (Q.x - P.x) * t, P.y + (Q.y - P.y) * t)); }
                arrput(nxt, Q);
            } else if (dp >= 0) { float t = dp / (dp - dq); arrput(nxt, v2(P.x + (Q.x - P.x) * t, P.y + (Q.y - P.y) * t)); }
        }
        V2 *t = cur; cur = nxt; nxt = t;
    }
    arrfree(nxt);
    return cur;
}

// ear clipping; false if the polygon isn't simple enough
static bool triangulate(const V2 *p, int n, V2 **tris) {
    int *ix = NULL;
    for (int i = 0; i < n; i++) arrput(ix, i);
    float o = area2(p, n) > 0 ? 1.0f : -1.0f;
    for (int guard = 0; arrlen(ix) > 3 && guard < n * n + 16; guard++) {
        int m = (int)arrlen(ix);
        bool cut = false;
        for (int i = 0; i < m; i++) {
            V2 a = p[ix[(i + m - 1) % m]], b = p[ix[i]], c = p[ix[(i + 1) % m]];
            if (o * cross2(a, b, c) <= 1e-9f) continue;
            bool empty = true;
            for (int j = 0; j < m && empty; j++) {
                if (j == i || j == (i + 1) % m || j == (i + m - 1) % m) continue;
                V2 q = p[ix[j]];
                empty = !(o * cross2(a, b, q) > 0 && o * cross2(b, c, q) > 0 && o * cross2(c, a, q) > 0);
            }
            if (!empty) continue;
            arrput(*tris, a); arrput(*tris, b); arrput(*tris, c);
            arrdel(ix, i);
            cut = true;
            break;
        }
        if (!cut) { arrfree(ix); return false; }
    }
    if (arrlen(ix) == 3) { arrput(*tris, p[ix[0]]); arrput(*tris, p[ix[1]]); arrput(*tris, p[ix[2]]); }
    arrfree(ix);
    return true;
}

// the part of a fill inside the region (a box, or a lasso cut into triangles)
static bool fill_inside(const Item *it, const V2 *R, int nr, Item *out) {
    V2 *pieces = NULL; int per = 3;
    if (nr == 4) { for (int i = 0; i < 4; i++) arrput(pieces, R[i]); per = 4; }
    else if (!triangulate(R, nr, &pieces)) { arrfree(pieces); return false; }
    *out = item_copy(it);
    arrsetlen(out->poly, 0); arrsetlen(out->starts, 0);
    int np = (int)arrlen(it->poly), nc = (int)arrlen(it->starts);
    for (ptrdiff_t q = 0; q + per <= arrlen(pieces); q += per)
        for (int c = 0; c < nc; c++) {
            int a0 = it->starts[c], a1 = c + 1 < nc ? it->starts[c + 1] : np;
            V2 *cl = clip_convex(it->poly + a0, a1 - a0, pieces + q, per);
            if (arrlen(cl) >= 3 && fabsf(area2(cl, (int)arrlen(cl))) > 1e-4f) {
                arrput(out->starts, (int)arrlen(out->poly));
                for (ptrdiff_t i = 0; i < arrlen(cl); i++) arrput(out->poly, cl[i]);
            }
            arrfree(cl);
        }
    arrfree(pieces);
    if (!arrlen(out->starts)) { item_free(out); return false; }
    item_bounds(out);
    return true;
}

static bool sec_overlaps(Section s) {
    for (ptrdiff_t i = 0; i < arrlen(SL.secs); i++) if (SL.secs[i].item == s.item && SL.secs[i].a < s.b && s.a < SL.secs[i].b) return true;
    return false;
}

// what the region touches; in Sections mode strokes are cut at its edge and the parts inside are picked
static void region_select(Drawing *dr, const V2 *R, int nr) {
    bool committed = false;
    float rx0 = R[0].x, ry0 = R[0].y, rx1 = rx0, ry1 = ry0;
    for (int j = 1; j < nr; j++) { rx0 = MIN(rx0, R[j].x); ry0 = MIN(ry0, R[j].y); rx1 = MAX(rx1, R[j].x); ry1 = MAX(ry1, R[j].y); }
    for (int k = 0; k < (int)arrlen(dr->items); k++) {
        Item *it = &dr->items[k];
        float b[4];
        if (!target_ok(it) || !bounds_of(it, b) || b[2] < rx0 || b[0] > rx1 || b[3] < ry0 || b[1] > ry1) continue;
        if (it->kind == IT_STROKE) {
            Path P = path_of(it, cut_step());
            int n = (int)arrlen(P.p);
            bool inside = false;
            float *cuts = NULL;
            for (int i = 0; i < n && !inside; i++) inside = region_has(R, nr, v2(P.p[i].x, P.p[i].y));
            for (int i = 0; i + 1 < n; i++) {
                V2 a = v2(P.p[i].x, P.p[i].y), c = v2(P.p[i + 1].x, P.p[i + 1].y);
                for (int j = 0; j < nr; j++) { float t; if (seg_isect(a, c, R[j], R[(j + 1) % nr], &t, NULL)) arrput(cuts, P.s[i] + (P.s[i + 1] - P.s[i]) * t); }
            }
            if (inside || arrlen(cuts)) {
                if (app.sel_box == SELB_TOUCHED || !arrlen(cuts)) sel_add(k);
                else {
                    for (ptrdiff_t i = 1; i < arrlen(cuts); i++) for (ptrdiff_t j = i; j > 0 && cuts[j - 1] > cuts[j]; j--) { float t = cuts[j]; cuts[j] = cuts[j - 1]; cuts[j - 1] = t; }
                    int nc = (int)arrlen(cuts), outside = 0;
                    float L = P.len;
                    Section *got = NULL;
                    for (int i = 0; i <= nc; i++) {
                        float a = i ? cuts[i - 1] : 0, e = i < nc ? cuts[i] : L;
                        if (e - a < 1e-4f) continue;
                        SPt mid = path_at(&P, (a + e) / 2);
                        if (region_has(R, nr, v2(mid.x, mid.y))) arrput(got, ((Section){k, a, e})); else outside++;
                    }
                    int g = (int)arrlen(got);
                    if (!outside) sel_add(k);
                    else if (!is_sel(k)) {
                        // closed strokes: the parts before the first cut and after the last one are a single part
                        if ((it->flags & ITF_CLOSED) && g >= 2 && got[0].a <= 1e-4f && got[g - 1].b >= L - 1e-4f) { got[g - 1].b = L + got[0].b; arrdel(got, 0); }
                        for (ptrdiff_t i = 0; i < arrlen(got); i++) if (!sec_overlaps(got[i])) arrput(SL.secs, got[i]);
                    }
                    arrfree(got);
                }
            }
            arrfree(cuts);
            path_free(&P);
        } else if (it->kind == IT_FILL) {
            int np = (int)arrlen(it->poly), nc = (int)arrlen(it->starts);
            bool touched = false;
            for (int i = 0; i < np && !touched; i++) touched = region_has(R, nr, it->poly[i]);
            for (int j = 0; j < nr && !touched; j++) touched = point_in_poly(it->poly, it->starts, np, nc, R[j]);
            for (int c = 0; c < nc && !touched; c++) {
                int a0 = it->starts[c], a1 = c + 1 < nc ? it->starts[c + 1] : np;
                for (int i = a0; i < a1 && !touched; i++) touched = region_crosses(R, nr, it->poly[i], it->poly[i + 1 < a1 ? i + 1 : a0]);
            }
            if (!touched) continue;
            bool all_in = true;   // a fill the region only partly covers is cut: the part inside is picked
            for (int i = 0; i < np && all_in; i++) all_in = region_has(R, nr, it->poly[i]);
            Item in;
            if (all_in || app.sel_box == SELB_TOUCHED || is_sel(k) || !fill_inside(it, R, nr, &in)) { sel_add(k); continue; }
            if (!committed) { app_commit(); committed = true; }
            // what is left: the fill with the cut part's contours added (even-odd takes it out)
            Item *rest = &dr->items[k];
            for (ptrdiff_t c = 0; c < arrlen(in.starts); c++) {
                int a0 = in.starts[c], a1 = c + 1 < arrlen(in.starts) ? in.starts[c + 1] : (int)arrlen(in.poly);
                arrput(rest->starts, (int)arrlen(rest->poly));
                for (int i = a0; i < a1; i++) arrput(rest->poly, in.poly[i]);
            }
            item_bounds(rest);
            arrins(dr->items, k + 1, in);
            for (ptrdiff_t i = 0; i < arrlen(SL.idx); i++) if (SL.idx[i] > k) SL.idx[i]++;
            for (ptrdiff_t i = 0; i < arrlen(SL.secs); i++) if (SL.secs[i].item > k) SL.secs[i].item++;
            sel_add(k + 1);
            k++;   // the new piece is done
            doc_touch(app.doc);
        } else {
            V2 q[4] = {{b[0], b[1]}, {b[2], b[1]}, {b[2], b[3]}, {b[0], b[3]}};
            bool touched = false;
            for (int i = 0; i < 4 && !touched; i++) touched = region_has(R, nr, q[i]) || region_crosses(R, nr, q[i], q[(i + 1) % 4]);
            for (int j = 0; j < nr && !touched; j++) touched = R[j].x >= b[0] && R[j].x <= b[2] && R[j].y >= b[1] && R[j].y <= b[3];
            if (touched) sel_add(k);
        }
    }
}

/* ── Subselection: nodes of one stroke ── */

static int node_at(Drawing *dr, V2 m) {
    if (!dr || SL.node_item < 0 || SL.node_item >= arrlen(dr->items)) return -1;
    Item *it = &dr->items[SL.node_item];
    for (ptrdiff_t i = arrlen(it->pts) - 1; i >= 0; i--) {
        V2 s = st_screen(v2(it->pts[i].x, it->pts[i].y));
        if (fabsf(m.x - s.x) <= 5 && fabsf(m.y - s.y) <= 5) return (int)i;
    }
    return -1;
}

/* ── shaping vectors with the Selection tool, the Animate way ── */

// A drag on a corner moves that point; on an edge it bends the edge between the corners on either side; Ctrl on
// an edge makes a new corner there. Every fill contour and stroke centreline lying on the dragged geometry
// follows (a rectangle's outline and its fill stay together).
enum { SE_NONE, SE_CORNER, SE_BEND };
static struct {
    int kind, item;
    V2 grab;
    V2 *run; float *rs; float rlen, s0;   // bend: the edge between two corners, its arc lengths, the grab on it
    Item *orig;                           // every item as it was, edges on the run subdivided
} SE;
#define SE_EPS 0.6f   // drawing units: geometry this close to what is dragged is the same geometry

static float seg_dist(V2 p, V2 a, V2 b, float *t) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float u = l2 > 0 ? clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0, 1) : 0;
    if (t) *t = u;
    return hypotf(p.x - a.x - dx * u, p.y - a.y - dy * u);
}
static float turn_at(V2 a, V2 b, V2 c) {   // how sharply the outline turns at b (radians)
    float a1 = atan2f(b.y - a.y, b.x - a.x), a2 = atan2f(c.y - b.y, c.x - b.x), d = a2 - a1;
    while (d > VX_PI) d -= 2 * VX_PI;
    while (d < -VX_PI) d += 2 * VX_PI;
    return fabsf(d);
}
// a polyline of an item: a fill contour (closed) or a stroke centreline
typedef struct { V2 *p; int n; bool closed; } Poly;
static Poly poly_of(const Item *it, int c, V2 **tmp) {
    Poly o = {0};
    if (it->kind == IT_FILL) {
        int np = (int)arrlen(it->poly), nc = (int)arrlen(it->starts);
        int a0 = it->starts[c], a1 = c + 1 < nc ? it->starts[c + 1] : np;
        o.p = it->poly + a0; o.n = a1 - a0; o.closed = true;
    } else {
        arrsetlen(*tmp, 0);
        for (ptrdiff_t i = 0; i < arrlen(it->pts); i++) arrput(*tmp, v2(it->pts[i].x, it->pts[i].y));
        o.p = *tmp; o.n = (int)arrlen(*tmp); o.closed = it->flags & ITF_CLOSED;
    }
    return o;
}
static int npolys(const Item *it) { return it->kind == IT_FILL ? (int)arrlen(it->starts) : 1; }
static bool is_corner(Poly P, int i) {
    if (!P.closed && (i == 0 || i == P.n - 1)) return true;
    V2 a = P.p[(i + P.n - 1) % P.n], b = P.p[i], c = P.p[(i + 1) % P.n];
    return turn_at(a, b, c) > 0.55f;
}

// what a press at p would shape: SE_CORNER at a corner, SE_BEND on an edge (Ctrl: a new corner)
static int edit_at(Drawing *dr, V2 p, bool ctrl, int *item, int *poly, int *seg, float *segt) {
    if (!dr) return SE_NONE;
    float tolc = 6 * st_unit(), tole = 4 * st_unit();
    V2 *tmp = NULL;
    int found = SE_NONE;
    for (ptrdiff_t k = arrlen(dr->items) - 1; k >= 0 && !found; k--) {
        Item *it = &dr->items[k];
        if ((it->kind != IT_FILL && it->kind != IT_STROKE) || it->erase || !target_ok(it)) continue;
        for (int c = 0; c < npolys(it) && !found; c++) {
            Poly P = poly_of(it, c, &tmp);
            if (P.n < 2) continue;
            float r = 0;
            if (it->kind == IT_STROKE) for (ptrdiff_t i = 0; i < arrlen(it->pts); i++) r = MAX(r, it->pts[i].r);
            for (int i = 0; i < P.n && !ctrl; i++)
                if (is_corner(P, i) && hypotf(p.x - P.p[i].x, p.y - P.p[i].y) <= tolc + r) { found = SE_CORNER; *item = (int)k; *poly = c; *seg = i; *segt = 0; break; }
            if (found) break;
            int ns = P.closed ? P.n : P.n - 1;
            float best = 1e30f; int bs = -1; float bt = 0;
            for (int i = 0; i < ns; i++) { float t, d = seg_dist(p, P.p[i], P.p[(i + 1) % P.n], &t); if (d < best) { best = d; bs = i; bt = t; } }
            if (bs >= 0 && best <= tole + r) { found = SE_BEND; *item = (int)k; *poly = c; *seg = bs; *segt = bt; }
        }
    }
    arrfree(tmp);
    return found;
}

static float run_param(V2 q, float *dist) {   // arc length of the closest point of the run, and how far
    float best = 1e30f, bs = 0;
    for (ptrdiff_t i = 0; i + 1 < arrlen(SE.run); i++) {
        float t, d = seg_dist(q, SE.run[i], SE.run[i + 1], &t);
        if (d < best) { best = d; bs = SE.rs[i] + (SE.rs[i + 1] - SE.rs[i]) * t; }
    }
    *dist = best;
    return bs;
}
static float bend_w(float s) {   // 1 at the grab with a level top, 0 at the corners
    if (s <= SE.s0) return SE.s0 > 1e-6f ? sinf(VX_PI / 2 * s / SE.s0) : 1;
    return SE.rlen - SE.s0 > 1e-6f ? sinf(VX_PI / 2 * (SE.rlen - s) / (SE.rlen - SE.s0)) : 1;
}

// geometry on the run gets more points, so a straight edge can bend smoothly
static void subdivide_on_run(Item *it) {
    float step = 1.5f;
    if (it->kind == IT_STROKE) {
        SPt *o = NULL;
        int n = (int)arrlen(it->pts), ns = (it->flags & ITF_CLOSED) ? n : n - 1;
        for (int i = 0; i < n; i++) {
            arrput(o, it->pts[i]);
            if (i >= ns) continue;
            SPt a = it->pts[i], b = it->pts[(i + 1) % n];
            float d1, d2, dm;
            run_param(v2(a.x, a.y), &d1); run_param(v2(b.x, b.y), &d2); run_param(v2((a.x + b.x) / 2, (a.y + b.y) / 2), &dm);
            if (d1 > SE_EPS || d2 > SE_EPS || dm > SE_EPS) continue;
            int k = (int)(hypotf(b.x - a.x, b.y - a.y) / step);
            for (int j = 1; j < k; j++) { float t = (float)j / k; arrput(o, ((SPt){lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.r, b.r, t)})); }
        }
        arrfree(it->pts); it->pts = o;
        return;
    }
    V2 *o = NULL; int *st = NULL;
    int np = (int)arrlen(it->poly), nc = (int)arrlen(it->starts);
    for (int c = 0; c < nc; c++) {
        int a0 = it->starts[c], a1 = c + 1 < nc ? it->starts[c + 1] : np;
        arrput(st, (int)arrlen(o));
        for (int i = a0; i < a1; i++) {
            V2 a = it->poly[i], b = it->poly[i + 1 < a1 ? i + 1 : a0];
            arrput(o, a);
            float d1, d2, dm;
            run_param(a, &d1); run_param(b, &d2); run_param(v2((a.x + b.x) / 2, (a.y + b.y) / 2), &dm);
            if (d1 > SE_EPS || d2 > SE_EPS || dm > SE_EPS) continue;
            int k = (int)(hypotf(b.x - a.x, b.y - a.y) / step);
            for (int j = 1; j < k; j++) { float t = (float)j / k; arrput(o, v2(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t))); }
        }
    }
    arrfree(it->poly); arrfree(it->starts); it->poly = o; it->starts = st;
}

// the press: remember everything as it is (nothing changes until the pointer moves)
static void shape_begin(Drawing *dr, int kind, int item, int poly, int seg, float segt, V2 p, bool ctrl) {
    for (ptrdiff_t i = 0; i < arrlen(SE.orig); i++) item_free(&SE.orig[i]);
    arrsetlen(SE.orig, 0); arrsetlen(SE.run, 0); arrsetlen(SE.rs, 0);
    for (ptrdiff_t i = 0; i < arrlen(dr->items); i++) arrput(SE.orig, item_copy(&dr->items[i]));
    SE.kind = kind; SE.item = item;
    V2 *tmp = NULL;
    Poly P = poly_of(&SE.orig[item], poly, &tmp);
    if (kind == SE_CORNER) { SE.grab = P.p[seg]; arrfree(tmp); return; }
    V2 a = P.p[seg], b = P.p[(seg + 1) % P.n], at = v2(lerpf(a.x, b.x, segt), lerpf(a.y, b.y, segt));
    if (ctrl) {   // a new corner where the edge was grabbed: every polyline through that spot gets the point
        for (ptrdiff_t k = 0; k < arrlen(SE.orig); k++) {
            Item *it = &SE.orig[k];
            if (it->kind == IT_STROKE) {
                int n = (int)arrlen(it->pts), ns = (it->flags & ITF_CLOSED) ? n : n - 1;
                for (int i = 0; i < ns; i++) {
                    SPt q0 = it->pts[i], q1 = it->pts[(i + 1) % n];
                    float t, d = seg_dist(at, v2(q0.x, q0.y), v2(q1.x, q1.y), &t);
                    if (d > SE_EPS || t <= 0.001f || t >= 0.999f) continue;
                    arrins(it->pts, i + 1, ((SPt){at.x, at.y, lerpf(q0.r, q1.r, t)}));
                    break;
                }
            } else if (it->kind == IT_FILL) {
                int nc = (int)arrlen(it->starts);
                for (int c = 0; c < nc; c++) {
                    int a0 = it->starts[c], a1 = c + 1 < nc ? it->starts[c + 1] : (int)arrlen(it->poly);
                    bool done = false;
                    for (int i = a0; i < a1 && !done; i++) {
                        float t, d = seg_dist(at, it->poly[i], it->poly[i + 1 < a1 ? i + 1 : a0], &t);
                        if (d > SE_EPS || t <= 0.001f || t >= 0.999f) continue;
                        arrins(it->poly, i + 1, at);
                        for (int q = c + 1; q < nc; q++) it->starts[q]++;
                        done = true;
                    }
                }
            }
        }
        SE.kind = SE_CORNER; SE.grab = at;
        arrfree(tmp);
        return;
    }
    // the edge to bend: from the corner before the grabbed segment to the one after it
    int n = P.n, i0 = seg, i1 = (seg + 1) % n;
    int lim = n;
    if (P.closed) {
        bool any = false;
        for (int i = 0; i < n; i++) any |= is_corner(P, i);
        if (!any) { i0 = (seg - n / 4 + n) % n; i1 = (seg + 1 + n / 4) % n; }   // a smooth loop: a quarter either way
        else {
            while (!is_corner(P, i0) && lim-- > 0) i0 = (i0 + n - 1) % n;
            lim = n;
            while (!is_corner(P, i1) && lim-- > 0) i1 = (i1 + 1) % n;
        }
        arrput(SE.run, P.p[i0]);   // round to the next corner (all the way round if it is the same one)
        for (int i = (i0 + 1) % n, g = 0; g <= n; i = (i + 1) % n, g++) { arrput(SE.run, P.p[i]); if (i == i1) break; }
    } else {
        while (i0 > 0 && !is_corner(P, i0)) i0--;
        while (i1 < n - 1 && !is_corner(P, i1)) i1++;
        for (int i = i0; i <= i1; i++) arrput(SE.run, P.p[i]);
    }
    arrfree(tmp);
    float acc = 0;
    for (ptrdiff_t i = 0; i < arrlen(SE.run); i++) { if (i) acc += hypotf(SE.run[i].x - SE.run[i - 1].x, SE.run[i].y - SE.run[i - 1].y); arrput(SE.rs, acc); }
    SE.rlen = acc;
    float d;
    SE.s0 = run_param(at, &d);
    SE.grab = at;
    for (ptrdiff_t k = 0; k < arrlen(SE.orig); k++) if (SE.orig[k].kind == IT_FILL || SE.orig[k].kind == IT_STROKE) subdivide_on_run(&SE.orig[k]);
}

// the drag: everything rebuilt from the originals, moved by delta (corner) or delta times the bend weight
static void shape_drag(Drawing *dr, V2 delta) {
    for (ptrdiff_t k = 0; k < arrlen(dr->items) && k < arrlen(SE.orig); k++) {
        Item *src = &SE.orig[k];
        if (src->kind != IT_FILL && src->kind != IT_STROKE) continue;
        Item *dst = &dr->items[k];
        item_free(dst);
        *dst = item_copy(src);
        if (SE.kind == SE_CORNER) {
            for (ptrdiff_t i = 0; i < arrlen(dst->pts); i++) if (hypotf(dst->pts[i].x - SE.grab.x, dst->pts[i].y - SE.grab.y) <= SE_EPS) { dst->pts[i].x += delta.x; dst->pts[i].y += delta.y; }
            for (ptrdiff_t i = 0; i < arrlen(dst->poly); i++) if (hypotf(dst->poly[i].x - SE.grab.x, dst->poly[i].y - SE.grab.y) <= SE_EPS) { dst->poly[i].x += delta.x; dst->poly[i].y += delta.y; }
        } else {
            float d;
            for (ptrdiff_t i = 0; i < arrlen(dst->pts); i++) {
                float s = run_param(v2(dst->pts[i].x, dst->pts[i].y), &d);
                if (d <= SE_EPS) { float w = bend_w(s); dst->pts[i].x += delta.x * w; dst->pts[i].y += delta.y * w; }
            }
            for (ptrdiff_t i = 0; i < arrlen(dst->poly); i++) {
                float s = run_param(dst->poly[i], &d);
                if (d <= SE_EPS) { float w = bend_w(s); dst->poly[i].x += delta.x * w; dst->poly[i].y += delta.y * w; }
            }
        }
        item_bounds(dst);
    }
    doc_touch(app.doc);
}

/* ── the tool ── */

void select_tool(UiResp *re, char tool) {
    Drawing *dr = sel_drawing();
    V2 m = ui.in.mouse, p = st_local(m);
    bool shift = ui.in.mods & MOD_SHIFT, alt = ui.in.mods & MOD_ALT;
    if (tool != 'A') SL.node_item = -1;

    // under the pointer
    SL.hover = -1; SL.hsec_on = false;
    if (SL.mode == M_NONE) SL.node = -1;
    if (re->hover && SL.mode == M_NONE && dr) {
        if (tool == 'A') SL.node = node_at(dr, m);
        bool on_handles = tool == 'Q' && select_count() && (pivot_hit(dr, m) || handle_at(dr, m, &(bool){0}) >= 0);
        if (SL.node < 0 && !on_handles) SL.hover = hit_item(dr, p);
        if (SL.hover >= 0 && tool != 'A') SL.hsec_on = section_at(dr, SL.hover, p, &SL.hsec);
    }

    if (re->pressed) {
        SL.start = SL.last = p; SL.sstart = m; SL.moved = false; SL.committed = false;
        if (dr) sel_bounds(dr, SL.ob);
        if (tool == 'A' && dr && (SL.node = node_at(dr, m)) >= 0) {
            const char *why = st_cannot_edit();
            if (why) { app_toast("%s", why); SL.mode = M_NONE; return; }
            SL.mode = M_NODE;
            return;
        }
        if (tool == 'Q' && dr && select_count()) {
            if (pivot_hit(dr, m)) {
                if (re->dbl) { SL.pivot_on = false; SL.mode = M_NONE; return; }   // back to the middle
                SL.pivot = current_pivot(dr); SL.pivot_on = true; SL.pivot_sig = sel_sig();
                SL.mode = M_PIVOT;
                return;
            }
            bool rot = false;
            int h = handle_at(dr, m, &rot);
            if (h >= 0) { SL.mode = rot ? M_ROTATE : M_SCALE; SL.handle = h; SL.opivot = current_pivot(dr); return; }
        }
        if (tool == 'L') { SL.mode = M_LASSO; arrsetlen(SL.lasso, 0); arrput(SL.lasso, p); if (!shift) clear_items(); return; }
        if (tool == 'V' && !shift && !re->dbl && !st_cannot_edit()) {
            Drawing *ad = st_drawing(false);
            int it, pl, sg; float st;
            int kind = ad && !point_selected(ad, p) ? edit_at(ad, p, ui.in.mods & MOD_CTRL, &it, &pl, &sg, &st) : SE_NONE;
            if (kind) {
                SL.drawing = drawing_index(ad);
                clear_items();
                shape_begin(ad, kind, it, pl, sg, st, p, ui.in.mods & MOD_CTRL);
                SL.mode = M_SHAPE;
                return;
            }
        }
        int k = hit_any_layer(p);
        dr = sel_drawing();
        if (re->dbl && k >= 0 && dr->items[k].kind == IT_INSTANCE) { enter_instance(dr, k); SL.mode = M_NONE; return; }
        if (re->dbl && k < 0 && app.doc->nedit) { doc_exit_symbol(app.doc); select_clear(); SL.mode = M_NONE; return; }
        if (k >= 0) {
            Section s;
            bool part = tool != 'A' && section_at(dr, k, p, &s);
            if (shift) {
                if (part) { int i = sec_at(dr, k, p); if (i >= 0) arrdel(SL.secs, i); else if (!is_sel(k)) arrput(SL.secs, s); }
                else if (is_sel(k)) sel_remove(k);
                else sel_add(k);
            } else if (!point_selected(dr, p)) {
                clear_items();
                if (part) arrput(SL.secs, s); else sel_add(k);
            }
            if (tool == 'A') SL.node_item = dr->items[k].kind == IT_STROKE ? k : -1;
            if (st_cannot_edit()) { SL.mode = M_NONE; return; }   // locked or hidden: select, don't move
            SL.mode = M_MOVE;
        } else {
            if (!shift) clear_items();
            if (tool == 'A') SL.node_item = -1;
            SL.mode = M_MARQUEE;
        }
        return;
    }

    if ((re->held || re->released) && SL.mode != M_NONE) {   // the release frame's position counts too
        if (hypotf(m.x - SL.sstart.x, m.y - SL.sstart.y) > 2) SL.moved = true;
        switch (SL.mode) {
        case M_MOVE:
            if (SL.moved && !st_cannot_edit()) {
                V2 dlt = v2(p.x - SL.start.x, p.y - SL.start.y);
                if (shift) { if (fabsf(dlt.x) > fabsf(dlt.y)) dlt.y = 0; else dlt.x = 0; }
                apply_transform(mat_tr(dlt.x, dlt.y));
            }
            break;
        case M_LASSO:
            if (hypotf(p.x - SL.last.x, p.y - SL.last.y) > 2 * st_unit()) { arrput(SL.lasso, p); SL.last = p; }
            break;
        case M_PIVOT:
            SL.pivot = snap_pivot(p); SL.pivot_on = true; SL.pivot_sig = sel_sig();
            break;
        case M_SCALE: {
            if (!SL.moved || st_cannot_edit()) break;
            float *b = SL.ob;
            static const float hx[8] = {0, 1, 1, 0, 0.5f, 1, 0.5f, 0}, hy[8] = {0, 0, 1, 1, 0, 0.5f, 1, 0.5f};
            int h = SL.handle;
            float px = b[0] + (b[2] - b[0]) * hx[h], py = b[1] + (b[3] - b[1]) * hy[h];   // the dragged handle
            // the opposite side stays put; with Alt the transformation point does
            float ax = alt ? SL.opivot.x : b[0] + (b[2] - b[0]) * (1 - hx[h]), ay = alt ? SL.opivot.y : b[1] + (b[3] - b[1]) * (1 - hy[h]);
            float sx = 1, sy = 1;
            if (hx[h] != 0.5f && fabsf(px - ax) > 1e-6f) sx = (p.x - ax) / (px - ax);
            if (hy[h] != 0.5f && fabsf(py - ay) > 1e-6f) sy = (p.y - ay) / (py - ay);
            if (shift && h < 4) { float s = fabsf(sx) > fabsf(sy) ? sx : sy; sx = sy = s; }
            if (fabsf(sx) < 0.01f) sx = 0.01f;
            if (fabsf(sy) < 0.01f) sy = 0.01f;
            apply_transform(mat_mul(mat_tr(ax, ay), mat_mul(mat_scale(sx, sy), mat_tr(-ax, -ay))));
            break;
        }
        case M_ROTATE: {
            if (!SL.moved || st_cannot_edit()) break;
            V2 c = SL.opivot;
            float a0 = atan2f(SL.start.y - c.y, SL.start.x - c.x), a1 = atan2f(p.y - c.y, p.x - c.x), da = a1 - a0;
            if (shift) da = roundf(da / (VX_PI / 12)) * (VX_PI / 12);
            apply_transform(mat_mul(mat_tr(c.x, c.y), mat_mul(mat_rot(da), mat_tr(-c.x, -c.y))));
            break;
        }
        case M_SHAPE: {
            if (!SL.moved) break;
            if (!SL.committed) { app_commit(); SL.committed = true; }
            Drawing *d2 = sel_drawing();
            if (d2) shape_drag(d2, v2(p.x - SL.start.x, p.y - SL.start.y));
            break;
        }
        case M_NODE: {
            if (!SL.moved || SL.node_item < 0) break;
            if (!SL.committed) { app_commit(); SL.committed = true; }
            Drawing *d2 = sel_drawing();
            if (!d2 || SL.node_item >= arrlen(d2->items)) break;
            Item *it = &d2->items[SL.node_item];
            if (SL.node < arrlen(it->pts)) { it->pts[SL.node].x = p.x; it->pts[SL.node].y = p.y; item_bounds(it); doc_touch(app.doc); }
            break;
        }
        }
        SL.last = SL.mode == M_LASSO ? SL.last : p;
    }

    if (re->released && SL.mode != M_NONE) {
        dr = sel_drawing();
        if (SL.mode == M_MARQUEE && SL.moved && dr) {
            float x0 = MIN(SL.start.x, p.x), x1 = MAX(SL.start.x, p.x), y0 = MIN(SL.start.y, p.y), y1 = MAX(SL.start.y, p.y);
            V2 R[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            region_select(dr, R, 4);
        }
        if (SL.mode == M_LASSO && dr && arrlen(SL.lasso) > 2) region_select(dr, SL.lasso, (int)arrlen(SL.lasso));
        if (SL.mode == M_SHAPE && !SL.moved && dr) {   // only a click: it selects, as before
            int k = hit_item(dr, p);
            if (k >= 0) {
                Section s;
                if (section_at(dr, k, p, &s)) arrput(SL.secs, s); else sel_add(k);
            }
        }
        if (SL.mode == M_MOVE && !SL.moved && !shift && dr) {   // a click inside a multiple selection picks just that
            int k = hit_item(dr, p);
            if (k >= 0) {
                Section s;
                bool part = tool != 'A' && section_at(dr, k, p, &s);
                clear_items();
                if (part) arrput(SL.secs, s); else sel_add(k);
            }
        }
        SL.mode = M_NONE;
    }

    // cursors
    if (re->hover && SL.mode == M_NONE && dr) {
        bool rot = false;
        int h = tool == 'Q' ? handle_at(dr, m, &rot) : -1;
        if (tool == 'Q' && pivot_hit(dr, m)) ui_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
        else if (h >= 0) {
            static const int cur[8] = {SAPP_MOUSECURSOR_RESIZE_NWSE, SAPP_MOUSECURSOR_RESIZE_NESW, SAPP_MOUSECURSOR_RESIZE_NWSE, SAPP_MOUSECURSOR_RESIZE_NESW,
                                       SAPP_MOUSECURSOR_RESIZE_NS, SAPP_MOUSECURSOR_RESIZE_EW, SAPP_MOUSECURSOR_RESIZE_NS, SAPP_MOUSECURSOR_RESIZE_EW};
            ui_cursor(rot ? CUR_ROTATE : cur[h]);
        } else if (SL.node >= 0) ui_cursor(SAPP_MOUSECURSOR_POINTING_HAND);
        else if (SL.hover >= 0 && point_selected(dr, p)) ui_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
        else if (tool == 'V') {
            int it, pl, sg; float st;
            Drawing *ad = st_drawing(false);
            int kind = ad ? edit_at(ad, p, ui.in.mods & MOD_CTRL, &it, &pl, &sg, &st) : SE_NONE;
            if (kind == SE_CORNER) ui_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
            else if (kind == SE_BEND) ui_cursor(SAPP_MOUSECURSOR_POINTING_HAND);
        }
    }
}

/* ── drawing ── */

// the hatch over selected vectors, drawn into the stage image (m: drawing space -> stage pixels)
static void hatch(DrawList *dl, const Item *it, Mat m, Color c) {
    if (it->kind == IT_STROKE) brush_draw(dl, it, m, c, false);
    else if (it->kind == IT_FILL) fill_draw(dl, it, m, c, false);
}
void select_render(DrawList *dl, Mat m) {
    char t = app.tool;
    if (t != 'V' && t != 'A' && t != 'Q' && t != 'L') return;
    Drawing *dr = st_drawing(false);
    if (!dr) return;
    bool mine = drawing_index(dr) == SL.drawing;
    brush_cover_style(K_STRIPES, roundf(8 * ui.scale));
    if (SL.hover >= 0 && SL.hover < arrlen(dr->items) && !(mine && is_sel(SL.hover))) {
        brush_union_begin();
        if (SL.hsec_on) { Item pc = section_piece(dr, SL.hsec); hatch(dl, &pc, m, 0); item_free(&pc); }
        else hatch(dl, &dr->items[SL.hover], m, 0);
        brush_union_end(dl, col_a(C_SEL, 0x60));
    }
    if (mine && select_count()) {
        brush_union_begin();   // one shading for everything selected: crossings aren't darker
        for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) if (dr->items[SL.idx[k]].kind == IT_FILL) hatch(dl, &dr->items[SL.idx[k]], m, 0);
        for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) if (dr->items[SL.idx[k]].kind == IT_STROKE) hatch(dl, &dr->items[SL.idx[k]], m, 0);
        for (ptrdiff_t k = 0; k < arrlen(SL.secs); k++) { Item pc = section_piece(dr, SL.secs[k]); hatch(dl, &pc, m, 0); item_free(&pc); }
        brush_union_end(dl, col_a(C_SEL, 0xc8));
    }
    brush_cover_style(K_SOLID, 0);
}

static void box_lines(const float b[4], Color c, float w) {
    V2 q[4];
    bbox_screen(b, q);
    for (int i = 0; i < 4; i++) dl_line(ui.dl, q[i], q[(i + 1) % 4], w, c);
}

void select_overlay(void) {
    Drawing *dr = sel_drawing();
    Color ac = C_SEL;
    if (dr) {
        // symbol instances are objects: a box shows them (the hatch is for raw vectors)
        float b[4];
        if (SL.hover >= 0 && SL.hover < arrlen(dr->items) && item_placed(&dr->items[SL.hover]) && !is_sel(SL.hover) && bounds_of(&dr->items[SL.hover], b))
            box_lines(b, col_a(ac, 0x70), 1);
        for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++)
            if (item_placed(&dr->items[SL.idx[k]]) && bounds_of(&dr->items[SL.idx[k]], b)) box_lines(b, ac, 1);
    }
    if (dr && app.tool == 'Q' && select_count()) {
        float b[4];
        V2 c[4];
        if (sel_bounds(dr, b)) {
            bbox_screen(b, c);
            for (int i = 0; i < 4; i++) dl_line(ui.dl, c[i], c[(i + 1) % 4], 1.2f, ac);
            for (int h = 0; h < 8; h++) {
                V2 q = handle_pos(c, h);
                dl_rrect(ui.dl, rect(q.x - 4, q.y - 4, 8, 8), 1.5f, hex(0xffffff));
                dl_rrect_border(ui.dl, rect(q.x - 4, q.y - 4, 8, 8), 1.5f, 1.2f, ac);
            }
            // transformation point
            V2 pv = st_screen(current_pivot(dr));
            dl_circle(ui.dl, pv, 5.5f, hex(0xffffff));
            dl_circle_border(ui.dl, pv, 5.5f, 1.4f, ac);
            dl_circle(ui.dl, pv, 1.5f, ac);
        }
    }
    if (dr && app.tool == 'A' && SL.node_item >= 0 && SL.node_item < arrlen(dr->items)) {
        Item *it = &dr->items[SL.node_item];
        for (ptrdiff_t i = 0; i + 1 < arrlen(it->pts); i++)
            dl_line(ui.dl, st_screen(v2(it->pts[i].x, it->pts[i].y)), st_screen(v2(it->pts[i + 1].x, it->pts[i + 1].y)), 1, col_a(ac, 0x90));
        for (ptrdiff_t i = 0; i < arrlen(it->pts); i++) {
            V2 q = st_screen(v2(it->pts[i].x, it->pts[i].y));
            bool on = i == SL.node;
            dl_rect(ui.dl, rect(q.x - 3.5f, q.y - 3.5f, 7, 7), on ? ac : hex(0xffffff));
            dl_rrect_border(ui.dl, rect(q.x - 3.5f, q.y - 3.5f, 7, 7), 0, 1, on ? hex(0xffffff) : ac);
        }
    }
    if (SL.mode == M_MARQUEE && SL.moved) {
        V2 a = st_screen(SL.start), b = ui.in.mouse;
        Rect r = {MIN(a.x, b.x), MIN(a.y, b.y), fabsf(b.x - a.x), fabsf(b.y - a.y)};
        dl_rect(ui.dl, r, col_a(ac, 0x18));
        dl_rrect_border(ui.dl, r, 0, 1, ac);
    }
    if (SL.mode == M_LASSO && arrlen(SL.lasso) > 1) {
        for (ptrdiff_t i = 0; i + 1 < arrlen(SL.lasso); i++) dl_line(ui.dl, st_screen(SL.lasso[i]), st_screen(SL.lasso[i + 1]), 1.2f, ac);
        dl_line(ui.dl, st_screen(arrlast(SL.lasso)), ui.in.mouse, 1, col_a(ac, 0x80));
    }
}

/* ── commands ── */

void select_all(void) {
    Drawing *dr = sel_drawing();
    if (!dr) return;
    clear_items();
    for (ptrdiff_t k = 0; k < arrlen(dr->items); k++) if (!dr->items[k].erase) arrput(SL.idx, (int)k);
}

void select_delete(void) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count() || st_cannot_edit()) return;
    app_commit();
    materialize();
    dr = sel_drawing();
    for (ptrdiff_t k = arrlen(SL.idx) - 1; k >= 0; k--) { item_free(&dr->items[SL.idx[k]]); arrdel(dr->items, SL.idx[k]); }
    clear_items();
    doc_touch(app.doc);
}

// the color panel moved: the selected vectors take the color right away, as in Umoupen. With only lines
// or only fills selected they all do; with both, the fill color goes to the fills and the stroke color
// to the lines. commit starts a new undo step (once per drag of the wheel)
bool select_recolor(Color c, int target, bool commit) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count() || st_cannot_edit()) return false;
    bool lines = arrlen(SL.secs) > 0, fills = false;
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) {
        Item *it = &dr->items[SL.idx[k]];
        if (it->erase) continue;
        lines |= it->kind == IT_STROKE;
        fills |= it->kind == IT_FILL;
    }
    if (!lines && !fills) return false;
    bool to_lines = !fills || target == 1, to_fills = !lines || target == 0;
    if (commit) app_commit();
    materialize();
    dr = sel_drawing();
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) {
        Item *it = &dr->items[SL.idx[k]];
        if (it->erase) continue;
        if (it->kind == IT_STROKE && to_lines) it->color = c;
        if (it->kind == IT_FILL && to_fills) { it->color = c; it->grad = 0; it->sym = 0; arrfree(it->stops); }
    }
    doc_touch(app.doc);
    return true;
}

void select_copy(bool cut) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count()) return;
    for (ptrdiff_t i = 0; i < arrlen(CLIP); i++) item_free(&CLIP[i]);
    arrsetlen(CLIP, 0);
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) arrput(CLIP, item_copy(&dr->items[SL.idx[k]]));
    for (ptrdiff_t k = 0; k < arrlen(SL.secs); k++) arrput(CLIP, section_piece(dr, SL.secs[k]));   // copying leaves the strokes whole
    if (cut) select_delete();
    else app_toast("Copied %d item%s", (int)arrlen(CLIP), arrlen(CLIP) == 1 ? "" : "s");
}

void select_paste(bool in_place) {
    if (!arrlen(CLIP)) { app_toast("The clipboard is empty"); return; }
    const char *why = st_cannot_edit();
    if (why) { app_toast("%s", why); return; }
    app_commit();
    Drawing *dr = st_drawing(true);
    SL.drawing = drawing_index(dr);
    clear_items();
    float off = in_place ? 0 : 12;
    for (ptrdiff_t i = 0; i < arrlen(CLIP); i++) {
        Item it = {0};
        transform_item(&it, &CLIP[i], mat_tr(off, off));
        arrput(dr->items, it);
        arrput(SL.idx, (int)arrlen(dr->items) - 1);
    }
    doc_touch(app.doc);
}

static void nudge(float dx, float dy) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count() || st_cannot_edit()) return;
    app_commit();
    materialize();
    dr = sel_drawing();
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) { Item c = item_copy(&dr->items[SL.idx[k]]); transform_item(&dr->items[SL.idx[k]], &c, mat_tr(dx, dy)); item_free(&c); }
    if (SL.pivot_on) { SL.pivot.x += dx; SL.pivot.y += dy; }
    SL.pivot_sig = sel_sig();
    doc_touch(app.doc);
}

void select_convert_to_symbol(void) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count()) { app_toast("Select something to convert to a symbol"); return; }
    if (st_cannot_edit()) { app_toast("%s", st_cannot_edit()); return; }
    app_commit();
    materialize();
    Doc *d = app.doc;
    dr = sel_drawing();
    float b[4];
    sel_bounds(dr, b);
    V2 c = v2(roundf((b[0] + b[2]) / 2), roundf((b[1] + b[3]) / 2));
    char name[64];
    snprintf(name, sizeof(name), "Symbol %d", (int)arrlen(d->symbols) + 1);
    int sid = doc_add_symbol(d, name, SYM_GRAPHIC);
    Symbol *s = doc_symbol(d, sid);
    Layer *l = &s->tl.layers[0];
    Drawing *sd = &d->drawings[l->keys[0].d];
    dr = sel_drawing();   // drawings may have moved
    for (ptrdiff_t k = 0; k < arrlen(SL.idx); k++) { Item it = {0}; transform_item(&it, &dr->items[SL.idx[k]], mat_tr(-c.x, -c.y)); arrput(sd->items, it); }
    int at = SL.idx[0];
    for (ptrdiff_t k = arrlen(SL.idx) - 1; k >= 0; k--) { item_free(&dr->items[SL.idx[k]]); arrdel(dr->items, SL.idx[k]); }
    Item inst = {.kind = IT_INSTANCE, .sym = sid, .first = 1, .loop = LOOP_LOOP, .m = mat_tr(c.x, c.y), .alpha = 1};
    arrins(dr->items, at, inst);
    arrsetlen(SL.idx, 0);
    arrput(SL.idx, at);
    doc_touch(d);
    app_toast("Converted to %s", name);
}

void select_break_apart(void) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count()) return;
    if (st_cannot_edit()) { app_toast("%s", st_cannot_edit()); return; }
    app_commit();
    materialize();
    Doc *d = app.doc;
    dr = sel_drawing();
    int *newsel = NULL;
    for (ptrdiff_t k = arrlen(SL.idx) - 1; k >= 0; k--) {
        int at = SL.idx[k];
        Item inst = dr->items[at];
        if (inst.kind != IT_INSTANCE) { arrput(newsel, at); continue; }
        Timeline *t = doc_symbol_tl(d, inst.sym);
        int f = symbol_frame(d, &inst, 0);
        Item *out = NULL;
        for (ptrdiff_t li = arrlen(t ? t->layers : NULL) - 1; li >= 0; li--) {
            Layer *l = &t->layers[li];
            if (l->type == LT_FOLDER || tl_hidden(t, l)) continue;
            Key *key = doc_key_for(l, f);
            if (!key || key->d < 0) continue;
            Drawing *src = &d->drawings[key->d];
            for (ptrdiff_t i = 0; i < arrlen(src->items); i++) { Item it = {0}; transform_item(&it, &src->items[i], inst.m); arrput(out, it); }
        }
        dr = sel_drawing();
        item_free(&dr->items[at]);
        arrdel(dr->items, at);
        for (ptrdiff_t i = arrlen(out) - 1; i >= 0; i--) arrins(dr->items, at, out[i]);
        for (ptrdiff_t i = 0; i < arrlen(out); i++) arrput(newsel, at + (int)i);
        arrfree(out);
    }
    arrsetlen(SL.idx, 0);
    for (ptrdiff_t i = 0; i < arrlen(newsel); i++) sel_add(newsel[i]);
    arrfree(newsel);
    doc_touch(d);
}

// a placement from the Library (screen x < 0: the middle of the stage)
static void place(Item inst, V2 screen) {
    const char *why = st_cannot_edit();
    if (why) { app_toast("%s", why); return; }
    app_commit();
    Drawing *dr = st_drawing(true);
    if (screen.x < 0) {
        Doc *d = app.doc;
        float b[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        V2 c = mat_ap(mat_inv(doc_edit_matrix(d)), v2(d->w / 2.0f, d->h / 2.0f));
        if (item_bounds_m(d, &inst, 0, mat_id(), b, 0)) inst.m.tx += roundf(c.x - (b[0] + b[2]) / 2), inst.m.ty += roundf(c.y - (b[1] + b[3]) / 2);
    } else {
        V2 p = st_local(screen);
        inst.m.tx += roundf(p.x); inst.m.ty += roundf(p.y);
    }
    arrput(dr->items, inst);
    SL.drawing = drawing_index(dr);
    clear_items();
    arrput(SL.idx, (int)arrlen(dr->items) - 1);
    doc_touch(app.doc);
    app.tool = 'V';
}
void library_place(int sym, V2 screen) {
    if (doc_editing(app.doc) == sym) { app_toast("A symbol can't contain itself"); return; }
    place((Item){.kind = IT_INSTANCE, .sym = sym, .first = 1, .loop = LOOP_LOOP, .m = mat_id(), .alpha = 1}, screen);
}
void bitmap_place(int id, V2 screen) {   // dropped: its middle under the mouse
    Bitmap *b = bitmap_get(id);
    if (!b) return;
    Mat m = screen.x < 0 ? mat_id() : mat_tr(-roundf(b->w / 2.0f), -roundf(b->h / 2.0f));
    place((Item){.kind = IT_BITMAP, .sym = id, .m = m, .alpha = 1}, screen);
}

void symbol_edit(int sym) {
    Doc *d = app.doc;
    while (d->nedit) doc_exit_symbol(d);
    select_clear();
    doc_enter_symbol(d, sym, mat_tr(d->w / 2.0f, d->h / 2.0f));
}

void select_shortcuts(void) {
    Input *in = &ui.in;
    if (act(ACT_SELECT_ALL)) select_all();
    if (act(ACT_COPY)) select_copy(false);
    if (act(ACT_CUT)) select_copy(true);
    if (act(ACT_PASTE)) select_paste(false);
    if (act(ACT_PASTE_IN_PLACE)) select_paste(true);
    if (act(ACT_DUPLICATE)) { select_copy(false); select_paste(false); }
    if (act(ACT_BREAK)) select_break_apart();
    if (act(ACT_SYMBOL)) select_convert_to_symbol();
    if (act(ACT_DELETE)) select_delete();
    float st = (in->mods & MOD_SHIFT) ? 10 : 1;
    if (select_count()) {
        if (act(ACT_NUDGE_LEFT)) nudge(-st, 0);
        if (act(ACT_NUDGE_RIGHT)) nudge(st, 0);
        if (act(ACT_NUDGE_UP)) nudge(0, -st);
        if (act(ACT_NUDGE_DOWN)) nudge(0, st);
    }
    if (in->key_pressed[SAPP_KEYCODE_ESCAPE] && !in->mods) {
        if (select_count()) select_clear();
        else if (app.doc->nedit) doc_exit_symbol(app.doc);
    }
}

// for the self-tests: selection bounds and the transformation point (drawing space)
bool select_info(float b[4], V2 *pivot) {
    Drawing *dr = sel_drawing();
    if (!dr || !select_count() || !sel_bounds(dr, b)) return false;
    if (pivot) *pivot = current_pivot(dr);
    return true;
}
