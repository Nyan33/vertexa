// Clip Studio Paint sub tool (.sut) import.
// A .sut file is an SQLite database: Node rows name the sub tools and point at Variant rows, which keep
// every brush setting in its own column. winsqlite3.dll ships with Windows 10+, so it is loaded on demand
// instead of bundling SQLite.
// Effector blobs (the pressure / tilt / velocity / random dynamics of a setting) are big-endian:
//   u32 header size (44), u32 available inputs, u32 enabled inputs (0x10 pressure, 0x20 tilt, 0x40 velocity,
//   0x80 random), u32 minimum %, ..., u32 size of the pressure curve block (at byte 32), ...
// followed by curve blocks: u32 12, u32 count, u32 16, count x (f64 x, f64 y). The first one is pressure.
#include "app/app.h"
#include <windows.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

static struct {
    HMODULE dll; bool tried;
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*close)(sqlite3 *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*step)(sqlite3_stmt *);
    int (*finalize)(sqlite3_stmt *);
    int (*column_count)(sqlite3_stmt *);
    const char *(*column_name)(sqlite3_stmt *, int);
    int (*column_type)(sqlite3_stmt *, int);
    double (*column_double)(sqlite3_stmt *, int);
    const void *(*column_blob)(sqlite3_stmt *, int);
    int (*column_bytes)(sqlite3_stmt *, int);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
} SQ;

enum { SQ_OPEN_READONLY = 1, SQ_ROW = 100, SQ_NULL = 5 };

static bool sym(void *dst, const char *name) {
    FARPROC p = GetProcAddress(SQ.dll, name);
    memcpy(dst, &p, sizeof(p));
    return p != NULL;
}

static bool sq_load(void) {
    if (SQ.tried) return SQ.dll != NULL;
    SQ.tried = true;
    SQ.dll = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!SQ.dll) return false;
    bool ok = sym(&SQ.open_v2, "sqlite3_open_v2") && sym(&SQ.close, "sqlite3_close") && sym(&SQ.prepare_v2, "sqlite3_prepare_v2") &&
              sym(&SQ.step, "sqlite3_step") && sym(&SQ.finalize, "sqlite3_finalize") && sym(&SQ.column_count, "sqlite3_column_count") &&
              sym(&SQ.column_name, "sqlite3_column_name") && sym(&SQ.column_type, "sqlite3_column_type") &&
              sym(&SQ.column_double, "sqlite3_column_double") && sym(&SQ.column_blob, "sqlite3_column_blob") &&
              sym(&SQ.column_bytes, "sqlite3_column_bytes") && sym(&SQ.column_text, "sqlite3_column_text");
    if (!ok) { FreeLibrary(SQ.dll); SQ.dll = NULL; }
    return ok;
}

/* ── a Variant row, columns looked up by name ── */

typedef struct { sqlite3_stmt *st; int n; } Row;

static int col(Row *r, const char *name) {
    for (int i = 0; i < r->n; i++) { const char *c = SQ.column_name(r->st, i); if (c && !strcmp(c, name)) return i; }
    return -1;
}
static bool has(Row *r, const char *name) { int i = col(r, name); return i >= 0 && SQ.column_type(r->st, i) != SQ_NULL; }
static float num(Row *r, const char *name, float def) { int i = col(r, name); return i >= 0 && SQ.column_type(r->st, i) != SQ_NULL ? (float)SQ.column_double(r->st, i) : def; }
static const u8 *blob(Row *r, const char *name, int *len) {
    int i = col(r, name);
    *len = 0;
    if (i < 0 || SQ.column_type(r->st, i) == SQ_NULL) return NULL;
    const u8 *b = SQ.column_blob(r->st, i);
    *len = SQ.column_bytes(r->st, i);
    return b;
}

/* ── effectors ── */

enum { IN_PRESSURE = 0x10, IN_TILT = 0x20, IN_VELOCITY = 0x40, IN_RANDOM = 0x80 };
typedef struct { u32 inputs; float min; int n; float x[32], y[32]; } Effector;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static double be64f(const u8 *p) { u64 v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; double d; memcpy(&d, &v, 8); return d; }

static bool effector(const u8 *b, int n, Effector *e) {
    memset(e, 0, sizeof(*e));
    if (!b || n < 36) return false;
    u32 hl = be32(b);
    if (hl < 36 || hl > (u32)n) return false;
    e->inputs = be32(b + 8);
    e->min = clampf(be32(b + 12) / 100.0f, 0, 1);
    size_t at = hl;
    if (be32(b + 32) && at + 12 <= (size_t)n && be32(b + at) == 12 && be32(b + at + 8) == 16) {
        u32 cnt = be32(b + at + 4);
        at += 12;
        for (u32 i = 0; i < cnt && e->n < 32 && at + 16 <= (size_t)n; i++, at += 16) {
            double x = be64f(b + at), y = be64f(b + at + 8);
            if (!(x == x) || !(y == y)) break;   // NaN: not a curve after all
            e->x[e->n] = clampf((float)x, 0, 1); e->y[e->n] = clampf((float)y, 0, 1);
            e->n++;
        }
    }
    return true;
}

static inline float eff_eval(const Effector *e, float p) {
    if (e->n < 2) return p;
    if (p <= e->x[0]) return e->y[0];
    for (int i = 1; i < e->n; i++) {
        if (p > e->x[i]) continue;
        float t = (p - e->x[i - 1]) / MAX(1e-6f, e->x[i] - e->x[i - 1]);
        return e->y[i - 1] + (e->y[i] - e->y[i - 1]) * t;
    }
    return e->y[e->n - 1];
}

// CSP curve -> ours: the same points, scaled into [minimum, 1] (CSP's minimum is the size at no pressure)
static Curve csp_curve(const Effector *e) {
    Curve c = {0};
    if (e->n < 2) return (Curve){2, {0, 1}, {e->min, 1}};
    int n = e->n, step = (n + CURVE_MAX - 3) / (CURVE_MAX - 2);   // more points than we keep: thin them out
    float lastx = -1;
    for (int i = 0; i < n && c.n < CURVE_MAX - 1; i += (i == 0 || i >= n - 2) ? 1 : MAX(1, step)) {
        if (e->x[i] <= lastx + 1e-3f) continue;
        c.x[c.n] = e->x[i]; c.y[c.n] = e->min + (1 - e->min) * e->y[i]; c.n++;
        lastx = e->x[i];
    }
    if (c.x[0] > 1e-3f) { for (int k = c.n; k > 0; k--) { c.x[k] = c.x[k - 1]; c.y[k] = c.y[k - 1]; } c.x[0] = 0; c.y[0] = c.y[1]; c.n++; }
    else c.x[0] = 0;
    if (c.x[c.n - 1] < 1 - 1e-3f && c.n < CURVE_MAX) { c.x[c.n] = 1; c.y[c.n] = c.y[c.n - 1]; c.n++; }
    else c.x[c.n - 1] = 1;
    return c;
}

/* ── import ── */

typedef struct { char name[64]; long long variant; } NodeRef;

static void base_noext(const char *path, char *out, int cap) {
    const char *b = path;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    snprintf(out, (size_t)cap, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static bool read_variant(sqlite3 *db, long long id, BrushPreset *p, u32 *approx) {
    char q[96];
    snprintf(q, sizeof(q), "SELECT * FROM Variant WHERE VariantID = %lld", id);
    Row r = {0};
    if (SQ.prepare_v2(db, q, -1, &r.st, NULL) || !r.st) return false;
    bool ok = SQ.step(r.st) == SQ_ROW;
    if (ok) {
        r.n = SQ.column_count(r.st);
        int len;
        const u8 *b;
        p->size = clampf(roundf(num(&r, "BrushSize", 10)), 1, 100);
        p->smoothing = 50;
        Effector e;
        b = blob(&r, "BrushSizeEffector", &len);
        if (effector(b, len, &e) && (e.inputs & IN_PRESSURE)) p->curve = csp_curve(&e);
        else p->curve = (Curve){2, {0, 1}, {1, 1}};   // size ignores pressure
        float thick = num(&r, "BrushThickness", 100);
        p->tip = thick < 99.5f ? TIP_OVAL : TIP_ROUND;
        p->aspect = p->tip == TIP_OVAL ? clampf(thick / 100, 0.05f, 1) : 1;
        float rot = num(&r, "BrushRotation", 0) + (num(&r, "BrushVerticalThicknes", 0) != 0 ? 90 : 0);
        p->angle = roundf(fmodf(fmodf(rot, 180) + 180, 180));
        bool in = num(&r, "BrushUseIn", 0) != 0, out = num(&r, "BrushUseOut", 0) != 0;
        p->taper_s = in ? 50 : 0; p->taper_e = out ? 50 : 0;
        p->opacity = clampf(roundf(num(&r, "Opacity", 100)), 1, 100);
        p->fit = p->smoothing; p->tex_density = 60; p->tex_scale = 100;
        // what a solid vector tip cannot reproduce yet
        if (num(&r, "BrushUsePatternImage", 0) != 0 && blob(&r, "BrushPatternImageArray", &len) && len > 0) *approx |= 1;
        if (has(&r, "TextureImage") && blob(&r, "TextureImage", &len) && len > 0) *approx |= 2;
        if (num(&r, "BrushUseSpray", 0) != 0) *approx |= 4;
        if (num(&r, "BrushUseWaterColor", 0) != 0 || num(&r, "BrushUseWaterEdge", 0) != 0) *approx |= 8;
    }
    SQ.finalize(r.st);
    return ok;
}

bool import_sut(const char *path, char *msg, int cap) {
    if (!sq_load()) { snprintf(msg, (size_t)cap, "Clip Studio brushes need winsqlite3.dll (Windows 10 or newer)"); return false; }
    sqlite3 *db = NULL;
    if (SQ.open_v2(path, &db, SQ_OPEN_READONLY, NULL) || !db) {
        if (db) SQ.close(db);
        snprintf(msg, (size_t)cap, "Could not open %s", path);
        return false;
    }
    NodeRef *nodes = NULL;
    sqlite3_stmt *st = NULL;
    if (!SQ.prepare_v2(db, "SELECT NodeName, NodeVariantID FROM Node WHERE NodeVariantID IS NOT NULL AND NodeVariantID > 0", -1, &st, NULL) && st) {
        while (SQ.step(st) == SQ_ROW && arrlen(nodes) < 64) {
            NodeRef n = {.variant = (long long)SQ.column_double(st, 1)};
            const unsigned char *t = SQ.column_text(st, 0);
            snprintf(n.name, sizeof(n.name), "%s", t ? (const char *)t : "");
            arrput(nodes, n);
        }
    }
    if (st) SQ.finalize(st);
    if (!arrlen(nodes)) {
        SQ.close(db);
        arrfree(nodes);
        snprintf(msg, (size_t)cap, "Not a Clip Studio Paint sub tool (.sut) file");
        return false;
    }
    char fallback[64];
    base_noext(path, fallback, sizeof(fallback));
    int added = 0, first = (int)arrlen(app.presets);
    u32 approx = 0;
    BrushPreset last = {0};
    for (ptrdiff_t i = 0; i < arrlen(nodes); i++) {
        BrushPreset p = {0};
        if (!read_variant(db, nodes[i].variant, &p, &approx)) continue;
        const char *name = nodes[i].name[0] && font_covers(nodes[i].name) ? nodes[i].name : fallback;
        if (arrlen(nodes) > 1 && name == fallback) snprintf(p.name, sizeof(p.name), "%s %d", fallback, (int)i + 1);
        else snprintf(p.name, sizeof(p.name), "%s", name);
        app_add_preset(&p);
        last = p;
        added++;
    }
    SQ.close(db);
    arrfree(nodes);
    if (!added) { snprintf(msg, (size_t)cap, "No brush settings found in %s", fallback); return false; }
    app_apply_preset(first);
    app.right_tab = 0;
    if (!app_tool_paints(app.tool) || app.tool == 'E') app.tool = 'B';
    char what[160];
    if (added == 1) {
        static const char *tips[] = {"round", "oval", "square"};
        bool flat = true;
        for (int k = 0; k < last.curve.n; k++) flat &= last.curve.y[k] > 0.99f;
        snprintf(what, sizeof(what), "Imported \"%s\": %d px, %s tip%s", last.name, (int)last.size, tips[last.tip],
                 flat ? "" : ", pressure curve");
    } else snprintf(what, sizeof(what), "Imported %d brushes from %s", added, fallback);
    if (approx) snprintf(msg, (size_t)cap, "%s. %s drawn as a solid vector tip", what,
                         approx & 3 ? "texture" : approx & 4 ? "spray" : "watercolor");
    else snprintf(msg, (size_t)cap, "%s", what);
    return true;
}
