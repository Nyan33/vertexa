#include "render/scene.h"
#include "brush/brush.h"
#include "geom/geom.h"

/* ── targets ── */

void target_make(Target *t, int w, int h, int samples) {
    target_free(t);
    t->w = w; t->h = h; t->samples = samples;
    if (w <= 0 || h <= 0) return;
    if (samples <= 1) {   // no anti-aliasing: draw straight into the texture, nothing to resolve
        t->res = sg_make_image(&(sg_image_desc){.usage.color_attachment = true, .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1});
        t->ds = sg_make_image(&(sg_image_desc){.usage.depth_stencil_attachment = true, .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL, .sample_count = 1});
        t->v_c = sg_make_view(&(sg_view_desc){.color_attachment.image = t->res});
        t->v_ds = sg_make_view(&(sg_view_desc){.depth_stencil_attachment.image = t->ds});
        t->v_tex = sg_make_view(&(sg_view_desc){.texture.image = t->res});
        return;
    }
    t->msaa = sg_make_image(&(sg_image_desc){.usage.color_attachment = true, .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = samples});
    t->res = sg_make_image(&(sg_image_desc){.usage.resolve_attachment = true, .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1});
    t->ds = sg_make_image(&(sg_image_desc){.usage.depth_stencil_attachment = true, .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL, .sample_count = samples});
    t->v_c = sg_make_view(&(sg_view_desc){.color_attachment.image = t->msaa});
    t->v_res = sg_make_view(&(sg_view_desc){.resolve_attachment.image = t->res});
    t->v_ds = sg_make_view(&(sg_view_desc){.depth_stencil_attachment.image = t->ds});
    t->v_tex = sg_make_view(&(sg_view_desc){.texture.image = t->res});
}
void target_free(Target *t) {
    sg_view *vs[] = {&t->v_c, &t->v_res, &t->v_ds, &t->v_tex};
    for (int i = 0; i < 4; i++) if (vs[i]->id) { sg_destroy_view(*vs[i]); vs[i]->id = 0; }
    sg_image *is[] = {&t->msaa, &t->res, &t->ds};
    for (int i = 0; i < 3; i++) if (is[i]->id) { sg_destroy_image(*is[i]); is[i]->id = 0; }
}

void pool_free(Pool *p) { for (int i = 0; i < GROUP_MAX; i++) target_free(&p->t[i]); target_free(&p->back); target_free(&p->tmp); }

/* ── frames: a sequence of passes ── */

static Seg *seg_new(Frame *F, int kind, int level, bool clear) {
    if (F->nsegs >= arrlen(F->segs)) arrput(F->segs, calloc(1, sizeof(Seg)));
    Seg *s = F->segs[F->nsegs++];
    s->kind = kind; s->level = level; s->clear = clear; s->resolve = false;
    dl_reset(&s->dl, 1, rect(0, 0, (float)F->w, (float)F->h));
    dl_set_tex(&s->dl, F->tex);
    if (kind == SEG_DRAW) F->last_at[level] = F->nsegs - 1;
    return s;
}
void frame_begin(Frame *F, Target *main, Pool *pool, u32 any_tex) {
    F->nsegs = 0; F->main = main; F->pool = pool; F->w = main->w; F->h = main->h; F->tex = any_tex; F->level = 0;
    seg_new(F, SEG_DRAW, 0, true);
}
DrawList *frame_dl(Frame *F) { return &F->segs[F->nsegs - 1]->dl; }

static void ensure(Target *t, int w, int h, int samples) {
    if (t->w != w || t->h != h || t->samples != samples || !t->v_tex.id) target_make(t, w, h, samples);
}
bool frame_push(Frame *F) {
    if (!F->pool || F->level >= GROUP_MAX || F->nsegs > 4000) return false;
    int L = F->level + 1;
    ensure(&F->pool->t[L - 1], F->w, F->h, F->main->samples);
    seg_new(F, SEG_DRAW, L, true);
    F->level = L;
    return true;
}
static u32 level_tex(Frame *F, int L) { return L ? F->pool->t[L - 1].v_tex.id : F->main->v_tex.id; }

void frame_pop(Frame *F, u8 blend, float opacity) {
    int L = F->level, P = L - 1;
    if (L <= 0) return;
    F->segs[F->last_at[L]]->resolve = true;
    static const u8 shader_mode[BLEND_COUNT] = {
        [BLEND_DARKEN] = CB_DARKEN, [BLEND_LIGHTEN] = CB_LIGHTEN, [BLEND_DIFFERENCE] = CB_DIFFERENCE,
        [BLEND_OVERLAY] = CB_OVERLAY, [BLEND_HARDLIGHT] = CB_HARDLIGHT, [BLEND_INVERT] = CB_INVERT,
    };
    u8 mode = blend < BLEND_COUNT ? shader_mode[blend] : 0;
    float w = (float)F->w, h = (float)F->h;
    if (mode) {   // the blend shader needs what is below: copy it first
        ensure(&F->pool->back, F->w, F->h, 1);
        F->segs[F->last_at[P]]->resolve = true;
        Seg *c = seg_new(F, SEG_COPY, P, true);
        dl_set_pipe(&c->dl, PIPE_COPY);
        dl_image(&c->dl, rect(0, 0, w, h), level_tex(F, P), 0xffffffff, 0, 0, 1, 1);
    }
    DrawList *dl = &seg_new(F, SEG_DRAW, P, false)->dl;
    F->level = P;
    u32 src = level_tex(F, L);
    float o = clampf(opacity, 0, 1);
    if (mode) {
        dl_set_pipe(dl, PIPE_COMPOSITE);
        dl_set_tex(dl, src); dl_set_tex2(dl, F->pool->back.v_tex.id);
        dl_quad_px(dl, 0, 0, w, h, 0, 0, 1, 1, 0xffffffff, mode, o);
        dl_set_tex2(dl, 0); dl_set_tex(dl, F->tex);
    } else {
        u8 pipes[2] = {PIPE_NORMAL, 0}; int np = 1;
        switch (blend) {
        case BLEND_MULTIPLY: pipes[0] = PIPE_MULTIPLY; pipes[1] = PIPE_MULTIPLY2; np = 2; break;
        case BLEND_SCREEN: pipes[0] = PIPE_SCREEN; break;
        case BLEND_ADD: pipes[0] = PIPE_ADD; break;
        case BLEND_SUBTRACT: pipes[0] = PIPE_SUBTRACT; break;
        case BLEND_ALPHA: pipes[0] = PIPE_ALPHA; break;
        case BLEND_ERASE: pipes[0] = PIPE_ERASE_OUT; break;
        default: break;
        }
        u8 a = (u8)roundf(o * 255);
        for (int i = 0; i < np; i++) {
            dl_set_pipe(dl, pipes[i]);
            dl_image(dl, rect(0, 0, w, h), src, rgba(a, a, a, a), 0, 0, 1, 1);
        }
    }
    dl_set_pipe(dl, PIPE_NORMAL);
}
/* ── filters ── */

// Flash blurs with a box run `quality` times; a Gaussian with the same spread looks the same
static float box_sigma(float w, int q) { return sqrtf(MAX(0.0f, q * (w * w - 1) / 12.0f)); }
static void fx_taps(float P[FX_P], float sigma) {
    float stp = MAX(1.0f, ceilf(3 * sigma / 48));
    P[1] = sigma; P[2] = stp; P[13] = ceilf(3 * sigma / stp);
}
static void fx_pass(Frame *F, int kind, int level, u32 t0, u32 t1, const float P[FX_P], Rect px) {
    Seg *s = seg_new(F, kind, level, kind != SEG_DRAW);
    dl_filter_quad(&s->dl, px, t0, t1 ? t1 : t0, P);
    dl_set_pipe(&s->dl, PIPE_NORMAL); dl_set_tex(&s->dl, F->tex); dl_set_tex2(&s->dl, 0);
}
// the colour matrix of Animate's Adjust Color (the ColorMatrix maths of fl.motion), rows r g b a + offsets
static void adjust_matrix(const float adj[4], float out[20]) {
    static const float DELTA[101] = {0, 0.01f, 0.02f, 0.04f, 0.05f, 0.06f, 0.07f, 0.08f, 0.1f, 0.11f, 0.12f, 0.14f, 0.15f, 0.16f, 0.17f, 0.18f, 0.20f, 0.21f, 0.22f, 0.24f,
        0.25f, 0.27f, 0.28f, 0.30f, 0.32f, 0.34f, 0.36f, 0.38f, 0.40f, 0.42f, 0.44f, 0.46f, 0.48f, 0.5f, 0.53f, 0.56f, 0.59f, 0.62f, 0.65f, 0.68f, 0.71f, 0.74f, 0.77f,
        0.80f, 0.83f, 0.86f, 0.89f, 0.92f, 0.95f, 0.98f, 1.0f, 1.06f, 1.12f, 1.18f, 1.24f, 1.30f, 1.36f, 1.42f, 1.48f, 1.54f, 1.60f, 1.66f, 1.72f, 1.78f, 1.84f, 1.90f,
        1.96f, 2.0f, 2.12f, 2.25f, 2.37f, 2.50f, 2.62f, 2.75f, 2.87f, 3.0f, 3.2f, 3.4f, 3.6f, 3.8f, 4.0f, 4.3f, 4.7f, 4.9f, 5.0f, 5.5f, 6.0f, 6.5f, 6.8f, 7.0f, 7.3f,
        7.5f, 7.8f, 8.0f, 8.4f, 8.7f, 9.0f, 9.4f, 9.6f, 9.8f, 10.0f};
    // 5x5 matrices, applied hue, contrast, brightness, saturation (as fl.motion.AdjustColor builds them)
    float M[25] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    #define MUL(A) do { float R[25]; for (int r = 0; r < 5; r++) for (int c = 0; c < 5; c++) { float v = 0; for (int k = 0; k < 5; k++) v += (A)[r * 5 + k] * M[k * 5 + c]; R[r * 5 + c] = v; } memcpy(M, R, sizeof(R)); } while (0)
    float h = clampf(adj[3], -180, 180) * VX_PI / 180, cv = cosf(h), sv = sinf(h), lr = 0.213f, lg = 0.715f, lb = 0.072f;
    float H[25] = {lr + cv * (1 - lr) + sv * (-lr), lg + cv * (-lg) + sv * (-lg), lb + cv * (-lb) + sv * (1 - lb), 0, 0,
                   lr + cv * (-lr) + sv * 0.143f, lg + cv * (1 - lg) + sv * 0.140f, lb + cv * (-lb) + sv * (-0.283f), 0, 0,
                   lr + cv * (-lr) + sv * (-(1 - lr)), lg + cv * (-lg) + sv * lg, lb + cv * (1 - lb) + sv * lb, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    MUL(H);
    float c = clampf(adj[1], -100, 100), x;
    if (c < 0) x = 127 + c / 100 * 127;
    else { int ci = (int)c; float fr = c - ci; x = (ci < 100 ? DELTA[ci] * (1 - fr) + DELTA[ci + 1] * fr : DELTA[100]) * 127 + 127; }
    float C[25] = {x / 127, 0, 0, 0, 0.5f * (127 - x), 0, x / 127, 0, 0, 0.5f * (127 - x), 0, 0, x / 127, 0, 0.5f * (127 - x), 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    MUL(C);
    float b = clampf(adj[0], -100, 100);
    float B[25] = {1, 0, 0, 0, b, 0, 1, 0, 0, b, 0, 0, 1, 0, b, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    MUL(B);
    float sa = clampf(adj[2], -100, 100), sx = 1 + (sa > 0 ? 3 * sa / 100 : sa / 100), r0 = 0.3086f, g0 = 0.6094f, b0 = 0.0820f;
    float S[25] = {r0 * (1 - sx) + sx, g0 * (1 - sx), b0 * (1 - sx), 0, 0, r0 * (1 - sx), g0 * (1 - sx) + sx, b0 * (1 - sx), 0, 0,
                   r0 * (1 - sx), g0 * (1 - sx), b0 * (1 - sx) + sx, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    MUL(S);
    #undef MUL
    for (int r = 0; r < 4; r++) { for (int k = 0; k < 4; k++) out[r * 4 + k] = M[r * 5 + k]; out[16 + r] = M[r * 5 + 4] / 255.0f; }
}

void frame_filter(Frame *F, const Filter *f, float scale, Rect px) {
    int L = F->level;
    if (L <= 0 || (f->flags & FXF_OFF)) return;
    Rect full = {0, 0, (float)F->w, (float)F->h};
    px = rect_isect(px, full);
    if (px.w <= 0 || px.h <= 0) return;
    ensure(&F->pool->back, F->w, F->h, 1);
    ensure(&F->pool->tmp, F->w, F->h, 1);
    F->segs[F->last_at[L]]->resolve = true;
    u32 src = level_tex(F, L), back = F->pool->back.v_tex.id, tmp = F->pool->tmp.v_tex.id;
    float P[FX_P] = {0};
    int q = CLAMP(f->quality, 1, 3);
    float sx = box_sigma(f->bx * scale, q), sy = box_sigma(f->by * scale, q);
    if (f->type == FX_BLUR) {
        P[0] = FXM_BLUR; P[4] = 1; fx_taps(P, sx);
        fx_pass(F, SEG_COPY, L, src, 0, P, px);
        P[4] = 0; P[5] = 1; fx_taps(P, sy);
        fx_pass(F, SEG_DRAW, L, back, 0, P, px);   // straight back into the group
    } else if (f->type == FX_GLOW || f->type == FX_SHADOW) {
        bool inner = f->flags & FXF_INNER;
        float a = f->angle * VX_PI / 180, d = f->type == FX_SHADOW ? f->dist * scale : 0;
        P[0] = inner ? FXM_ALPHA_INV : FXM_ALPHA; P[4] = 1; P[6] = cosf(a) * d; P[7] = sinf(a) * d; fx_taps(P, sx);
        fx_pass(F, SEG_COPY, L, src, 0, P, px);
        float ca = ((f->color >> 24) & 255) / 255.0f;
        P[0] = inner ? FXM_INNER : FXM_OUTER; P[3] = f->strength; P[4] = 0; P[5] = 1; P[6] = P[7] = 0;
        P[8] = (f->color & 255) / 255.0f * ca; P[9] = ((f->color >> 8) & 255) / 255.0f * ca; P[10] = ((f->color >> 16) & 255) / 255.0f * ca; P[11] = ca;
        P[12] = (float)((f->flags & FXF_KNOCKOUT ? 1 : 0) | (f->flags & FXF_HIDE ? 2 : 0));
        fx_taps(P, sy);
        fx_pass(F, SEG_TMP, L, back, src, P, px);
        float C[FX_P] = {FXM_COPY};
        fx_pass(F, SEG_DRAW, L, tmp, 0, C, px);
    } else if (f->type == FX_ADJUST) {
        P[0] = FXM_MATRIX;
        adjust_matrix(f->adj, P + 16);
        fx_pass(F, SEG_TMP, L, src, 0, P, px);
        float C[FX_P] = {FXM_COPY};
        fx_pass(F, SEG_DRAW, L, tmp, 0, C, px);
    }
}

/* ── gradient ramps: 256 x 1 textures, made per set of stops (and colour effect) ── */

static struct { u64 key; sg_image img; sg_view view; } *GRAMP;
void scene_frame_start(void) {   // a bounded cache: emptied between frames when it grows too big
    if (arrlen(GRAMP) < 512) return;
    for (ptrdiff_t i = 0; i < arrlen(GRAMP); i++) { sg_destroy_view(GRAMP[i].view); sg_destroy_image(GRAMP[i].img); }
    arrsetlen(GRAMP, 0);
}

int frame_lists(Frame *F, DrawList **out, int cap) {
    while (F->level > 0) frame_pop(F, BLEND_NORMAL, 1);   // a group left open by mistake
    if (F->nsegs) F->segs[F->last_at[0]]->resolve = true;
    int n = 0;
    for (int i = 0; i < F->nsegs && n < cap; i++) out[n++] = &F->segs[i]->dl;
    return n;
}
void frame_passes(Frame *F, PipeSet *ps, const float clear[4]) {
    static PipeSet single; static bool single_ok;   // the backdrop copy is one sample
    static const float none[4] = {0, 0, 0, 0};
    for (int i = 0; i < F->nsegs; i++) {
        Seg *s = F->segs[i];
        Target *T = s->kind == SEG_COPY ? &F->pool->back : s->kind == SEG_TMP ? &F->pool->tmp : s->level ? &F->pool->t[s->level - 1] : F->main;
        const float *cc = s->kind == SEG_DRAW && s->level == 0 ? clear : none;
        bool msaa = T->samples > 1;
        sg_begin_pass(&(sg_pass){
            .attachments = {.colors[0] = T->v_c, .resolves[0] = msaa && s->resolve ? T->v_res : (sg_view){0}, .depth_stencil = T->v_ds},
            .action = {
                .colors[0] = {.load_action = s->clear ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD, .store_action = SG_STOREACTION_STORE,
                              .clear_value = {cc[0], cc[1], cc[2], cc[3]}},
                .depth = {.load_action = SG_LOADACTION_DONTCARE, .store_action = SG_STOREACTION_DONTCARE},
                .stencil = {.load_action = SG_LOADACTION_CLEAR, .store_action = SG_STOREACTION_DONTCARE, .clear_value = 0},
            },
            .label = s->kind == SEG_COPY ? "vx-backdrop" : s->kind == SEG_TMP ? "vx-filter" : s->level ? "vx-group" : "vx-scene",
        });
        if (s->kind == SEG_COPY || s->kind == SEG_TMP) {
            if (!single_ok) { single = gfx_make_pipes(1, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL); single_ok = true; }
            gfx_draw(&s->dl, &single, F->w, F->h);
        } else gfx_draw(&s->dl, ps, F->w, F->h);
        sg_end_pass();
    }
}

/* ── helpers ── */

Layer *tl_layer(Timeline *t, int id) {
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) if (t->layers[i].id == id) return &t->layers[i];
    return NULL;
}
// hiding a mask turns the masking off; it doesn't hide the layers it masks
bool tl_hidden(Timeline *t, const Layer *l) {
    for (int guard = 0; l && guard < 64; guard++) {
        if (l->hidden) return true;
        Layer *p = l->parent ? tl_layer(t, l->parent) : NULL;
        if (p && p->type == LT_MASK) p = p->parent ? tl_layer(t, p->parent) : NULL;
        l = p;
    }
    return false;
}

int symbol_frame(Doc *d, const Item *inst, int off) {
    Symbol *s = doc_symbol(d, inst->sym);
    if (!s) return 1;
    int len = doc_symbol_len(d, inst->sym), first = CLAMP(inst->first, 1, len);
    if (s->type == SYM_MOVIECLIP || inst->loop == LOOP_SINGLE) return first;
    if (inst->loop == LOOP_ONCE) return MIN(first + off, len);
    return ((first - 1 + off) % len + len) % len + 1;
}

// the colour effect in force: instances' effects concatenated down the symbol tree (child first)
static float CXM[3] = {1, 1, 1}, CXA[3] = {0, 0, 0};
static bool CX_ON;

static void cx_of(const ColorFx *c, float mul[4], float add[4]) {
    for (int i = 0; i < 4; i++) { mul[i] = 1; add[i] = 0; }
    float t = clampf(c->amount, -1, 1);
    float rgb[3] = {(c->tint & 255) / 255.0f, ((c->tint >> 8) & 255) / 255.0f, ((c->tint >> 16) & 255) / 255.0f};
    if (c->kind == CX_BRIGHTNESS) for (int i = 0; i < 3; i++) { mul[i] = 1 - fabsf(t); add[i] = t > 0 ? t : 0; }
    else if (c->kind == CX_TINT) for (int i = 0; i < 3; i++) { mul[i] = 1 - clampf(t, 0, 1); add[i] = rgb[i] * clampf(t, 0, 1); }
    else if (c->kind == CX_ADVANCED) for (int i = 0; i < 4; i++) { mul[i] = c->mul[i]; add[i] = c->add[i]; }
}
static Color cx_apply(Color c) {
    if (!CX_ON) return c;
    u8 v[3];
    for (int i = 0; i < 3; i++) v[i] = (u8)roundf(clampf(((c >> (8 * i)) & 255) / 255.0f * CXM[i] + CXA[i], 0, 1) * 255);
    return rgba(v[0], v[1], v[2], (u8)(c >> 24));
}
static Color item_color(Color c, float alpha, Color tint) { return tint ? tint : col_alpha(cx_apply(c), alpha); }

/* ── bitmaps ── */

// the GPU texture of a bitmap, with mipmaps (box filtered; the pixels are premultiplied, so averaging is right)
static u32 bitmap_tex(Bitmap *b) {
    if (b->view) return b->view;
    sg_image_data data = {0};
    u8 *lv[16] = {b->px};
    int w = b->w, h = b->h, n = 1;
    data.mip_levels[0] = (sg_range){b->px, (size_t)w * h * 4};
    while ((w > 1 || h > 1) && n < 16) {
        int nw = MAX(1, w / 2), nh = MAX(1, h / 2);
        u8 *src = lv[n - 1], *m = malloc((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    int x0 = MIN(2 * x, w - 1), x1 = MIN(2 * x + 1, w - 1), y0 = MIN(2 * y, h - 1), y1 = MIN(2 * y + 1, h - 1);
                    int sum = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] + src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
                    m[((size_t)y * nw + x) * 4 + c] = (u8)((sum + 2) / 4);
                }
        lv[n] = m;
        data.mip_levels[n] = (sg_range){m, (size_t)nw * nh * 4};
        n++; w = nw; h = nh;
    }
    sg_image img = sg_make_image(&(sg_image_desc){.width = b->w, .height = b->h, .num_mipmaps = n, .pixel_format = SG_PIXELFORMAT_RGBA8, .data = data, .label = "vx-bitmap"});
    for (int i = 1; i < n; i++) free(lv[i]);
    b->img = img.id;
    b->view = sg_make_view(&(sg_view_desc){.texture.image = img}).id;
    return b->view;
}

u32 bitmap_texture(int id) { Bitmap *b = bitmap_get(id); return b && b->px ? bitmap_tex(b) : 0; }

static void bitmap_quad(DrawList *dl, Bitmap *b, Mat m, Color c, u8 pipe, const float *fxp) {
    V2 p[4] = {mat_ap(m, v2(0, 0)), mat_ap(m, v2((float)b->w, 0)), mat_ap(m, v2((float)b->w, (float)b->h)), mat_ap(m, v2(0, (float)b->h))};
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) { x0 = MIN(x0, p[i].x); y0 = MIN(y0, p[i].y); x1 = MAX(x1, p[i].x); y1 = MAX(y1, p[i].y); }
    Rect clip = dl_clip(dl);
    if (x1 < clip.x || y1 < clip.y || x0 > clip.x + clip.w || y0 > clip.y + clip.h) return;
    u32 old = dl->tex;
    dl_set_tex(dl, bitmap_tex(b));
    dl_set_sampler(dl, b->smooth ? SMP_MIP : SMP_NEAREST);
    dl_set_pipe(dl, pipe);
    static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    u32 v0 = 0;
    for (int i = 0; i < 4; i++) { u32 v = fxp ? dl_vtx_uvp(dl, p[i].x, p[i].y, uv[i][0], uv[i][1], c, K_IMAGE_FX, fxp) : dl_vtx_uv(dl, p[i].x, p[i].y, uv[i][0], uv[i][1], c, K_IMAGE); if (!i) v0 = v; }
    dl_idx(dl, v0, v0 + 1, v0 + 2); dl_idx(dl, v0, v0 + 2, v0 + 3);
    dl_set_pipe(dl, PIPE_NORMAL);
    dl_set_sampler(dl, SMP_LINEAR);
    dl_set_tex(dl, old);
}

// a placed bitmap; blend modes the pipelines can do on their own need no group
static void render_bitmap(Frame *F, const Item *it, Mat m, float alpha, Color tint) {
    Bitmap *b = bitmap_get(it->sym);
    if (!b || !b->px) return;
    Mat mm = mat_mul(m, it->m);
    if (tint) {   // onion skin, mask: the tint, premultiplied, through the bitmap's alpha
        float ta = ((tint >> 24) & 255) / 255.0f;
        Color c = rgba((u8)((tint & 255) * ta), (u8)(((tint >> 8) & 255) * ta), (u8)(((tint >> 16) & 255) * ta), (u8)(ta * 255));
        bitmap_quad(frame_dl(F), b, mm, c, PIPE_NORMAL, NULL);
        return;
    }
    float al = clampf(alpha * it->alpha, 0, 1);
    u8 a = (u8)roundf(al * 255);
    Color c = rgba(a, a, a, a);
    float fxp[4] = {0}, *fx = NULL;
    if (CX_ON) {   // a colour effect from above: multipliers in the colour, offsets beside
        c = rgba((u8)roundf(clampf(CXM[0], 0, 1) * al * 255), (u8)roundf(clampf(CXM[1], 0, 1) * al * 255), (u8)roundf(clampf(CXM[2], 0, 1) * al * 255), a);
        for (int i = 0; i < 3; i++) fxp[i] = CXA[i] * al;
        fx = fxp;
    }
    switch (it->blend) {
    case BLEND_NORMAL: case BLEND_LAYER: bitmap_quad(frame_dl(F), b, mm, c, PIPE_NORMAL, fx); return;
    case BLEND_MULTIPLY: bitmap_quad(frame_dl(F), b, mm, c, PIPE_MULTIPLY, fx); bitmap_quad(frame_dl(F), b, mm, c, PIPE_MULTIPLY2, fx); return;
    case BLEND_SCREEN: bitmap_quad(frame_dl(F), b, mm, c, PIPE_SCREEN, fx); return;
    case BLEND_ADD: bitmap_quad(frame_dl(F), b, mm, c, PIPE_ADD, fx); return;
    case BLEND_SUBTRACT: bitmap_quad(frame_dl(F), b, mm, c, PIPE_SUBTRACT, fx); return;
    case BLEND_ALPHA: if (F->level) bitmap_quad(frame_dl(F), b, mm, c, PIPE_ALPHA, fx); return;
    case BLEND_ERASE: if (F->level) bitmap_quad(frame_dl(F), b, mm, c, PIPE_ERASE_OUT, fx); return;
    default:
        if (frame_push(F)) { bitmap_quad(frame_dl(F), b, mm, c, PIPE_NORMAL, fx); frame_pop(F, it->blend, 1); }
        else bitmap_quad(frame_dl(F), b, mm, c, PIPE_NORMAL, fx);
    }
}

/* ── fills: solid, bitmap, gradient ── */

static Color col_lerp(Color a, Color b, float t);
static u32 ramp_tex(const GStop *st, int n) {
    u8 px[256 * 4];
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        Color c = st[0].c;
        if (t >= st[n - 1].pos) c = st[n - 1].c;
        else for (int k = 0; k + 1 < n; k++) if (t >= st[k].pos && t <= st[k + 1].pos) {
            float u = st[k + 1].pos > st[k].pos ? (t - st[k].pos) / (st[k + 1].pos - st[k].pos) : 0;
            c = col_lerp(st[k].c, st[k + 1].c, u);
            break;
        }
        c = cx_apply(c);
        float a = ((c >> 24) & 255) / 255.0f;
        px[i * 4 + 0] = (u8)roundf((c & 255) * a); px[i * 4 + 1] = (u8)roundf(((c >> 8) & 255) * a);
        px[i * 4 + 2] = (u8)roundf(((c >> 16) & 255) * a); px[i * 4 + 3] = (u8)(c >> 24);
    }
    u64 key = 1469598103934665603ull;
    for (int i = 0; i < 1024; i++) { key ^= px[i]; key *= 1099511628211ull; }
    for (ptrdiff_t i = 0; i < arrlen(GRAMP); i++) if (GRAMP[i].key == key) return GRAMP[i].view.id;
    sg_image img = sg_make_image(&(sg_image_desc){.width = 256, .height = 1, .pixel_format = SG_PIXELFORMAT_RGBA8, .data.mip_levels[0] = {px, sizeof(px)}, .label = "vx-ramp"});
    sg_view view = sg_make_view(&(sg_view_desc){.texture.image = img});
    arrput(GRAMP, ((typeof(*GRAMP)){key, img, view}));
    return view.id;
}

static void draw_fill(DrawList *dl, const Item *it, Mat m, float alpha, Color tint) {
    if (!tint && !it->erase) {
        u8 a = (u8)roundf(clampf(alpha, 0, 1) * 255);
        Bitmap *b = it->sym ? bitmap_get(it->sym) : NULL;
        if (it->grad && arrlen(it->stops)) {   // gradient space is 1638.4 px across
            float p[4] = {it->grad == GR_RADIAL ? 1.0f : 0.0f, (float)it->spread, 0, 0};
            Mat uvm = mat_mul(mat_scale(1 / 819.2f, 1 / 819.2f), mat_inv(it->m));
            fill_draw_tex(dl, it, m, ramp_tex(it->stops, (int)arrlen(it->stops)), uvm, rgba(a, a, a, a), SMP_LINEAR, K_GRADIENT, p);
            return;
        }
        if (b && b->px) {   // a bitmap fill: the bitmap, tiled, inside the fill
            Mat uvm = mat_mul(mat_scale(1.0f / b->w, 1.0f / b->h), mat_inv(it->m));
            fill_draw_tex(dl, it, m, bitmap_tex(b), uvm, rgba(a, a, a, a), SMP_REPEAT, K_IMAGE, NULL);
            return;
        }
    }
    fill_draw(dl, it, m, item_color(it->color, alpha, tint), it->erase);
}

/* ── tweens ── */

typedef struct { float tx, ty, rot, sx, sy, k; } Decomp;
static Decomp decompose(Mat m) {
    Decomp o;
    o.tx = m.tx; o.ty = m.ty;
    o.sx = hypotf(m.a, m.b);
    o.rot = atan2f(m.b, m.a);
    float c = cosf(o.rot), s = sinf(o.rot);
    // R(-rot) * [a c; b d] = [sx k; 0 sy]
    o.k = c * m.c + s * m.d;
    o.sy = -s * m.c + c * m.d;
    return o;
}
static Mat compose(Decomp o) {
    float c = cosf(o.rot), s = sinf(o.rot);
    return (Mat){c * o.sx, s * o.sx, c * o.k - s * o.sy, s * o.k + c * o.sy, o.tx, o.ty};
}
static Mat mat_lerp(Mat a, Mat b, float t) {
    Decomp A = decompose(a), B = decompose(b), o;
    float dr = B.rot - A.rot;
    while (dr > VX_PI) dr -= 2 * VX_PI;
    while (dr < -VX_PI) dr += 2 * VX_PI;
    o.tx = lerpf(A.tx, B.tx, t); o.ty = lerpf(A.ty, B.ty, t); o.rot = A.rot + dr * t;
    o.sx = lerpf(A.sx, B.sx, t); o.sy = lerpf(A.sy, B.sy, t); o.k = lerpf(A.k, B.k, t);
    return compose(o);
}
static Color col_lerp(Color a, Color b, float t) {
    u8 c[4];
    for (int i = 0; i < 4; i++) c[i] = (u8)roundf(lerpf((float)((a >> (8 * i)) & 255), (float)((b >> (8 * i)) & 255), t));
    return rgba(c[0], c[1], c[2], c[3]);
}

static const Item *single_instance(Drawing *dr) {
    if (!dr || arrlen(dr->items) != 1 || !item_placed(&dr->items[0])) return NULL;
    return &dr->items[0];
}

static void render_symbol(Frame *F, Doc *d, const Item *inst, int off, Mat m, float alpha, Color tint, int depth);

// Shape tween, the Flash way: any two drawings morph. Strokes pair with strokes and fills with fills in
// drawing order; whatever has no partner shrinks away (or grows in) at its own centre. A stroke may run
// either way and a contour may start anywhere, so each pair is aligned first (direction, start point)
// to make the shortest morph. Symbol instances cross-fade.
#define TW_N 96
static void stroke_samples(const Item *it, SPt *out) {   // closed outlines: around the loop, end not repeated
    Path p = path_of(it, 1.5f);
    float div = (it->flags & ITF_CLOSED) ? TW_N : TW_N - 1;
    for (int k = 0; k < TW_N; k++) out[k] = path_at(&p, p.len * k / div);
    path_free(&p);
}
// a closed outline may start anywhere and run either way: rotate / flip b to follow a
static void align_loop(const SPt *a, SPt *b) {
    static SPt tmp[TW_N];
    float best = 1e30f; int bo = 0; bool brev = false;
    for (int rev = 0; rev < 2; rev++)
        for (int o = 0; o < TW_N; o += 2) {
            float e = 0;
            for (int k = 0; k < TW_N; k += 2) { SPt q = b[rev ? (o - k + TW_N * 2) % TW_N : (o + k) % TW_N]; e += (q.x - a[k].x) * (q.x - a[k].x) + (q.y - a[k].y) * (q.y - a[k].y); }
            if (e < best) { best = e; bo = o; brev = rev; }
        }
    for (int k = 0; k < TW_N; k++) tmp[k] = b[brev ? (bo - k + TW_N * 2) % TW_N : (bo + k) % TW_N];
    memcpy(b, tmp, sizeof(tmp));
}
static void contour_samples(const V2 *p, int n, V2 *out) {   // closed contour, even spacing along its length
    float L = 0;
    for (int i = 0; i < n; i++) L += hypotf(p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].y - p[i].y);
    int i = 0; float acc = 0;
    for (int k = 0; k < TW_N; k++) {
        float s = L * k / TW_N;
        for (;;) {
            float d = hypotf(p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].y - p[i].y);
            if (acc + d >= s || i >= n - 1) { float t = d > 1e-6f ? clampf((s - acc) / d, 0, 1) : 0; out[k] = v2(lerpf(p[i].x, p[(i + 1) % n].x, t), lerpf(p[i].y, p[(i + 1) % n].y, t)); break; }
            acc += d; i++;
        }
    }
}
static float contour_area(const V2 *p, int n) { float a = 0; for (int i = 0; i < n; i++) a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y; return fabsf(a) / 2; }
static V2 centre_of(const V2 *p, int n) { V2 c = {0, 0}; for (int i = 0; i < n; i++) { c.x += p[i].x; c.y += p[i].y; } return v2(c.x / MAX(1, n), c.y / MAX(1, n)); }

// b re-ordered (start offset, direction) to follow a as closely as possible
static void align_contour(const V2 *a, V2 *b) {
    V2 tmp[TW_N];
    float best = 1e30f; int bo = 0; bool brev = false;
    for (int rev = 0; rev < 2; rev++)
        for (int o = 0; o < TW_N; o += 3) {
            float e = 0;
            for (int k = 0; k < TW_N; k += 2) { V2 q = b[rev ? (o - k + TW_N * 2) % TW_N : (o + k) % TW_N]; e += (q.x - a[k].x) * (q.x - a[k].x) + (q.y - a[k].y) * (q.y - a[k].y); }
            if (e < best) { best = e; bo = o; brev = rev; }
        }
    for (int k = 0; k < TW_N; k++) tmp[k] = b[brev ? (bo - k + TW_N * 2) % TW_N : (bo + k) % TW_N];
    memcpy(b, tmp, sizeof(tmp));
}

typedef struct { const Item *it; int c; float area; } Contour;   // a contour of a fill (c = its index)

static void draw_morph_stroke(DrawList *dl, const Item *style, const SPt *a, const SPt *b, float t, Mat m, Color col, bool erase) {
    Item o = *style;
    o.pts = NULL; o.poly = NULL; o.starts = NULL;
    o.flags = style->flags | ITF_LINEAR;   // closed outlines stay closed
    for (int k = 0; k < TW_N; k++) arrput(o.pts, ((SPt){lerpf(a[k].x, b[k].x, t), lerpf(a[k].y, b[k].y, t), lerpf(a[k].r, b[k].r, t)}));
    item_bounds(&o);
    brush_draw(dl, &o, m, col, erase);
    arrfree(o.pts);
}

static bool shape_tween(Frame *F, Doc *d, Drawing *a, Drawing *b, float t, Mat m, float alpha, Color tint) {
    if (!a || !b) return false;
    DrawList *dl = frame_dl(F);
    // strokes, in drawing order
    const Item *sa[256], *sb[256]; int nsa = 0, nsb = 0;
    for (ptrdiff_t i = 0; i < arrlen(a->items) && nsa < 256; i++) if (a->items[i].kind == IT_STROKE) sa[nsa++] = &a->items[i];
    for (ptrdiff_t i = 0; i < arrlen(b->items) && nsb < 256; i++) if (b->items[i].kind == IT_STROKE) sb[nsb++] = &b->items[i];
    const Item *fa[256], *fb[256]; int nfa = 0, nfb = 0;
    for (ptrdiff_t i = 0; i < arrlen(a->items) && nfa < 256; i++) if (a->items[i].kind == IT_FILL) fa[nfa++] = &a->items[i];
    for (ptrdiff_t i = 0; i < arrlen(b->items) && nfb < 256; i++) if (b->items[i].kind == IT_FILL) fb[nfb++] = &b->items[i];
    if (!nsa && !nsb && !nfa && !nfb) return false;   // only symbols: nothing to morph

    static SPt A[TW_N], B[TW_N], R[TW_N];
    for (int i = 0; i < MAX(nsa, nsb); i++) {
        const Item *ia = i < nsa ? sa[i] : NULL, *ib = i < nsb ? sb[i] : NULL;
        const Item *style = ia ? ia : ib;
        if (ia) stroke_samples(ia, A);
        if (ib) stroke_samples(ib, B);
        bool loop = ia && ib && (ia->flags & ITF_CLOSED) && (ib->flags & ITF_CLOSED);
        if (loop) align_loop(A, B);
        else if (ia && ib) {   // the other way round may be the shorter morph
            float e0 = 0, e1 = 0;
            for (int k = 0; k < TW_N; k++) { e0 += hypotf(A[k].x - B[k].x, A[k].y - B[k].y); e1 += hypotf(A[k].x - B[TW_N - 1 - k].x, A[k].y - B[TW_N - 1 - k].y); }
            if (e1 < e0) { for (int k = 0; k < TW_N; k++) R[k] = B[TW_N - 1 - k]; memcpy(B, R, sizeof(B)); }
        } else {          // no partner: shrinks to (or grows from) its own middle
            SPt *src = ia ? A : B, *dst = ia ? B : A, mid = src[TW_N / 2];
            for (int k = 0; k < TW_N; k++) dst[k] = (SPt){mid.x, mid.y, 0};
        }
        Item st = *style;
        st.flags = loop ? (st.flags | ITF_CLOSED) : (st.flags & ~ITF_CLOSED);
        if (ia && ib) { st.angle = lerpf(ia->angle, ib->angle, t); st.aspect = lerpf(ia->aspect, ib->aspect, t); }
        Color col = ia && ib ? col_lerp(ia->color, ib->color, t) : style->color;
        if (!(style->erase && tint)) draw_morph_stroke(dl, &st, A, B, t, m, item_color(col, alpha, tint), style->erase && !tint);
    }

    // fills pair with fills; inside a pair, contours (outlines and holes) pair biggest with biggest,
    // so a fill keeps its holes while it morphs
    static V2 PA[TW_N], PB[TW_N];
    for (int i = 0; i < MAX(nfa, nfb); i++) {
        const Item *ia = i < nfa ? fa[i] : NULL, *ib = i < nfb ? fb[i] : NULL;
        Contour *ca = NULL, *cb = NULL;
        for (int side = 0; side < 2; side++) {
            const Item *it = side ? ib : ia;
            if (!it) continue;
            int nc = (int)arrlen(it->starts);
            for (int c = 0; c < nc; c++) {
                int c0 = it->starts[c], c1 = c + 1 < nc ? it->starts[c + 1] : (int)arrlen(it->poly);
                if (c1 - c0 < 3) continue;
                Contour k = {it, c, contour_area(it->poly + c0, c1 - c0)};
                if (side) arrput(cb, k); else arrput(ca, k);
            }
            Contour *cs = side ? cb : ca;
            for (ptrdiff_t x = 1; x < arrlen(cs); x++) for (ptrdiff_t y = x; y > 0 && cs[y - 1].area < cs[y].area; y--) { Contour q = cs[y]; cs[y] = cs[y - 1]; cs[y - 1] = q; }
        }
        V2 whole = {0, 0};   // a fill without a partner shrinks into its middle
        if (!ia || !ib) { const Item *it = ia ? ia : ib; whole = v2((it->bx0 + it->bx1) / 2, (it->by0 + it->by1) / 2); }
        Item o = *(ia ? ia : ib);
        o.pts = NULL; o.poly = NULL; o.starts = NULL;
        for (ptrdiff_t j = 0; j < MAX(arrlen(ca), arrlen(cb)); j++) {
            Contour *ka = j < arrlen(ca) ? &ca[j] : NULL, *kb = j < arrlen(cb) ? &cb[j] : NULL;
            for (int side = 0; side < 2; side++) {
                Contour *k = side ? kb : ka;
                if (!k) continue;
                int nc = (int)arrlen(k->it->starts), c0 = k->it->starts[k->c], c1 = k->c + 1 < nc ? k->it->starts[k->c + 1] : (int)arrlen(k->it->poly);
                contour_samples(k->it->poly + c0, c1 - c0, side ? PB : PA);
            }
            if (ka && kb) align_contour(PA, PB);
            else {
                V2 *src = ka ? PA : PB, *dst = ka ? PB : PA, c = (ia && ib) ? centre_of(src, TW_N) : whole;
                for (int k = 0; k < TW_N; k++) dst[k] = c;
            }
            arrput(o.starts, (int)arrlen(o.poly));
            for (int k = 0; k < TW_N; k++) arrput(o.poly, v2(lerpf(PA[k].x, PB[k].x, t), lerpf(PA[k].y, PB[k].y, t)));
        }
        Color col = ia && ib ? col_lerp(ia->color, ib->color, t) : o.color;
        item_bounds(&o);
        o.color = col;
        draw_fill(dl, &o, m, alpha, tint);
        arrfree(o.poly); arrfree(o.starts);
        arrfree(ca); arrfree(cb);
    }

    // symbols don't morph: they cross-fade
    for (int side = 0; side < 2; side++) {
        Drawing *dr = side ? b : a;
        for (ptrdiff_t i = 0; i < arrlen(dr->items); i++) {
            const Item *it = &dr->items[i];
            if (!item_placed(it)) continue;
            Item tmp = *it;
            tmp.alpha = it->alpha * (side ? t : 1 - t);
            if (it->kind == IT_BITMAP) render_bitmap(F, &tmp, m, alpha, tint);
            else render_symbol(F, d, &tmp, 0, m, alpha, tint, 0);
            dl = frame_dl(F);
        }
    }
    return true;
}

/* ── rendering ── */

static bool R_export;   // rendering an export: guides and editing-only looks (see-through layers) are left out

// A symbol with a blend mode is drawn on its own surface, then blended in as a whole. Alpha and Erase
// only mean something inside a group (Layer blending), as in Animate; on their own they draw nothing.
static void render_symbol(Frame *F, Doc *d, const Item *inst, int off, Mat m, float alpha, Color tint, int depth) {
    if (depth > 12) return;
    Timeline *t = doc_symbol_tl(d, inst->sym);
    if (!t) return;
    u8 bl = tint ? BLEND_NORMAL : inst->blend;
    if ((bl == BLEND_ALPHA || bl == BLEND_ERASE) && !F->level) return;
    int nfx = 0;
    for (ptrdiff_t i = 0; !tint && i < arrlen(inst->filters); i++) nfx += !(inst->filters[i].flags & FXF_OFF);
    bool iso = (bl != BLEND_NORMAL || nfx) && frame_push(F);
    float om[3], oa[3]; bool oon = CX_ON;
    memcpy(om, CXM, sizeof(om)); memcpy(oa, CXA, sizeof(oa));
    if (!tint && inst->cx.kind != CX_NONE) {
        float mm[4], aa[4];
        cx_of(&inst->cx, mm, aa);
        for (int i = 0; i < 3; i++) { CXM[i] = mm[i] * om[i]; CXA[i] = aa[i] * om[i] + oa[i]; }
        CX_ON = true;
    }
    render_timeline(F, d, t, symbol_frame(d, inst, off), mat_mul(m, inst->m), alpha * inst->alpha, tint, depth + 1, R_export);
    memcpy(CXM, om, sizeof(om)); memcpy(CXA, oa, sizeof(oa)); CX_ON = oon;
    if (!iso) return;
    if (nfx) {   // filters work on the group, over the instance's bounds grown by how far each one reaches
        float scale = mat_scalef(m), b[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        Rect px = {0, 0, (float)F->w, (float)F->h};
        if (item_bounds_m(d, inst, off, m, b, depth)) {
            float g = 2;
            for (ptrdiff_t i = 0; i < arrlen(inst->filters); i++) {
                const Filter *f = &inst->filters[i];
                if (f->flags & FXF_OFF) continue;
                float sg = box_sigma(MAX(f->bx, f->by) * scale, CLAMP(f->quality, 1, 3));
                g += 3 * sg + (f->type == FX_SHADOW ? fabsf(f->dist) * scale : 0);
            }
            px = rect(floorf(b[0] - g), floorf(b[1] - g), ceilf(b[2] - b[0] + 2 * g) + 1, ceilf(b[3] - b[1] + 2 * g) + 1);
        }
        for (ptrdiff_t i = 0; i < arrlen(inst->filters); i++) frame_filter(F, &inst->filters[i], scale, px);
    }
    frame_pop(F, bl, 1);
}

void render_items(Frame *F, Doc *d, Drawing *dr, int off, Mat m, float alpha, Color tint, int depth) {
    if (!dr) return;
    for (ptrdiff_t i = 0; i < arrlen(dr->items); i++) {
        Item *it = &dr->items[i];
        DrawList *dl = frame_dl(F);
        if (it->kind == IT_STROKE) {
            if (it->erase && tint) continue;
            brush_draw(dl, it, m, it->erase ? 0xffffffffu : item_color(it->color, alpha, tint), it->erase);
        } else if (it->kind == IT_FILL) {
            if (it->erase && tint) continue;
            draw_fill(dl, it, m, alpha, tint);
        } else if (it->kind == IT_BITMAP) render_bitmap(F, it, m, alpha, tint);
        else render_symbol(F, d, it, off, m, alpha, tint, depth);
    }
}

static bool has_erase(Drawing *dr) {
    for (ptrdiff_t i = 0; dr && i < arrlen(dr->items); i++) if (dr->items[i].erase) return true;
    return false;
}

static void render_layer(Frame *F, Doc *d, Layer *l, int frame, Mat m, float alpha, Color tint, int depth) {
    int ki = doc_key_index_for(l, frame);
    if (ki < 0 || frame > l->len) return;
    Key *k = &l->keys[ki];
    Drawing *dr = k->d >= 0 ? &d->drawings[k->d] : NULL;
    Key *next = ki + 1 < arrlen(l->keys) ? &l->keys[ki + 1] : NULL;
    Drawing *ndr = next && next->d >= 0 ? &d->drawings[next->d] : NULL;
    float t01 = next ? (float)(frame - k->f) / (float)(next->f - k->f) : 0;
    int off = frame - k->f;
    // a see-through layer shows at its percentage (Animate's alphaPercent, 50 by default) while editing;
    // as in Animate it is an editing aid, exports draw the layer fully
    float op = l->opacity * (l->transparent && !R_export ? (l->see_pct ? l->see_pct : 50) / 100.0f : 1);
    bool iso = !tint && (op < 0.999f || l->blend != BLEND_NORMAL || has_erase(dr)) && frame_push(F);
    float a = iso ? 1 : alpha * op;
    bool done = false;
    if (k->tw == TW_CLASSIC && next && frame > k->f) {
        const Item *ia = single_instance(dr), *ib = single_instance(ndr);
        if (ia && ib && ia->kind == ib->kind && ia->sym == ib->sym) {
            Item tmp = *ia;
            tmp.m = mat_lerp(ia->m, ib->m, t01);
            tmp.alpha = lerpf(ia->alpha, ib->alpha, t01);
            if (ia->cx.kind == ib->cx.kind && ia->cx.kind != CX_NONE) {
                tmp.cx.amount = lerpf(ia->cx.amount, ib->cx.amount, t01);
                tmp.cx.tint = col_lerp(ia->cx.tint, ib->cx.tint, t01);
                for (int q = 0; q < 4; q++) { tmp.cx.mul[q] = lerpf(ia->cx.mul[q], ib->cx.mul[q], t01); tmp.cx.add[q] = lerpf(ia->cx.add[q], ib->cx.add[q], t01); }
            }
            Filter *twf = NULL;   // filters in between, when both ends have the same ones
            if (arrlen(ia->filters) && arrlen(ia->filters) == arrlen(ib->filters)) {
                for (ptrdiff_t q = 0; q < arrlen(ia->filters); q++) {
                    Filter fa = ia->filters[q], fb = ib->filters[q];
                    if (fa.type == fb.type) {
                        fa.bx = lerpf(fa.bx, fb.bx, t01); fa.by = lerpf(fa.by, fb.by, t01); fa.strength = lerpf(fa.strength, fb.strength, t01);
                        fa.angle = lerpf(fa.angle, fb.angle, t01); fa.dist = lerpf(fa.dist, fb.dist, t01); fa.color = col_lerp(fa.color, fb.color, t01);
                        for (int k = 0; k < 4; k++) fa.adj[k] = lerpf(fa.adj[k], fb.adj[k], t01);
                    }
                    arrput(twf, fa);
                }
                tmp.filters = twf;
            }
            if (tmp.kind == IT_BITMAP) render_bitmap(F, &tmp, m, a, tint);
            else render_symbol(F, d, &tmp, off, m, a, tint, depth);
            arrfree(twf);
            done = true;
        }
    } else if (k->tw == TW_SHAPE && next && frame > k->f) done = shape_tween(F, d, dr, ndr, t01, m, a, tint);
    if (!done) render_items(F, d, dr, off, m, a, tint, depth);
    if (iso) frame_pop(F, l->blend, alpha * op);
}

static bool layer_skipped(Timeline *t, Layer *l) {
    if (l->type == LT_FOLDER || l->type == LT_AUDIO || l->type == LT_CAMERA || tl_hidden(t, l)) return true;
    return R_export && l->type == LT_GUIDE;
}

// a mask: the layers under it drawn on their own surface, then kept only where the mask's shapes are
static void render_masked(Frame *F, Doc *d, Timeline *t, Layer *mask, int frame, Mat m, float alpha, Color tint, int depth) {
    bool group = !tint && frame_push(F);
    for (ptrdiff_t li = arrlen(t->layers) - 1; li >= 0; li--) {
        Layer *l = &t->layers[li];
        if (l->parent == mask->id && l->type != LT_MASK && !layer_skipped(t, l)) render_layer(F, d, l, frame, m, alpha, tint, depth);
    }
    if (!group) return;
    if (frame_push(F)) {
        render_layer(F, d, mask, frame, m, 1, hex(0xffffff), depth);   // coverage only: every shape opaque white
        frame_pop(F, BLEND_ALPHA, 1);
    }
    frame_pop(F, BLEND_NORMAL, 1);
}

void render_timeline(Frame *F, Doc *d, Timeline *t, int frame, Mat m, float alpha, Color tint, int depth, bool export_mode) {
    if (depth == 0) R_export = export_mode;
    for (ptrdiff_t li = arrlen(t->layers) - 1; li >= 0; li--) {
        Layer *l = &t->layers[li];
        Layer *p = l->parent ? tl_layer(t, l->parent) : NULL;
        if (p && p->type == LT_MASK && !p->hidden && !layer_skipped(t, p)) continue;   // drawn with its mask
        if (l->type == LT_MASK && !l->hidden) { if (!layer_skipped(t, l)) render_masked(F, d, t, l, frame, m, alpha, tint, depth); continue; }
        if (layer_skipped(t, l) || l->type == LT_MASK) continue;
        render_layer(F, d, l, frame, m, alpha, tint, depth);
    }
}

/* ── bounds ── */

static void grow(float b[4], V2 p) { b[0] = MIN(b[0], p.x); b[1] = MIN(b[1], p.y); b[2] = MAX(b[2], p.x); b[3] = MAX(b[3], p.y); }

bool item_bounds_m(Doc *d, const Item *it, int off, Mat m, float b[4], int depth) {
    if (it->kind == IT_BITMAP) {
        Bitmap *bm = bitmap_get(it->sym);
        if (!bm) return false;
        Mat mm = mat_mul(m, it->m);
        grow(b, mat_ap(mm, v2(0, 0))); grow(b, mat_ap(mm, v2((float)bm->w, 0)));
        grow(b, mat_ap(mm, v2((float)bm->w, (float)bm->h))); grow(b, mat_ap(mm, v2(0, (float)bm->h)));
        return true;
    }
    if (it->kind == IT_INSTANCE) {
        if (depth > 12) return false;
        Timeline *t = doc_symbol_tl(d, it->sym);
        if (!t) return false;
        int f = symbol_frame(d, it, off);
        Mat mm = mat_mul(m, it->m);
        bool any = false;
        for (ptrdiff_t li = 0; li < arrlen(t->layers); li++) {
            Layer *l = &t->layers[li];
            if (l->type == LT_FOLDER || tl_hidden(t, l)) continue;
            Key *k = doc_key_for(l, f);
            if (k && k->d >= 0) any |= drawing_bounds(d, &d->drawings[k->d], f - k->f, mm, b, depth + 1);
        }
        return any;
    }
    if (it->bx1 < it->bx0) return false;
    grow(b, mat_ap(m, v2(it->bx0, it->by0))); grow(b, mat_ap(m, v2(it->bx1, it->by0)));
    grow(b, mat_ap(m, v2(it->bx1, it->by1))); grow(b, mat_ap(m, v2(it->bx0, it->by1)));
    return true;
}

bool drawing_bounds(Doc *d, Drawing *dr, int off, Mat m, float b[4], int depth) {
    bool any = false;
    for (ptrdiff_t i = 0; dr && i < arrlen(dr->items); i++) if (!dr->items[i].erase) any |= item_bounds_m(d, &dr->items[i], off, m, b, depth);
    return any;
}
