// Line (N), Rectangle (R), Oval (O) and the Paint Bucket (K).
// The bucket floods a raster of the drawing's lines (with optional gap closing), traces the region
// back to vector contours, smooths them and tucks the fill under the lines.
#include "app/app.h"
#include "geom/geom.h"

static struct { bool active; char tool; V2 a, b; Item prev[2]; int nprev; } SH;

static void free_prev(void) { for (int i = 0; i < SH.nprev; i++) item_free(&SH.prev[i]); SH.nprev = 0; }

static void build(char tool, V2 a, V2 b, bool shift, bool alt, Item *out, int *n) {
    *n = 0;
    float w = MAX(0.5f, app.shape_width);
    Item line = {.kind = IT_STROKE, .tip = TIP_ROUND, .aspect = 1, .color = app.stroke, .flags = ITF_LINEAR};
    if (tool == 'N') {
        if (shift) {   // snap to 45 degrees
            float ang = atan2f(b.y - a.y, b.x - a.x), len = hypotf(b.x - a.x, b.y - a.y);
            ang = roundf(ang / (VX_PI / 4)) * (VX_PI / 4);
            b = v2(a.x + cosf(ang) * len, a.y + sinf(ang) * len);
        }
        arrput(line.pts, ((SPt){a.x, a.y, w / 2}));
        arrput(line.pts, ((SPt){b.x, b.y, w / 2}));
        item_bounds(&line);
        out[(*n)++] = line;
        return;
    }
    float dx = b.x - a.x, dy = b.y - a.y;
    if (shift) { float m = MAX(fabsf(dx), fabsf(dy)); dx = dx < 0 ? -m : m; dy = dy < 0 ? -m : m; }
    float x0, y0, x1, y1;
    if (alt) { x0 = a.x - fabsf(dx); x1 = a.x + fabsf(dx); y0 = a.y - fabsf(dy); y1 = a.y + fabsf(dy); }
    else { x0 = MIN(a.x, a.x + dx); x1 = MAX(a.x, a.x + dx); y0 = MIN(a.y, a.y + dy); y1 = MAX(a.y, a.y + dy); }
    V2 *pts = NULL;
    if (tool == 'R') { arrput(pts, v2(x0, y0)); arrput(pts, v2(x1, y0)); arrput(pts, v2(x1, y1)); arrput(pts, v2(x0, y1)); }
    else {
        float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
        int seg = (int)CLAMP((rx + ry) / st_unit() * 0.5f, 24, 160);
        for (int i = 0; i < seg; i++) { float t = i * 2 * VX_PI / seg; arrput(pts, v2(cx + cosf(t) * rx, cy + sinf(t) * ry)); }
    }
    if (app.shape_fill) {
        Item f = {.kind = IT_FILL, .color = col_alpha(app.fill, app.alpha)};
        for (ptrdiff_t i = 0; i < arrlen(pts); i++) arrput(f.poly, pts[i]);
        arrput(f.starts, 0);
        item_bounds(&f);
        out[(*n)++] = f;
    }
    if (app.shape_line) {
        line.flags |= ITF_CLOSED;
        for (ptrdiff_t i = 0; i < arrlen(pts); i++) arrput(line.pts, ((SPt){pts[i].x, pts[i].y, w / 2}));
        item_bounds(&line);
        out[(*n)++] = line;
    } else item_free(&line);
    arrfree(pts);
}

void shape_tool(UiResp *re, char tool) {
    V2 p = st_local(ui.in.mouse);
    if (re->pressed) {
        const char *why = st_cannot_edit();
        if (why) { app_toast("%s", why); return; }
        if (tool != 'N' && !app.shape_fill && !app.shape_line) { app_toast("Turn on Fill or Outline in the options above"); return; }
        SH.active = true; SH.tool = tool; SH.a = SH.b = p;
    }
    if (!SH.active) return;
    SH.b = p;
    free_prev();
    build(SH.tool, SH.a, SH.b, ui.in.mods & MOD_SHIFT, ui.in.mods & MOD_ALT, SH.prev, &SH.nprev);
    if (!re->held) {
        SH.active = false;
        if (hypotf(SH.b.x - SH.a.x, SH.b.y - SH.a.y) > 2 * st_unit() && SH.nprev) {
            app_commit();
            Drawing *dr = st_drawing(true);
            for (int i = 0; i < SH.nprev; i++) arrput(dr->items, SH.prev[i]);
            SH.nprev = 0;
            doc_touch(app.doc);
        }
        free_prev();
    }
}

bool shape_preview(Item **out, int *n) {
    if (!SH.active || !SH.nprev) { *out = NULL; *n = 0; return false; }
    *out = SH.prev; *n = SH.nprev;
    return true;
}

/* ── paint bucket ── */

static void stamp(u8 *g, int W, int H, float cx, float cy, float r) {
    int x0 = MAX(0, (int)floorf(cx - r)), x1 = MIN(W - 1, (int)ceilf(cx + r)), y0 = MAX(0, (int)floorf(cy - r)), y1 = MIN(H - 1, (int)ceilf(cy + r));
    float r2 = r * r;
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) { float dx = x + 0.5f - cx; if (dx * dx + dy * dy <= r2) g[y * W + x] = 1; }
    }
}

static void dilate(u8 *m, int W, int H, int steps) {
    u8 *t = malloc((size_t)W * H);
    for (int s = 0; s < steps; s++) {
        memcpy(t, m, (size_t)W * H);
        bool diag = s % 2;   // alternate 4 / 8 neighbours for a rounder grow
        for (int y = 1; y < H - 1; y++)
            for (int x = 1; x < W - 1; x++) {
                if (t[y * W + x]) continue;
                bool n = t[y * W + x - 1] || t[y * W + x + 1] || t[(y - 1) * W + x] || t[(y + 1) * W + x];
                if (!n && diag) n = t[(y - 1) * W + x - 1] || t[(y - 1) * W + x + 1] || t[(y + 1) * W + x - 1] || t[(y + 1) * W + x + 1];
                if (n) m[y * W + x] = 1;
            }
    }
    free(t);
}

static V2 *chaikin(const V2 *p, int n) {
    V2 *o = NULL;
    for (int i = 0; i < n; i++) {
        V2 a = p[i], b = p[(i + 1) % n];
        arrput(o, v2(a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f));
        arrput(o, v2(a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f));
    }
    return o;
}

void bucket_click(V2 p) {
    Drawing *dr = st_drawing(false);
    Color col = col_alpha(app.fill, app.alpha);
    // clicking an existing fill recolors it
    if (dr) for (ptrdiff_t k = arrlen(dr->items) - 1; k >= 0; k--) {
        Item *it = &dr->items[k];
        if (it->kind == IT_STROKE && !it->erase && item_hit(it, p, 0)) break;   // a line is on top
        if (it->kind == IT_FILL && !it->erase && item_hit(it, p, 0)) {
            app_commit();
            st_drawing(false)->items[k].color = col;
            doc_touch(app.doc);
            return;
        }
    }
    float b[4] = {p.x, p.y, p.x, p.y};
    bool any = false;
    for (ptrdiff_t k = 0; dr && k < arrlen(dr->items); k++) {
        Item *it = &dr->items[k];
        if (it->kind != IT_STROKE || it->erase) continue;
        b[0] = MIN(b[0], it->bx0); b[1] = MIN(b[1], it->by0); b[2] = MAX(b[2], it->bx1); b[3] = MAX(b[3], it->by1);
        any = true;
    }
    if (!any) { app_toast("Nothing to fill here. Draw a closed shape first"); return; }
    float unit = st_unit();
    float bw = b[2] - b[0] + 16 * unit, bh = b[3] - b[1] + 16 * unit;
    float cell = clampf(MAX(0.6f * unit, MAX(bw, bh) / 2600), 0.1f, 8);
    float ox = b[0] - 8 * unit, oy = b[1] - 8 * unit;
    int W = (int)ceilf(bw / cell) + 2, H = (int)ceilf(bh / cell) + 2;
    u8 *wall = calloc((size_t)W * H, 1);
    static const float gaps_px[4] = {0, 3, 7, 14};
    float gap = gaps_px[CLAMP(app.bucket_gap, 0, 3)] * unit / 2;
    for (ptrdiff_t k = 0; k < arrlen(dr->items); k++) {
        Item *it = &dr->items[k];
        if (it->kind != IT_STROKE || it->erase) continue;
        SPt *r = brush_resample(it, cell * 0.5f);
        for (ptrdiff_t i = 0; i < arrlen(r); i++) stamp(wall, W, H, (r[i].x - ox) / cell, (r[i].y - oy) / cell, (r[i].r + gap) / cell + 0.3f);
    }
    int sx = (int)((p.x - ox) / cell), sy = (int)((p.y - oy) / cell);
    if (sx < 0 || sy < 0 || sx >= W || sy >= H || wall[sy * W + sx]) { free(wall); app_toast("Click inside an area enclosed by lines"); return; }
    u8 *fillm = calloc((size_t)W * H, 1);
    int *q = malloc(sizeof(int) * W * H), qh = 0, qt = 0;
    bool leak = false;
    q[qt++] = sy * W + sx; fillm[sy * W + sx] = 1;
    while (qh < qt) {
        int c = q[qh++], x = c % W, y = c / W;
        if (x == 0 || y == 0 || x == W - 1 || y == H - 1) { leak = true; break; }
        int nb[4] = {c - 1, c + 1, c - W, c + W};
        for (int i = 0; i < 4; i++) if (!wall[nb[i]] && !fillm[nb[i]]) { fillm[nb[i]] = 1; q[qt++] = nb[i]; }
    }
    free(q); free(wall);
    if (leak) { free(fillm); app_toast("This area isn't closed. Pick a larger gap size in the options above"); return; }
    // grow back over the gap margin and slightly under the lines, so no hairline shows
    dilate(fillm, W, H, (int)ceilf(gap / cell) + 2);
    V2 *poly = NULL; int *starts = NULL;
    trace_mask(fillm, W, H, &poly, &starts);
    free(fillm);
    Item f = {.kind = IT_FILL, .color = col};
    int nc = (int)arrlen(starts);
    for (int c = 0; c < nc; c++) {
        int a = starts[c], e = c + 1 < nc ? starts[c + 1] : (int)arrlen(poly);
        if (e - a < 4) continue;
        V2 *s = simplify_v2(poly + a, e - a, 0.75f, true);
        if (arrlen(s) >= 3) {
            V2 *sm = chaikin(s, (int)arrlen(s));
            arrput(f.starts, (int)arrlen(f.poly));
            for (ptrdiff_t i = 0; i < arrlen(sm); i++) arrput(f.poly, v2(ox + sm[i].x * cell, oy + sm[i].y * cell));
            arrfree(sm);
        }
        arrfree(s);
    }
    arrfree(poly); arrfree(starts);
    if (!arrlen(f.poly)) { item_free(&f); return; }
    item_bounds(&f);
    app_commit();
    dr = st_drawing(true);
    arrins(dr->items, 0, f);   // fills sit under the lines
    doc_touch(app.doc);
}
