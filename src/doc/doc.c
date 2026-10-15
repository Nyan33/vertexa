#include "doc/doc.h"

const char *LAYER_TYPE_NAMES[LT_COUNT] = {"Vector", "Raster", "Camera", "Audio", "Folder", "Mask", "Guide", "Reference"};
const char *BLEND_NAMES[BLEND_COUNT] = {"Normal", "Layer", "Darken", "Multiply", "Lighten", "Screen", "Overlay", "Hard Light",
                                        "Add", "Subtract", "Difference", "Invert", "Alpha", "Erase"};

static const u32 LABEL_RGB[] = {0xff5d73, 0xffb347, 0x46c2ff, 0x8b7bff, 0x3ddc97, 0xff8a65, 0x9aa0a6, 0x5ec2b7};

/* ── items ── */

void item_free(Item *it) { arrfree(it->pts); arrfree(it->poly); arrfree(it->starts); arrfree(it->filters); arrfree(it->stops); }

const char *FILTER_NAMES[FX_COUNT] = {"Blur", "Glow", "Drop Shadow", "Adjust Color"};

/* ── eases ── */

const char *EASE_NAMES[EASE_COUNT] = {"None", "Classic", "Quad", "Cubic", "Quart", "Quint", "Sine", "Back", "Circ",
                                      "Bounce", "Elastic", "Custom"};
static float ease_in(int e, float t) {   // the "in" form of a family
    switch (e) {
    case EASE_QUAD: return t * t;
    case EASE_CUBIC: return t * t * t;
    case EASE_QUART: return t * t * t * t;
    case EASE_QUINT: return t * t * t * t * t;
    case EASE_SINE: return 1 - cosf(t * VX_PI / 2);
    case EASE_BACK: { const float c1 = 1.70158f, c3 = c1 + 1; return c3 * t * t * t - c1 * t * t; }
    case EASE_CIRC: return 1 - sqrtf(MAX(0, 1 - t * t));
    case EASE_BOUNCE: {
        float u = 1 - t, b;   // the out bounce, mirrored
        const float n1 = 7.5625f, d1 = 2.75f;
        if (u < 1 / d1) b = n1 * u * u;
        else if (u < 2 / d1) { u -= 1.5f / d1; b = n1 * u * u + 0.75f; }
        else if (u < 2.5f / d1) { u -= 2.25f / d1; b = n1 * u * u + 0.9375f; }
        else { u -= 2.625f / d1; b = n1 * u * u + 0.984375f; }
        return 1 - b;
    }
    case EASE_ELASTIC:
        if (t <= 0 || t >= 1) return t;
        return -powf(2, 10 * t - 10) * sinf((t * 10 - 10.75f) * (2 * VX_PI / 3));
    default: return t;
    }
}
static float bez1(float a, float b, float c, float d, float u) { float v = 1 - u; return v * v * v * a + 3 * v * v * u * b + 3 * v * u * u * c + u * u * u * d; }
static float ease_custom(const Key *k, float t) {
    int segs = (k->nez - 1) / 3;
    for (int s = 0; s < segs; s++) {
        const V2 *p = &k->ez[s * 3];
        if (t > p[3].x && s + 1 < segs) continue;
        float lo = 0, hi = 1;   // the curve's x grows with u: find the u where x = t
        for (int i = 0; i < 30; i++) { float mid = (lo + hi) / 2; if (bez1(p[0].x, p[1].x, p[2].x, p[3].x, mid) < t) lo = mid; else hi = mid; }
        return bez1(p[0].y, p[1].y, p[2].y, p[3].y, (lo + hi) / 2);
    }
    return t;
}
float key_ease(const Key *k, float t) {
    t = clampf(t, 0, 1);
    switch (k->ease) {
    case EASE_NONE: return t;
    case EASE_CLASSIC: { float e = clampf(k->ease_amt / 100, -1, 1); return t + e * t * (1 - t); }   // Flash: out > 0, in < 0
    case EASE_CUSTOM: return k->nez >= 4 ? ease_custom(k, t) : t;
    default:
        if (k->ease_dir == EASE_IN) return ease_in(k->ease, t);
        if (k->ease_dir == EASE_OUT) return 1 - ease_in(k->ease, 1 - t);
        return t < 0.5f ? ease_in(k->ease, 2 * t) / 2 : 1 - ease_in(k->ease, 2 - 2 * t) / 2;
    }
}
void key_ease_custom_default(Key *k) {
    k->nez = 4;
    k->ez[0] = v2(0, 0); k->ez[1] = v2(0.4f, 0); k->ez[2] = v2(0.6f, 1); k->ez[3] = v2(1, 1);
}
bool ease_from_name(const char *m, u8 *ease, u8 *dir) {
    static const char *fam[] = {"quad", "cubic", "quart", "quint", "sine", "back", "circ", "bounce", "elastic"};
    if (!m) return false;
    for (int i = 0; i < COUNT(fam); i++) {
        size_t n = strlen(fam[i]);
        if (strncmp(m, fam[i], n)) continue;
        const char *r = m + n;
        *ease = (u8)(EASE_QUAD + i);
        *dir = !strcmp(r, "In") ? EASE_IN : !strcmp(r, "Out") ? EASE_OUT : EASE_INOUT;
        return true;
    }
    return false;
}

void colorfx_update(ColorFx *c) {
    float t = clampf(c->amount, -1, 1);
    float rgb[3] = {(c->tint & 255) / 255.0f, ((c->tint >> 8) & 255) / 255.0f, ((c->tint >> 16) & 255) / 255.0f};
    if (c->kind == CX_BRIGHTNESS)   // towards black or white
        for (int i = 0; i < 3; i++) { c->mul[i] = 1 - fabsf(t); c->add[i] = t > 0 ? t : 0; }
    else if (c->kind == CX_TINT)
        for (int i = 0; i < 3; i++) { c->mul[i] = 1 - clampf(t, 0, 1); c->add[i] = rgb[i] * clampf(t, 0, 1); }
    else if (c->kind == CX_NONE)
        for (int i = 0; i < 3; i++) { c->mul[i] = 1; c->add[i] = 0; }
    if (c->kind != CX_ADVANCED) { c->mul[3] = 1; c->add[3] = 0; }
}
void stroke_free(Stroke *s) { item_free(s); }

Item item_copy(const Item *it) {
    Item o = *it;
    o.pts = NULL; o.poly = NULL; o.starts = NULL;   // (filters and stops below)
    if (arrlen(it->pts)) memcpy(arraddnptr(o.pts, arrlen(it->pts)), it->pts, sizeof(SPt) * arrlen(it->pts));
    if (arrlen(it->poly)) memcpy(arraddnptr(o.poly, arrlen(it->poly)), it->poly, sizeof(V2) * arrlen(it->poly));
    if (arrlen(it->starts)) memcpy(arraddnptr(o.starts, arrlen(it->starts)), it->starts, sizeof(int) * arrlen(it->starts));
    o.filters = NULL; o.stops = NULL;
    if (arrlen(it->filters)) memcpy(arraddnptr(o.filters, arrlen(it->filters)), it->filters, sizeof(Filter) * arrlen(it->filters));
    if (arrlen(it->stops)) memcpy(arraddnptr(o.stops, arrlen(it->stops)), it->stops, sizeof(GStop) * arrlen(it->stops));
    return o;
}

void item_bounds(Item *it) {
    it->bx0 = it->by0 = 1e30f; it->bx1 = it->by1 = -1e30f;
    if (it->kind == IT_STROKE) {
        float k = it->tip == TIP_ROUND ? 1.0f : 1.42f;
        for (ptrdiff_t i = 0; i < arrlen(it->pts); i++) {
            SPt p = it->pts[i];
            it->bx0 = MIN(it->bx0, p.x - p.r * k); it->by0 = MIN(it->by0, p.y - p.r * k);
            it->bx1 = MAX(it->bx1, p.x + p.r * k); it->by1 = MAX(it->by1, p.y + p.r * k);
        }
    } else if (it->kind == IT_FILL) {
        for (ptrdiff_t i = 0; i < arrlen(it->poly); i++) {
            V2 p = it->poly[i];
            it->bx0 = MIN(it->bx0, p.x); it->by0 = MIN(it->by0, p.y);
            it->bx1 = MAX(it->bx1, p.x); it->by1 = MAX(it->by1, p.y);
        }
    }
}
void stroke_bounds(Stroke *s) { item_bounds(s); }

/* ── timelines ── */

static void timeline_free(Timeline *t) {
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) arrfree(t->layers[i].keys);
    arrfree(t->layers);
    t->layers = NULL;
}
static Timeline timeline_copy(const Timeline *s) {
    Timeline t = *s;
    t.layers = NULL;
    for (ptrdiff_t i = 0; i < arrlen(s->layers); i++) {
        Layer l = s->layers[i];
        l.keys = NULL;
        if (arrlen(s->layers[i].keys)) memcpy(arraddnptr(l.keys, arrlen(s->layers[i].keys)), s->layers[i].keys, sizeof(Key) * arrlen(s->layers[i].keys));
        arrput(t.layers, l);
    }
    return t;
}

Timeline *doc_tl(Doc *d) {
    if (d->nedit) { Symbol *s = doc_symbol(d, d->edit[d->nedit - 1]); if (s) return &s->tl; }
    return &d->scene;
}
Symbol *doc_symbol(Doc *d, int id) {
    for (ptrdiff_t i = 0; i < arrlen(d->symbols); i++) if (d->symbols[i].id == id) return &d->symbols[i];
    return NULL;
}
Timeline *doc_symbol_tl(Doc *d, int id) { Symbol *s = doc_symbol(d, id); return s ? &s->tl : NULL; }
int doc_editing(Doc *d) { return d->nedit ? d->edit[d->nedit - 1] : 0; }
void doc_enter_symbol(Doc *d, int id, Mat m) {
    if (d->nedit >= COUNT(d->edit) || !doc_symbol(d, id)) return;
    d->edit_m[d->nedit] = m;
    d->edit[d->nedit++] = id;
    d->sel.on = false;
    doc_touch(d);
}
Mat doc_edit_matrix(Doc *d) {
    Mat m = mat_id();
    for (int i = 0; i < d->nedit; i++) m = mat_mul(m, d->edit_m[i]);
    return m;
}
void doc_exit_symbol(Doc *d) { if (d->nedit) { d->nedit--; d->sel.on = false; doc_touch(d); } }
int doc_symbol_len(Doc *d, int id) {
    Timeline *t = doc_symbol_tl(d, id);
    if (!t) return 1;
    int m = 1;
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) m = MAX(m, t->layers[i].len);
    return m;
}

/* ── document ── */

static Layer make_layer(Doc *d, u8 type, const char *name);

Doc *doc_new(void) {
    Doc *d = calloc(1, sizeof(Doc));
    snprintf(d->name, sizeof(d->name), "Untitled");
    d->w = 1280; d->h = 720; d->fps = 24;
    d->loop_on = false; d->loop_a = 1; d->loop_b = 24;
    d->onion_on = false; d->onion_prev = 2; d->onion_next = 1;
    d->next_id = 1;
    d->xp.format = XF_VSLICE; d->xp.scale = 1;
    d->bg = hex(0xffffff);
    d->scene.len = 24; d->scene.frame = 1;
    Layer l = make_layer(d, LT_VECTOR, "Layer 1");
    arrput(d->scene.layers, l);
    d->scene.active = l.id;
    return d;
}

void doc_free(Doc *d) {
    if (!d) return;
    timeline_free(&d->scene);
    for (ptrdiff_t i = 0; i < arrlen(d->symbols); i++) timeline_free(&d->symbols[i].tl);
    arrfree(d->symbols);
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++) {
        for (ptrdiff_t s = 0; s < arrlen(d->drawings[i].items); s++) item_free(&d->drawings[i].items[s]);
        arrfree(d->drawings[i].items);
    }
    arrfree(d->drawings);
    arrfree(d->bitmaps);
    arrfree(d->xp.clips);
    free(d);
}

Doc *doc_clone(const Doc *src) {
    Doc *d = malloc(sizeof(Doc));
    *d = *src;
    d->scene = timeline_copy(&src->scene);
    d->symbols = NULL;
    for (ptrdiff_t i = 0; i < arrlen(src->symbols); i++) {
        Symbol s = src->symbols[i];
        s.tl = timeline_copy(&src->symbols[i].tl);
        arrput(d->symbols, s);
    }
    d->xp.clips = NULL;
    for (ptrdiff_t i = 0; i < arrlen(src->xp.clips); i++) arrput(d->xp.clips, src->xp.clips[i]);
    d->bitmaps = NULL;
    for (ptrdiff_t i = 0; i < arrlen(src->bitmaps); i++) arrput(d->bitmaps, src->bitmaps[i]);
    d->drawings = NULL;
    for (ptrdiff_t i = 0; i < arrlen(src->drawings); i++) {
        Drawing dr = {NULL, src->drawings[i].used};
        for (ptrdiff_t s = 0; s < arrlen(src->drawings[i].items); s++) arrput(dr.items, item_copy(&src->drawings[i].items[s]));
        arrput(d->drawings, dr);
    }
    return d;
}

void doc_touch(Doc *d) { d->version++; }

Layer *doc_layer(Doc *d, int id) {
    Timeline *t = doc_tl(d);
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) if (t->layers[i].id == id) return &t->layers[i];
    return NULL;
}
int doc_layer_index(Doc *d, int id) {
    Timeline *t = doc_tl(d);
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) if (t->layers[i].id == id) return (int)i;
    return -1;
}
Layer *doc_active(Doc *d) { return doc_layer(d, doc_tl(d)->active); }
Layer *doc_parent(Doc *d, const Layer *l) { return l->parent ? doc_layer(d, l->parent) : NULL; }
int doc_depth(Doc *d, const Layer *l) { int n = 0; for (Layer *p = doc_parent(d, l); p; p = doc_parent(d, p)) n++; return n; }
// hiding a mask turns the masking off; it doesn't hide the layers it masks
bool doc_hidden(Doc *d, const Layer *l) {
    for (int guard = 0; l && guard < 64; guard++) {
        if (l->hidden) return true;
        Layer *p = doc_parent(d, l);
        if (p && p->type == LT_MASK) p = doc_parent(d, p);
        l = p;
    }
    return false;
}
bool doc_locked(Doc *d, const Layer *l) { for (; l; l = doc_parent(d, l)) if (l->locked) return true; return false; }
bool doc_drawable(const Layer *l) { return l->type == LT_VECTOR || l->type == LT_RASTER || l->type == LT_MASK || l->type == LT_GUIDE; }

static bool collapsed_away(Doc *d, const Layer *l) {
    for (Layer *p = doc_parent(d, l); p; p = doc_parent(d, p)) if (p->collapsed) return true;
    return false;
}
int doc_rows(Doc *d, int *out, int cap) {
    Timeline *t = doc_tl(d);
    int n = 0;
    for (ptrdiff_t i = 0; i < arrlen(t->layers) && n < cap; i++) if (!collapsed_away(d, &t->layers[i])) out[n++] = (int)i;
    return n;
}

static void expand_into(Doc *d, Layer *l, Layer **out, int *n, int cap) {
    if (*n >= cap) return;
    Timeline *t = doc_tl(d);
    if (l->type == LT_FOLDER) {
        for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) if (t->layers[i].parent == l->id) expand_into(d, &t->layers[i], out, n, cap);
        return;
    }
    for (int i = 0; i < *n; i++) if (out[i] == l) return;
    if (!doc_locked(d, l)) out[(*n)++] = l;
}
int doc_targets(Doc *d, Layer **out, int cap) {
    int n = 0;
    Timeline *t = doc_tl(d);
    if (d->sel.on) {
        int rows[512]; int nr = doc_rows(d, rows, 512);
        for (int r = d->sel.r0; r <= d->sel.r1 && r < nr; r++) expand_into(d, &t->layers[rows[r]], out, &n, cap);
    } else if (doc_active(d)) expand_into(d, doc_active(d), out, &n, cap);
    return n;
}

int doc_key_index_for(Layer *l, int f) {
    int k = -1;
    for (ptrdiff_t i = 0; i < arrlen(l->keys); i++) { if (l->keys[i].f <= f) k = (int)i; else break; }
    return k;
}
Key *doc_key_for(Layer *l, int f) {
    if (f < 1 || f > l->len) return NULL;
    int k = doc_key_index_for(l, f);
    return k >= 0 ? &l->keys[k] : NULL;
}
Key *doc_key_at(Layer *l, int f) {
    for (ptrdiff_t i = 0; i < arrlen(l->keys); i++) if (l->keys[i].f == f) return &l->keys[i];
    return NULL;
}
int doc_span_end(Layer *l, int ki) { return ki + 1 < arrlen(l->keys) ? l->keys[ki + 1].f - 1 : l->len; }
int doc_content_end(Doc *d) {
    Timeline *t = doc_tl(d);
    int m = 1;
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) m = MAX(m, t->layers[i].len);
    return m;
}

int doc_new_drawing(Doc *d) {
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++) if (!d->drawings[i].used) {
        Drawing *dr = &d->drawings[i];
        for (ptrdiff_t s = 0; s < arrlen(dr->items); s++) item_free(&dr->items[s]);
        arrsetlen(dr->items, 0);   // a recycled slot must start empty
        dr->used = true;
        return (int)i;
    }
    Drawing dr = {NULL, true};
    arrput(d->drawings, dr);
    return (int)arrlen(d->drawings) - 1;
}
int doc_clone_drawing(Doc *d, int src) {
    int id = doc_new_drawing(d);
    for (ptrdiff_t s = 0; s < arrlen(d->drawings[src].items); s++) arrput(d->drawings[id].items, item_copy(&d->drawings[src].items[s]));
    return id;
}

static void sort_keys(Layer *l) {
    for (ptrdiff_t i = 1; i < arrlen(l->keys); i++) {
        Key k = l->keys[i]; ptrdiff_t j = i - 1;
        while (j >= 0 && l->keys[j].f > k.f) { l->keys[j + 1] = l->keys[j]; j--; }
        l->keys[j + 1] = k;
    }
}

static Layer make_layer(Doc *d, u8 type, const char *name) {
    Layer l = {0};
    l.id = d->next_id++;
    l.type = type;
    l.opacity = 1;
    l.color = hex(LABEL_RGB[(l.id - 1) % COUNT(LABEL_RGB)]);
    if (name) snprintf(l.name, sizeof(l.name), "%s", name);
    else if (type == LT_VECTOR) snprintf(l.name, sizeof(l.name), "Layer %d", l.id);
    else snprintf(l.name, sizeof(l.name), "%s %d", LAYER_TYPE_NAMES[type], l.id);
    if (type != LT_FOLDER) {
        Key k = {.f = 1, .d = doc_drawable(&l) ? doc_new_drawing(d) : -1};
        arrput(l.keys, k);
        l.len = 1;
    }
    return l;
}

Layer *doc_add_layer(Doc *d, u8 type, const char *name) {
    Layer l = make_layer(d, type, name);
    Timeline *t = doc_tl(d);
    int at = 0;
    Layer *act = doc_active(d);
    // above the active layer, in its folder; a folder that is active gets it as its first layer
    if (act) { at = doc_layer_index(d, act->id) + (act->type == LT_FOLDER); l.parent = act->type == LT_FOLDER ? act->id : act->parent; }
    arrins(t->layers, at, l);
    doc_touch(d);
    return &t->layers[at];
}

int doc_add_symbol(Doc *d, const char *name, u8 type) {
    Symbol s = {0};
    s.id = d->next_id++;
    s.type = type;
    snprintf(s.name, sizeof(s.name), "%s", name);
    Layer l = make_layer(d, LT_VECTOR, "Layer 1");
    arrput(s.tl.layers, l);
    s.tl.active = l.id; s.tl.frame = 1; s.tl.len = 1;
    arrput(d->symbols, s);
    doc_touch(d);
    return s.id;
}

void doc_delete_layer(Doc *d, int id) {
    Timeline *t = doc_tl(d);
    int i = doc_layer_index(d, id);
    if (i < 0 || arrlen(t->layers) <= 1) return;
    Layer *l = &t->layers[i];
    int parent = l->parent;
    for (ptrdiff_t c = 0; c < arrlen(t->layers); c++) if (t->layers[c].parent == id) t->layers[c].parent = parent;
    for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) if (l->keys[k].d >= 0) d->drawings[l->keys[k].d].used = false;
    arrfree(l->keys);
    arrdel(t->layers, i);
    t->active = t->layers[MIN(i, (int)arrlen(t->layers) - 1)].id;
    doc_touch(d);
}

void doc_insert_frames(Doc *d, Layer **ls, int n, int f, int count) {
    for (int i = 0; i < n; i++) {
        Layer *l = ls[i];
        if (!arrlen(l->keys)) { Key k = {.f = 1, .d = doc_drawable(l) ? doc_new_drawing(d) : -1}; arrput(l->keys, k); l->len = MAX(1, f); continue; }
        if (f <= l->len) { for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) if (l->keys[k].f > f) l->keys[k].f += count; l->len += count; }
        else l->len = f;
    }
    doc_touch(d);
}

void doc_remove_frames(Doc *d, Layer **ls, int n, int f0, int f1) {
    for (int i = 0; i < n; i++) {
        Layer *l = ls[i];
        for (int g = MIN(f1, l->len); g >= f0; g--) {
            int ki = -1;
            for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) if (l->keys[k].f == g) ki = (int)k;
            if (ki >= 0 && doc_span_end(l, ki) == g && !(ki == 0 && arrlen(l->keys) == 1 && l->len > 1)) {
                if (l->keys[ki].d >= 0) d->drawings[l->keys[ki].d].used = false;
                arrdel(l->keys, ki);
            }
            for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) if (l->keys[k].f > g) l->keys[k].f--;
            l->len--;
        }
        if (l->len <= 0 || !arrlen(l->keys)) { l->len = 0; arrsetlen(l->keys, 0); }
        else l->keys[0].f = 1;
    }
    doc_touch(d);
}

void doc_insert_key(Doc *d, Layer **ls, int n, int f, bool blank) {
    for (int i = 0; i < n; i++) {
        Layer *l = ls[i];
        if (l->type == LT_FOLDER) continue;
        Key *prev = doc_key_for(l, MIN(f, l->len));
        int prev_d = prev ? prev->d : -1; u8 prev_tw = prev ? prev->tw : TW_NONE;
        if (f > l->len) l->len = f;
        Key *at = doc_key_at(l, f);
        if (at) { if (blank && at->d >= 0) { d->drawings[at->d].used = false; at->d = -1; } continue; }
        Key k = {.f = f, .d = -1, .tw = prev_tw};
        if (doc_drawable(l)) k.d = blank ? -1 : prev_d >= 0 ? doc_clone_drawing(d, prev_d) : doc_new_drawing(d);
        Key *pk = doc_key_for(l, MIN(f, MAX(1, l->len)));
        if (pk && l->type == LT_CAMERA) memcpy(k.cam, pk->cam, sizeof(k.cam));
        arrput(l->keys, k);
        sort_keys(l);
    }
    doc_touch(d);
}

void doc_clear_key(Doc *d, Layer **ls, int n, int f) {
    for (int i = 0; i < n; i++) {
        Layer *l = ls[i];
        for (ptrdiff_t k = 1; k < arrlen(l->keys); k++) if (l->keys[k].f == f) {
            if (l->keys[k].d >= 0) d->drawings[l->keys[k].d].used = false;
            arrdel(l->keys, k); break;
        }
    }
    doc_touch(d);
}

void doc_set_tween(Doc *d, Layer **ls, int n, int f0, int f1, u8 kind) {
    for (int i = 0; i < n; i++) {
        Layer *l = ls[i];
        for (ptrdiff_t k = 0; k + 1 < arrlen(l->keys); k++) {
            int e = doc_span_end(l, (int)k);
            if (l->keys[k].f <= f1 && e >= f0) l->keys[k].tw = kind;
        }
    }
    doc_touch(d);
}

Drawing *doc_drawing_for_edit(Doc *d, Layer *l, int f) {
    if (f > l->len || !arrlen(l->keys)) { Layer *one[1] = {l}; doc_insert_key(d, one, 1, f, true); }
    Key *k = doc_key_for(l, f);
    if (k->d < 0) k->d = doc_new_drawing(d);
    doc_touch(d);
    return &d->drawings[k->d];
}

Drawing *doc_drawing_at(Doc *d, Layer *l, int f) {
    Key *k = doc_key_for(l, f);
    return k && k->d >= 0 ? &d->drawings[k->d] : NULL;
}

/* ── history ── */

static History H0, *H = &H0;   // the steps of the document tab being edited
static int H_limit = 100;
void hist_bind(History *h) { H = h ? h : &H0; }
void hist_set_limit(int n) {
    H_limit = CLAMP(n, 10, 2000);
    while (arrlen(H->undo) > H_limit) { doc_free(H->undo[0]); arrdel(H->undo, 0); }
}

void hist_commit(Doc *d) {
    arrput(H->undo, doc_clone(d));
    if (arrlen(H->undo) > H_limit) { doc_free(H->undo[0]); arrdel(H->undo, 0); }
    for (ptrdiff_t i = 0; i < arrlen(H->redo); i++) doc_free(H->redo[i]);
    arrsetlen(H->redo, 0);
}
void hist_free(History *h) {
    for (ptrdiff_t i = 0; i < arrlen(h->undo); i++) doc_free(h->undo[i]);
    for (ptrdiff_t i = 0; i < arrlen(h->redo); i++) doc_free(h->redo[i]);
    arrfree(h->undo); arrfree(h->redo);
}
void hist_clear(void) { hist_free(H); }
static Doc *swap_to(Doc *cur, Doc ***from, Doc ***to) {
    if (!arrlen(*from)) return NULL;
    Doc *prev = arrpop(*from);
    arrput(*to, cur);
    prev->version = cur->version + 1;
    // keep where the user is looking
    memcpy(prev->path, cur->path, sizeof(prev->path));
    if (prev->nedit == cur->nedit) doc_tl(prev)->frame = doc_tl(cur)->frame;
    Timeline *t = doc_tl(prev);
    if (!doc_layer(prev, t->active) && arrlen(t->layers)) t->active = t->layers[0].id;
    prev->sel.on = false;
    return prev;
}
Doc *hist_undo(Doc *d) { return swap_to(d, &H->undo, &H->redo); }
Doc *hist_redo(Doc *d) { return swap_to(d, &H->redo, &H->undo); }

/* ── moving frames and layers (timeline drag and drop) ── */

typedef struct { int d; u8 tw; bool start, frame; char label[32]; } Cell;   // one frame of a layer

// the frames f0..f1 of each layer move by df, as in Flash: keys in the block go along (with their tweens
// and labels), keys they land on are replaced, and the frames they leave continue the key before them
void doc_move_frames(Doc *d, Layer **ls, int n, int f0, int f1, int df) {
    if (!df || f1 < f0 || f0 + df < 1) return;
    for (int i = 0; i < n; i++) {
        Layer *l = ls[i];
        int L = l->len;
        if (f0 > L || !arrlen(l->keys)) continue;
        int b1 = MIN(f1, L), bn = b1 - f0 + 1, NL = MAX(L, b1 + df);
        Cell *c = calloc((size_t)NL + 2, sizeof(Cell));
        for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) {
            Key *key = &l->keys[k];
            int e = doc_span_end(l, (int)k);
            for (int f = key->f; f <= e && f <= NL; f++) {
                c[f] = (Cell){key->d, key->tw, f == key->f, true, {0}};
                if (f == key->f) memcpy(c[f].label, key->label, sizeof(c[f].label));
            }
        }
        Cell *blk = malloc(sizeof(Cell) * (size_t)bn);
        memcpy(blk, c + f0, sizeof(Cell) * (size_t)bn);
        blk[0].start = true;
        // leave the block: those frames continue the key before it
        for (int f = f0; f <= b1; f++) c[f] = (Cell){f == 1 ? -1 : 0, 0, f == 1, true, {0}};
        for (int j = 0; j < bn; j++) c[f0 + df + j] = blk[j];
        free(blk);
        // no holes: frames before the block that didn't exist continue the key before them
        int last = 0;
        for (int f = 1; f <= NL; f++) if (c[f].frame) last = f;
        for (int f = 1; f <= last; f++) if (!c[f].frame) c[f] = (Cell){f == 1 ? -1 : 0, 0, f == 1, true, {0}};
        // drawings no key shows any more are freed
        int *before = NULL;
        for (ptrdiff_t k = 0; k < arrlen(l->keys); k++) if (l->keys[k].d >= 0) arrput(before, l->keys[k].d);
        arrsetlen(l->keys, 0);
        for (int f = 1; f <= last; f++) {
            if (!c[f].start) continue;
            Key k = {.f = f, .d = c[f].d, .tw = c[f].tw};
            memcpy(k.label, c[f].label, sizeof(k.label));
            arrput(l->keys, k);
        }
        for (ptrdiff_t b = 0; b < arrlen(before); b++) {
            bool kept = false;
            for (ptrdiff_t k = 0; k < arrlen(l->keys) && !kept; k++) kept = l->keys[k].d == before[b];
            if (!kept) d->drawings[before[b]].used = false;
        }
        arrfree(before);
        l->len = MAX(1, last);
        free(c);
    }
    doc_touch(d);
}

static bool under(Doc *d, const Layer *l, int anc) {   // l is inside folder anc (at any depth)
    for (const Layer *p = l; p && p->parent; p = doc_parent(d, p)) if (p->parent == anc) return true;
    return false;
}

// moves layers (folders with everything in them) above / into / below the reference layer
// (ref 0: to the bottom of the top level); a folder never goes into itself
bool doc_move_layers(Doc *d, const int *ids, int n, int ref, int where) {
    Timeline *t = doc_tl(d);
    int cnt = (int)arrlen(t->layers);
    bool *take = calloc((size_t)cnt + 1, 1);
    int taken = 0;
    for (int i = 0; i < cnt; i++) {
        Layer *l = &t->layers[i];
        for (int k = 0; k < n && !take[i]; k++) take[i] = l->id == ids[k] || under(d, l, ids[k]);
        taken += take[i];
    }
    Layer *rl = ref ? doc_layer(d, ref) : NULL;
    if (!taken || (ref && !rl) || (rl && take[doc_layer_index(d, ref)]) || (where == 1 && rl && rl->type != LT_FOLDER)) { free(take); return false; }
    int parent = where == 1 ? ref : rl ? rl->parent : 0;
    Layer *moved = NULL, *rest = NULL;
    for (int i = 0; i < cnt; i++) {
        Layer l = t->layers[i];
        if (!take[i]) { arrput(rest, l); continue; }
        bool top = true;   // its folder isn't moving along: it gets the new place's folder
        for (int k = 0; k < cnt; k++) if (take[k] && t->layers[k].id == l.parent) top = false;
        if (top) l.parent = parent;
        arrput(moved, l);
    }
    free(take);
    int at = (int)arrlen(rest);
    if (rl) {
        int ri = 0;
        while (ri < arrlen(rest) && rest[ri].id != ref) ri++;
        at = ri;
        if (where == 1) at = ri + 1;
        else if (where == 2) {
            at = ri + 1;
            while (at < arrlen(rest) && under(d, &rest[at], ref)) at++;   // after everything inside it
        }
    }
    Layer *out = NULL;
    for (int i = 0; i < at; i++) arrput(out, rest[i]);
    for (ptrdiff_t i = 0; i < arrlen(moved); i++) arrput(out, moved[i]);
    for (ptrdiff_t i = at; i < arrlen(rest); i++) arrput(out, rest[i]);
    arrfree(t->layers);
    t->layers = out;
    arrfree(moved); arrfree(rest);
    doc_touch(d);
    return true;
}

/* ── camera ── */

void doc_camera_default(const Doc *d, float cam[4]) { cam[0] = d->w / 2.0f; cam[1] = d->h / 2.0f; cam[2] = 1; cam[3] = 0; }

Layer *doc_camera_layer(Doc *d) {
    Timeline *t = &d->scene;
    for (ptrdiff_t i = 0; i < arrlen(t->layers); i++) {
        Layer *l = &t->layers[i];
        bool hidden = l->hidden;
        for (int pid = l->parent, guard = 0; pid && !hidden && guard < 64; guard++) {   // folders it sits in
            const Layer *p = NULL;
            for (ptrdiff_t k = 0; k < arrlen(t->layers); k++) if (t->layers[k].id == pid) p = &t->layers[k];
            if (!p) break;
            hidden = p->hidden; pid = p->parent;
        }
        if (l->type == LT_CAMERA && !hidden) return l;
    }
    return NULL;
}

static void key_cam(const Doc *d, const Key *k, float cam[4]) {
    if (k->cam[2] > 0) memcpy(cam, k->cam, sizeof(float) * 4);
    else doc_camera_default(d, cam);
}

bool doc_camera_at(Doc *d, int frame, float cam[4]) {
    Layer *l = doc_camera_layer(d);
    if (!l) return false;
    int ki = doc_key_index_for(l, MIN(frame, MAX(1, l->len)));
    if (ki < 0) { doc_camera_default(d, cam); return true; }
    Key *k = &l->keys[ki];
    key_cam(d, k, cam);
    if (k->tw == TW_CLASSIC && ki + 1 < arrlen(l->keys) && frame > k->f) {
        Key *n = &l->keys[ki + 1];
        float b[4], t = key_ease(k, (float)(frame - k->f) / (float)(n->f - k->f));
        key_cam(d, n, b);
        float dr = b[3] - cam[3];
        while (dr > VX_PI) dr -= 2 * VX_PI;
        while (dr < -VX_PI) dr += 2 * VX_PI;
        cam[0] = lerpf(cam[0], b[0], t); cam[1] = lerpf(cam[1], b[1], t);
        cam[2] = expf(lerpf(logf(cam[2]), logf(b[2]), t));   // zoom feels even when it changes in ratios
        cam[3] += dr * t;
    }
    return true;
}

Mat doc_camera_matrix(const Doc *d, const float cam[4]) {
    return mat_mul(mat_mul(mat_tr(d->w / 2.0f, d->h / 2.0f), mat_mul(mat_scale(cam[2], cam[2]), mat_rot(-cam[3]))), mat_tr(-cam[0], -cam[1]));
}

int doc_add_bitmap(Doc *d, int id) {
    for (ptrdiff_t i = 0; i < arrlen(d->bitmaps); i++) if (d->bitmaps[i] == id) return id;
    if (id) arrput(d->bitmaps, id);
    return id;
}
bool doc_delete_bitmap(Doc *d, int id) {   // only one nothing places
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++) {
        if (!d->drawings[i].used) continue;
        for (ptrdiff_t k = 0; k < arrlen(d->drawings[i].items); k++) if ((d->drawings[i].items[k].kind == IT_BITMAP || d->drawings[i].items[k].kind == IT_FILL) && d->drawings[i].items[k].sym == id) return false;
    }
    for (ptrdiff_t i = 0; i < arrlen(d->bitmaps); i++) if (d->bitmaps[i] == id) { arrdel(d->bitmaps, i); doc_touch(d); return true; }
    return false;
}

bool doc_delete_symbol(Doc *d, int id) {   // only one nothing uses (its drawings are freed)
    for (ptrdiff_t i = 0; i < arrlen(d->drawings); i++) {
        if (!d->drawings[i].used) continue;
        for (ptrdiff_t k = 0; k < arrlen(d->drawings[i].items); k++) if (d->drawings[i].items[k].kind == IT_INSTANCE && d->drawings[i].items[k].sym == id) return false;
    }
    for (ptrdiff_t i = 0; i < arrlen(d->symbols); i++) {
        if (d->symbols[i].id != id) continue;
        Timeline *t = &d->symbols[i].tl;
        for (ptrdiff_t li = 0; li < arrlen(t->layers); li++)
            for (ptrdiff_t k = 0; k < arrlen(t->layers[li].keys); k++) if (t->layers[li].keys[k].d >= 0) d->drawings[t->layers[li].keys[k].d].used = false;
        timeline_free(t);
        arrdel(d->symbols, i);
        doc_touch(d);
        return true;
    }
    return false;
}
