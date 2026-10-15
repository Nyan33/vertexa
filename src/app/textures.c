// Brush textures (paper grain) and tip images. The grains' built-ins are drawn here as tileable noise,
// the user's are image files in %APPDATA%/Vertexa/textures; tip images (stamped or laid along a stroke)
// are files in %APPDATA%/Vertexa/tips. A texture becomes a bitmap in the session's bank the first time a
// brush uses it; strokes keep that bitmap, so a document carries the textures it was drawn with.
#include "app/app.h"
#include "platform/platform.h"

typedef struct { char name[48]; char file[128]; int kind; int bitmap; } Tex;   // kind: built-in 1.., 0 = the user's file

enum { TX_PAPER = 1, TX_PENCIL, TX_CANVAS, TX_WATERCOLOR, TX_CHALK };
static const char *BUILTIN_TX[] = {NULL, "Paper", "Pencil", "Canvas", "Watercolor", "Chalk"};

/* ── tileable noise ── */

static float hash2(int x, int y, int seed) {
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u + (u32)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xffffff) / 16777215.0f;
}
// value noise with whole periods px, py across [0, 1)
static float vnoise(float u, float v, int px, int py, int seed) {
    float x = u * px, y = v * py;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    int xa = ((x0 % px) + px) % px, xb = (xa + 1) % px, ya = ((y0 % py) + py) % py, yb = (ya + 1) % py;
    float a = hash2(xa, ya, seed), b = hash2(xb, ya, seed), c = hash2(xa, yb, seed), d = hash2(xb, yb, seed);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}
static float fbm(float u, float v, int px, int py, int oct, int seed) {
    float s = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < oct; o++, px *= 2, py *= 2, amp *= 0.5f) { s += amp * vnoise(u, v, px, py, seed + o * 17); norm += amp; }
    return s / norm;
}
static float smooth(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

// lightness: 1 lets the ink through, 0 is where the paper keeps it off
static float grain(int kind, float u, float v) {
    switch (kind) {
    case TX_PAPER: return 0.3f + 0.7f * smooth(0.22f, 0.72f, fbm(u, v, 8, 8, 5, 1));
    case TX_PENCIL: return smooth(0.3f, 0.62f, 0.6f * fbm(u, v, 64, 64, 2, 2) + 0.4f * fbm(u, v, 16, 16, 3, 3));
    case TX_CANVAS: {
        int n = 32;
        float hx = 0.5f + 0.5f * cosf(2 * VX_PI * u * n), vy = 0.5f + 0.5f * cosf(2 * VX_PI * v * n);
        bool over = (((int)floorf(u * n * 2) + (int)floorf(v * n * 2)) & 1) != 0;
        float t = over ? hx : vy;
        return clampf(0.25f + 0.75f * powf(t, 0.7f) * (0.8f + 0.2f * fbm(u, v, 16, 16, 3, 4)), 0, 1);
    }
    case TX_WATERCOLOR: return clampf(0.35f + 0.55f * smooth(0.28f, 0.72f, fbm(u, v, 3, 3, 4, 5)) + 0.1f * fbm(u, v, 32, 32, 2, 6), 0, 1);
    case TX_CHALK: return smooth(0.28f, 0.5f, fbm(u, v, 24, 24, 3, 7));
    }
    return 1;
}

static int make_builtin(int kind) {
    const int S = 256;
    u8 *px = malloc((size_t)S * S * 4);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            u8 g = (u8)roundf(grain(kind, (x + 0.5f) / S, (y + 0.5f) / S) * 255);
            u8 *p = px + ((size_t)y * S + x) * 4;
            p[0] = p[1] = p[2] = g; p[3] = 255;
        }
    char name[64];
    snprintf(name, sizeof(name), "Texture %s", BUILTIN_TX[kind]);
    return bitmap_add_pixels(name, S, S, px, true);
}

/* ── the lists: grains (textures/) and tip images (tips/) ── */

typedef struct { Tex *t; bool scanned; const char *dir, *prefix; } Set;
static Set SETS[2] = {{NULL, false, "textures", "Texture"}, {NULL, false, "tips", "Tip"}};

static void name_for_file(const char *file, char *out, int cap) {
    char n[48];
    snprintf(n, sizeof(n), "%.47s", file);
    char *dot = strrchr(n, '.');
    if (dot) *dot = 0;
    snprintf(out, (size_t)cap, "%s", n);
    for (int k = 1; k < COUNT(BUILTIN_TX); k++) if (!strcmp(n, BUILTIN_TX[k])) snprintf(out, (size_t)cap, "%.38s (mine)", n);
}

static int find_in(Set *s, const char *name) {
    if (!name || !name[0]) return -1;
    for (ptrdiff_t i = 0; i < arrlen(s->t); i++) if (!strcmp(s->t[i].name, name)) return (int)i;
    return -1;
}

static Set *scan(int which) {
    Set *s = &SETS[which];
    if (s->scanned) return s;
    s->scanned = true;
    for (int k = 1; which == 0 && k < COUNT(BUILTIN_TX); k++) {
        Tex t = {0};
        snprintf(t.name, sizeof(t.name), "%s", BUILTIN_TX[k]);
        t.kind = k;
        arrput(s->t, t);
    }
    char dir[1024];
    if (!plat_config_path(s->dir, dir, sizeof(dir))) return s;
    static const char *pats[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp"};
    for (int p = 0; p < COUNT(pats); p++) {
        char names[64][128];
        int n = plat_list_files(dir, pats[p], names, 64);
        for (int i = 0; i < n; i++) {
            Tex t = {0};
            snprintf(t.file, sizeof(t.file), "%.127s", names[i]);
            name_for_file(names[i], t.name, sizeof(t.name));
            if (find_in(s, t.name) < 0) arrput(s->t, t);
        }
    }
    return s;
}

static int bitmap_of(int which, const char *name) {
    Set *s = scan(which);
    int i = find_in(s, name);
    if (i < 0) return 0;
    Tex *t = &s->t[i];
    if (t->bitmap && bitmap_get(t->bitmap)) return t->bitmap;
    if (t->kind) t->bitmap = make_builtin(t->kind);
    else {
        char dir[1024], path[1200];
        if (!plat_config_path(s->dir, dir, sizeof(dir))) return 0;
        snprintf(path, sizeof(path), "%s/%s", dir, t->file);
        FILE *f = plat_fopen(path, "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *data = n > 0 ? malloc((size_t)n) : NULL;
        bool ok = data && fread(data, 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        char err[200], bname[64];
        snprintf(bname, sizeof(bname), "%s %.50s", s->prefix, t->name);
        t->bitmap = ok ? bitmap_add_file(bname, data, (u32)n, err, sizeof(err)) : 0;
        free(data);
    }
    return t->bitmap;
}

// writes an image file into the set's folder under file_name; name gets what the brushes call it
static bool add_file(int which, const char *file_name, const u8 *data, size_t size, char *name, int cap) {
    Set *s = scan(which);
    char dir[1024], dst[1200];
    if (!plat_config_path(s->dir, dir, sizeof(dir)) || !plat_mkdirs(dir)) return false;
    snprintf(dst, sizeof(dst), "%s/%s", dir, file_name);
    FILE *out = plat_fopen(dst, "wb");
    if (!out) return false;
    bool ok = fwrite(data, 1, size, out) == size;
    ok &= fclose(out) == 0;
    if (!ok) return false;
    Tex t = {0};
    snprintf(t.file, sizeof(t.file), "%s", file_name);
    name_for_file(file_name, t.name, sizeof(t.name));
    int k = find_in(s, t.name);
    if (k >= 0) s->t[k] = t;   // the same file again: read anew
    else arrput(s->t, t);
    snprintf(name, (size_t)cap, "%s", t.name);
    return bitmap_of(which, t.name) != 0;
}

static bool import_file(int which, const char *path, char *name, int cap) {
    const char *base = path;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    FILE *in = plat_fopen(path, "rb");
    if (!in) return false;
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *data = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = data && fread(data, 1, (size_t)n, in) == (size_t)n;
    fclose(in);
    ok = ok && add_file(which, base, data, (size_t)n, name, cap);
    free(data);
    return ok;
}

int texture_count(void) { return (int)arrlen(scan(0)->t); }
const char *texture_name(int i) { Set *s = scan(0); return i >= 0 && i < arrlen(s->t) ? s->t[i].name : ""; }
bool texture_is_user(int i) { Set *s = scan(0); return i >= 0 && i < arrlen(s->t) && !s->t[i].kind; }
int texture_find(const char *name) { return find_in(scan(0), name); }
int texture_bitmap(const char *name) { return bitmap_of(0, name); }
bool texture_import(const char *path, char *name, int cap) { return import_file(0, path, name, cap); }

int tipimg_count(void) { return (int)arrlen(scan(1)->t); }
const char *tipimg_name(int i) { Set *s = scan(1); return i >= 0 && i < arrlen(s->t) ? s->t[i].name : ""; }
int tipimg_find(const char *name) { return find_in(scan(1), name); }
int tipimg_bitmap(const char *name) { return bitmap_of(1, name); }
bool tipimg_import(const char *path, char *name, int cap) { return import_file(1, path, name, cap); }
bool tipimg_add(const char *base, const u8 *png, size_t size, char *name, int cap) {
    char file[128];
    snprintf(file, sizeof(file), "%.100s.png", base);
    for (char *c = file; *c; c++) if (strchr("\\/:*?\"<>|", *c)) *c = '_';
    return add_file(1, file, png, size, name, cap);
}
