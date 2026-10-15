// Flash / Animate import: binary .fla (Flash 5 ... CS4, OLE2 container), XFL in a zip (.fla from
// CS5 on) and unpacked XFL folders (.xfl). Ported from Vertexa v1 (docs/FLA_FORMAT.md describes the
// binary layouts). Shapes become fills (contours chained from the edges per fill style) and
// strokes; colours keep their alpha, instances keep their alpha multiplier.
#include "app/app.h"
#include "io/xml.h"
#include <windows.h>
#include <ctype.h>

#define TWIPS 20.0f

/* ── shapes: Flash edges -> items ── */

typedef struct { V2 a, c, b; bool quad; int fl, fr, st; } FEdge;
typedef struct { u8 kind, spread; GStop *stops; } FGrad;
typedef struct { FEdge *edges; Color *fills; int *fill_bm; Mat *fill_m; FGrad *fill_gr; Color *stroke_col; float *stroke_w; } FShape;   // bitmap and gradient fills

static void fshape_free(FShape *s) {
    for (ptrdiff_t i = 0; i < arrlen(s->fill_gr); i++) arrfree(s->fill_gr[i].stops);
    arrfree(s->edges); arrfree(s->fills); arrfree(s->fill_bm); arrfree(s->fill_m); arrfree(s->fill_gr); arrfree(s->stroke_col); arrfree(s->stroke_w);
    memset(s, 0, sizeof(*s));
}

static void flatten(const FEdge *e, bool reverse, V2 **out, bool skip_first) {
    V2 a = reverse ? e->b : e->a, b = reverse ? e->a : e->b, c = e->c;
    if (!skip_first) arrput(*out, a);
    if (!e->quad) { arrput(*out, b); return; }
    float len = hypotf(c.x - a.x, c.y - a.y) + hypotf(b.x - c.x, b.y - c.y);
    int n = (int)CLAMP(len / 3, 2, 32);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        arrput(*out, v2(u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y));
    }
}

static u64 pkey(V2 p) { return ((u64)(u32)(i32)lroundf(p.x * 32) << 32) | (u32)(i32)lroundf(p.y * 32); }
typedef struct { u64 key; int value; } PKV;

static void shape_items(FShape *s, Mat m, Item **out) {
    for (ptrdiff_t i = 0; i < arrlen(s->edges); i++) { FEdge *e = &s->edges[i]; e->a = mat_ap(m, e->a); e->b = mat_ap(m, e->b); e->c = mat_ap(m, e->c); }
    float ms = mat_scalef(m);
    // fills: per style, edges oriented so the style is on one side, chained into loops
    for (int fsi = 1; fsi <= (int)arrlen(s->fills); fsi++) {
        int *ids = NULL; u8 *rev = NULL;
        for (ptrdiff_t i = 0; i < arrlen(s->edges); i++) {
            FEdge *e = &s->edges[i];
            if (e->fl == fsi && e->fr != fsi) { arrput(ids, (int)i); arrput(rev, 0); }
            else if (e->fr == fsi && e->fl != fsi) { arrput(ids, (int)i); arrput(rev, 1); }
        }
        int n = (int)arrlen(ids);
        if (!n) { arrfree(ids); arrfree(rev); continue; }
        PKV *map = NULL; hmdefault(map, -1);
        int *next = malloc(sizeof(int) * n);
        for (int i = 0; i < n; i++) {
            FEdge *e = &s->edges[ids[i]];
            u64 k = pkey(rev[i] ? e->b : e->a);
            next[i] = hmget(map, k);
            hmput(map, k, i);
        }
        u8 *used = calloc(n, 1);
        Item f = {.kind = IT_FILL, .color = s->fills[fsi - 1]};
        if (fsi - 1 < arrlen(s->fill_bm) && s->fill_bm[fsi - 1]) { f.sym = s->fill_bm[fsi - 1]; f.m = mat_mul(m, s->fill_m[fsi - 1]); }
        if (fsi - 1 < arrlen(s->fill_gr) && s->fill_gr[fsi - 1].kind) {
            FGrad *g = &s->fill_gr[fsi - 1];
            f.grad = g->kind; f.spread = g->spread; f.m = mat_mul(m, s->fill_m[fsi - 1]);
            for (ptrdiff_t q = 0; q < arrlen(g->stops); q++) arrput(f.stops, g->stops[q]);
        }
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            arrput(f.starts, (int)arrlen(f.poly));
            int cur = i; bool first = true;
            V2 start = rev[i] ? s->edges[ids[i]].b : s->edges[ids[i]].a;
            while (cur >= 0 && !used[cur]) {
                used[cur] = 1;
                FEdge *e = &s->edges[ids[cur]];
                flatten(e, rev[cur], &f.poly, !first);
                first = false;
                V2 end = rev[cur] ? e->a : e->b;
                if (pkey(end) == pkey(start)) break;
                int c = hmget(map, pkey(end)), pick = -1;
                for (; c >= 0; c = next[c]) if (!used[c]) { pick = c; break; }
                cur = pick;
            }
            if (arrlen(f.poly) - arrlast(f.starts) < 3) { arrsetlen(f.poly, arrlast(f.starts)); arrpop(f.starts); }
        }
        hmfree(map); free(next); free(used); arrfree(ids); arrfree(rev);
        if (arrlen(f.poly)) { item_bounds(&f); arrput(*out, f); } else item_free(&f);
    }
    // strokes: consecutive edges of a style chained into polylines
    for (int si = 1; si <= (int)arrlen(s->stroke_w); si++) {
        int *ids = NULL;
        for (ptrdiff_t i = 0; i < arrlen(s->edges); i++) if (s->edges[i].st == si) arrput(ids, (int)i);
        int n = (int)arrlen(ids);
        u8 *used = calloc(n ? n : 1, 1);
        float r = MAX(0.5f, s->stroke_w[si - 1] * ms / 2);
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            V2 *pts = NULL;
            used[i] = 1;
            flatten(&s->edges[ids[i]], false, &pts, false);
            for (bool grew = true; grew;) {
                grew = false;
                V2 end = arrlast(pts);
                for (int j = 0; j < n; j++) {
                    if (used[j]) continue;
                    FEdge *e = &s->edges[ids[j]];
                    if (pkey(e->a) == pkey(end)) { used[j] = 1; flatten(e, false, &pts, true); grew = true; break; }
                    if (pkey(e->b) == pkey(end)) { used[j] = 1; flatten(e, true, &pts, true); grew = true; break; }
                }
            }
            Item st = {.kind = IT_STROKE, .tip = TIP_ROUND, .aspect = 1, .color = s->stroke_col[si - 1], .flags = ITF_LINEAR};
            int np = (int)arrlen(pts);
            if (np > 2 && pkey(pts[0]) == pkey(pts[np - 1])) { st.flags |= ITF_CLOSED; np--; }
            for (int k = 0; k < np; k++) arrput(st.pts, ((SPt){pts[k].x, pts[k].y, r}));
            arrfree(pts);
            item_bounds(&st);
            arrput(*out, st);
        }
        free(used); arrfree(ids);
    }
}

/* ── import context ── */

typedef struct { char key[256]; int sym; } NameMap;
typedef struct {
    Doc *d;
    char warn[256];
    NameMap *names;     // XFL: library name -> symbol id
    NameMap *images;    // XFL: library name -> bitmap id
    struct { u32 ref; int sym; } *refs;   // binary: Flash symbol number -> id
    int instances, shapes;
} Ctx;

static void warn(Ctx *c, const char *w) { if (!c->warn[0]) snprintf(c->warn, sizeof(c->warn), "%s", w); }

static Layer new_layer(Ctx *c, const char *name, u8 type) {
    Layer l = {0};
    l.id = c->d->next_id++;
    l.type = type;
    l.opacity = 1;
    l.color = hex(0x4fff4f);
    snprintf(l.name, sizeof(l.name), "%s", name && name[0] ? name : "Layer");
    return l;
}

static int add_drawing(Ctx *c, Item *items) {
    int di = doc_new_drawing(c->d);
    for (ptrdiff_t i = 0; i < arrlen(items); i++) arrput(c->d->drawings[di].items, items[i]);
    return di;
}

/* ════════════════════════ binary FLA ════════════════════════ */

typedef struct { const u8 *p; size_t n, pos; bool bad; } Rd;
static u8 r8(Rd *r) { if (r->pos + 1 > r->n) { r->bad = true; return 0; } return r->p[r->pos++]; }
static u16 r16(Rd *r) { if (r->pos + 2 > r->n) { r->bad = true; r->pos = r->n; return 0; } u16 v = r->p[r->pos] | (r->p[r->pos + 1] << 8); r->pos += 2; return v; }
static u32 r32(Rd *r) { if (r->pos + 4 > r->n) { r->bad = true; r->pos = r->n; return 0; } u32 v; memcpy(&v, r->p + r->pos, 4); r->pos += 4; return v; }
static void rskip(Rd *r, size_t k) { if (r->pos + k > r->n) { r->bad = true; r->pos = r->n; } else r->pos += k; }
static void rstring(Rd *r, char *out, int cap) {
    out[0] = 0;
    u8 b = r8(r);
    u32 n; bool wide = false;
    if (b < 0xff) n = b;
    else {
        u16 ext = r16(r);
        if (ext == 0xfffe) { wide = true; n = r8(r); if (n == 0xff) n = r16(r); }
        else if (ext == 0xffff) n = r32(r);
        else n = ext;
    }
    if (r->bad) return;
    size_t bytes = (size_t)n * (wide ? 2 : 1);
    if (r->pos + bytes > r->n) { r->bad = true; return; }
    wchar_t *w = malloc(sizeof(wchar_t) * (n + 1));
    for (u32 i = 0; i < n; i++) w[i] = wide ? (wchar_t)(r->p[r->pos + i * 2] | (r->p[r->pos + i * 2 + 1] << 8)) : (wchar_t)r->p[r->pos + i];
    w[n] = 0;
    r->pos += bytes;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    free(w);
}
static Mat rmatrix(Rd *r) {
    Mat m;
    m.a = (i32)r32(r) / 65536.0f; m.b = (i32)r32(r) / 65536.0f; m.c = (i32)r32(r) / 65536.0f; m.d = (i32)r32(r) / 65536.0f;
    m.tx = (i32)r32(r) / TWIPS; m.ty = (i32)r32(r) / TWIPS;
    return m;
}
static Color rcolor(Rd *r) { u32 v = r32(r); return rgba(v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255); }

enum { PO_PAGE, PO_LAYER, PO_FRAME, PO_SHAPE, PO_SYMBOL };
typedef struct PObj PObj;
struct PObj {
    int kind;
    PObj **kids;
    // shape / frame
    FShape shape; Mat shape_m;
    int span, flags; char label[64];
    // layer
    char name[128]; bool locked, hidden; Color color; int mode; PObj *parent;
    // symbol instance
    Mat m; int first, loop; float alpha; u32 ref; char cls[32];
};

typedef struct { bool is_class; char cls[64]; PObj *obj; } TabEnt;
typedef struct { Rd r; TabEnt *tab; PObj **all; Ctx *c; } Ar;

static PObj *read_object(Ar *a);

static void fill_style(Ar *a, bool caps, int ds, Color *out) {
    Rd *r = &a->r;
    Color color = rcolor(r);
    u8 sub = r8(r); r8(r);
    int sel = sub & 0x70;
    *out = color;
    if (sel & 0x10) {   // gradient: keep the first stop colour
        rmatrix(r);
        int n = r8(r);
        if (caps) { r16(r); r8(r); }
        if (ds >= 5) rskip(r, 5);
        for (int i = 0; i < n; i++) { r8(r); Color c = rcolor(r); if (i == 0) *out = c; }
        warn(a->c, "Gradients are imported as their first colour");
        return;
    }
    if (sel & 0x40) { rmatrix(r); r32(r); warn(a->c, "Bitmap fills are imported as a solid colour"); return; }
    if (sel & 0x20) { rmatrix(r); r32(r); rskip(r, 8); }
}

static void line_style(Ar *a, bool caps, int ds, Color *col, float *w) {
    Rd *r = &a->r;
    *col = rcolor(r);
    int width = r16(r);
    rskip(r, 4);
    *w = width == 0 ? 1.0f : width / TWIPS;
    if (caps) {
        r8(r); r8(r); r8(r); r8(r); r16(r);
        Color pc; fill_style(a, caps, ds, &pc);
    }
}

static void shape_data(Ar *a, FShape *g, int shape_schema) {
    Rd *r = &a->r;
    bool caps = shape_schema > 2;
    int ds = r8(r);
    r32(r);
    int nf = r16(r);
    for (int i = 0; i < nf && !r->bad; i++) { Color c; fill_style(a, caps, ds, &c); arrput(g->fills, c); }
    int nl = r16(r);
    for (int i = 0; i < nl && !r->bad; i++) { Color c; float w; line_style(a, caps, ds, &c, &w); arrput(g->stroke_col, c); arrput(g->stroke_w, w); }
    double unit = 1.0 / (TWIPS * (shape_schema > 2 ? 256.0 : 128.0));
    bool extra = shape_schema >= 6;
    i64 x = 0, y = 0;
    int line = 0, fl = 0, fr = 0;
    for (int guard = 0; guard < 10000000 && !r->bad; guard++) {
        u8 f = r8(r);
        if (!f) break;
        if (f & 0x40) {
            if (f & 0x80) { line = r8(r); fr = r8(r); fl = r8(r); }
            else { line = r16(r) & 0x7fff; fr = r16(r) & 0x7fff; fl = r16(r) & 0x7fff; }
        }
        i64 d[3][2];
        for (int k = 0; k < 3; k++) {
            int t = (f >> (2 * k)) & 3;
            if (t == 0) d[k][0] = d[k][1] = 0;
            else if (t == 1) { d[k][0] = (int16_t)r16(r); d[k][1] = (int16_t)r16(r); }
            else if (t == 2) { d[k][0] = (i32)r32(r); d[k][1] = (i32)r32(r); }
            else { d[k][0] = (i64)(int16_t)r16(r) * 128; d[k][1] = (i64)(int16_t)r16(r) * 128; }
        }
        bool straight = (f & 0x0c) == 0;
        if (straight && extra) r8(r);
        i64 fx = x + d[0][0], fy = y + d[0][1], tx = fx + d[2][0], ty = fy + d[2][1];
        x = tx; y = ty;
        int s = line >= 0 && line <= (int)arrlen(g->stroke_w) ? line : 0;
        int l = fl >= 0 && fl <= (int)arrlen(g->fills) ? fl : 0, rr = fr >= 0 && fr <= (int)arrlen(g->fills) ? fr : 0;
        if ((!l && !rr && !s) || (straight && fx == tx && fy == ty)) continue;
        FEdge e = {v2((float)(fx * unit), (float)(fy * unit)), v2((float)((fx + d[1][0]) * unit), (float)((fy + d[1][1]) * unit)),
                   v2((float)(tx * unit), (float)(ty * unit)), !straight, l, rr, s};
        arrput(g->edges, e);
    }
    if (ds > 4 && !r->bad) {
        i32 n = (i32)r32(r);
        if (n < 0 || n > 10000000) { r->bad = true; return; }
        for (i32 i = 0; i < n && !r->bad; i++) {
            if (shape_schema >= 6) { rskip(r, 32); int k = r8(r); rskip(r, (size_t)k * 10); u8 fl2 = r8(r); if (fl2 & 1) rskip(r, 8); if (fl2 & 2) rskip(r, 8); }
            else rskip(r, 32);
        }
    }
}

static void base(Ar *a, PObj *o) {
    Rd *r = &a->r;
    int schema = r8(r); r8(r);
    for (int guard = 0; guard < 1000000 && !r->bad; guard++) { PObj *k = read_object(a); if (!k) break; arrput(o->kids, k); }
    if (schema >= 1) { r32(r); r32(r); }
    if (schema >= 3) r8(r);
    if (schema >= 4) r8(r);
}

static void timeline_sub(Ar *a) {
    Rd *r = &a->r;
    u32 type = r32(r), format = r32(r);
    if (type >= 1) { r32(r); u32 n = r32(r); if (n > 100000) { r->bad = true; return; } rskip(r, (size_t)n * 4); }
    if (format == 1 && type >= 4) { if (type >= 5) r32(r); char s[256]; rstring(r, s, sizeof(s)); }
    else if (format == 0) { r32(r); u32 n = r32(r); if (n > 100000) { r->bad = true; return; } rskip(r, (size_t)n * 4); }
}

static void shape_tail(Ar *a, PObj *s) {
    int sch = r8(&a->r);
    s->shape_m = rmatrix(&a->r);
    shape_data(a, &s->shape, sch);
}

static void frame_tail(Ar *a, PObj *f) {
    Rd *r = &a->r;
    int fs = r8(r);
    f->span = r16(r);
    f->flags = fs > 2 ? r16(r) : r8(r);
    if (fs > 1) r16(r);
    if (fs > 4) r16(r);
    if (fs > 5) { int n = r16(r); rskip(r, (size_t)n * 8); }
    if (fs > 6) { r16(r); r8(r); r32(r); r32(r); }
    if (fs > 7) r16(r);
    if (fs > 8) {
        if (fs >= 23) rstring(r, f->label, sizeof(f->label));
        if (fs >= 19) timeline_sub(a);
        else { r->bad = true; warn(a->c, "This FLA uses a frame layout that isn't supported yet"); return; }
        if (fs > 10) {
            r32(r); r32(r);
            if (fs > 11) r32(r);
            if (fs > 12 && read_object(a)) warn(a->c, "Shape tweens were imported as keyframes");
            if (fs > 13) r32(r);
            if (fs > 14) read_object(a);
            if (fs > 15 && fs >= 23) { char s[256]; rstring(r, s, sizeof(s)); }
            if (fs > 19) r32(r);
            if (fs > 20) r32(r);
            if (fs >= 22) r32(r);
            if (fs >= 24) { r32(r); r32(r); }
        }
    }
}

static void symbol_obj(Ar *a, PObj *s) {
    Rd *r = &a->r;
    base(a, s);
    int schema = r8(r);
    s->m = rmatrix(r);
    if (schema < 22) { r->bad = true; warn(a->c, "This FLA uses an old symbol layout that isn't supported yet"); return; }
    s->first = r16(r);
    s->loop = r16(r);
    s->alpha = 1;
    if (r8(r)) {
        int16_t v[8];
        for (int i = 0; i < 8; i++) v[i] = (int16_t)r16(r);
        s->alpha = clampf(v[6] / 256.0f + v[7] / 255.0f, 0, 1);
    }
    r16(r); r16(r); r32(r);
    char nm[128]; rstring(r, nm, sizeof(nm));
    s->ref = r32(r);
    rskip(r, 3);
    if (r8(r)) { u32 n = r32(r); if (n > 64) { r->bad = true; return; } rskip(r, (size_t)n * 48); }
    r8(r); r16(r); rskip(r, 64); rskip(r, 24); r32(r); r32(r); rskip(r, 6);
    if (!strcmp(s->cls, "CPicSprite") || !strcmp(s->cls, "CPicButton")) {
        r8(r); timeline_sub(a);
        char t[256]; rstring(r, t, sizeof(t)); r32(r); rskip(r, 18);
        char *big = malloc(65536); rstring(r, big, 65536); free(big);
    }
}

static PObj *read_object(Ar *a) {
    Rd *r = &a->r;
    if (r->bad) return NULL;
    u16 tag = r16(r);
    if (!tag) return NULL;
    char cls[64] = "";
    if (tag == 0xffff) {
        r16(r);
        u16 n = r16(r);
        if (n > 63) { r->bad = true; return NULL; }
        memcpy(cls, r->p + r->pos, n); cls[n] = 0; rskip(r, n);
        TabEnt e = {.is_class = true}; snprintf(e.cls, sizeof(e.cls), "%s", cls);
        arrput(a->tab, e);
    } else {
        u32 idx; bool cl;
        if (tag == 0x7fff) { u32 i = r32(r); cl = i & 0x80000000u; idx = i & 0x7fffffffu; }
        else { cl = tag & 0x8000; idx = tag & 0x7fff; }
        if (!idx || idx > (u32)arrlen(a->tab) || a->tab[idx - 1].is_class != cl) { r->bad = true; return NULL; }
        if (!cl) return a->tab[idx - 1].obj;
        snprintf(cls, sizeof(cls), "%s", a->tab[idx - 1].cls);
    }
    PObj *o = calloc(1, sizeof(PObj));
    snprintf(o->cls, sizeof(o->cls), "%s", cls);
    o->alpha = 1; o->shape_m = mat_id(); o->m = mat_id();
    arrput(a->all, o);
    TabEnt e = {.is_class = false, .obj = o};
    snprintf(e.cls, sizeof(e.cls), "%s", cls);
    arrput(a->tab, e);
    if (!strcmp(cls, "CPicPage")) { o->kind = PO_PAGE; base(a, o); }
    else if (!strcmp(cls, "CPicLayer")) {
        o->kind = PO_LAYER;
        base(a, o);
        int ls = r8(r);
        rstring(r, o->name, sizeof(o->name));
        if (ls <= 3) r8(r);
        if (ls >= 4 && ls <= 30) { r8(r); o->locked = r8(r) != 0; o->hidden = r8(r) != 0; }
        if (ls >= 5 && ls <= 30) r32(r);
        if (ls >= 6 && ls <= 30) { Color c = rcolor(r); o->color = c | 0xff000000u; r32(r); }
        if (ls >= 8 && ls <= 30) r32(r);
        o->mode = r8(r);
        o->parent = read_object(a);
        if (ls >= 7 && ls < 9) read_object(a);
        if (ls >= 2 && ls < 6) r8(r);
        if (ls >= 3 && ls < 9) r8(r);
        if (ls >= 9) r8(r);
        if (ls >= 10) r8(r);
        if (ls >= 11) r8(r);
    } else if (!strcmp(cls, "CPicFrame")) { o->kind = PO_FRAME; base(a, o); shape_tail(a, o); frame_tail(a, o); }
    else if (!strcmp(cls, "CPicShape")) { o->kind = PO_SHAPE; base(a, o); shape_tail(a, o); }
    else if (!strcmp(cls, "CPicSymbol") || !strcmp(cls, "CPicSprite") || !strcmp(cls, "CPicButton")) { o->kind = PO_SYMBOL; symbol_obj(a, o); }
    else {
        if (!strcmp(cls, "CPicBitmap")) warn(a->c, "Bitmaps are not imported yet");
        else warn(a->c, "Some objects of an unknown kind were skipped");
        r->bad = true;
        return NULL;
    }
    return o;
}

static int sym_for_ref(Ctx *c, u32 ref) {
    for (ptrdiff_t i = 0; i < arrlen(c->refs); i++) if (c->refs[i].ref == ref) return c->refs[i].sym;
    return 0;
}

static void bin_elements(Ctx *c, PObj *s, Item **out, int depth) {
    if (depth > 16) return;
    if (arrlen(s->shape.edges)) { shape_items(&s->shape, s->shape_m, out); c->shapes++; }
    for (ptrdiff_t i = 0; i < arrlen(s->kids); i++) {
        PObj *k = s->kids[i];
        if (k->kind == PO_SYMBOL) {
            int sym = sym_for_ref(c, k->ref);
            if (!sym) { warn(c, "An instance refers to a missing symbol"); continue; }
            Item it = {.kind = IT_INSTANCE, .sym = sym, .m = k->m, .first = k->first + 1, .alpha = k->alpha,
                       .loop = k->loop == 1 ? LOOP_ONCE : k->loop == 2 ? LOOP_SINGLE : LOOP_LOOP};
            arrput(*out, it);
            c->instances++;
        } else if (k->kind == PO_SHAPE) bin_elements(c, k, out, depth + 1);
    }
}

static Timeline bin_timeline(Ctx *c, PObj *page) {
    Timeline t = {0};
    t.frame = 1; t.len = 1;
    if (!page) return t;
    PObj **layers = NULL;
    for (ptrdiff_t i = 0; i < arrlen(page->kids); i++) {
        PObj *k = page->kids[i];
        if (k->kind != PO_LAYER) continue;
        bool dup = false;
        for (ptrdiff_t j = 0; j < arrlen(layers); j++) if (layers[j] == k) dup = true;
        if (!dup) arrput(layers, k);
    }
    int n = (int)arrlen(layers);
    int *ids = malloc(sizeof(int) * (n ? n : 1));
    // stored bottom first; ours are top first
    for (int i = 0; i < n; i++) ids[i] = c->d->next_id++;
    for (int i = n - 1; i >= 0; i--) {
        PObj *pl = layers[i];
        static const u8 modes[6] = {LT_VECTOR, LT_GUIDE, LT_VECTOR, LT_MASK, LT_VECTOR, LT_FOLDER};
        Layer l = new_layer(c, pl->name, pl->mode >= 0 && pl->mode < 6 ? modes[pl->mode] : LT_VECTOR);
        c->d->next_id--;   // new_layer took an id; use the reserved one
        l.id = ids[i];
        l.locked = pl->locked; l.hidden = pl->hidden;
        if (pl->color) l.color = pl->color;
        for (int j = 0; j < n; j++) if (pl->parent && layers[j] == pl->parent) l.parent = ids[j];
        int start = 1;
        for (ptrdiff_t k = 0; k < arrlen(pl->kids); k++) {
            PObj *f = pl->kids[k];
            if (f->kind != PO_FRAME || f->span <= 0) continue;
            Item *items = NULL;
            bin_elements(c, f, &items, 0);
            Key key = {.f = start, .d = -1, .tw = (f->flags & 1) ? TW_CLASSIC : TW_NONE};
            snprintf(key.label, sizeof(key.label), "%s", f->label);
            if (l.type != LT_FOLDER) key.d = add_drawing(c, items);
            arrfree(items);
            arrput(l.keys, key);
            start += f->span;
        }
        l.len = start - 1;
        if (!arrlen(l.keys) && l.type != LT_FOLDER) { Key k = {.f = 1, .d = doc_new_drawing(c->d)}; arrput(l.keys, k); l.len = 1; }
        t.len = MAX(t.len, l.len);
        arrput(t.layers, l);
    }
    free(ids); arrfree(layers);
    return t;
}

typedef struct { char stream[128]; char name[128]; u32 id; int type; size_t at; } PageInfo;

#include "io/cfb.inc"

static Doc *import_binary(const u8 *data, size_t n, char *err, int cap) {
    Cfb cf;
    if (!cfb_open(&cf, data, n)) { snprintf(err, cap, "This FLA's container is damaged"); return NULL; }
    size_t clen = 0;
    u8 *contents = cfb_stream(&cf, "Contents", &clen);
    if (!contents) { cfb_close(&cf); snprintf(err, cap, "Not a Flash document"); return NULL; }
    Ctx c = {0};
    Doc *d = c.d = calloc(1, sizeof(Doc));
    d->w = 550; d->h = 400; d->fps = 24; d->next_id = 1; d->scene.frame = 1;
    d->loop_a = 1; d->loop_b = 24; d->onion_prev = 2; d->onion_next = 1;
    // stage rectangle in twips, background and 8.8 frame rate
    for (size_t at = 0; at + 64 <= clen; at++) {
        i32 x0, x1, y0, y1; u16 fps;
        memcpy(&x0, contents + at, 4); memcpy(&x1, contents + at + 4, 4); memcpy(&y0, contents + at + 8, 4); memcpy(&y1, contents + at + 12, 4);
        if (x0 || y0 || x1 < 20 || y1 < 20 || x1 > 20 * 8192 || y1 > 20 * 8192 || x1 % 20 || y1 % 20) continue;
        if (contents[at + 56] != 0xff) continue;
        memcpy(&fps, contents + at + 62, 2);
        if (fps < 0x100 || fps > 0x7800) continue;
        d->w = x1 / 20; d->h = y1 / 20; d->fps = MAX(1, (int)roundf(fps / 256.0f));
        break;
    }
    // pages: stream names are stored (u8 length + UTF-16) in Contents, followed by name, id and type
    PageInfo *pages = NULL;
    for (int i = 0; i < cf.npaths; i++) {
        const char *st = cf.paths[i].name;
        if (strncmp(st, "P ", 2) && strncmp(st, "S ", 2) && strncmp(st, "Page ", 5) && strncmp(st, "Symbol ", 7)) continue;
        int L = (int)strlen(st);
        u8 key[300]; key[0] = (u8)L;
        for (int k = 0; k < L; k++) { key[1 + k * 2] = (u8)st[k]; key[2 + k * 2] = 0; }
        int klen = 1 + L * 2;
        for (size_t at = 0; at + klen <= clen; at++) {
            if (memcmp(contents + at, key, klen)) continue;
            PageInfo p = {0};
            snprintf(p.stream, sizeof(p.stream), "%s", st);
            Rd r = {contents, clen, at + klen, false};
            rstring(&r, p.name, sizeof(p.name));
            p.id = r32(&r); p.type = r8(&r); p.at = at;
            if (r.bad) snprintf(p.name, sizeof(p.name), "%s", st);
            arrput(pages, p);
            break;
        }
    }
    for (ptrdiff_t i = 1; i < arrlen(pages); i++) { PageInfo t = pages[i]; ptrdiff_t j = i - 1; while (j >= 0 && pages[j].at > t.at) { pages[j + 1] = pages[j]; j--; } pages[j + 1] = t; }
    // symbols first: instances refer to them by number
    for (ptrdiff_t i = 0; i < arrlen(pages); i++) {
        if (pages[i].stream[0] != 'S') continue;
        Symbol s = {0};
        s.id = d->next_id++;
        s.type = pages[i].type == 2 ? SYM_MOVIECLIP : SYM_GRAPHIC;
        const char *nm = strrchr(pages[i].name, '/');
        snprintf(s.name, sizeof(s.name), "%s", nm ? nm + 1 : pages[i].name);
        arrput(d->symbols, s);
        arrput(c.refs, ((typeof(*c.refs)){pages[i].id, s.id}));
    }
    bool scene_done = false;
    for (int pass = 0; pass < 2; pass++)
        for (ptrdiff_t i = 0; i < arrlen(pages); i++) {
            bool is_sym = pages[i].stream[0] == 'S';
            if (pass == 0 ? !is_sym : is_sym) continue;
            size_t sl = 0;
            u8 *sd = cfb_stream(&cf, pages[i].stream, &sl);
            if (!sd) continue;
            Ar a = {.r = {sd, sl, 0, false}, .c = &c};
            r8(&a.r);
            PObj *root = read_object(&a);
            if (a.r.bad) { warn(&c, "Parts of this FLA couldn't be read and were skipped"); if (0) vx_log("fla: %s stopped at %zu of %zu (%d objects)", pages[i].stream, a.r.pos, a.r.n, (int)arrlen(a.all)); }
            Timeline t = bin_timeline(&c, root);
            if (is_sym) {
                int sid = sym_for_ref(&c, pages[i].id);
                Symbol *s = doc_symbol(d, sid);
                if (s) { s->tl = t; s->tl.active = arrlen(t.layers) ? t.layers[0].id : 0; }
            } else if (!scene_done) { d->scene = t; scene_done = true; }
            else { warn(&c, "Only the first scene was imported"); for (ptrdiff_t k = 0; k < arrlen(t.layers); k++) arrfree(t.layers[k].keys); arrfree(t.layers); }
            for (ptrdiff_t k = 0; k < arrlen(a.all); k++) { fshape_free(&a.all[k]->shape); arrfree(a.all[k]->kids); free(a.all[k]); }
            arrfree(a.all); arrfree(a.tab);
            free(sd);
        }
    arrfree(pages); free(contents); cfb_close(&cf);
    if (!scene_done) { doc_free(d); arrfree(c.refs); snprintf(err, cap, "The document has no scenes"); return NULL; }
    for (ptrdiff_t i = 0; i < arrlen(d->symbols); i++) if (!arrlen(d->symbols[i].tl.layers)) { Layer l = new_layer(&c, "Layer 1", LT_VECTOR); Key k = {.f = 1, .d = doc_new_drawing(d)}; arrput(l.keys, k); l.len = 1; arrput(d->symbols[i].tl.layers, l); d->symbols[i].tl.len = 1; d->symbols[i].tl.frame = 1; }
    if (!arrlen(d->scene.layers)) { Layer l = new_layer(&c, "Layer 1", LT_VECTOR); Key k = {.f = 1, .d = doc_new_drawing(d)}; arrput(l.keys, k); l.len = 1; arrput(d->scene.layers, l); }
    d->scene.active = d->scene.layers[0].id;
    snprintf(err, cap, "%s", c.warn);
    arrfree(c.refs);
    return d;
}

/* ════════════════════════ XFL ════════════════════════ */

#include "io/zip.inc"

typedef struct { Zip *zip; char dir[512]; } Src;
static char *src_read(Src *s, const char *name, size_t *len) {
    if (s->zip) return zip_read(s->zip, name, len);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", s->dir, name);
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024);
    FILE *f = _wfopen(w, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (len) *len = got;
    return b;
}

static float xnum(XNode *n, const char *a, float def) { const char *v = xml_attr(n, a); return v ? (float)atof(v) : def; }
static Color xcolor(XNode *n, const char *ca, const char *aa, Color def) {
    Color c = def;
    const char *v = xml_attr(n, ca);
    if (v && v[0] == '#' && strlen(v) >= 7) { unsigned x; if (sscanf(v + 1, "%6x", &x) == 1) c = hex(x); }
    const char *al = xml_attr(n, aa);
    if (al) c = col_alpha(c | 0xff000000u, (float)atof(al));
    return c;
}
static Mat xmatrix(XNode *owner) {
    XNode *m = owner ? xml_child(owner, "matrix") : NULL;
    XNode *mm = m ? xml_child(m, "Matrix") : NULL;
    if (!mm) return mat_id();
    return (Mat){xnum(mm, "a", 1), xnum(mm, "b", 0), xnum(mm, "c", 0), xnum(mm, "d", 1), xnum(mm, "tx", 0), xnum(mm, "ty", 0)};
}

static bool xcoord(const char **s, float *out) {
    while (**s == ' ' || **s == '\t' || **s == '\n' || **s == '\r') (*s)++;
    const char *p = *s;
    if (*p == '#') {
        p++;
        const char *b = p;
        while (isxdigit((unsigned char)*p)) p++;
        int nd = (int)(p - b);
        long long iv = 0;
        for (const char *q = b; q < p; q++) iv = iv * 16 + (isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10));
        if (nd >= 6 && iv >= (1LL << (nd * 4 - 1))) iv -= 1LL << (nd * 4);
        double frac = 0;
        if (*p == '.') {
            p++;
            const char *fb = p; long long fv = 0;
            while (isxdigit((unsigned char)*p)) { fv = fv * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10)); p++; }
            frac = (double)fv / (double)(1LL << (4 * (p - fb)));
        }
        *out = (float)((iv >= 0 ? iv + frac : iv - frac) / TWIPS);
        *s = p;
        return nd > 0;
    }
    char *end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = (float)(v / TWIPS);
    *s = end;
    return true;
}

static int image_by_name(Ctx *c, const char *name);
static void xfl_shape(Ctx *c, XNode *sh, Item **out) {
    FShape g = {0};
    XNode *fills = xml_child(sh, "fills"), *strokes = xml_child(sh, "strokes"), *edges = xml_child(sh, "edges");
    int maxf = 0, maxs = 0;
    for (int i = 0; fills && i < xml_nkids(fills); i++) maxf = MAX(maxf, (int)xnum(xml_kid(fills, i), "index", 0));
    for (int i = 0; strokes && i < xml_nkids(strokes); i++) maxs = MAX(maxs, (int)xnum(xml_kid(strokes, i), "index", 0));
    for (int i = 0; i < maxf; i++) { arrput(g.fills, hex(0x000000)); arrput(g.fill_bm, 0); arrput(g.fill_m, mat_id()); arrput(g.fill_gr, (FGrad){0}); }
    for (int i = 0; i < maxs; i++) { arrput(g.stroke_col, hex(0x000000)); arrput(g.stroke_w, 1); }
    for (int i = 0; fills && i < xml_nkids(fills); i++) {
        XNode *fs = xml_kid(fills, i);
        int idx = (int)xnum(fs, "index", 0);
        if (idx < 1 || idx > maxf || !xml_nkids(fs)) continue;
        XNode *k = xml_kid(fs, 0);
        if (!strcmp(xml_name(k), "SolidColor")) g.fills[idx - 1] = xcolor(k, "color", "alpha", hex(0));
        else if (!strcmp(xml_name(k), "LinearGradient") || !strcmp(xml_name(k), "RadialGradient")) {
            FGrad *gr = &g.fill_gr[idx - 1];
            gr->kind = !strcmp(xml_name(k), "RadialGradient") ? GR_RADIAL : GR_LINEAR;
            const char *sp = xml_attr(k, "spreadMethod");
            gr->spread = sp && !strcmp(sp, "reflect") ? SP_REFLECT : sp && !strcmp(sp, "repeat") ? SP_REPEAT : SP_PAD;
            for (int q = 0; q < xml_nkids(k); q++) {
                XNode *e = xml_kid(k, q);
                if (strcmp(xml_name(e), "GradientEntry")) continue;
                GStop st = {clampf(xnum(e, "ratio", 0), 0, 1), xcolor(e, "color", "alpha", hex(0))};
                arrput(gr->stops, st);
            }
            if (!arrlen(gr->stops)) gr->kind = GR_NONE;
            else g.fills[idx - 1] = gr->stops[0].c;   // what the colour panel shows
            g.fill_m[idx - 1] = xmatrix(k);
        }
        else if (!strcmp(xml_name(k), "BitmapFill")) {   // its matrix scales 20 to a pixel, as in a SWF
            g.fills[idx - 1] = hex(0x808080);
            g.fill_bm[idx - 1] = image_by_name(c, xml_attr(k, "bitmapPath"));
            Mat bm = xmatrix(k);
            bm.a /= TWIPS; bm.b /= TWIPS; bm.c /= TWIPS; bm.d /= TWIPS;
            g.fill_m[idx - 1] = bm;
            if (!g.fill_bm[idx - 1]) warn(c, "A bitmap fill refers to a missing image");
        }
    }
    for (int i = 0; strokes && i < xml_nkids(strokes); i++) {
        XNode *ss = xml_kid(strokes, i);
        int idx = (int)xnum(ss, "index", 0);
        if (idx < 1 || idx > maxs || !xml_nkids(ss)) continue;
        XNode *k = xml_kid(ss, 0);
        float w = xnum(k, "weight", 1);
        if (xml_attr(k, "solidStyle") && !strcmp(xml_attr(k, "solidStyle"), "hairline")) w = 1;
        g.stroke_w[idx - 1] = w;
        XNode *fl = xml_child(k, "fill");
        if (fl && xml_nkids(fl)) {
            XNode *sc = xml_kid(fl, 0);
            if (!strcmp(xml_name(sc), "SolidColor")) g.stroke_col[idx - 1] = xcolor(sc, "color", "alpha", hex(0));
            else for (int q = 0; q < xml_nkids(sc); q++) if (!strcmp(xml_name(xml_kid(sc, q)), "GradientEntry")) { g.stroke_col[idx - 1] = xcolor(xml_kid(sc, q), "color", "alpha", hex(0)); break; }
        }
    }
    for (int i = 0; edges && i < xml_nkids(edges); i++) {
        XNode *e = xml_kid(edges, i);
        const char *data = xml_attr(e, "edges");
        if (!data) continue;
        int f0 = (int)xnum(e, "fillStyle0", 0), f1 = (int)xnum(e, "fillStyle1", 0), st = (int)xnum(e, "strokeStyle", 0);
        if (f0 < 0 || f0 > maxf) f0 = 0;
        if (f1 < 0 || f1 > maxf) f1 = 0;
        if (st < 0 || st > maxs) st = 0;
        if (!f0 && !f1 && !st) continue;
        V2 cur = {0};
        const char *p = data;
        for (int guard = 0; guard < 10000000; guard++) {
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
            if (!*p) break;
            char cmd = *p++;
            if (cmd == '!') { if (!xcoord(&p, &cur.x) || !xcoord(&p, &cur.y)) break; }
            else if (cmd == '|' || cmd == '/') {
                V2 q; if (!xcoord(&p, &q.x) || !xcoord(&p, &q.y)) break;
                if (q.x != cur.x || q.y != cur.y) { FEdge fe = {cur, cur, q, false, f0, f1, st}; arrput(g.edges, fe); }
                cur = q;
            } else if (cmd == '[' || cmd == ']') {
                V2 cq, q; if (!xcoord(&p, &cq.x) || !xcoord(&p, &cq.y) || !xcoord(&p, &q.x) || !xcoord(&p, &q.y)) break;
                FEdge fe = {cur, cq, q, true, f0, f1, st}; arrput(g.edges, fe);
                cur = q;
            } else if (cmd == 'S') { while (isdigit((unsigned char)*p)) p++; }
            else break;
        }
    }
    shape_items(&g, xmatrix(sh), out);
    fshape_free(&g);
    c->shapes++;
}

/* ── bitmaps ── */

char *stbi_zlib_decode_malloc(const char *buffer, int len, int *outlen);

// Animate keeps a library bitmap in bin/*.dat: a plain JPEG, or its own lossless format:
//   u8 3, u8 kind (5: 32-bit ARGB, 3: 8-bit with a palette), u16 row bytes, u16 width, u16 height,
//   4 x u32 frame in twips, u8 has alpha, [kind 3: u16 colours, colours x ARGB], u8 compressed,
//   then the rows: raw, or one zlib stream cut into u16-length chunks (a 0 length ends it).
// Colours are premultiplied when there is alpha.
static int dat_bitmap(const char *name, const u8 *b, size_t n, bool smooth) {
    char err[96];
    if (n >= 4 && ((b[0] == 0xff && b[1] == 0xd8) || (b[0] == 0x89 && b[1] == 'P') || (b[0] == 'G' && b[1] == 'I'))) {
        int id = bitmap_add_file(name, b, (u32)n, err, sizeof(err));
        if (id) bitmap_get(id)->smooth = smooth;
        return id;
    }
    if (n < 28 || b[0] != 3 || (b[1] != 5 && b[1] != 3)) return 0;
    int row = b[2] | b[3] << 8, w = b[4] | b[5] << 8, h = b[6] | b[7] << 8;
    bool alpha = b[24] != 0, pal8 = b[1] == 3;
    u32 pal[256] = {0};
    size_t at;
    if (pal8) {
        int nc = b[25] | b[26] << 8;
        if (nc > 256 || 27 + (size_t)nc * 4 + 1 > n) return 0;
        for (int i = 0; i < nc; i++) { const u8 *e = b + 27 + i * 4; pal[i] = (u32)e[0] << 24 | (u32)e[1] << 16 | (u32)e[2] << 8 | e[3]; }   // A R G B
        at = 27 + (size_t)nc * 4;
    } else at = 25;
    bool packed = b[at++] != 0;
    size_t need = (size_t)row * h;
    if (w <= 0 || h <= 0 || row < (pal8 ? w : w * 4)) return 0;
    u8 *raw = NULL; const u8 *rows = b + at;
    if (packed) {
        u8 *z = NULL;
        while (at + 2 <= n) {
            size_t k = b[at] | b[at + 1] << 8; at += 2;
            if (!k || at + k > n) break;
            memcpy(arraddnptr(z, k), b + at, k); at += k;
        }
        int ol = 0;
        raw = z ? (u8 *)stbi_zlib_decode_malloc((const char *)z, (int)arrlen(z), &ol) : NULL;
        arrfree(z);
        if (!raw || (size_t)ol < need) { free(raw); return 0; }
        rows = raw;
    } else if (at + need > n) return 0;
    u8 *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 *d = px + ((size_t)y * w + x) * 4;
            u32 argb;
            if (pal8) argb = pal[rows[(size_t)y * row + x]];
            else { const u8 *q = rows + (size_t)y * row + x * 4; argb = (u32)q[0] << 24 | (u32)q[1] << 16 | (u32)q[2] << 8 | q[3]; }
            u8 a = alpha ? (u8)(argb >> 24) : 255;
            d[0] = (u8)MIN(a, (argb >> 16) & 255); d[1] = (u8)MIN(a, (argb >> 8) & 255); d[2] = (u8)MIN(a, argb & 255); d[3] = a;
        }
    free(raw);
    return bitmap_add_pixels(name, w, h, px, smooth);
}

static int image_by_name(Ctx *c, const char *name) {
    for (ptrdiff_t i = 0; name && i < arrlen(c->images); i++) if (!strcmp(c->images[i].key, name)) return c->images[i].sym;
    return 0;
}

static u8 blend_of(const char *v) {
    static const char *names[BLEND_COUNT] = {"normal", "layer", "darken", "multiply", "lighten", "screen", "overlay", "hardlight",
                                             "add", "subtract", "difference", "invert", "alpha", "erase"};
    for (int i = 0; v && i < BLEND_COUNT; i++) if (!strcmp(v, names[i])) return (u8)i;
    return BLEND_NORMAL;
}

static int sym_by_name(Ctx *c, const char *name) {
    for (ptrdiff_t i = 0; i < arrlen(c->names); i++) if (!strcmp(c->names[i].key, name)) return c->names[i].sym;
    return 0;
}

static void xfl_elements(Ctx *c, XNode *list, Item **out, int depth) {
    if (!list || depth > 16) return;
    for (int i = 0; i < xml_nkids(list); i++) {
        XNode *e = xml_kid(list, i);
        const char *n = xml_name(e);
        if (!strcmp(n, "DOMShape")) xfl_shape(c, e, out);
        else if (!strcmp(n, "DOMSymbolInstance")) {
            const char *lib = xml_attr(e, "libraryItemName");
            int sym = lib ? sym_by_name(c, lib) : 0;
            if (!sym) { warn(c, "An instance refers to a missing library item"); continue; }
            Item it = {.kind = IT_INSTANCE, .sym = sym, .m = xmatrix(e), .alpha = 1, .first = (int)xnum(e, "firstFrame", 0) + 1};
            const char *lp = xml_attr(e, "loop");
            it.loop = lp && !strncmp(lp, "play once", 9) ? LOOP_ONCE : lp && !strcmp(lp, "single frame") ? LOOP_SINGLE : LOOP_LOOP;
            XNode *col = xml_child(e, "color");
            XNode *cc = col && xml_nkids(col) ? xml_kid(col, 0) : NULL;
            if (cc) it.alpha = clampf(xnum(cc, "alphaMultiplier", 1) + xnum(cc, "alphaOffset", 0) / 255.0f, 0, 1);
            it.blend = blend_of(xml_attr(e, "blendMode"));
            if (cc) {   // colour effect: brightness, tint or the advanced multipliers and offsets
                if (xml_attr(cc, "brightness")) { it.cx.kind = CX_BRIGHTNESS; it.cx.amount = xnum(cc, "brightness", 0); }
                else if (xml_attr(cc, "tintMultiplier") || xml_attr(cc, "tintColor")) {
                    it.cx.kind = CX_TINT; it.cx.amount = xnum(cc, "tintMultiplier", 0); it.cx.tint = xcolor(cc, "tintColor", "__none", hex(0));
                } else if (xml_attr(cc, "redMultiplier") || xml_attr(cc, "greenMultiplier") || xml_attr(cc, "blueMultiplier") ||
                           xml_attr(cc, "redOffset") || xml_attr(cc, "greenOffset") || xml_attr(cc, "blueOffset")) {
                    static const char *mn[3] = {"redMultiplier", "greenMultiplier", "blueMultiplier"}, *on[3] = {"redOffset", "greenOffset", "blueOffset"};
                    it.cx.kind = CX_ADVANCED;
                    for (int q = 0; q < 3; q++) { it.cx.mul[q] = xnum(cc, mn[q], 1); it.cx.add[q] = xnum(cc, on[q], 0) / 255.0f; }
                    it.cx.mul[3] = 1; it.cx.add[3] = 0;
                }
                colorfx_update(&it.cx);
            } else colorfx_update(&it.cx);
            XNode *fl = xml_child(e, "filters");
            for (int q = 0; fl && q < xml_nkids(fl); q++) {
                XNode *x = xml_kid(fl, q);
                const char *fn = xml_attr(x, "enabled");
                Filter f = {.quality = (u8)CLAMP((int)xnum(x, "quality", 1), 1, 3), .bx = xnum(x, "blurX", 5), .by = xnum(x, "blurY", 5),
                            .strength = xnum(x, "strength", 1), .angle = xnum(x, "angle", 45), .dist = xnum(x, "distance", 5)};
                if (fn && !strcmp(fn, "false")) f.flags |= FXF_OFF;
                const char *t = xml_attr(x, "inner"), *k = xml_attr(x, "knockout"), *h = xml_attr(x, "hideObject");
                if (t && !strcmp(t, "true")) f.flags |= FXF_INNER;
                if (k && !strcmp(k, "true")) f.flags |= FXF_KNOCKOUT;
                if (h && !strcmp(h, "true")) f.flags |= FXF_HIDE;
                const char *nm = xml_name(x);
                if (!strcmp(nm, "BlurFilter")) f.type = FX_BLUR;
                else if (!strcmp(nm, "GlowFilter")) { f.type = FX_GLOW; f.color = xcolor(x, "color", "alpha", hex(0xff0000)); }
                else if (!strcmp(nm, "DropShadowFilter")) { f.type = FX_SHADOW; f.color = xcolor(x, "color", "alpha", hex(0x000000)); }
                else if (!strcmp(nm, "AdjustColorFilter")) {
                    f.type = FX_ADJUST;
                    f.adj[0] = xnum(x, "brightness", 0); f.adj[1] = xnum(x, "contrast", 0); f.adj[2] = xnum(x, "saturation", 0); f.adj[3] = xnum(x, "hue", 0);
                } else { if (strcmp(nm, "DeblockingFilter")) warn(c, "Bevel and gradient filters are left out"); continue; }
                arrput(it.filters, f);
            }
            const char *vis = xml_attr(e, "isVisible");
            if (vis && !strcmp(vis, "false")) it.alpha = 0;
            arrput(*out, it);
            c->instances++;
        } else if (!strcmp(n, "DOMGroup")) xfl_elements(c, xml_child(e, "members"), out, depth + 1);
        else if (!strcmp(n, "DOMBitmapInstance")) {
            int id = image_by_name(c, xml_attr(e, "libraryItemName"));
            if (!id) { warn(c, "An image refers to a missing library item"); continue; }
            Item it = {.kind = IT_BITMAP, .sym = id, .m = xmatrix(e), .alpha = 1};
            arrput(*out, it);
        }
        else if (strstr(n, "Text")) warn(c, "Text is not imported yet");
    }
}

static Timeline xfl_timeline(Ctx *c, XNode *tlnode) {
    Timeline t = {0};
    t.frame = 1; t.len = 1;
    XNode *layers = tlnode ? xml_child(tlnode, "layers") : NULL;
    int n = layers ? xml_nkids(layers) : 0;
    int *ids = malloc(sizeof(int) * (n ? n : 1));
    for (int i = 0; i < n; i++) ids[i] = c->d->next_id++;
    for (int i = 0; i < n; i++) {
        XNode *xl = xml_kid(layers, i);
        const char *lt = xml_attr(xl, "layerType");
        u8 type = !lt ? LT_VECTOR : !strcmp(lt, "guide") ? LT_GUIDE : !strcmp(lt, "mask") ? LT_MASK : !strcmp(lt, "folder") ? LT_FOLDER : !strcmp(lt, "camera") ? LT_CAMERA : LT_VECTOR;
        Layer l = new_layer(c, xml_attr(xl, "name"), type);
        c->d->next_id--;
        l.id = ids[i];
        const char *vis = xml_attr(xl, "visible"), *lk = xml_attr(xl, "locked");
        l.hidden = vis && !strcmp(vis, "false");
        l.locked = lk && !strcmp(lk, "true");
        const char *tr = xml_attr(xl, "transparent");
        l.transparent = tr && !strcmp(tr, "true");
        if (xml_attr(xl, "alphaPercent")) l.see_pct = (u8)CLAMP((int)xnum(xl, "alphaPercent", 50), 1, 100);
        l.color = xcolor(xl, "color", "__none", hex(0x4fff4f));
        if (xml_attr(xl, "parentLayerIndex")) { int p = (int)xnum(xl, "parentLayerIndex", -1); if (p >= 0 && p < n && p != i) l.parent = ids[p]; }
        XNode *frames = xml_child(xl, "frames");
        int end = 0;
        for (int k = 0; frames && k < xml_nkids(frames); k++) {
            XNode *f = xml_kid(frames, k);
            int start = (int)xnum(f, "index", 0), dur = MAX(1, (int)xnum(f, "duration", 1));
            if (start < end) continue;
            if (start > end && type != LT_FOLDER) { Key blank = {.f = end + 1, .d = -1}; arrput(l.keys, blank); }
            Item *items = NULL;
            xfl_elements(c, xml_child(f, "elements"), &items, 0);
            Key key = {.f = start + 1, .d = -1};
            const char *tw = xml_attr(f, "tweenType");
            key.tw = tw && !strcmp(tw, "motion") ? TW_CLASSIC : tw && !strcmp(tw, "shape") ? TW_SHAPE : TW_NONE;
            if (tw && strcmp(tw, "motion") && strcmp(tw, "shape")) warn(c, "Motion tweens were imported as keyframes");
            // the ease: a custom curve, a preset (Ease method), or the classic acceleration
            XNode *tws = xml_child(f, "tweens");
            for (int q = 0; tws && q < xml_nkids(tws); q++) {
                XNode *e = xml_kid(tws, q);
                const char *tg = xml_attr(e, "target");
                if (tg && strcmp(tg, "all")) continue;
                if (!strcmp(xml_name(e), "CustomEase")) {
                    key.ease = EASE_CUSTOM; key.nez = 0;
                    for (int r = 0; r < xml_nkids(e) && key.nez < EASE_PTS; r++)
                        if (!strcmp(xml_name(xml_kid(e, r)), "Point")) key.ez[key.nez++] = v2(xnum(xml_kid(e, r), "x", 0), xnum(xml_kid(e, r), "y", 0));
                    key.nez = (u8)(key.nez >= 4 ? 1 + (key.nez - 1) / 3 * 3 : 0);
                    if (!key.nez) key.ease = EASE_NONE;
                    else { key.ez[0] = v2(0, 0); key.ez[key.nez - 1] = v2(1, 1); }
                    break;
                }
                if (!strcmp(xml_name(e), "Ease") && ease_from_name(xml_attr(e, "method"), &key.ease, &key.ease_dir)) break;
            }
            if (key.ease == EASE_NONE && ease_from_name(xml_attr(f, "easeMethodName"), &key.ease, &key.ease_dir)) {}
            if (key.ease == EASE_NONE && xml_attr(f, "acceleration")) { key.ease = EASE_CLASSIC; key.ease_amt = clampf(xnum(f, "acceleration", 0), -100, 100); }
            const char *lab = xml_attr(f, "name"), *ltp = xml_attr(f, "labelType");
            if (lab && (!ltp || !strcmp(ltp, "name"))) snprintf(key.label, sizeof(key.label), "%s", lab);
            if (type != LT_FOLDER) key.d = add_drawing(c, items);
            else for (ptrdiff_t q = 0; q < arrlen(items); q++) item_free(&items[q]);
            arrfree(items);
            if (type != LT_FOLDER) arrput(l.keys, key);
            end = start + dur;
        }
        l.len = type == LT_FOLDER ? 0 : end;
        if (type != LT_FOLDER && !arrlen(l.keys)) { Key k = {.f = 1, .d = doc_new_drawing(c->d)}; arrput(l.keys, k); l.len = 1; }
        t.len = MAX(t.len, l.len);
        arrput(t.layers, l);
    }
    free(ids);
    if (arrlen(t.layers)) t.active = t.layers[0].id;
    return t;
}

static Doc *import_xfl_src(Src *src, char *err, int cap) {
    size_t n = 0;
    char *xml = src_read(src, "DOMDocument.xml", &n);
    if (!xml) { snprintf(err, cap, "No DOMDocument.xml, so this isn't an Animate document"); return NULL; }
    XNode *root = xml_parse(xml, n);
    free(xml);
    if (!root || strcmp(xml_name(root), "DOMDocument")) { xml_free(root); snprintf(err, cap, "DOMDocument.xml couldn't be read"); return NULL; }
    Ctx c = {0};
    Doc *d = c.d = calloc(1, sizeof(Doc));
    d->w = (int)xnum(root, "width", 550); d->h = (int)xnum(root, "height", 400); d->fps = MAX(1, (int)roundf(xnum(root, "frameRate", 24)));
    d->bg = xcolor(root, "backgroundColor", "__none", hex(0xffffff));
    d->next_id = 1; d->loop_a = 1; d->loop_b = 24; d->onion_prev = 2; d->onion_next = 1;
    // library: register every symbol first, instances refer to them by name
    XNode *syms = xml_child(root, "symbols");
    XNode **items = NULL;
    for (int i = 0; syms && i < xml_nkids(syms); i++) {
        const char *href = xml_attr(xml_kid(syms, i), "href");
        if (!href) continue;
        char path[600]; snprintf(path, sizeof(path), "LIBRARY/%s", href);
        size_t ln = 0;
        char *sx = src_read(src, path, &ln);
        XNode *it = sx ? xml_parse(sx, ln) : NULL;
        free(sx);
        if (!it || strcmp(xml_name(it), "DOMSymbolItem")) { xml_free(it); warn(&c, "Some library items couldn't be read"); continue; }
        Symbol s = {0};
        s.id = d->next_id++;
        const char *full = xml_attr(it, "name");
        const char *st = xml_attr(it, "symbolType");
        s.type = st && !strcmp(st, "movie clip") ? SYM_MOVIECLIP : SYM_GRAPHIC;
        const char *slash = full ? strrchr(full, '/') : NULL;
        snprintf(s.name, sizeof(s.name), "%s", slash ? slash + 1 : full ? full : href);
        arrput(d->symbols, s);
        NameMap nm = {.sym = s.id}; snprintf(nm.key, sizeof(nm.key), "%s", full ? full : href);
        arrput(c.names, nm);
        arrput(items, it);
    }
    XNode *media = xml_child(root, "media");
    for (int i = 0; media && i < xml_nkids(media); i++) {
        XNode *m = xml_kid(media, i);
        if (strcmp(xml_name(m), "DOMBitmapItem")) continue;
        const char *name = xml_attr(m, "name"), *href = xml_attr(m, "href"), *dat = xml_attr(m, "bitmapDataHRef");
        const char *sm = xml_attr(m, "allowSmoothing");
        bool smooth = sm && !strcmp(sm, "true");
        if (!name) continue;
        const char *slash = strrchr(name, '/');
        int id = 0;
        char path[700]; size_t n = 0;
        if (dat) {
            snprintf(path, sizeof(path), "bin/%s", dat);
            u8 *b = (u8 *)src_read(src, path, &n);
            if (b) id = dat_bitmap(slash ? slash + 1 : name, b, n, smooth);
            free(b);
        }
        if (!id && href) {   // an uncompressed XFL keeps the image file itself
            snprintf(path, sizeof(path), "LIBRARY/%s", href);
            u8 *b = (u8 *)src_read(src, path, &n);
            if (b) id = dat_bitmap(slash ? slash + 1 : name, b, n, smooth);
            free(b);
        }
        if (!id) { warn(&c, "Some images couldn't be read"); continue; }
        doc_add_bitmap(d, id);
        NameMap nm = {.sym = id}; snprintf(nm.key, sizeof(nm.key), "%s", name);
        arrput(c.images, nm);
    }
    for (ptrdiff_t i = 0; i < arrlen(items); i++) {
        XNode *tl = xml_child(items[i], "timeline");
        Timeline t = xfl_timeline(&c, tl && xml_nkids(tl) ? xml_kid(tl, 0) : NULL);
        d->symbols[i].tl = t;
        xml_free(items[i]);
    }
    arrfree(items);
    XNode *tls = xml_child(root, "timelines");
    if (!tls || !xml_nkids(tls)) { xml_free(root); doc_free(d); arrfree(c.names); arrfree(c.images); snprintf(err, cap, "The document has no scenes"); return NULL; }
    if (xml_nkids(tls) > 1) warn(&c, "Only the first scene was imported");
    d->scene = xfl_timeline(&c, xml_kid(tls, 0));
    xml_free(root);
    for (ptrdiff_t i = 0; i < arrlen(d->symbols); i++) if (!arrlen(d->symbols[i].tl.layers)) { Layer l = new_layer(&c, "Layer 1", LT_VECTOR); Key k = {.f = 1, .d = doc_new_drawing(d)}; arrput(l.keys, k); l.len = 1; arrput(d->symbols[i].tl.layers, l); d->symbols[i].tl.active = l.id; }
    if (!arrlen(d->scene.layers)) { Layer l = new_layer(&c, "Layer 1", LT_VECTOR); Key k = {.f = 1, .d = doc_new_drawing(d)}; arrput(l.keys, k); l.len = 1; arrput(d->scene.layers, l); d->scene.active = l.id; }
    snprintf(err, cap, "%s", c.warn);
    arrfree(c.names); arrfree(c.images);
    return d;
}

/* ── entry ── */

Doc *import_xfl(const char *path, char *err, int cap) {
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024);
    FILE *f = _wfopen(w, L"rb");
    if (!f) { snprintf(err, cap, "Can't open the file"); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *data = malloc((size_t)MAX(n, 1));
    size_t got = fread(data, 1, (size_t)MAX(n, 0), f);
    fclose(f);
    Doc *d = NULL;
    char warnmsg[300] = "";
    const char *dot = strrchr(path, '.');
    if (got >= 8 && !memcmp(data, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) d = import_binary(data, got, warnmsg, sizeof(warnmsg));
    else if (got >= 4 && !memcmp(data, "PK\x03\x04", 4)) {
        Zip z;
        if (zip_open(&z, data, got)) { Src s = {.zip = &z}; d = import_xfl_src(&s, warnmsg, sizeof(warnmsg)); zip_close(&z); }
        else snprintf(warnmsg, sizeof(warnmsg), "The FLA archive is damaged");
    } else if (dot && !_stricmp(dot, ".xfl")) {
        Src s = {0};
        snprintf(s.dir, sizeof(s.dir), "%s", path);
        char *sl = strrchr(s.dir, '\\'), *sl2 = strrchr(s.dir, '/');
        if (sl2 > sl) sl = sl2;
        if (sl) *sl = 0;
        d = import_xfl_src(&s, warnmsg, sizeof(warnmsg));
    } else snprintf(warnmsg, sizeof(warnmsg), "This isn't a Flash or Animate document");
    free(data);
    if (!d) { snprintf(err, cap, "%s", warnmsg[0] ? warnmsg : "Import failed"); return NULL; }
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(d->name, sizeof(d->name), "%s", base);
    char *e = strrchr(d->name, '.'); if (e) *e = 0;
    if (d->scene.len < 1) d->scene.len = 1;
    d->loop_b = MAX(1, d->scene.len);
    snprintf(err, cap, "%s", warnmsg);
    return d;
}
