// Minimal zip reader (stored and deflate entries) for XFL documents. Included by fla.c.
// Inflate comes from stb_image's zlib decoder.

char *stbi_zlib_decode_noheader_malloc(const char *buffer, int len, int *outlen);

typedef struct { char name[512]; u32 method, csize, usize, local; } ZipEnt;
typedef struct { const u8 *d; size_t n; ZipEnt *ents; } Zip;

static u32 zu32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 zu16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static void zip_close(Zip *z) { arrfree(z->ents); }

static bool zip_open(Zip *z, const u8 *d, size_t n) {
    memset(z, 0, sizeof(*z));
    z->d = d; z->n = n;
    if (n < 22) return false;
    size_t eocd = (size_t)-1;
    for (size_t i = n - 22; i + 1 > 0 && n - i < 66000; i--) { if (zu32(d + i) == 0x06054b50) { eocd = i; break; } if (!i) break; }
    if (eocd == (size_t)-1) return false;
    u32 count = zu16(d + eocd + 10), cd = zu32(d + eocd + 16);
    size_t at = cd;
    for (u32 i = 0; i < count; i++) {
        if (at + 46 > n || zu32(d + at) != 0x02014b50) break;
        ZipEnt e = {0};
        e.method = zu16(d + at + 10); e.csize = zu32(d + at + 20); e.usize = zu32(d + at + 24);
        u32 nl = zu16(d + at + 28), xl = zu16(d + at + 30), cl = zu16(d + at + 32);
        e.local = zu32(d + at + 42);
        if (at + 46 + nl > n) break;
        memcpy(e.name, d + at + 46, MIN(nl, sizeof(e.name) - 1));
        for (char *c = e.name; *c; c++) if (*c == '\\') *c = '/';
        arrput(z->ents, e);
        at += 46 + nl + xl + cl;
    }
    return arrlen(z->ents) > 0;
}

static char *zip_read(Zip *z, const char *name, size_t *len) {
    for (ptrdiff_t i = 0; i < arrlen(z->ents); i++) {
        ZipEnt *e = &z->ents[i];
        if (_stricmp(e->name, name)) continue;
        size_t lh = e->local;
        if (lh + 30 > z->n || zu32(z->d + lh) != 0x04034b50) return NULL;
        size_t data = lh + 30 + zu16(z->d + lh + 26) + zu16(z->d + lh + 28);
        if (data + e->csize > z->n) return NULL;
        if (e->method == 0) {
            char *out = malloc(e->csize + 1);
            memcpy(out, z->d + data, e->csize); out[e->csize] = 0;
            if (len) *len = e->csize;
            return out;
        }
        if (e->method != 8) return NULL;
        int ol = 0;
        char *raw = stbi_zlib_decode_noheader_malloc((const char *)z->d + data, (int)e->csize, &ol);
        if (!raw) return NULL;
        char *out = malloc((size_t)ol + 1);
        memcpy(out, raw, (size_t)ol); out[ol] = 0;
        free(raw);
        if (len) *len = (size_t)ol;
        return out;
    }
    return NULL;
}
