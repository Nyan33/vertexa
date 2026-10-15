// Exporters. Rendering happens one frame per app frame, so the UI keeps running and shows progress.
//  * PNG sequence of the scene (through its camera, if there is one).
//  * Clips (the Export Symbols window): every clip is one animation - a symbol, or a frame label range of
//    the scene when there are no symbols. Frames are rendered around one shared origin, trimmed and packed
//    into a Sparrow sheet (identical frames stored once); all of them share one frame box, the union of the
//    trimmed frames, so the game puts the character's feet at the same place (the box's bottom middle) in
//    every animation. FNF V-Slice and Psych get their character JSON next to the sheet.
//  * Thumbnails for the start screen.
#include "app/app.h"
#include "render/scene.h"
#include "platform/platform.h"
#include "stb/stb_image_write.h"
#include "stb/stb_rect_pack.h"
#include <ctype.h>
#include <windows.h>

typedef struct {
    int x0, y0, w, h;          // trimmed rect inside the render target (w = 0: an empty frame)
    u8 *px;                    // trimmed pixels (straight alpha), NULL if a duplicate
    u64 hash;
    int same_as;               // index of the identical earlier frame or -1
    char name[80];             // SubTexture name
    int ax, ay;                // atlas position
} Shot;

enum { JOB_PNG, JOB_CLIPS, JOB_THUMB };

static struct {
    bool active;
    int kind, format;
    char path[512], name[64];
    int frame, total, w, h;    // frame counts every frame of every clip
    float k;                   // thumbnails: scale of the stage
    float ox, oy;              // clips: clip space -> target pixels (translation)
    ClipSrc *clips;
    Target tgt; Pool pool;
    PipeSet pipes; bool pipes_ready;
    Frame F;
    bool pending;              // a frame was rendered this app frame and must be read back
    int failed;                // PNG frames that could not be written
    Shot *shots;
} J;

static void unpremultiply(u8 *px, int n) {
    for (int i = 0; i < n; i++) {
        u8 *p = px + i * 4;
        if (p[3] && p[3] < 255) for (int c = 0; c < 3; c++) p[c] = (u8)MIN(255, (p[c] * 255 + p[3] / 2) / p[3]);
    }
}

static bool start(int kind, const char *path, int w, int h, int total) {
    Doc *d = app.doc;
    if (J.active) return false;
    J.kind = kind; J.active = true; J.frame = 1; J.failed = 0; J.total = MAX(1, total); J.pending = false;
    J.w = MAX(1, w); J.h = MAX(1, h); J.k = 1;
    snprintf(J.path, sizeof(J.path), "%s", path);
    snprintf(J.name, sizeof(J.name), "%s", d->name);
    for (char *c = J.name; *c; c++) if (strchr(" /\\:*?\"<>|", *c) || (u8)*c < 0x20) *c = '-';   // a valid file name
    if (!J.pipes_ready) { J.pipes = gfx_make_pipes(8, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL); J.pipes_ready = true; }
    target_make(&J.tgt, J.w, J.h, 8);
    return true;
}

static int scene_end(Doc *d) {   // the last frame with drawing in it (a long song in an audio layer doesn't count)
    int end = 1;
    for (ptrdiff_t i = 0; i < arrlen(d->scene.layers); i++)
        if (d->scene.layers[i].type != LT_AUDIO && d->scene.layers[i].type != LT_CAMERA) end = MAX(end, d->scene.layers[i].len);
    return MIN(end, 1000);
}

bool export_png_sequence(const char *dir, char *msg, int cap) {
    if (!plat_mkdirs(dir)) { snprintf(msg, cap, "Couldn't create %s", dir); return false; }
    if (!start(JOB_PNG, dir, app.doc->w, app.doc->h, scene_end(app.doc))) { snprintf(msg, cap, "An export is already running"); return false; }
    snprintf(msg, cap, "Exporting %d frames\xE2\x80\xA6", J.total);
    return true;
}

bool export_thumbnail(const char *png) {   // the first frame, small, quietly in the background
    Doc *d = app.doc;
    float k = MIN(1.0f, MIN(480.0f / d->w, 270.0f / d->h));
    if (!start(JOB_THUMB, png, (int)roundf(d->w * k), (int)roundf(d->h * k), 1)) return false;
    J.k = k;
    return true;
}

/* ── clips ── */

ExportClip *export_clip_settings(Doc *d, const char *key, const char *name) {
    for (ptrdiff_t i = 0; i < arrlen(d->xp.clips); i++) if (!strcmp(d->xp.clips[i].key, key)) return &d->xp.clips[i];
    ExportClip c = {0};
    snprintf(c.key, sizeof(c.key), "%s", key);
    snprintf(c.prefix, sizeof(c.prefix), "%s", name);
    c.fps = d->fps;
    // a guess at the engine name from the symbol / label name ("BF NOTE LEFT MISS" -> singLEFTmiss)
    char low[64]; int n = 0;
    for (const char *s = name; *s && n < 63; s++) low[n++] = (char)tolower((u8)*s);
    low[n] = 0;
    bool miss = strstr(low, "miss") != NULL;
    const char *dir = strstr(low, "left") ? "LEFT" : strstr(low, "right") ? "RIGHT" : strstr(low, "up") ? "UP" : strstr(low, "down") ? "DOWN" : NULL;
    if (dir && !strstr(low, "dance")) snprintf(c.anim, sizeof(c.anim), "sing%s%s", dir, miss ? "miss" : "");
    else if (strstr(low, "dance") && dir) snprintf(c.anim, sizeof(c.anim), "dance%s", !strcmp(dir, "LEFT") ? "Left" : "Right");
    else if (strstr(low, "idle")) snprintf(c.anim, sizeof(c.anim), "idle");
    else if (strstr(low, "hey")) snprintf(c.anim, sizeof(c.anim), "hey");
    else if (strstr(low, "cheer")) snprintf(c.anim, sizeof(c.anim), "cheer");
    c.loop = strstr(low, "loop") != NULL;
    arrput(d->xp.clips, c);
    return &arrlast(d->xp.clips);
}

// the clips: the given symbols (every symbol when none are given) or, without symbols, the scene's labels
int export_clip_sources(Doc *d, const int *syms, int nsyms, ClipSrc *out, int cap) {
    int n = 0;
    for (int k = 0; k < (nsyms ? nsyms : (int)arrlen(d->symbols)) && n < cap; k++) {   // in the order they were picked
        Symbol *s = nsyms ? doc_symbol(d, syms[k]) : &d->symbols[k];
        if (!s) continue;
        ClipSrc c = {.sym = s->id, .f0 = 1, .f1 = doc_symbol_len(d, s->id)};
        snprintf(c.name, sizeof(c.name), "%s", s->name);
        snprintf(c.key, sizeof(c.key), "sym:%d", s->id);
        out[n++] = c;
    }
    if (n || arrlen(d->symbols)) return n;
    // no symbols: every label of the scene starts an animation that runs to the next label
    int end = scene_end(d);
    for (int f = 1; f <= end && n < cap; f++) {
        const char *lab = NULL;
        for (ptrdiff_t li = arrlen(d->scene.layers) - 1; li >= 0; li--) {
            Layer *l = &d->scene.layers[li];
            for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) if (l->keys[k].f == f && l->keys[k].label[0]) lab = l->keys[k].label;
        }
        if (!lab) continue;
        if (n) out[n - 1].f1 = f - 1;
        ClipSrc c = {.sym = 0, .f0 = f, .f1 = end};
        snprintf(c.name, sizeof(c.name), "%s", lab);
        snprintf(c.key, sizeof(c.key), "label:%s", lab);
        out[n++] = c;
    }
    if (!n) { ClipSrc c = {.sym = 0, .f0 = 1, .f1 = end}; snprintf(c.name, sizeof(c.name), "%s", d->name); snprintf(c.key, sizeof(c.key), "label:"); out[n++] = c; }
    return n;
}

int export_clip_len(const ClipSrc *c) { return MAX(1, c->f1 - c->f0 + 1); }

static Timeline *clip_tl(Doc *d, const ClipSrc *c) { return c->sym ? doc_symbol_tl(d, c->sym) : &d->scene; }

void export_render_clip(Frame *F, Doc *d, const ClipSrc *c, int frame, Mat m, float alpha) {
    Timeline *t = clip_tl(d, c);
    if (t) render_timeline(F, d, t, c->f0 + CLAMP(frame, 1, export_clip_len(c)) - 1, m, alpha, 0, 0, true);
}

// the drawing's bounds over every frame of the clips (tween ends included), in clip space
bool export_clip_bounds(Doc *d, const ClipSrc *cs, int n, float b[4]) {
    b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f;
    bool any = false;
    for (int i = 0; i < n; i++) {
        Timeline *t = clip_tl(d, &cs[i]);
        if (!t) continue;
        for (ptrdiff_t li = 0; li < arrlen(t->layers); li++) {
            Layer *l = &t->layers[li];
            if (l->type == LT_FOLDER || l->type == LT_AUDIO || l->type == LT_CAMERA || l->type == LT_GUIDE || tl_hidden(t, l)) continue;
            for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) {
                Key *key = &l->keys[k];
                if (key->d < 0 || doc_span_end(l, (int)k) < cs[i].f0 || key->f > cs[i].f1) continue;
                any |= drawing_bounds(d, &d->drawings[key->d], 0, mat_id(), b, 0);
            }
        }
    }
    return any;
}

bool export_clips(const ClipSrc *cs, int n, int format, const char *dest, char *msg, int cap) {
    Doc *d = app.doc;
    if (!n) { snprintf(msg, cap, "Nothing to export"); return false; }
    float b[4];
    if (!export_clip_bounds(d, cs, n, b)) { snprintf(msg, cap, "The animations are empty"); return false; }
    // a roomy target: the exact frame box comes from the trimmed frames afterwards
    float m = 16 + 0.1f * MAX(b[2] - b[0], b[3] - b[1]);
    int w = (int)ceilf(b[2] - b[0] + 2 * m), h = (int)ceilf(b[3] - b[1] + 2 * m);
    if (w > 8192 || h > 8192) { snprintf(msg, cap, "The character is too big (%d \xC3\x97 %d)", w, h); return false; }
    int total = 0;
    for (int i = 0; i < n; i++) total += export_clip_len(&cs[i]);
    if (!start(JOB_CLIPS, dest, w, h, total)) { snprintf(msg, cap, "An export is already running"); return false; }
    J.format = format;
    J.ox = m - b[0]; J.oy = m - b[1];
    arrsetlen(J.clips, 0);
    for (int i = 0; i < n; i++) arrput(J.clips, cs[i]);
    if (d->xp.id[0]) snprintf(J.name, sizeof(J.name), "%s", d->xp.id);
    for (char *c = J.name; *c; c++) if (strchr(" /\\:*?\"<>|", *c) || (u8)*c < 0x20) *c = '-';
    snprintf(msg, cap, "Exporting %d frames\xE2\x80\xA6", total);
    return true;
}

static int clip_of(int frame, int *local) {   // which clip a job frame belongs to
    int f = frame;
    for (int i = 0; i < arrlen(J.clips); i++) {
        int len = export_clip_len(&J.clips[i]);
        if (f <= len) { if (local) *local = f; return i; }
        f -= len;
    }
    if (local) *local = 1;
    return (int)arrlen(J.clips) - 1;
}

/* ── rendering ── */

int export_lists(DrawList **out, int cap) {
    if (!J.active || J.frame > J.total) return 0;
    Doc *d = app.doc;
    frame_begin(&J.F, &J.tgt, &J.pool, font_view());
    if (J.kind == JOB_CLIPS) {
        int local, ci = clip_of(J.frame, &local);
        export_render_clip(&J.F, d, &J.clips[ci], local, mat_tr(J.ox, J.oy), 1);
    } else {
        float cam[4];
        Mat cm = doc_camera_at(d, J.frame, cam) ? doc_camera_matrix(d, cam) : mat_id();   // a camera frames the export
        if (J.kind == JOB_THUMB) cm = mat_mul(mat_scale(J.k, J.k), cm);
        render_timeline(&J.F, d, &d->scene, J.frame, cm, 1, 0, 0, true);
    }
    return frame_lists(&J.F, out, cap);
}

void export_render(void) {
    if (!J.active || J.frame > J.total || !J.F.nsegs) return;
    static const float clear[4] = {0, 0, 0, 0};
    frame_passes(&J.F, &J.pipes, clear);
    J.pending = true;
}

static u64 hash_bytes(const u8 *p, size_t n, u64 h) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

static const char *clip_prefix(int ci) {
    ExportClip *s = export_clip_settings(app.doc, J.clips[ci].key, J.clips[ci].name);
    return s->prefix[0] ? s->prefix : J.clips[ci].name;
}

static void capture_frame(void) {
    size_t n = (size_t)J.w * J.h;
    u8 *px = malloc(n * 4);
    if (!plat_read_image(J.tgt.res, J.w, J.h, px)) { free(px); return; }
    unpremultiply(px, (int)n);
    if (J.kind == JOB_THUMB) {   // on the paper, as on the stage
        Color bg = app.doc->bg;
        for (size_t i = 0; i < n; i++) {
            u8 *p = px + i * 4; int a = p[3];
            for (int c = 0; c < 3; c++) p[c] = (u8)((p[c] * a + (int)((bg >> (8 * c)) & 255) * (255 - a)) / 255);
            p[3] = 255;
        }
        if (stbi_write_png(J.path, J.w, J.h, 4, px, J.w * 4)) home_thumb_ready(J.path);
        free(px);
        return;
    }
    if (J.kind == JOB_PNG) {
        char file[700];
        snprintf(file, sizeof(file), "%s\\%s_%04d.png", J.path, J.name, J.frame);
        if (!stbi_write_png(file, J.w, J.h, 4, px, J.w * 4)) J.failed++;
        free(px);
        return;
    }
    // trim transparent borders
    int x0 = J.w, y0 = J.h, x1 = -1, y1 = -1;
    for (int y = 0; y < J.h; y++) for (int x = 0; x < J.w; x++) if (px[((size_t)y * J.w + x) * 4 + 3]) { x0 = MIN(x0, x); x1 = MAX(x1, x); y0 = MIN(y0, y); y1 = MAX(y1, y); }
    bool empty = x1 < 0;
    if (empty) { x0 = y0 = 0; x1 = y1 = 0; }
    Shot s = {.x0 = x0, .y0 = y0, .w = x1 - x0 + 1, .h = y1 - y0 + 1, .same_as = -1};
    s.px = malloc((size_t)s.w * s.h * 4);
    for (int y = 0; y < s.h; y++) memcpy(s.px + (size_t)y * s.w * 4, px + ((size_t)(y0 + y) * J.w + x0) * 4, (size_t)s.w * 4);
    free(px);
    if (empty) s.w = s.h = 0;   // stays out of the shared frame box
    int dims[4] = {s.x0, s.y0, s.w, s.h};
    size_t bytes = (size_t)MAX(s.w, 1) * MAX(s.h, 1) * 4;
    s.hash = hash_bytes((const u8 *)dims, sizeof(dims), hash_bytes(s.px, bytes, 1469598103934665603ull));
    for (ptrdiff_t i = 0; i < arrlen(J.shots); i++)
        if (J.shots[i].same_as < 0 && J.shots[i].hash == s.hash && J.shots[i].w == s.w && J.shots[i].h == s.h && !memcmp(J.shots[i].px, s.px, bytes)) {
            s.same_as = (int)i; free(s.px); s.px = NULL; break;
        }
    int local, ci = clip_of(J.frame, &local);
    snprintf(s.name, sizeof(s.name), "%s%04d", clip_prefix(ci), local - 1);   // Sparrow names: prefix + index
    arrput(J.shots, s);
}

/* ── writing ── */

static const char *xml_esc(const char *in, char *out, int cap) {   // user text in XML attributes
    int o = 0;
    for (; *in && o < cap - 7; in++) {
        const char *r = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        if (r) { int k = (int)strlen(r); memcpy(out + o, r, (size_t)k); o += k; }
        else out[o++] = *in;
    }
    out[o] = 0;
    return out;
}
static const char *json_esc(const char *in, char *out, int cap) {   // ... and in JSON strings
    int o = 0;
    for (; *in && o < cap - 7; in++) {
        u8 c = (u8)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += snprintf(out + o, (size_t)(cap - o), "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

static bool write_atlas(const char *png, const char *image_name, char *msg, int cap) {
    int n = (int)arrlen(J.shots), uniq = 0;
    // the shared frame box: every frame's trimmed rect lies inside it
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    for (int i = 0; i < n; i++) {
        Shot *s = &J.shots[i];
        if (!s->w) continue;
        bx0 = MIN(bx0, s->x0); by0 = MIN(by0, s->y0); bx1 = MAX(bx1, s->x0 + s->w); by1 = MAX(by1, s->y0 + s->h);
    }
    if (bx1 < 0) { bx0 = by0 = 0; bx1 = by1 = 1; }
    stbrp_rect *rs = calloc(n ? n : 1, sizeof(stbrp_rect));
    for (int i = 0; i < n; i++) if (J.shots[i].same_as < 0) { rs[uniq].id = i; rs[uniq].w = MAX(1, J.shots[i].w) + 2; rs[uniq].h = MAX(1, J.shots[i].h) + 2; uniq++; }
    int W = 256, H = 256;
    bool packed = false;
    stbrp_node *nodes = malloc(sizeof(stbrp_node) * 8192);
    while (!packed && W <= 8192) {
        stbrp_context c;
        stbrp_init_target(&c, W, H, nodes, 8192);
        packed = stbrp_pack_rects(&c, rs, uniq) == 1;
        if (!packed) { if (W <= H) W *= 2; else H *= 2; }
    }
    free(nodes);
    if (!packed) { free(rs); snprintf(msg, cap, "The frames don't fit in an 8192 px sheet. Use fewer or smaller frames"); return false; }
    int uw = 1, uh = 1;   // shrink to the used area
    for (int i = 0; i < uniq; i++) { uw = MAX(uw, rs[i].x + rs[i].w); uh = MAX(uh, rs[i].y + rs[i].h); }
    u8 *sheet = calloc((size_t)uw * uh, 4);
    for (int i = 0; i < uniq; i++) {
        Shot *s = &J.shots[rs[i].id];
        s->ax = rs[i].x + 1; s->ay = rs[i].y + 1;
        for (int y = 0; y < s->h; y++) memcpy(sheet + ((size_t)(s->ay + y) * uw + s->ax) * 4, s->px + (size_t)y * s->w * 4, (size_t)s->w * 4);
    }
    free(rs);
    bool ok = stbi_write_png(png, uw, uh, 4, sheet, uw * 4) != 0;
    free(sheet);
    char xml[700];
    snprintf(xml, sizeof(xml), "%.*s.xml", (int)(strrchr(png, '.') ? strrchr(png, '.') - png : (long)strlen(png)), png);
    FILE *f = plat_fopen(xml, "wb");
    if (!f || !ok) { if (f) fclose(f); snprintf(msg, cap, "Couldn't write %s", ok ? xml : png); return false; }
    char e[400];
    fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- Created with Vertexa -->\n<TextureAtlas imagePath=\"%s\">\n", xml_esc(image_name, e, sizeof(e)));
    for (int i = 0; i < n; i++) {
        Shot *s = &J.shots[i], *src = s->same_as >= 0 ? &J.shots[s->same_as] : s;
        fprintf(f, "\t<SubTexture name=\"%s\" x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" frameX=\"%d\" frameY=\"%d\" frameWidth=\"%d\" frameHeight=\"%d\"/>\n",
                xml_esc(s->name, e, sizeof(e)), src->ax, src->ay, MAX(1, src->w), MAX(1, src->h), -(src->x0 - bx0), -(src->y0 - by0), bx1 - bx0, by1 - by0);
    }
    fprintf(f, "</TextureAtlas>\n");
    fclose(f);
    snprintf(msg, cap, "%d frames, %d unique, sheet %d\xC3\x97%d", n, uniq, uw, uh);
    return true;
}

static void write_character(char *msg, int cap) {
    Doc *d = app.doc;
    bool vs = J.format == XF_VSLICE;
    char img_dir[600], png[700], json_dir[600], json[700], m2[200];
    snprintf(img_dir, sizeof(img_dir), "%s\\images\\characters", J.path);
    snprintf(json_dir, sizeof(json_dir), vs ? "%s\\data\\characters" : "%s\\characters", J.path);
    plat_mkdirs(img_dir); plat_mkdirs(json_dir);
    snprintf(png, sizeof(png), "%s\\%s.png", img_dir, J.name);
    char image_name[80]; snprintf(image_name, sizeof(image_name), "%s.png", J.name);
    if (!write_atlas(png, image_name, m2, sizeof(m2))) { snprintf(msg, cap, "%s", m2); return; }
    snprintf(json, sizeof(json), "%s\\%s.json", json_dir, J.name);
    FILE *f = plat_fopen(json, "wb");
    if (!f) { snprintf(msg, cap, "Couldn't write %s", json); return; }
    int n = MIN((int)arrlen(J.clips), 64);
    char en[64][100], ep[64][140], id[120];
    ExportClip st[64];
    const char *start_anim = NULL;
    for (int i = 0; i < n; i++) {
        st[i] = *export_clip_settings(d, J.clips[i].key, J.clips[i].name);
        const char *anim = st[i].anim[0] ? st[i].anim : J.clips[i].name;
        // Sparrow prefixes match by "starts with": "Left" would also take "Left Miss0000". Frame names are
        // <prefix><4 digits>, so "<prefix>0" pins it when another prefix starts the same way.
        char pre[64]; snprintf(pre, sizeof(pre), "%s", clip_prefix(i));
        bool clash = false;
        for (int k = 0; k < n; k++) { const char *o = clip_prefix(k); if (k != i && strlen(o) > strlen(pre) && !strncmp(o, pre, strlen(pre))) clash = true; }
        char p2[80]; snprintf(p2, sizeof(p2), clash ? "%s0" : "%s", pre);
        json_esc(anim, en[i], sizeof(en[i])); json_esc(p2, ep[i], sizeof(ep[i]));
        if (!start_anim || !strcmp(anim, "idle") || (!strcmp(anim, "danceRight") && strcmp(start_anim, "idle"))) start_anim = st[i].anim[0] ? st[i].anim : J.clips[i].name;
    }
    char sa[100]; json_esc(start_anim ? start_anim : "idle", sa, sizeof(sa));
    json_esc(J.name, id, sizeof(id));
    float sc = d->xp.scale > 0 ? d->xp.scale : 1;
    if (vs) {   // funkin.data.character.CharacterData 1.0.x
        char nm[120]; json_esc(d->xp.id[0] ? d->xp.id : d->name, nm, sizeof(nm));
        fprintf(f, "{\n  \"version\": \"1.0.0\",\n  \"name\": \"%s\",\n  \"renderType\": \"sparrow\",\n  \"assetPath\": \"characters/%s\",\n", nm, id);
        fprintf(f, "  \"scale\": %g,\n  \"offsets\": [%g, %g],\n  \"cameraOffsets\": [0, 0],\n  \"singTime\": 8.0,\n  \"flipX\": false,\n  \"isPixel\": false,\n",
                sc, d->xp.pos[0], d->xp.pos[1]);
        fprintf(f, "  \"healthIcon\": { \"id\": \"%s\" },\n  \"startingAnimation\": \"%s\",\n  \"animations\": [\n", id, sa);
        for (int i = 0; i < n; i++)
            fprintf(f, "    { \"name\": \"%s\", \"prefix\": \"%s\", \"offsets\": [%g, %g], \"frameRate\": %d, \"looped\": %s }%s\n",
                    en[i], ep[i], st[i].off[0], st[i].off[1], st[i].fps, st[i].loop ? "true" : "false", i + 1 < n ? "," : "");
        fprintf(f, "  ]\n}\n");
    } else {    // Psych Engine character file
        fprintf(f, "{\n  \"animations\": [\n");
        for (int i = 0; i < n; i++)
            fprintf(f, "    { \"anim\": \"%s\", \"name\": \"%s\", \"fps\": %d, \"loop\": %s, \"indices\": [], \"offsets\": [%g, %g] }%s\n",
                    en[i], ep[i], st[i].fps, st[i].loop ? "true" : "false", st[i].off[0], st[i].off[1], i + 1 < n ? "," : "");
        fprintf(f, "  ],\n  \"image\": \"characters/%s\",\n  \"scale\": %g,\n  \"sing_duration\": 4,\n  \"healthicon\": \"%s\",\n", id, sc, id);
        fprintf(f, "  \"position\": [%g, %g],\n  \"camera_position\": [0, 0],\n  \"flip_x\": false,\n  \"no_antialiasing\": false,\n  \"healthbar_colors\": [161, 161, 161]\n}\n",
                d->xp.pos[0], d->xp.pos[1]);
    }
    fclose(f);
    snprintf(msg, cap, "%s \xE2\x80\x9C%s\xE2\x80\x9D: %d animations, %s", vs ? "V-Slice character" : "Psych character", J.name, n, m2);
}

static void finish(void) {
    char msg[300] = "Exported";
    if (J.kind == JOB_PNG) {
        if (J.failed) snprintf(msg, sizeof(msg), "Couldn't write %d of %d PNG frames", J.failed, J.total);
        else snprintf(msg, sizeof(msg), "Exported %d PNG frames", J.total);
    } else if (J.kind == JOB_CLIPS) {
        if (J.format == XF_SPARROW) {
            const char *base = J.path;
            for (const char *c = J.path; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
            char m2[200];
            if (write_atlas(J.path, base, m2, sizeof(m2))) snprintf(msg, sizeof(msg), "Sparrow sheet: %s", m2);
            else snprintf(msg, sizeof(msg), "%s", m2);
        } else write_character(msg, sizeof(msg));
    }
    for (ptrdiff_t i = 0; i < arrlen(J.shots); i++) free(J.shots[i].px);
    arrsetlen(J.shots, 0);
    target_free(&J.tgt); pool_free(&J.pool);
    J.active = false;
    if (J.kind != JOB_THUMB) app_toast("%s", msg);
}

void export_after_render(void) {
    if (!J.active) return;
    if (J.pending) { capture_frame(); J.pending = false; J.frame++; }
    if (J.frame > J.total) finish();
    else if (J.kind != JOB_THUMB) { app.toast_until = ui.time + 1; snprintf(app.toast, sizeof(app.toast), "Exporting frame %d of %d\xE2\x80\xA6", J.frame, J.total); }
}

bool export_busy(void) { return J.active; }
