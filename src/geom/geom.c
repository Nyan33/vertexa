#include "geom/geom.h"
#include "brush/brush.h"

float dist_point_seg(V2 p, V2 a, V2 b, float *t) {
    float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
    float u = L2 > 1e-12f ? clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / L2, 0, 1) : 0;
    if (t) *t = u;
    return hypotf(p.x - (a.x + dx * u), p.y - (a.y + dy * u));
}

bool seg_isect(V2 a, V2 b, V2 c, V2 d, float *t, float *u) {
    float rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-12f) return false;
    float qx = c.x - a.x, qy = c.y - a.y;
    float tt = (qx * sy - qy * sx) / den, uu = (qx * ry - qy * rx) / den;
    if (tt < 0 || tt > 1 || uu < 0 || uu > 1) return false;
    if (t) *t = tt;
    if (u) *u = uu;
    return true;
}

bool point_in_poly(const V2 *poly, const int *starts, int npoly, int nstarts, V2 p) {
    bool in = false;
    for (int c = 0; c < nstarts; c++) {
        int a = starts[c], b = c + 1 < nstarts ? starts[c + 1] : npoly;
        for (int i = a, j = b - 1; i < b; j = i++) {
            V2 pi = poly[i], pj = poly[j];
            if ((pi.y > p.y) != (pj.y > p.y) && p.x < (pj.x - pi.x) * (p.y - pi.y) / (pj.y - pi.y) + pi.x) in = !in;
        }
    }
    return in;
}

Path path_of(const Item *st, float step) {
    Path P = {0};
    SPt *r = brush_resample(st, step);
    int n = (int)arrlen(r);
    if (n) memcpy(arraddnptr(P.p, n), r, sizeof(SPt) * n);
    arrsetlen(P.s, n);
    float acc = 0;
    for (int i = 0; i < n; i++) {
        if (i) acc += hypotf(P.p[i].x - P.p[i - 1].x, P.p[i].y - P.p[i - 1].y);
        P.s[i] = acc;
    }
    P.len = acc;
    return P;
}
void path_free(Path *p) { arrfree(p->p); arrfree(p->s); p->len = 0; }

SPt path_at(const Path *p, float s) {
    int n = (int)arrlen(p->p);
    if (!n) return (SPt){0};
    if (s <= 0) return p->p[0];
    if (s >= p->len) return p->p[n - 1];
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (p->s[mid] <= s) lo = mid; else hi = mid; }
    float seg = p->s[hi] - p->s[lo], t = seg > 1e-9f ? (s - p->s[lo]) / seg : 0;
    SPt a = p->p[lo], b = p->p[hi];
    return (SPt){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.r + (b.r - a.r) * t};
}

float path_project(const Path *p, V2 q, float *dist) {
    int n = (int)arrlen(p->p);
    float best = 1e30f, bs = 0;
    for (int i = 0; i + 1 < n; i++) {
        float t, dd = dist_point_seg(q, v2(p->p[i].x, p->p[i].y), v2(p->p[i + 1].x, p->p[i + 1].y), &t);
        if (dd < best) { best = dd; bs = p->s[i] + (p->s[i + 1] - p->s[i]) * t; }
    }
    if (n == 1) { best = hypotf(q.x - p->p[0].x, q.y - p->p[0].y); bs = 0; }
    if (dist) *dist = best;
    return bs;
}

bool item_hit(const Item *it, V2 p, float tol) {
    if (p.x < it->bx0 - tol || p.y < it->by0 - tol || p.x > it->bx1 + tol || p.y > it->by1 + tol) return false;
    if (it->kind == IT_FILL) return point_in_poly(it->poly, it->starts, (int)arrlen(it->poly), (int)arrlen(it->starts), p);
    if (it->kind != IT_STROKE) return false;
    SPt *r = brush_resample(it, 1.0f);
    int n = (int)arrlen(r);
    for (int i = 0; i < n; i++) {
        SPt a = r[i], b = i + 1 < n ? r[i + 1] : a;
        float t, dd = dist_point_seg(p, v2(a.x, a.y), v2(b.x, b.y), &t);
        float rr = a.r + (b.r - a.r) * t;
        if (it->tip != TIP_ROUND) rr *= 1.2f;
        if (dd <= rr + tol) return true;
    }
    return false;
}

SPt *simplify_spt(const SPt *p, int n, float eps) {
    SPt *out = NULL;
    if (n <= 2) { for (int i = 0; i < n; i++) arrput(out, p[i]); return out; }
    u8 *keep = calloc(n, 1);
    int *stack = malloc(sizeof(int) * 2 * n), sp = 0;
    keep[0] = keep[n - 1] = 1;
    stack[sp++] = 0; stack[sp++] = n - 1;
    while (sp) {
        int bi = stack[--sp], ai = stack[--sp];
        SPt a = p[ai], b = p[bi];
        float md = 0; int mi = -1;
        for (int i = ai + 1; i < bi; i++) {
            float t, d = dist_point_seg(v2(p[i].x, p[i].y), v2(a.x, a.y), v2(b.x, b.y), &t);
            d += fabsf(p[i].r - (a.r + (b.r - a.r) * t)) * 1.5f;
            if (d > md) { md = d; mi = i; }
        }
        if (md > eps && mi > 0) { keep[mi] = 1; stack[sp++] = ai; stack[sp++] = mi; stack[sp++] = mi; stack[sp++] = bi; }
    }
    for (int i = 0; i < n; i++) if (keep[i]) arrput(out, p[i]);
    free(keep); free(stack);
    return out;
}

V2 *simplify_v2(const V2 *p, int n, float eps, bool closed) {
    SPt *tmp = NULL;
    for (int i = 0; i < n; i++) arrput(tmp, ((SPt){p[i].x, p[i].y, 0}));
    if (closed && n > 2) arrput(tmp, ((SPt){p[0].x, p[0].y, 0}));
    SPt *s = simplify_spt(tmp, (int)arrlen(tmp), eps);
    V2 *out = NULL;
    int m = (int)arrlen(s) - (closed && n > 2 ? 1 : 0);
    for (int i = 0; i < m; i++) arrput(out, v2(s[i].x, s[i].y));
    arrfree(tmp); arrfree(s);
    return out;
}

Item stroke_from_dense(const SPt *pts, int n, const Item *style, float eps) {
    Item o = *style;
    o.pts = simplify_spt(pts, n, eps);
    o.poly = NULL; o.starts = NULL; o.filters = NULL; o.stops = NULL;
    o.flags &= (u8)~ITF_CLOSED;
    item_bounds(&o);
    return o;
}

/* ── contour tracing: directed boundary edges (filled on the left) linked into loops ── */

typedef struct { int key; int value; } KV;

void trace_mask(const u8 *mask, int w, int h, V2 **poly, int **starts) {
    // vertices are lattice points (x, y) with 0 <= x <= w, 0 <= y <= h; key = y * (w + 1) + x
    int W1 = w + 1;
    int *from = NULL, *to = NULL;
#define M(x, y) ((x) >= 0 && (y) >= 0 && (x) < w && (y) < h && mask[(y) * w + (x)])
    for (int y = 0; y <= h; y++)
        for (int x = 0; x <= w; x++) {
            // horizontal edge between rows y-1 and y at column x..x+1
            if (x < w) {
                bool up = M(x, y - 1), dn = M(x, y);
                if (dn && !up) { arrput(from, y * W1 + x); arrput(to, y * W1 + x + 1); }        // filled below, walk right... keep filled on the left in y-down
                else if (up && !dn) { arrput(from, y * W1 + x + 1); arrput(to, y * W1 + x); }
            }
            if (y < h) {
                bool lf = M(x - 1, y), rt = M(x, y);
                if (lf && !rt) { arrput(from, y * W1 + x); arrput(to, (y + 1) * W1 + x); }
                else if (rt && !lf) { arrput(from, (y + 1) * W1 + x); arrput(to, y * W1 + x); }
            }
        }
#undef M
    int ne = (int)arrlen(from);
    KV *map = NULL;   // start vertex -> first edge index (lists through next[])
    hmdefault(map, -1);
    int *next = malloc(sizeof(int) * (ne ? ne : 1));
    for (int i = 0; i < ne; i++) { int head = hmget(map, from[i]); next[i] = head; hmput(map, from[i], i); }
    u8 *used = calloc(ne ? ne : 1, 1);
    for (int i = 0; i < ne; i++) {
        if (used[i]) continue;
        arrput(*starts, (int)arrlen(*poly));
        int e = i;
        while (e >= 0 && !used[e]) {
            used[e] = 1;
            int v = from[e];
            arrput(*poly, v2((float)(v % W1), (float)(v / W1)));
            // next edge starting where this one ends (prefer an unused one)
            int cand = hmget(map, to[e]), pick = -1;
            for (int c = cand; c >= 0; c = next[c]) if (!used[c]) { pick = c; break; }
            e = pick;
        }
    }
    hmfree(map);
    free(next); free(used);
    arrfree(from); arrfree(to);
}
