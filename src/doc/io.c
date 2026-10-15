// .vxa files: a small versioned binary format. Written to a temp file and renamed, so a crash never
// leaves a half-written document behind.
#include "doc/doc.h"
#include "platform/platform.h"
#include <windows.h>

#define VXA_MAGIC 0x32415856u   // "VXA2"
#define VXA_VERSION 6u   // 6: image strokes; 5: eases on keys, the background colour   // 2: camera keys, audio layers and the embedded sounds; 3: export settings;
                         // 4: bitmaps, item blend modes, see-through layers, Animate's blend mode order

typedef struct { u8 *p; size_t n; } Buf;
static void put(Buf *b, const void *src, size_t n) { memcpy(arraddnptr(b->p, n), src, n); }
static void put32(Buf *b, u32 v) { put(b, &v, 4); }
static void putf(Buf *b, float v) { put(b, &v, 4); }
static void putstr(Buf *b, const char *s, int cap) { char tmp[512] = {0}; snprintf(tmp, sizeof(tmp), "%s", s); put(b, tmp, cap); }

static void put_timeline(Buf *b, const Timeline *t) {
    put32(b, (u32)t->len); put32(b, (u32)t->active); put32(b, (u32)t->frame);
    put32(b, (u32)arrlen(t->layers));
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) {
        const Layer *l = &t->layers[i];
        put32(b, (u32)l->id); putstr(b, l->name, 48); put32(b, l->type); put32(b, l->color); put32(b, (u32)l->parent);
        put32(b, (u32)l->len); put32(b, (u32)l->hidden | ((u32)l->locked << 1) | ((u32)l->collapsed << 2) | ((u32)l->transparent << 3) | ((u32)l->see_pct << 8));
        putf(b, l->opacity); put32(b, l->blend);
        put32(b, (u32)arrlen(l->keys));
        for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) {
            const Key *key = &l->keys[k];
            put32(b, (u32)key->f); put32(b, (u32)key->d); put32(b, key->tw); putstr(b, key->label, 32);
            for (int c = 0; c < 4; c++) putf(b, key->cam[c]);
            u8 e[4] = {key->ease, key->ease_dir, key->nez, 0};
            put(b, e, 4); putf(b, key->ease_amt);
            for (int q = 0; q < key->nez; q++) { putf(b, key->ez[q].x); putf(b, key->ez[q].y); }
        }
        put32(b, (u32)l->sound); put32(b, (u32)l->sound_frame);
    }
}

bool doc_save(Doc *d, const char *path) {
    Buf b = {0};
    put32(&b, VXA_MAGIC); put32(&b, VXA_VERSION);
    putstr(&b, d->name, 64);
    put32(&b, (u32)d->w); put32(&b, (u32)d->h); put32(&b, (u32)d->fps);
    put32(&b, d->loop_on); put32(&b, (u32)d->loop_a); put32(&b, (u32)d->loop_b);
    put32(&b, d->onion_on); put32(&b, (u32)d->onion_prev); put32(&b, (u32)d->onion_next);
    put32(&b, (u32)d->next_id);
    put_timeline(&b, &d->scene);
    put32(&b, (u32)arrlen(d->symbols));
    for (ptrdiff_t i = 0; i < arrlen(d->symbols); i++) {
        Symbol *s = &d->symbols[i];
        put32(&b, (u32)s->id); putstr(&b, s->name, 64); put32(&b, s->type);
        put_timeline(&b, &s->tl);
    }
    put32(&b, (u32)arrlen(d->drawings));
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++) {
        Drawing *dr = &d->drawings[i];
        put32(&b, dr->used);
        put32(&b, (u32)arrlen(dr->items));
        for (ptrdiff_t k = 0; k < arrlen(dr->items); k++) {
            Item *it = &dr->items[k];
            u8 hdr[8] = {it->kind, it->erase, it->tip, it->texture, it->flags, it->loop, it->blend, 0};
            put(&b, hdr, 8);
            put32(&b, it->color); putf(&b, it->angle); putf(&b, it->aspect);
            put32(&b, (u32)arrlen(it->pts)); if (arrlen(it->pts)) put(&b, it->pts, sizeof(SPt) * arrlen(it->pts));
            put32(&b, (u32)arrlen(it->poly)); if (arrlen(it->poly)) put(&b, it->poly, sizeof(V2) * arrlen(it->poly));
            put32(&b, (u32)arrlen(it->starts)); if (arrlen(it->starts)) put(&b, it->starts, sizeof(int) * arrlen(it->starts));
            put32(&b, (u32)it->sym); put32(&b, (u32)it->first); put(&b, &it->m, sizeof(Mat)); putf(&b, it->alpha);
            // 4: colour effect, filters, gradient
            u8 x[4] = {it->cx.kind, it->grad, it->spread, 0};
            put(&b, x, 4); putf(&b, it->cx.amount); put32(&b, it->cx.tint);
            for (int q = 0; q < 4; q++) { putf(&b, it->cx.mul[q]); putf(&b, it->cx.add[q]); }
            put32(&b, (u32)arrlen(it->filters));
            for (ptrdiff_t q = 0; q < arrlen(it->filters); q++) {
                const Filter *f = &it->filters[q];
                u8 h[4] = {f->type, f->quality, f->flags, 0};
                put(&b, h, 4); putf(&b, f->bx); putf(&b, f->by); putf(&b, f->strength); putf(&b, f->angle); putf(&b, f->dist);
                put32(&b, f->color); for (int k = 0; k < 4; k++) putf(&b, f->adj[k]);
            }
            put32(&b, (u32)arrlen(it->stops));
            for (ptrdiff_t q = 0; q < arrlen(it->stops); q++) { putf(&b, it->stops[q].pos); put32(&b, it->stops[q].c); }
            put32(&b, (u32)it->img); putf(&b, it->img_gap);   // 6
            putf(&b, it->img_r0); putf(&b, it->img_r1); putf(&b, it->img_w0); putf(&b, it->img_w1);
            put32(&b, (u32)it->dual_img); putf(&b, it->dual_gap); putf(&b, it->dual_angle); putf(&b, it->dual_scale);
            u8 dl[4] = {it->dual_flags, 0, 0, 0};
            put(&b, dl, 4);
            u8 im[4] = {it->img_mode, it->img_flags, 0, 0};
            put(&b, im, 4);
        }
    }
    // the sounds audio layers use, embedded as their original files (WAV / MP3 / OGG)
    int ids[256], ns = 0;
    Timeline *tls[256]; int nt = 0;
    tls[nt++] = &d->scene;
    for (ptrdiff_t i = 0; i < arrlen(d->symbols) && nt < 256; i++) tls[nt++] = &d->symbols[i].tl;
    for (int t = 0; t < nt; t++)
        for (ptrdiff_t i = 0; i < arrlen(tls[t]->layers); i++) {
            int id = tls[t]->layers[i].sound;
            bool seen = false;
            for (int k = 0; k < ns; k++) seen |= ids[k] == id;
            if (id && !seen && ns < 256 && sound_get(id)) ids[ns++] = id;
        }
    put32(&b, (u32)ns);
    for (int k = 0; k < ns; k++) {
        Sound *so = sound_get(ids[k]);
        put32(&b, (u32)so->id); putstr(&b, so->name, 64); put32(&b, so->size); put(&b, so->data, so->size);
    }
    // Export Symbols settings
    put32(&b, (u32)d->xp.format); putstr(&b, d->xp.id, 48); putf(&b, d->xp.scale); putf(&b, d->xp.pos[0]); putf(&b, d->xp.pos[1]);
    put32(&b, (u32)arrlen(d->xp.clips));
    for (ptrdiff_t i = 0; i < arrlen(d->xp.clips); i++) {
        ExportClip *c = &d->xp.clips[i];
        putstr(&b, c->key, 64); putstr(&b, c->anim, 48); putstr(&b, c->prefix, 64);
        put32(&b, (u32)c->fps); put32(&b, c->loop); putf(&b, c->off[0]); putf(&b, c->off[1]);
    }
    // bitmaps: the library's and any placed one, as image files
    int *bms = NULL;
    for (ptrdiff_t i = 0; i < arrlen(d->bitmaps); i++) if (bitmap_get(d->bitmaps[i])) arrput(bms, d->bitmaps[i]);
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++)
        for (ptrdiff_t k = 0; d->drawings[i].used && k < arrlen(d->drawings[i].items); k++) {
            Item *it = &d->drawings[i].items[k];
            int use[3] = {(it->kind == IT_BITMAP || it->kind == IT_FILL || (it->kind == IT_STROKE && it->texture)) ? it->sym : 0,
                          it->kind == IT_STROKE ? it->img : 0, it->kind == IT_STROKE ? it->dual_img : 0};
            for (int u = 0; u < 3; u++) {
                if (!use[u] || !bitmap_get(use[u])) continue;
                bool seen = false;
                for (ptrdiff_t q = 0; q < arrlen(bms); q++) seen |= bms[q] == use[u];
                if (!seen) arrput(bms, use[u]);
            }
        }
    put32(&b, (u32)arrlen(bms));
    for (ptrdiff_t i = 0; i < arrlen(bms); i++) {
        Bitmap *bm = bitmap_get(bms[i]);
        u32 size = 0;
        const u8 *file = bitmap_encoded(bm, &size);
        put32(&b, (u32)bm->id); putstr(&b, bm->name, 64); put32(&b, bm->smooth); put32(&b, size); if (size) put(&b, file, size);
    }
    put32(&b, (u32)arrlen(d->bitmaps));
    for (ptrdiff_t i = 0; i < arrlen(d->bitmaps); i++) put32(&b, (u32)d->bitmaps[i]);
    put32(&b, d->bg);   // 5
    arrfree(bms);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = plat_fopen(tmp, "wb");
    bool ok = f && fwrite(b.p, 1, arrlen(b.p), f) == (size_t)arrlen(b.p);
    if (f) ok = fclose(f) == 0 && ok;
    arrfree(b.p);
    if (!ok) { plat_remove(tmp); return false; }
    return plat_replace_file(tmp, path);
}

/* ── reading (every read is bounds-checked) ── */

typedef struct { const u8 *p; size_t n, at; bool bad; } Rd;
static bool get(Rd *r, void *dst, size_t n) {
    if (r->bad || r->at + n > r->n) { r->bad = true; memset(dst, 0, n); return false; }
    memcpy(dst, r->p + r->at, n); r->at += n; return true;
}
static u32 get32(Rd *r) { u32 v = 0; get(r, &v, 4); return v; }
static float getf(Rd *r) { float v = 0; get(r, &v, 4); return v; }
static void getstr(Rd *r, char *dst, int cap) { get(r, dst, cap); dst[cap - 1] = 0; }
static u32 getcount(Rd *r, size_t elem) { u32 n = get32(r); if ((size_t)n * elem > r->n - MIN(r->n, r->at)) { r->bad = true; return 0; } return n; }

static u32 file_ver;
static void get_timeline(Rd *r, Timeline *t) {
    t->len = (int)get32(r); t->active = (int)get32(r); t->frame = (int)get32(r);
    u32 nl = getcount(r, 64);
    for (u32 i = 0; i < nl && !r->bad; i++) {
        Layer l = {0};
        l.id = (int)get32(r); getstr(r, l.name, 48); l.type = (u8)get32(r); l.color = get32(r); l.parent = (int)get32(r);
        l.len = (int)get32(r);
        u32 fl = get32(r); l.hidden = fl & 1; l.locked = (fl >> 1) & 1; l.collapsed = (fl >> 2) & 1; l.transparent = (fl >> 3) & 1; l.see_pct = (u8)MIN((fl >> 8) & 255, 100);
        u32 bl = 0;
        l.opacity = getf(r); bl = get32(r);
        if (file_ver < 4) { static const u8 old[4] = {BLEND_NORMAL, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_ADD}; bl = bl < 4 ? old[bl] : BLEND_NORMAL; }
        l.blend = bl < BLEND_COUNT ? (u8)bl : BLEND_NORMAL;
        if (l.type >= LT_COUNT) l.type = LT_VECTOR;
        u32 nk = getcount(r, 44);
        for (u32 k = 0; k < nk && !r->bad; k++) {
            Key key = {0};
            key.f = (int)get32(r); key.d = (int)get32(r); key.tw = (u8)get32(r); getstr(r, key.label, 32);
            if (file_ver >= 2) for (int c = 0; c < 4; c++) key.cam[c] = getf(r);
            if (file_ver >= 5) {
                u8 e[4]; get(r, e, 4);
                key.ease = e[0] < EASE_COUNT ? e[0] : EASE_NONE; key.ease_dir = e[1] <= EASE_INOUT ? e[1] : EASE_IN; key.nez = MIN(e[2], EASE_PTS);
                key.ease_amt = getf(r);
                for (int q = 0; q < key.nez; q++) { key.ez[q].x = getf(r); key.ez[q].y = getf(r); }
                if (key.ease == EASE_CUSTOM && key.nez < 4) key.ease = EASE_NONE;
            }
            arrput(l.keys, key);
        }
        if (file_ver >= 2) { l.sound = (int)get32(r); l.sound_frame = (int)get32(r); }
        arrput(t->layers, l);
    }
    if (t->frame < 1) t->frame = 1;
    if (t->len < 1) t->len = 1;
}

Doc *doc_load(const char *path, char *err, int errcap) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "Can't open the file"); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *data = malloc(sz > 0 ? (size_t)sz : 1);
    size_t got = fread(data, 1, (size_t)MAX(sz, 0), f);
    fclose(f);
    Rd r = {data, got, 0, false};
    if (get32(&r) != VXA_MAGIC) { free(data); snprintf(err, errcap, "This is not a Vertexa file"); return NULL; }
    u32 ver = get32(&r);
    file_ver = ver;
    if (ver > VXA_VERSION) { free(data); snprintf(err, errcap, "This file comes from a newer Vertexa (format %u)", ver); return NULL; }
    Doc *d = calloc(1, sizeof(Doc));
    d->bg = hex(0xffffff);
    getstr(&r, d->name, 64);
    d->w = (int)get32(&r); d->h = (int)get32(&r); d->fps = (int)get32(&r);
    d->loop_on = get32(&r) != 0; d->loop_a = (int)get32(&r); d->loop_b = (int)get32(&r);
    d->onion_on = get32(&r) != 0; d->onion_prev = (int)get32(&r); d->onion_next = (int)get32(&r);
    d->next_id = (int)get32(&r);
    get_timeline(&r, &d->scene);
    u32 ns = getcount(&r, 80);
    for (u32 i = 0; i < ns && !r.bad; i++) {
        Symbol s = {0};
        s.id = (int)get32(&r); getstr(&r, s.name, 64); s.type = (u8)get32(&r);
        get_timeline(&r, &s.tl);
        arrput(d->symbols, s);
    }
    u32 nd = getcount(&r, 8);
    for (u32 i = 0; i < nd && !r.bad; i++) {
        Drawing dr = {0};
        dr.used = get32(&r) != 0;
        u32 ni = getcount(&r, 64);
        for (u32 k = 0; k < ni && !r.bad; k++) {
            Item it = {0};
            u8 hdr[8]; get(&r, hdr, 8);
            it.kind = hdr[0]; it.erase = hdr[1]; it.tip = hdr[2]; it.texture = hdr[3]; it.flags = hdr[4]; it.loop = hdr[5];
            it.blend = file_ver >= 4 && hdr[6] < BLEND_COUNT ? hdr[6] : BLEND_NORMAL;
            if (it.kind > IT_BITMAP) it.kind = IT_FILL;
            it.color = get32(&r); it.angle = getf(&r); it.aspect = getf(&r);
            u32 np = getcount(&r, sizeof(SPt)); if (np) get(&r, arraddnptr(it.pts, np), sizeof(SPt) * np);
            u32 nq = getcount(&r, sizeof(V2)); if (nq) get(&r, arraddnptr(it.poly, nq), sizeof(V2) * nq);
            u32 nst = getcount(&r, sizeof(int)); if (nst) get(&r, arraddnptr(it.starts, nst), sizeof(int) * nst);
            it.sym = (int)get32(&r); it.first = (int)get32(&r); get(&r, &it.m, sizeof(Mat)); it.alpha = getf(&r);
            if (file_ver >= 4) {
                u8 x[4]; get(&r, x, 4);
                it.cx.kind = x[0] <= CX_ADVANCED ? x[0] : CX_NONE; it.grad = x[1] <= GR_RADIAL ? x[1] : GR_NONE; it.spread = x[2] <= SP_REPEAT ? x[2] : SP_PAD;
                it.cx.amount = getf(&r); it.cx.tint = get32(&r);
                for (int q = 0; q < 4; q++) { it.cx.mul[q] = getf(&r); it.cx.add[q] = getf(&r); }
                u32 nf = getcount(&r, 44);
                for (u32 q = 0; q < nf && !r.bad; q++) {
                    Filter f = {0};
                    u8 h[4]; get(&r, h, 4);
                    f.type = h[0] < FX_COUNT ? h[0] : FX_BLUR; f.quality = (u8)CLAMP(h[1], 1, 3); f.flags = h[2];
                    f.bx = getf(&r); f.by = getf(&r); f.strength = getf(&r); f.angle = getf(&r); f.dist = getf(&r);
                    f.color = get32(&r); for (int k = 0; k < 4; k++) f.adj[k] = getf(&r);
                    arrput(it.filters, f);
                }
                u32 ng = getcount(&r, 8);
                for (u32 q = 0; q < ng && !r.bad; q++) { GStop g; g.pos = getf(&r); g.c = get32(&r); arrput(it.stops, g); }
                if (!arrlen(it.stops)) it.grad = GR_NONE;
            }
            if (file_ver >= 6) {
                it.img = (int)get32(&r); it.img_gap = getf(&r);
                it.img_r0 = getf(&r); it.img_r1 = getf(&r); it.img_w0 = getf(&r); it.img_w1 = getf(&r);
                it.dual_img = (int)get32(&r); it.dual_gap = getf(&r); it.dual_angle = getf(&r); it.dual_scale = getf(&r);
                u8 dl[4]; get(&r, dl, 4);
                it.dual_flags = dl[0];
                u8 im[4]; get(&r, im, 4);
                it.img_mode = im[0] <= IMG_ALONG ? im[0] : IMG_NONE; it.img_flags = im[1];
                if (!it.img_mode) it.img = 0;
            }
            item_bounds(&it);
            arrput(dr.items, it);
        }
        arrput(d->drawings, dr);
    }
    // sounds: they join this session's bank under new ids
    char serr[128] = "";
    if (file_ver >= 2 && !r.bad) {
        u32 ns = getcount(&r, 72);
        int map[256][2]; int nm = 0;
        for (u32 k = 0; k < ns && !r.bad; k++) {
            int old = (int)get32(&r);
            char name[64]; getstr(&r, name, 64);
            u32 sz = getcount(&r, 1);
            if (r.bad || r.at + sz > r.n) { r.bad = true; break; }
            int id = sound_add(name, r.p + r.at, sz, serr, sizeof(serr));
            r.at += sz;
            if (nm < 256) { map[nm][0] = old; map[nm][1] = id; nm++; }
        }
        Timeline *tl2[256]; int n2 = 0;
        tl2[n2++] = &d->scene;
        for (ptrdiff_t i = 0; i < arrlen(d->symbols) && n2 < 256; i++) tl2[n2++] = &d->symbols[i].tl;
        for (int t = 0; t < n2; t++)
            for (ptrdiff_t i = 0; i < arrlen(tl2[t]->layers); i++) {
                Layer *l = &tl2[t]->layers[i];
                int to = 0;
                for (int k = 0; k < nm; k++) if (map[k][0] == l->sound) to = map[k][1];
                l->sound = to;
            }
    }
    d->xp.scale = 1; d->xp.format = XF_VSLICE;
    if (file_ver >= 3 && !r.bad) {
        u32 fmt = get32(&r);   // read once: CLAMP evaluates its argument more than once
        d->xp.format = (int)MIN(fmt, (u32)XF_PSYCH); getstr(&r, d->xp.id, 48);
        d->xp.scale = getf(&r); d->xp.pos[0] = getf(&r); d->xp.pos[1] = getf(&r);
        if (!(d->xp.scale > 0.01f && d->xp.scale < 100)) d->xp.scale = 1;
        u32 nc = getcount(&r, 64 + 48 + 64 + 16);
        for (u32 i = 0; i < nc && !r.bad; i++) {
            ExportClip c = {0};
            getstr(&r, c.key, 64); getstr(&r, c.anim, 48); getstr(&r, c.prefix, 64);
            u32 fps = get32(&r);
            c.fps = (int)CLAMP(fps, 1u, 120u); c.loop = get32(&r) != 0; c.off[0] = getf(&r); c.off[1] = getf(&r);
            arrput(d->xp.clips, c);
        }
    }
    // bitmaps join this session's bank under new ids
    if (file_ver >= 4 && !r.bad) {
        u32 nb = getcount(&r, 76);
        struct { int from, to; } *map = NULL;
        for (u32 k = 0; k < nb && !r.bad; k++) {
            int old = (int)get32(&r);
            char name[64]; getstr(&r, name, 64);
            bool smooth = get32(&r) != 0;
            u32 sz = getcount(&r, 1);
            if (r.bad || r.at + sz > r.n) { r.bad = true; break; }
            char berr[96];
            int id = bitmap_add_file(name, r.p + r.at, sz, berr, sizeof(berr));
            r.at += sz;
            if (id) bitmap_get(id)->smooth = smooth;
            arrput(map, ((typeof(*map)){old, id}));
        }
        u32 nl = getcount(&r, 4);
        for (u32 k = 0; k < nl && !r.bad; k++) {
            int old = (int)get32(&r);
            for (ptrdiff_t q = 0; q < arrlen(map); q++) if (map[q].from == old && map[q].to) doc_add_bitmap(d, map[q].to);
        }
        for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++)
            for (ptrdiff_t k = 0; k < arrlen(d->drawings[i].items); k++) {
                Item *it = &d->drawings[i].items[k];
                if (it->kind == IT_STROKE && it->dual_img) {
                    int to = 0;
                    for (ptrdiff_t q = 0; q < arrlen(map); q++) if (map[q].from == it->dual_img) to = map[q].to;
                    it->dual_img = to;
                }
                if (it->kind == IT_STROKE && it->img) {
                    int to = 0;
                    for (ptrdiff_t q = 0; q < arrlen(map); q++) if (map[q].from == it->img) to = map[q].to;
                    it->img = to;
                    if (!to) it->img_mode = IMG_NONE;
                }
                if (it->kind != IT_BITMAP && !(it->kind == IT_FILL && it->sym) && !(it->kind == IT_STROKE && it->texture && it->sym)) continue;
                int to = 0;
                for (ptrdiff_t q = 0; q < arrlen(map); q++) if (map[q].from == it->sym) to = map[q].to;
                it->sym = to;
            }
        arrfree(map);
    }
    if (file_ver >= 5 && !r.bad) d->bg = get32(&r) | 0xff000000u;
    free(data);
    // validate references, so a damaged file can't crash the editor
    bool ok = !r.bad && d->w > 0 && d->h > 0 && d->fps > 0 && arrlen(d->scene.layers) > 0;
    Timeline *tls[256]; int nt = 0;
    tls[nt++] = &d->scene;
    for (ptrdiff_t i = 0; i < arrlen(d->symbols) && nt < 256; i++) tls[nt++] = &d->symbols[i].tl;
    for (int t = 0; t < nt && ok; t++)
        for (ptrdiff_t i = 0; i < arrlen(tls[t]->layers); i++)
            for (ptrdiff_t k = 0; k < arrlen(tls[t]->layers[i].keys); k++) {
                int di = tls[t]->layers[i].keys[k].d;
                if (di >= (int)arrlen(d->drawings)) ok = false;
            }
    if (!ok) { doc_free(d); snprintf(err, errcap, "The file is damaged"); return NULL; }
    snprintf(d->path, sizeof(d->path), "%s", path);
    return d;
}
