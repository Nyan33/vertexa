// Clip Studio Paint sub tool (.sut) import.
// A .sut file is an SQLite database: Node rows name the sub tools and point at Variant rows, which keep
// every brush setting in its own column. winsqlite3.dll ships with Windows 10+, so it is loaded on demand
// instead of bundling SQLite.
// Effector blobs (the pressure / tilt / velocity / random dynamics of a setting) are big-endian:
//   u32 header size (44), u32 available inputs, u32 enabled inputs (0x10 pressure, 0x20 tilt, 0x40 velocity,
//   0x80 random), u32 minimum %, ..., u32 sizes of the pressure and tilt curve blocks (at bytes 32 and 36), ...
// followed by those curve blocks: u32 12, u32 count, u32 16, count x (f64 x, f64 y): pressure, then tilt.
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
typedef struct { u32 inputs; float min; int n; float x[32], y[32]; int tn; float tx[32], ty[32]; } Effector;   // t*: tilt

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
    for (int blk = 0; blk < 2; blk++) {   // pressure, tilt
        u32 bsize = hl >= 40 ? be32(b + 32 + 4 * blk) : 0;
        if (!bsize) continue;
        if (at + 12 > (size_t)n || be32(b + at) != 12 || be32(b + at + 8) != 16) break;
        u32 cnt = be32(b + at + 4);
        int *cn = blk ? &e->tn : &e->n;
        float *cx = blk ? e->tx : e->x, *cy = blk ? e->ty : e->y;
        size_t p = at + 12;
        for (u32 i = 0; i < cnt && *cn < 32 && p + 16 <= (size_t)n; i++, p += 16) {
            double x = be64f(b + p), y = be64f(b + p + 8);
            if (!(x == x) || !(y == y)) break;   // NaN: not a curve after all
            cx[*cn] = clampf((float)x, 0, 1); cy[*cn] = clampf((float)y, 0, 1);
            (*cn)++;
        }
        at += bsize;
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

// what of a name the interface can show: characters without a glyph (Japanese, Chinese...) are left out,
// then brackets left empty and loose spaces; "" if nothing is left
static void shown_name(const char *s, char *out, int cap) {
    int k = 0;
    while (*s && k < cap - 5) {
        int n = (*s & 0x80) == 0 ? 1 : (*s & 0xe0) == 0xc0 ? 2 : (*s & 0xf0) == 0xe0 ? 3 : 4;
        char ch[5] = {0};
        for (int i = 0; i < n && s[i]; i++) ch[i] = s[i];
        if (font_covers(ch)) { memcpy(out + k, ch, strlen(ch)); k += (int)strlen(ch); }
        s += strlen(ch) ? (int)strlen(ch) : 1;
    }
    out[k] = 0;
    for (char *b; (b = strstr(out, "()")) || (b = strstr(out, "[]")); ) memmove(b, b + 2, strlen(b + 2) + 1);
    for (char *c = out; *c; c++) if (*c == ' ' && (c[1] == ' ' || c[1] == 0)) { memmove(c, c + 1, strlen(c)); c--; }
    while (out[0] == ' ' || out[0] == '-' || out[0] == '_') memmove(out, out + 1, strlen(out));
}

static void base_noext(const char *path, char *out, int cap) {
    const char *b = path;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    snprintf(out, (size_t)cap, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

/* ── tip images ──
   A brush's images are MaterialFile rows; FileData is a tar archive of the image material. Its picture
   (data/material_0.layer) is protected, but thumbnail/thumbnail.png is a plain PNG of the same image,
   which is what the tip uses (up to a few hundred pixels across). */

static const u8 *tar_find(const u8 *t, int n, const char *want, u32 *size) {
    for (int at = 0; at + 512 <= n;) {
        const u8 *h = t + at;
        if (!h[0]) break;
        char name[101];
        memcpy(name, h, 100); name[100] = 0;
        u32 sz = 0;
        for (int i = 124; i < 136 && h[i] >= '0' && h[i] <= '7'; i++) sz = sz * 8 + (u32)(h[i] - '0');
        if (at + 512 + (int)sz > n) break;
        if (!strcmp(name, want)) { *size = sz; return h + 512; }
        at += 512 + (int)((sz + 511) / 512 * 512);
    }
    return NULL;
}

// the first image of the brush (pattern names the materials by their catalog paths, in UTF-16)
static bool tip_image(sqlite3 *db, const u8 *pattern, int plen, const char *brush, char *name, int cap) {
    char want[1024];
    int k = 0;
    for (int i = 0; i < plen && k < (int)sizeof(want) - 1; i++) if (pattern[i] >= 32 && pattern[i] < 127) want[k++] = (char)pattern[i];
    want[k] = 0;
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (SQ.prepare_v2(db, "SELECT CatalogPath, FileData FROM MaterialFile", -1, &st, NULL) || !st) return false;
    for (int pass = 0; pass < 2 && !ok; pass++) {   // the one the brush names, else any
        while (!ok && SQ.step(st) == SQ_ROW) {
            const unsigned char *cp = SQ.column_text(st, 0);
            if (!pass && (!cp || !cp[0] || !strstr(want, (const char *)cp))) continue;
            const u8 *data = SQ.column_blob(st, 1);
            int n = SQ.column_bytes(st, 1);
            u32 size = 0;
            const u8 *png = data ? tar_find(data, n, "thumbnail/thumbnail.png", &size) : NULL;
            if (png && size > 8) ok = tipimg_add(brush, png, size, name, cap);
        }
        if (!ok) { SQ.finalize(st); st = NULL; if (SQ.prepare_v2(db, "SELECT CatalogPath, FileData FROM MaterialFile", -1, &st, NULL) || !st) break; }
    }
    if (st) SQ.finalize(st);
    return ok;
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
        p->size = clampf(roundf(num(&r, "BrushSize", 10)), 1, 400);
        p->smoothing = 50;
        Effector e;
        b = blob(&r, "BrushSizeEffector", &len);
        bool eff = effector(b, len, &e);
        if (eff && (e.inputs & IN_PRESSURE)) p->curve = csp_curve(&e);
        else p->curve = (Curve){2, {0, 1}, {1, 1}};   // size ignores pressure
        if (eff && (e.inputs & IN_TILT) && e.tn >= 2) {   // size by pen tilt, on top of pressure
            Effector t = {0};
            t.n = e.tn; memcpy(t.x, e.tx, sizeof(t.x)); memcpy(t.y, e.ty, sizeof(t.y));
            p->tilt = csp_curve(&t);
            p->tilt_on = true;
        }
        float thick = num(&r, "BrushThickness", 100);
        p->tip = thick < 99.5f ? TIP_OVAL : TIP_ROUND;
        p->aspect = p->tip == TIP_OVAL ? clampf(thick / 100, 0.05f, 1) : 1;
        float rot = num(&r, "BrushRotation", 0) + (num(&r, "BrushVerticalThicknes", 0) != 0 ? 90 : 0);
        p->angle = roundf(fmodf(fmodf(rot, 180) + 180, 180));
        bool in = num(&r, "BrushUseIn", 0) != 0, out = num(&r, "BrushUseOut", 0) != 0;
        p->taper_s = in ? 50 : 0; p->taper_e = out ? 50 : 0;
        p->opacity = clampf(roundf(num(&r, "Opacity", 100)), 1, 100);
        p->fit = p->smoothing; p->tex_density = 60; p->tex_scale = 100;
        // an image tip: stamped (turned across the line, as feathers and leaves are), or laid along it (ribbon)
        const u8 *pat = num(&r, "BrushUsePatternImage", 0) != 0 ? blob(&r, "BrushPatternImageArray", &len) : NULL;
        if (pat && len > 0) {
            u8 *copy = malloc((size_t)len);
            memcpy(copy, pat, (size_t)len);
            int plen = len;
            char base[64];
            snprintf(base, sizeof(base), "%s", p->name[0] ? p->name : "CSP tip");
            if (tip_image(db, copy, plen, base, p->tip_image, sizeof(p->tip_image))) {
                float rot = num(&r, "BrushRotation", 0);
                float across = fabsf(fmodf(fabsf(rot), 180) - 90);   // 0: square to the line
                p->img_mode = num(&r, "BrushRibbon", 0) != 0 && across > 30 ? IMG_ALONG : IMG_STAMPS;
                p->img_spacing = clampf(num(&r, "BrushInterval", 80), 1, 300);
                p->angle = p->img_mode == IMG_STAMPS ? roundf(fmodf(fmodf(rot, 360) + 360, 360)) : 0;
                p->tip = TIP_ROUND; p->aspect = 1;
                // CSP's thickness: across the image, by pressure (feathers: long and thin when light, short and round when heavy)
                float tk = num(&r, "BrushThickness", 100) / 100;
                p->img_w0 = p->img_w1 = 100 * tk;
                Effector te;
                b = blob(&r, "BrushThicknessEffector", &len);
                if (effector(b, len, &te) && (te.inputs & IN_PRESSURE)) {
                    Curve tc = csp_curve(&te);
                    p->img_w0 = 100 * tk * curve_eval(&tc, 0); p->img_w1 = 100 * tk * curve_eval(&tc, 1);
                }
                // stamps far apart are each a whole tip (feathers, leaves); dense ones draw a line and taper as one
                if (p->img_mode == IMG_STAMPS && p->img_spacing >= 50) p->taper_s = p->taper_e = 0;
                // the angle turns with the line only when the rotation follows its direction (0x40)
                if (!((int)num(&r, "BrushRotationEffector", 0) & 0x40)) p->img_flags |= IMGF_FIXED;
                // the dual brush: a second tip whose stamps mask the stroke
                const u8 *dpat = num(&r, "UseDualBrush", 0) != 0 && num(&r, "DualUsePatternImage", 0) != 0 ? blob(&r, "DualPatternImageArray", &len) : NULL;
                if (dpat && len > 0) {
                    bool same = len == plen && !memcmp(dpat, copy, (size_t)len);
                    char dbase[64];
                    snprintf(dbase, sizeof(dbase), "%.56s dual", base);
                    if (same) snprintf(p->dual_image, sizeof(p->dual_image), "%s", p->tip_image);
                    else {
                        u8 *dcopy = malloc((size_t)len);
                        memcpy(dcopy, dpat, (size_t)len);
                        if (!tip_image(db, dcopy, len, dbase, p->dual_image, sizeof(p->dual_image))) p->dual_image[0] = 0;
                        free(dcopy);
                    }
                    if (p->dual_image[0]) {
                        float bs = MAX(1, num(&r, "BrushSize", 10));
                        p->dual_on = true;
                        p->dual_spacing = clampf(num(&r, "DualInterval", 10), 1, 300);
                        float drot = num(&r, "DualRotation", 0);
                        p->dual_angle = roundf(fmodf(fmodf(drot, 360) + 360, 360));
                        p->dual_scale = num(&r, "SyncDualBrushSize", 0) != 0 ? 100 : clampf(num(&r, "DualSize", bs) / bs * 100, 5, 400);
                        p->dual_flags = ((int)num(&r, "DualRotationEffector", 0) & 0x40) ? 0 : IMGF_FIXED;
                        *approx |= 32;
                    }
                }
                *approx |= 16;
            } else *approx |= 1;
            free(copy);
        }
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
    char fallback[64], fname[64];
    base_noext(path, fname, sizeof(fname));
    shown_name(fname, fallback, sizeof(fallback));
    if (!fallback[0]) snprintf(fallback, sizeof(fallback), "Clip Studio brush");
    int added = 0, first = (int)arrlen(app.presets);
    u32 approx = 0;
    BrushPreset last = {0};
    for (ptrdiff_t i = 0; i < arrlen(nodes); i++) {
        BrushPreset p = {0};
        char shown[64];
        shown_name(nodes[i].name, shown, sizeof(shown));
        const char *name = shown[0] ? shown : fallback;
        if (arrlen(nodes) > 1 && name == fallback) snprintf(p.name, sizeof(p.name), "%s %d", fallback, (int)i + 1);
        else snprintf(p.name, sizeof(p.name), "%s", name);
        if (!read_variant(db, nodes[i].variant, &p, &approx)) continue;
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
    (void)approx;
    if (added == 1) snprintf(msg, (size_t)cap, "Imported \"%s\"", last.name);
    else snprintf(msg, (size_t)cap, "Imported %d brushes", added);
    return true;
}
