// The Vertexa logo (assets/brand, carried over from Vertexa 1): the title bar image and the window icon.
// The 256 px source is box-filtered down to the exact pixel size, so it stays crisp at any DPI.
#include "gfx/logo.h"
#include "assets.h"
#include "stb/stb_image.h"
#include "sokol/sokol_app.h"

static struct { u8 *px; int w, h; bool failed; sg_image img; sg_view view; int size; } L;

static bool load(void) {
    if (L.px) return true;
    if (L.failed) return false;
    const Blob *b = asset_logo();
    int n;
    L.px = stbi_load_from_memory(b->data, (int)b->size, &L.w, &L.h, &n, 4);
    L.failed = !L.px;
    return L.px != NULL;
}

// area average over each destination pixel's footprint, in premultiplied space
static u8 *resample(int dw, int dh, bool premultiplied) {
    u8 *out = malloc((size_t)dw * dh * 4);
    float sx = (float)L.w / dw, sy = (float)L.h / dh;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
            float acc[4] = {0}, wsum = 0;
            for (int j = (int)y0; j < (int)ceilf(y1) && j < L.h; j++) {
                float wy = MIN(y1, j + 1) - MAX(y0, j);
                for (int i = (int)x0; i < (int)ceilf(x1) && i < L.w; i++) {
                    float w = (MIN(x1, i + 1) - MAX(x0, i)) * wy;
                    const u8 *p = L.px + ((size_t)j * L.w + i) * 4;
                    float a = p[3] / 255.0f;
                    acc[0] += p[0] * a * w; acc[1] += p[1] * a * w; acc[2] += p[2] * a * w; acc[3] += p[3] * w;
                    wsum += w;
                }
            }
            u8 *o = out + ((size_t)y * dw + x) * 4;
            float a = wsum > 0 ? acc[3] / wsum : 0;
            for (int k = 0; k < 3; k++) {
                float c = wsum > 0 ? acc[k] / wsum : 0;              // premultiplied
                if (!premultiplied) c = a > 0.5f ? c * 255.0f / a : 0;
                o[k] = (u8)clampf(roundf(c), 0, 255);
            }
            o[3] = (u8)clampf(roundf(a), 0, 255);
        }
    return out;
}

u32 logo_view(int px) {
    px = CLAMP(px, 8, 256);
    if (L.size == px && L.view.id) return L.view.id;
    if (!load()) return 0;
    if (L.view.id) { sg_destroy_view(L.view); sg_destroy_image(L.img); }
    u8 *p = resample(px, px, true);
    L.img = sg_make_image(&(sg_image_desc){
        .width = px, .height = px, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = {p, (size_t)px * px * 4}, .label = "vx-logo",
    });
    L.view = sg_make_view(&(sg_view_desc){.texture.image = L.img});
    L.size = px;
    free(p);
    return L.view.id;
}

void logo_window_icon(void) {
    if (!load()) return;
    static const int sizes[] = {16, 24, 32, 48, 64, 128};
    sapp_icon_desc d = {0};
    u8 *px[COUNT(sizes)];
    for (int i = 0; i < COUNT(sizes); i++) {
        px[i] = resample(sizes[i], sizes[i], false);
        d.images[i] = (sapp_image_desc){.width = sizes[i], .height = sizes[i], .pixels = {px[i], (size_t)sizes[i] * sizes[i] * 4}};
    }
    sapp_set_icon(&d);
    for (int i = 0; i < COUNT(sizes); i++) free(px[i]);
}

/* ── the mark: the V with its anchors, drawn from the logo's own geometry (no backing, no gradient) ── */

// logo.svg: viewBox 1024; the V is two cubic Beziers 116 wide, anchors and handles on top
static V2 bez(V2 a, V2 b, V2 c, V2 d, float t) {
    float u = 1 - t;
    return v2(u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x, u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y);
}
static float seg_dist(V2 p, V2 a, V2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0, 1) : 0;
    return hypotf(p.x - a.x - dx * t, p.y - a.y - dy * t);
}
static float rrect_sd(V2 p, float x, float y, float w, float h, float r) {   // signed distance to a rounded rect
    float cx = x + w / 2, cy = y + h / 2, qx = fabsf(p.x - cx) - (w / 2 - r), qy = fabsf(p.y - cy) - (h / 2 - r);
    return hypotf(MAX(qx, 0), MAX(qy, 0)) + MIN(MAX(qx, qy), 0) - r;
}

static struct { sg_image img; sg_view view; int h; } MKS[4];
static int mk_next;
#define MK_X0 204.0f
#define MK_Y0 248.0f
#define MK_W 616.0f
#define MK_H 562.0f

int logo_mark_width(int h) { return (int)roundf(h * MK_W / MK_H); }

u32 logo_mark_view(int h) {
    h = CLAMP(h, 8, 512);
    for (int i = 0; i < 4; i++) if (MKS[i].h == h && MKS[i].view.id) return MKS[i].view.id;
    int slot = mk_next++ & 3;
    #define MK MKS[slot]
    if (MK.view.id) { sg_destroy_view(MK.view); sg_destroy_image(MK.img); }
    int w = logo_mark_width(h);
    V2 line[129];
    V2 a0 = {262, 292}, a1 = {330, 540}, a2 = {450, 752}, a3 = {512, 752}, b1 = {574, 752}, b2 = {694, 540}, b3 = {762, 292};
    for (int i = 0; i <= 64; i++) { line[i] = bez(a0, a1, a2, a3, i / 64.0f); line[64 + i] = bez(a3, b1, b2, b3, i / 64.0f); }
    static const float RECTS[3][2] = {{474, 714}, {224, 254}, {724, 254}}, CIRC[2] = {412, 612};
    const float ink[3] = {1.0f, 0.353f, 0.212f}, light[3] = {0.957f, 0.945f, 0.918f}, dark[3] = {0.078f, 0.075f, 0.098f};
    u8 *px = calloc((size_t)w * h, 4);
    float k = MK_H / h;
    const int SS = 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[4] = {0};
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    V2 p = {MK_X0 + (x + (sx + 0.5f) / SS) * k, MK_Y0 + (y + (sy + 0.5f) / SS) * k};
                    const float *c = NULL;
                    float dv = 1e9f;
                    for (int i = 0; i < 128; i++) dv = MIN(dv, seg_dist(p, line[i], line[i + 1]));
                    if (dv <= 58) c = ink;
                    if (seg_dist(p, v2(412, 752), v2(612, 752)) <= 7) c = light;
                    for (int i = 0; i < 2; i++) { float d = hypotf(p.x - CIRC[i], p.y - 752); if (d <= 36) c = d >= 24 ? dark : light; }
                    for (int i = 0; i < 3; i++) { float d = rrect_sd(p, RECTS[i][0], RECTS[i][1], 76, 76, 10); if (d <= 6) c = d >= -6 ? dark : light; }
                    if (c) { acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += 1; }
                }
            u8 *o = px + ((size_t)y * w + x) * 4;   // premultiplied
            for (int i = 0; i < 4; i++) o[i] = (u8)clampf(roundf(acc[i] / (SS * SS) * 255), 0, 255);
        }
    MK.img = sg_make_image(&(sg_image_desc){.width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8,
                                            .data.mip_levels[0] = {px, (size_t)w * h * 4}, .label = "vx-logo-mark"});
    MK.view = sg_make_view(&(sg_view_desc){.texture.image = MK.img});
    MK.h = h;
    free(px);
    return MK.view.id;
    #undef MK
}
