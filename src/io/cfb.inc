// Microsoft Compound File Binary ("OLE2 structured storage") reader — the container of pre-CS5 .fla files.
// Included by fla.c.

typedef struct { char name[128]; int id; } CfbPath;
typedef struct { char name[128]; int type; u32 left, right, child, start; u64 size; } CfbEnt;
typedef struct {
    const u8 *d; size_t n;
    u32 ssize, mini_ssize, mini_cutoff;
    u32 *fat, *minifat;
    u8 *ministream; size_t ministream_n;
    CfbEnt *ents;
    CfbPath *paths; int npaths;
} Cfb;

#define CFB_END 0xfffffffeu
#define CFB_FREE 0xffffffffu

static u32 cfb_u32(const u8 *d, size_t n, size_t at) { if (at + 4 > n) return CFB_END; u32 v; memcpy(&v, d + at, 4); return v; }
static u16 cfb_u16(const u8 *d, size_t n, size_t at) { if (at + 2 > n) return 0; return (u16)(d[at] | (d[at + 1] << 8)); }

static u8 *cfb_chain(Cfb *c, u32 s, u64 size, bool mini, size_t *outn) {
    u8 *out = NULL;
    u32 *fat = mini ? c->minifat : c->fat;
    u32 sec = mini ? c->mini_ssize : c->ssize;
    size_t guard = 0, fn = (size_t)arrlen(fat);
    while (s < CFB_END && guard++ <= fn) {
        if (mini) {
            size_t at = (size_t)s * sec;
            if (at + sec > c->ministream_n) break;
            memcpy(arraddnptr(out, sec), c->ministream + at, sec);
        } else {
            size_t at = (size_t)(s + 1) * sec;
            if (at >= c->n) break;
            size_t k = MIN(sec, c->n - at);
            memcpy(arraddnptr(out, k), c->d + at, k);
        }
        if (size && (u64)arrlen(out) >= size) break;
        if (s >= fn) break;
        s = fat[s];
    }
    if (size && (u64)arrlen(out) > size) arrsetlen(out, (size_t)size);
    *outn = (size_t)arrlen(out);
    return out;
}

static void cfb_collect(Cfb *c, u32 id, const char *prefix, int depth) {
    if (id == CFB_FREE || id >= (u32)arrlen(c->ents) || depth > 64) return;
    CfbEnt *e = &c->ents[id];
    cfb_collect(c, e->left, prefix, depth + 1);
    char path[256];
    if (prefix[0]) snprintf(path, sizeof(path), "%s/%s", prefix, e->name); else snprintf(path, sizeof(path), "%s", e->name);
    if (e->type == 2) { CfbPath p = {.id = (int)id}; snprintf(p.name, sizeof(p.name), "%s", path); arrput(c->paths, p); c->npaths++; }
    else if (e->type == 1) cfb_collect(c, e->child, path, depth + 1);
    cfb_collect(c, e->right, prefix, depth + 1);
}

static void cfb_close(Cfb *c) { arrfree(c->fat); arrfree(c->minifat); arrfree(c->ministream); arrfree(c->ents); arrfree(c->paths); }

static bool cfb_open(Cfb *c, const u8 *d, size_t n) {
    memset(c, 0, sizeof(*c));
    c->d = d; c->n = n;
    if (n < 512 || memcmp(d, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) return false;
    u16 sshift = cfb_u16(d, n, 30), mshift = cfb_u16(d, n, 32);
    if (sshift < 7 || sshift > 16 || mshift > sshift) return false;
    c->ssize = 1u << sshift; c->mini_ssize = 1u << mshift;
    u32 nfat = cfb_u32(d, n, 44), first_dir = cfb_u32(d, n, 48);
    c->mini_cutoff = cfb_u32(d, n, 56);
    u32 first_mini = cfb_u32(d, n, 60), nmini = cfb_u32(d, n, 64), difat = cfb_u32(d, n, 68), ndifat = cfb_u32(d, n, 72);
    u32 *fsecs = NULL;
    for (int i = 0; i < 109 && (u32)arrlen(fsecs) < nfat; i++) { u32 s = cfb_u32(d, n, 76 + i * 4); if (s == CFB_FREE) break; arrput(fsecs, s); }
    u32 per = c->ssize / 4;
    for (u32 k = 0; k < ndifat && difat < CFB_END; k++) {
        size_t base = (size_t)(difat + 1) * c->ssize;
        for (u32 i = 0; i + 1 < per && (u32)arrlen(fsecs) < nfat; i++) { u32 s = cfb_u32(d, n, base + i * 4); if (s != CFB_FREE) arrput(fsecs, s); }
        difat = cfb_u32(d, n, base + (per - 1) * 4);
    }
    for (ptrdiff_t i = 0; i < arrlen(fsecs); i++) {
        size_t base = (size_t)(fsecs[i] + 1) * c->ssize;
        if (base + c->ssize > n) { arrfree(fsecs); cfb_close(c); return false; }
        for (u32 k = 0; k < per; k++) arrput(c->fat, cfb_u32(d, n, base + k * 4));
    }
    arrfree(fsecs);
    size_t dn;
    u8 *dir = cfb_chain(c, first_dir, 0, false, &dn);
    for (size_t at = 0; at + 128 <= dn; at += 128) {
        CfbEnt e = {0};
        u16 nl = cfb_u16(dir, dn, at + 64);
        wchar_t w[33] = {0};
        if (nl >= 2 && nl <= 64) for (int i = 0; i < nl / 2 - 1; i++) w[i] = (wchar_t)cfb_u16(dir, dn, at + i * 2);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, e.name, sizeof(e.name), NULL, NULL);
        e.type = dir[at + 66];
        e.left = cfb_u32(dir, dn, at + 68); e.right = cfb_u32(dir, dn, at + 72); e.child = cfb_u32(dir, dn, at + 76);
        e.start = cfb_u32(dir, dn, at + 116); e.size = cfb_u32(dir, dn, at + 120);
        if (c->ssize > 512) e.size |= (u64)cfb_u32(dir, dn, at + 124) << 32;
        arrput(c->ents, e);
    }
    arrfree(dir);
    if (!arrlen(c->ents) || c->ents[0].type != 5) { cfb_close(c); return false; }
    c->ministream = cfb_chain(c, c->ents[0].start, c->ents[0].size, false, &c->ministream_n);
    if (nmini && first_mini < CFB_END) {
        size_t mn;
        u8 *mf = cfb_chain(c, first_mini, 0, false, &mn);
        for (size_t at = 0; at + 4 <= mn; at += 4) arrput(c->minifat, cfb_u32(mf, mn, at));
        arrfree(mf);
    }
    cfb_collect(c, c->ents[0].child, "", 0);
    return true;
}

static u8 *cfb_stream(Cfb *c, const char *name, size_t *len) {
    for (int i = 0; i < c->npaths; i++) {
        if (strcmp(c->paths[i].name, name)) continue;
        CfbEnt *e = &c->ents[c->paths[i].id];
        if (!e->size) return NULL;
        size_t n;
        u8 *a = cfb_chain(c, e->start, e->size, e->size < c->mini_cutoff, &n);
        u8 *out = malloc(n ? n : 1);
        memcpy(out, a, n);
        arrfree(a);
        *len = n;
        return out;
    }
    return NULL;
}
