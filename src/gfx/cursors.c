// Cursors: drawn from distance fields on a 32-unit grid, white with a dark outline and a soft shadow
// (the subselection arrow the other way round), then bound to sokol's custom cursor slots.
#include "gfx/cursors.h"
#include "sokol/sokol_app.h"
#include "stb/stb_image_write.h"

/* ── shapes ── */

static float sd_seg(V2 p, V2 a, V2 b, float r) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0, 1) : 0;
    return hypotf(p.x - a.x - dx * t, p.y - a.y - dy * t) - r;
}
static float sd_circle(V2 p, V2 c, float r) { return hypotf(p.x - c.x, p.y - c.y) - r; }
static float sd_rrect(V2 p, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, qx = fabsf(p.x - cx) - ((x1 - x0) / 2 - r), qy = fabsf(p.y - cy) - ((y1 - y0) / 2 - r);
    return hypotf(MAX(qx, 0), MAX(qy, 0)) + MIN(MAX(qx, qy), 0) - r;
}
static float sd_ellipse(V2 p, V2 c, float rx, float ry) {   // close to the true distance near the edge
    float x = p.x - c.x, y = p.y - c.y, k0 = hypotf(x / rx, y / ry), k1 = hypotf(x / (rx * rx), y / (ry * ry));
    return k1 > 1e-6f ? k0 * (k0 - 1) / k1 : -MIN(rx, ry);
}
static float sd_poly(V2 p, const V2 *v, int n) {   // negative inside
    float d = 1e9f; bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        d = MIN(d, sd_seg(p, v[j], v[i], 0));
        if ((v[i].y > p.y) != (v[j].y > p.y) && p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) + v[i].x) in = !in;
    }
    return in ? -d : d;
}
static V2 turn(V2 p, V2 c, float a) {   // p turned by a around c
    float cs = cosf(a), sn = sinf(a), x = p.x - c.x, y = p.y - c.y;
    return v2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
}
#define U(a, b) MIN(a, b)

// the V of the logo (two Beziers, a round stroke), turned like an arrow cursor: its point up and to the
// left, one stroke going down, the other down to the right
static V2 VLINE[129];
static float V_R;
static V2 V_AT, V_HOT; static float V_ANG;
static void v_init(void) {
    V2 a0 = {262, 292}, a1 = {330, 540}, a2 = {450, 752}, a3 = {512, 752}, b1 = {574, 752}, b2 = {694, 540}, b3 = {762, 292};
    float s = 1 / 30.0f;
    for (int i = 0; i <= 64; i++) {
        float t = i / 64.0f, u = 1 - t;
        V2 p = {u * u * u * a0.x + 3 * u * u * t * a1.x + 3 * u * t * t * a2.x + t * t * t * a3.x, u * u * u * a0.y + 3 * u * u * t * a1.y + 3 * u * t * t * a2.y + t * t * t * a3.y};
        V2 q = {u * u * u * a3.x + 3 * u * u * t * b1.x + 3 * u * t * t * b2.x + t * t * t * b3.x, u * u * u * a3.y + 3 * u * u * t * b1.y + 3 * u * t * t * b2.y + t * t * t * b3.y};
        VLINE[i] = v2((p.x - 512) * s, (p.y - 752) * s);
        VLINE[64 + i] = v2((q.x - 512) * s, (q.y - 752) * s);
    }
    V_R = 58 * s;
    V_ANG = 157 * VX_PI / 180;
    // the outside of the bottom of the V is the point; the whole shape a unit from the top-left corner
    V2 dir = turn(v2(0, 1), v2(0, 0), V_ANG);
    float x0 = 1e9f, y0 = 1e9f, best = -1e9f;
    V2 tip = {0, 0};
    for (int i = 0; i <= 128; i++) {
        V2 q = turn(VLINE[i], v2(0, 0), V_ANG);
        x0 = MIN(x0, q.x - V_R); y0 = MIN(y0, q.y - V_R);
        float e = q.x * dir.x + q.y * dir.y;
        if (e > best) { best = e; tip = v2(q.x + dir.x * V_R, q.y + dir.y * V_R); }
    }
    V_AT = v2(1.2f - x0, 1.2f - y0);
    V_HOT = v2(V_AT.x + tip.x, V_AT.y + tip.y);
}
static float sd_v(V2 p) {
    V2 q = turn(v2(p.x - V_AT.x, p.y - V_AT.y), v2(0, 0), -V_ANG);
    float d = 1e9f;
    for (int i = 0; i < 128; i++) d = MIN(d, sd_seg(q, VLINE[i], VLINE[i + 1], 0));
    return d - V_R;
}

// The hands: AOSP's vector pointer icons (frameworks/base/core/res/res/drawable/pointer_{grab,grabbing,hand}_vector.xml,
// Copyright 2024 The Android Open Source Project, Apache License 2.0, see assets/cursors/LICENSE-Apache-2.0.txt).
// Each is an outline ring and a fill, on a 24-unit viewport.
static const char HAND_OPEN_LINE[] = "M20.442 7.562a2 2 0 0 0-2-2c-.366 0-.705.106-1 .277V4.686a2 2 0 0 0-2-2 2 2 0 0 0-1.004.279 1.995 1.995 0 0 0-3.986-.06 2 2 0 0 0-1.006-.28 2 2 0 0 0-2 2v6.501l-.247-.253a2.216 2.216 0 0 0-3.178 0 2.286 2.286 0 0 0 0 3.186l5.106 5.224q.063.061.131.118l-.001.001a6.58 6.58 0 0 0 4.624 1.901c3.587 0 6.565-2.906 6.565-6.516q0-.105-.004-.21m-6.561 5.727a5.58 5.58 0 0 1-3.922-1.613 1 1 0 0 1-.117-.106l-5.106-5.224a1.286 1.286 0 0 1 0-1.788 1.215 1.215 0 0 1 1.747 0l1.962 2.008V4.625a1 1 0 0 1 2 0v5.833q.463-.362.996-.623V3a1 1 0 0 1 2 0v6.29a6 6 0 0 1 1 .011V4.686a1 1 0 0 1 2 0v5.21c.357.185.693.408 1 .663V7.562a1 1 0 0 1 2 0v7.019h.001-.001q.004.104.004.207c.001 3.046-2.518 5.516-5.564 5.516";
static const char HAND_OPEN_FILL[] = "M19.442 14.581V7.562a1 1 0 0 0-2 0v2.997a5.7 5.7 0 0 0-1-.663v-5.21a1 1 0 0 0-2 0v4.615a5.5 5.5 0 0 0-1-.011V3a1 1 0 0 0-2 0v6.835a5.6 5.6 0 0 0-.996.623V4.625a1 1 0 0 0-2 0v8.955l-1.962-2.008a1.215 1.215 0 0 0-1.747 0 1.286 1.286 0 0 0 0 1.788l5.106 5.224q.056.057.117.106a5.58 5.58 0 0 0 3.922 1.613c3.046 0 5.565-2.469 5.565-5.516z";
static const char HAND_GRAB_LINE[] = "M19.485 12.622V8.508a2 2 0 0 0-3.12-1.657 1.993 1.993 0 0 0-2.99-1.006 1.99 1.99 0 0 0-1.886-1.361c-.903 0-1.658.603-1.906 1.425a2 2 0 0 0-3.09 1.674v2.206a2.2 2.2 0 0 0-2.159.586 2.285 2.285 0 0 0 0 3.185l3.847 3.936q.063.061.13.118l-.001.001a6.58 6.58 0 0 0 4.624 1.902c3.586 0 6.563-2.905 6.563-6.514a5 5 0 0 0-.012-.381m-6.55 5.895a5.58 5.58 0 0 1-3.922-1.613 1 1 0 0 1-.117-.106l-3.847-3.936c-.482-.494-.482-1.294 0-1.787s1.265-.494 1.747 0l.697.713V7.583a1 1 0 0 1 2 0v1.096q.463-.364.997-.625v-1.57a1 1 0 0 1 2 0v1.022a5.5 5.5 0 0 1 .996.009v-.007a1 1 0 0 1 2 0v.599q.537.277 1 .66v-.259a1 1 0 0 1 2 0v4.115q.013.189.013.38c-.001 3.045-2.518 5.514-5.564 5.514";
static const char HAND_GRAB_FILL[] = "M18.485 12.622V8.508a1 1 0 0 0-2 0v.259a5.6 5.6 0 0 0-1-.66v-.599a1 1 0 0 0-2 0v.008a5.6 5.6 0 0 0-.996-.009V6.484a1 1 0 0 0-2 0v1.57a5.7 5.7 0 0 0-.997.625V7.583a1 1 0 0 0-2 0v4.205l-.697-.713c-.482-.494-1.265-.494-1.747 0s-.482 1.294 0 1.787l3.847 3.936q.056.057.117.106a5.58 5.58 0 0 0 3.922 1.613c3.045 0 5.563-2.469 5.563-5.514a5 5 0 0 0-.012-.381";
static const char HAND_POINT_LINE[] = "M20.492 15.197v-4.198A1.995 1.995 0 0 0 18.5 9.001c-.413 0-.797.126-1.115.342a1.99 1.99 0 0 0-1.873-1.341c-.411 0-.792.125-1.109.339a1.99 1.99 0 0 0-1.879-1.361c-.363 0-.699.105-.992.275V3.998A1.99 1.99 0 0 0 9.542 2c-1.1 0-1.992.895-1.992 1.998v7.831l-.242-.249a2.2 2.2 0 0 0-3.164 0 2.29 2.29 0 0 0 0 3.183l5.084 5.219q.063.061.13.118l-.001.001A6.54 6.54 0 0 0 13.963 22c3.572 0 6.537-2.903 6.537-6.509q0-.148-.008-.294m-6.529 5.804a5.55 5.55 0 0 1-3.906-1.611 1 1 0 0 1-.117-.106l-5.084-5.219a1.286 1.286 0 0 1 0-1.786 1.21 1.21 0 0 1 1.74 0l1.95 2.002V3.998c0-.552.446-.999.996-.999s.996.447.996.999v7.17l.011-.007a.495.495 0 0 0 .989-.037V8.939a.992.992 0 0 1 1.984.039v.796l-.007 1.386a.5.5 0 0 0 .495.502h.003a.5.5 0 0 0 .498-.497l.006-1.157h.001V10a.997.997 0 1 1 1.991 0v.601l.004.003v1.02q.001.107.042.199a.5.5 0 0 0 .153.187l.031.021a.5.5 0 0 0 .231.083c.014.001.026.008.04.008a.5.5 0 0 0 .498-.5v-.642a.996.996 0 0 1 .993-.98c.55 0 .996.447.996.999v4.199a6 6 0 0 1 .008.293c-.001 3.043-2.509 5.51-5.542 5.51";
static const char HAND_POINT_FILL[] = "M19.496 10.999A.997.997 0 0 0 18.5 10a.995.995 0 0 0-.992.98v.644a.5.5 0 0 1-.498.5c-.014 0-.026-.007-.04-.008a.493.493 0 0 1-.457-.491v-1.02l-.004-.003V10c0-.552-.446-.999-.996-.999s-.996.447-.996.999v.008h-.001l-.005 1.003-.001.154a.5.5 0 0 1-.498.497h-.003a.5.5 0 0 1-.495-.502l.001-.159.006-1.227v-.796a.997.997 0 0 0-.996-.999.993.993 0 0 0-.988.96v2.185a.496.496 0 0 1-.989.037l-.011.007v-7.17a.997.997 0 0 0-.996-.999.997.997 0 0 0-.996.999V14.28l-1.95-2.002a1.21 1.21 0 0 0-1.74 0 1.286 1.286 0 0 0 0 1.786l5.084 5.219q.056.057.117.106A5.54 5.54 0 0 0 13.962 21c3.033 0 5.541-2.467 5.541-5.51a6 6 0 0 0-.008-.293z";

static float sd_rotate(V2 p) {   // three quarters of a ring and an arrowhead
    V2 c = {16, 16};
    float R = 8.6f, w = 1.7f, a0 = 150 * VX_PI / 180, a1 = 400 * VX_PI / 180;
    float a = atan2f(p.y - c.y, p.x - c.x);
    while (a < a0) a += 2 * VX_PI;
    float d;
    if (a <= a1) d = fabsf(hypotf(p.x - c.x, p.y - c.y) - R) - w;
    else {
        V2 e0 = {c.x + R * cosf(a0), c.y + R * sinf(a0)};
        d = sd_circle(p, e0, w);
    }
    V2 e = {c.x + R * cosf(a1), c.y + R * sinf(a1)}, t = {-sinf(a1), cosf(a1)}, n = {cosf(a1), sinf(a1)};
    V2 head[3] = {v2(e.x + t.x * 5.6f, e.y + t.y * 5.6f), v2(e.x + n.x * 5.2f - t.x * 0.6f, e.y + n.y * 5.2f - t.y * 0.6f), v2(e.x - n.x * 5.2f - t.x * 0.6f, e.y - n.y * 5.2f - t.y * 0.6f)};
    return U(d, sd_poly(p, head, 3));
}
static float sd_move(V2 p) {
    float d = U(sd_seg(p, v2(16, 6), v2(16, 26), 1.25f), sd_seg(p, v2(6, 16), v2(26, 16), 1.25f));
    static const V2 h[4][3] = {{{16, 1.6f}, {21, 7.4f}, {11, 7.4f}}, {{16, 30.4f}, {11, 24.6f}, {21, 24.6f}},
                               {{1.6f, 16}, {7.4f, 11}, {7.4f, 21}}, {{30.4f, 16}, {24.6f, 21}, {24.6f, 11}}};
    for (int i = 0; i < 4; i++) d = U(d, sd_poly(p, h[i], 3));
    return d;
}
static float sd_cross(V2 p) {
    float d = sd_seg(p, v2(16, 4), v2(16, 12.6f), 0.75f);
    d = U(d, sd_seg(p, v2(16, 19.4f), v2(16, 28), 0.75f));
    d = U(d, sd_seg(p, v2(4, 16), v2(12.6f, 16), 0.75f));
    d = U(d, sd_seg(p, v2(19.4f, 16), v2(28, 16), 0.75f));
    return U(d, sd_circle(p, v2(16, 16), 0.9f));
}
static float sd_dropper(V2 p) {
    float d = sd_seg(p, v2(3.2f, 28.8f), v2(8.6f, 23.4f), 0.95f);
    d = U(d, sd_seg(p, v2(8.2f, 23.8f), v2(18.6f, 13.4f), 2.3f));
    d = U(d, sd_seg(p, v2(16.8f, 11.2f), v2(20.8f, 15.2f), 1.5f));
    return U(d, sd_seg(p, v2(20.8f, 11.2f), v2(25.2f, 6.8f), 3.4f));
}
// the bucket, in the manner of the hands: a square pail tipped onto its corner, the mouth its upper right side,
// the handle that side running on past the top corner; the paint inside lies level, so it reaches the lip at the
// right corner, where a drop falls from (the point). Layers: a halo for the drop, the pail, the paint
static const V2 PAIL_C = {13.0f, 14.4f};
#define PAIL_R 7.0f   // centre to corner
#define DROP_R 1.75f
static float sd_pail_box(V2 p) { float h = PAIL_R / 1.41421356f; return sd_rrect(turn(p, PAIL_C, -VX_PI / 4), PAIL_C.x - h, PAIL_C.y - h, PAIL_C.x + h, PAIL_C.y + h, 1.1f); }
static float sd_pail(V2 p) {
    V2 top = {PAIL_C.x, PAIL_C.y - PAIL_R + 0.5f};
    return U(sd_pail_box(p), sd_seg(p, v2(top.x + 0.4f, top.y + 0.4f), v2(top.x - 2.7f, top.y - 2.7f), 0.75f));
}
// the drop, falling from the lip at the right corner (no stream: the user wants just the drop)
static const V2 DROP = {PAIL_C.x + PAIL_R + 0.5f, PAIL_C.y + 7.2f};
static float sd_drop(V2 p) {   // round below, pointed above
    static const V2 tip[3] = {{0, -3.6f}, {-1.62f, -0.55f}, {1.62f, -0.55f}};
    V2 q = v2(p.x - DROP.x, p.y - DROP.y);
    return U(sd_circle(q, v2(0, 0), DROP_R), sd_poly(q, tip, 3));
}
static float sd_paint(V2 p) { return U(MAX(sd_pail_box(p) + 1.6f, PAIL_C.y + 0.3f - p.y), sd_drop(p)); }   // level, inside a white wall
static float sd_pail_halo(V2 p) { return sd_drop(p) - 1.0f; }

static float sd_lens(V2 p) { return U(sd_circle(p, v2(13, 13), 8.6f), sd_seg(p, v2(19.2f, 19.2f), v2(27.6f, 27.6f), 2.4f)); }
static float sd_plus(V2 p) { return U(sd_seg(p, v2(9.2f, 13), v2(16.8f, 13), 0.95f), sd_seg(p, v2(13, 9.2f), v2(13, 16.8f), 0.95f)); }
static float sd_minus(V2 p) { return sd_seg(p, v2(9.2f, 13), v2(16.8f, 13), 0.95f); }
static float resize_at(V2 p, float a) {   // a double arrow through the middle, along angle a
    V2 q = turn(p, v2(16, 16), -a);
    float d = sd_seg(q, v2(9.5f, 16), v2(22.5f, 16), 1.25f);
    static const V2 r[3] = {{29.6f, 16}, {23.2f, 10.8f}, {23.2f, 21.2f}}, l[3] = {{2.4f, 16}, {8.8f, 21.2f}, {8.8f, 10.8f}};
    return U(d, U(sd_poly(q, r, 3), sd_poly(q, l, 3)));
}
static float sd_ew(V2 p) { return resize_at(p, 0); }
static float sd_ns(V2 p) { return resize_at(p, VX_PI / 2); }
static float sd_nwse(V2 p) { return resize_at(p, VX_PI / 4); }
static float sd_nesw(V2 p) { return resize_at(p, -VX_PI / 4); }

typedef float (*Sdf)(V2 p);
// a layer is a distance field (filled, outlined lw wide) or SVG path data filled with the nonzero rule
typedef struct { Sdf sd; u32 fill, line; float lw; const char *path; } Lay;   // colours 0xRRGGBB
// vs > 0: path layers are in a viewport placed at (vx, vy) with vs units per viewport unit, and so is the hotspot
// ss > 0: the distance-field layers drawn that much smaller or bigger about the middle (and the hotspot with them)
typedef struct { Lay l[4]; int n; float hx, hy; float vs, vx, vy, ss; } Def;

/* ── SVG path data: flattened once, filled with the nonzero winding rule ── */

typedef struct { const char *d; V2 *pt; int *start; } Outline;
static Outline OUTLINES[8];

static const char *ws(const char *s) { while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\r' || *s == '\t') s++; return s; }
static bool rnum(const char **sp, float *v) {
    const char *s = ws(*sp), *b = s;
    if (*s == '-' || *s == '+') s++;
    bool dig = false, dot = false;
    while ((*s >= '0' && *s <= '9') || (*s == '.' && !dot)) { if (*s == '.') dot = true; else dig = true; s++; }
    if (!dig) return false;
    if ((*s == 'e' || *s == 'E') && ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s[2] >= '0' && s[2] <= '9'))) { s += 2; while (*s >= '0' && *s <= '9') s++; }
    char tmp[48]; int n = (int)MIN(s - b, 47); memcpy(tmp, b, (size_t)n); tmp[n] = 0;
    *v = strtof(tmp, NULL);
    *sp = s;
    return true;
}
static bool rflag(const char **sp, float *v) {   // arc flags may be packed ("01")
    const char *s = ws(*sp);
    if (*s != '0' && *s != '1') return false;
    *v = (float)(*s - '0'); *sp = s + 1;
    return true;
}
static void arc_to(Outline *o, V2 a, float rx, float ry, float phi, int fa, int fs, V2 b) {   // SVG F.6.5
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f) { arrput(o->pt, b); return; }
    float cp = cosf(phi * VX_PI / 180), sp = sinf(phi * VX_PI / 180);
    float dx = (a.x - b.x) / 2, dy = (a.y - b.y) / 2, x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    float lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lam > 1) { rx *= sqrtf(lam); ry *= sqrtf(lam); }
    float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float co = den > 0 ? sqrtf(MAX(0, num / den)) : 0;
    if (fa == fs) co = -co;
    float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    float cx = cp * cxp - sp * cyp + (a.x + b.x) / 2, cy = sp * cxp + cp * cyp + (a.y + b.y) / 2;
    float t1 = atan2f((y1 - cyp) / ry, (x1 - cxp) / rx), t2 = atan2f((-y1 - cyp) / ry, (-x1 - cxp) / rx), dt = t2 - t1;
    if (!fs && dt > 0) dt -= 2 * VX_PI;
    else if (fs && dt < 0) dt += 2 * VX_PI;
    int n = MAX(2, (int)(fabsf(dt) / (VX_PI / 24)) + 1);
    for (int k = 1; k <= n; k++) {
        float t = t1 + dt * k / n;
        arrput(o->pt, v2(cx + rx * cosf(t) * cp - ry * sinf(t) * sp, cy + rx * cosf(t) * sp + ry * sinf(t) * cp));
    }
}
static void outline_parse(Outline *o) {
    const char *s = o->d;
    char cmd = 0;
    V2 cur = {0, 0}, st = {0, 0}, lc = {0, 0}, lq = {0, 0};
    bool hc = false, hq = false;
    for (int guard = 0; guard < 100000; guard++) {
        s = ws(s);
        if (!*s) break;
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
            cmd = *s++;
            if (cmd == 'Z' || cmd == 'z') { cur = st; hc = hq = false; continue; }
        } else if (!cmd) break;
        bool rel = cmd >= 'a';
        char C = rel ? (char)(cmd - 32) : cmd;
        float v[7];
        V2 o0 = rel ? cur : v2(0, 0);
        switch (C) {
        case 'M':
            if (!rnum(&s, &v[0]) || !rnum(&s, &v[1])) return;
            cur = st = v2(v[0] + o0.x, v[1] + o0.y);
            arrput(o->start, (int)arrlen(o->pt)); arrput(o->pt, cur);
            cmd = rel ? 'l' : 'L'; hc = hq = false;
            break;
        case 'L':
            if (!rnum(&s, &v[0]) || !rnum(&s, &v[1])) return;
            cur = v2(v[0] + o0.x, v[1] + o0.y); arrput(o->pt, cur); hc = hq = false;
            break;
        case 'H': if (!rnum(&s, &v[0])) return; cur.x = v[0] + o0.x; arrput(o->pt, cur); hc = hq = false; break;
        case 'V': if (!rnum(&s, &v[0])) return; cur.y = v[0] + o0.y; arrput(o->pt, cur); hc = hq = false; break;
        case 'C': case 'S': {
            V2 c1;
            if (C == 'C') { if (!rnum(&s, &v[0]) || !rnum(&s, &v[1])) return; c1 = v2(v[0] + o0.x, v[1] + o0.y); }
            else c1 = hc ? v2(2 * cur.x - lc.x, 2 * cur.y - lc.y) : cur;
            if (!rnum(&s, &v[2]) || !rnum(&s, &v[3]) || !rnum(&s, &v[4]) || !rnum(&s, &v[5])) return;
            V2 c2 = v2(v[2] + o0.x, v[3] + o0.y), e = v2(v[4] + o0.x, v[5] + o0.y);
            for (int k = 1; k <= 12; k++) {
                float t = k / 12.0f, u = 1 - t;
                arrput(o->pt, v2(u * u * u * cur.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * e.x,
                                 u * u * u * cur.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * e.y));
            }
            lc = c2; hc = true; hq = false; cur = e;
            break;
        }
        case 'Q': case 'T': {
            V2 c1;
            if (C == 'Q') { if (!rnum(&s, &v[0]) || !rnum(&s, &v[1])) return; c1 = v2(v[0] + o0.x, v[1] + o0.y); }
            else c1 = hq ? v2(2 * cur.x - lq.x, 2 * cur.y - lq.y) : cur;
            if (!rnum(&s, &v[2]) || !rnum(&s, &v[3])) return;
            V2 e = v2(v[2] + o0.x, v[3] + o0.y);
            for (int k = 1; k <= 12; k++) {
                float t = k / 12.0f, u = 1 - t;
                arrput(o->pt, v2(u * u * cur.x + 2 * u * t * c1.x + t * t * e.x, u * u * cur.y + 2 * u * t * c1.y + t * t * e.y));
            }
            lq = c1; hq = true; hc = false; cur = e;
            break;
        }
        case 'A':
            if (!rnum(&s, &v[0]) || !rnum(&s, &v[1]) || !rnum(&s, &v[2]) || !rflag(&s, &v[3]) || !rflag(&s, &v[4]) || !rnum(&s, &v[5]) || !rnum(&s, &v[6])) return;
            { V2 e = v2(v[5] + o0.x, v[6] + o0.y); arc_to(o, cur, v[0], v[1], v[2], (int)v[3], (int)v[4], e); cur = e; }
            hc = hq = false;
            break;
        default: return;
        }
    }
}
static Outline *outline(const char *d) {
    for (int i = 0; i < COUNT(OUTLINES); i++) {
        if (OUTLINES[i].d == d) return &OUTLINES[i];
        if (!OUTLINES[i].d) { OUTLINES[i].d = d; outline_parse(&OUTLINES[i]); return &OUTLINES[i]; }
    }
    return NULL;
}
static bool outline_inside(const Outline *o, V2 q) {   // nonzero winding; every subpath closes on itself
    int w = 0, ns = (int)arrlen(o->start);
    for (int k = 0; k < ns; k++) {
        int a = o->start[k], b = k + 1 < ns ? o->start[k + 1] : (int)arrlen(o->pt);
        for (int i = a; i < b; i++) {
            V2 p0 = o->pt[i], p1 = o->pt[i + 1 < b ? i + 1 : a];
            float c = (p1.x - p0.x) * (q.y - p0.y) - (q.x - p0.x) * (p1.y - p0.y);
            if (p0.y <= q.y) { if (p1.y > q.y && c > 0) w++; }
            else if (p1.y <= q.y && c < 0) w--;
        }
    }
    return w != 0;
}

#define W 0xffffff
#define K 0x141414
static const Def DEFS[16] = {
    {{{sd_v, W, K, 1.0f}}, 1, -1, -1},                                       // the V: the default arrow (hotspot: its point)
    {{{sd_v, K, W, 1.0f}}, 1, -1, -1},                                       // subselection: the V, dark
    {{{NULL, K, 0, 0, HAND_OPEN_LINE}, {NULL, W, 0, 0, HAND_OPEN_FILL}}, 2, 9.5f, 4.5f, 1.12f, 2.6f, 2.6f},
    {{{NULL, K, 0, 0, HAND_GRAB_LINE}, {NULL, W, 0, 0, HAND_GRAB_FILL}}, 2, 8.5f, 7.5f, 1.12f, 2.6f, 2.6f},
    {{{NULL, K, 0, 0, HAND_POINT_LINE}, {NULL, W, 0, 0, HAND_POINT_FILL}}, 2, 9.5f, 2.5f, 1.12f, 2.6f, 2.6f},
    {{{sd_rotate, W, K, 1.0f}}, 1, 16, 16, 0, 0, 0, 0.84f},
    {{{sd_move, W, K, 1.0f}}, 1, 16, 16, 0, 0, 0, 0.84f},
    {{{sd_cross, W, K, 1.0f}}, 1, 16, 16},
    {{{sd_dropper, W, K, 1.0f}}, 1, 2.6f, 29.4f, 0, 0, 0, 0.9f},
    {{{sd_pail_halo, W, W, 0}, {sd_pail, W, K, 1.0f}, {sd_paint, K, K, 0}}, 3, -2, -2},
    {{{sd_lens, W, K, 1.0f}, {sd_plus, K, K, 0}}, 2, 13, 13, 0, 0, 0, 0.84f},
    {{{sd_lens, W, K, 1.0f}, {sd_minus, K, K, 0}}, 2, 13, 13, 0, 0, 0, 0.84f},
    {{{sd_ew, W, K, 1.0f}}, 1, 16, 16, 0, 0, 0, 0.84f},
    {{{sd_ns, W, K, 1.0f}}, 1, 16, 16, 0, 0, 0, 0.84f},
    {{{sd_nwse, W, K, 1.0f}}, 1, 16, 16, 0, 0, 0, 0.84f},
    {{{sd_nesw, W, K, 1.0f}}, 1, 16, 16, 0, 0, 0, 0.84f},
};

// S x S straight RGBA: 4 x 4 samples per pixel, layers painted in order, a soft shadow below
static u8 *raster(const Def *d, int S) {
    float k = 32.0f / S;
    float lw[4];
    for (int i = 0; i < d->n; i++) lw[i] = d->l[i].lw;
    float *acc = calloc((size_t)S * S * 4, sizeof(float));
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float *o = acc + ((size_t)y * S + x) * 4;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    V2 p = {(x + (sx + 0.5f) / 4) * k, (y + (sy + 0.5f) / 4) * k};
                    u32 col = 0; bool hit = false;
                    for (int i = 0; i < d->n; i++) {
                        if (d->l[i].path) {
                            Outline *ol = outline(d->l[i].path);
                            if (ol && outline_inside(ol, v2((p.x - d->vx) / d->vs, (p.y - d->vy) / d->vs))) { col = d->l[i].fill; hit = true; }
                            continue;
                        }
                        float sc = d->ss > 0 ? d->ss : 1;
                        float dist = d->l[i].sd(v2(16 + (p.x - 16) / sc, 16 + (p.y - 16) / sc)) * sc;
                        if (dist < 0) { col = d->l[i].fill; hit = true; }
                        else if (dist < lw[i]) { col = d->l[i].line; hit = true; }
                    }
                    if (!hit) continue;
                    o[0] += ((col >> 16) & 255) / 255.0f; o[1] += ((col >> 8) & 255) / 255.0f; o[2] += (col & 255) / 255.0f; o[3] += 1;
                }
            for (int c = 0; c < 4; c++) o[c] /= 16;
        }
    // shadow: the alpha, blurred twice with a small box, a little lower
    int r = MAX(1, (int)roundf(S / 32.0f)), dy = MAX(1, (int)roundf(S / 24.0f));
    float *a = calloc((size_t)S * S, sizeof(float)), *t = calloc((size_t)S * S, sizeof(float));
    for (int i = 0; i < S * S; i++) a[i] = acc[i * 4 + 3];
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) { float s = 0; for (int q = -r; q <= r; q++) s += a[y * S + CLAMP(x + q, 0, S - 1)]; t[y * S + x] = s / (2 * r + 1); }
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) { float s = 0; for (int q = -r; q <= r; q++) s += t[CLAMP(y + q, 0, S - 1) * S + x]; a[y * S + x] = s / (2 * r + 1); }
    }
    u8 *out = malloc((size_t)S * S * 4);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float *o = acc + ((size_t)y * S + x) * 4;
            float sh = y - dy >= 0 ? a[(y - dy) * S + x] * 0.32f : 0;
            float al = o[3] + sh * (1 - o[3]);
            u8 *p = out + ((size_t)y * S + x) * 4;
            for (int c = 0; c < 3; c++) p[c] = al > 0 ? (u8)clampf(roundf(o[c] / al * 255), 0, 255) : 0;
            p[3] = (u8)clampf(roundf(al * 255), 0, 255);
        }
    free(acc); free(a); free(t);
    return out;
}

static float hot_x(int i) {
    const Def *d = &DEFS[i];
    float x = d->vs > 0 ? d->vx + d->vs * d->hx : d->hx == -2 ? DROP.x : d->hx < 0 ? V_HOT.x : d->hx;
    return d->ss > 0 ? 16 + (x - 16) * d->ss : x;
}
static float hot_y(int i) {
    const Def *d = &DEFS[i];
    float y = d->vs > 0 ? d->vy + d->vs * d->hy : d->hy == -2 ? DROP.y + DROP_R : d->hy < 0 ? V_HOT.y : d->hy;
    return d->ss > 0 ? 16 + (y - 16) * d->ss : y;
}

static bool bound[16];

void cursors_init(float scale) {
    if (!VLINE[1].x && !VLINE[1].y) v_init();
    int S = CLAMP((int)roundf(32 * scale), 24, 128);
    for (int i = 0; i < 16; i++) {
        sapp_mouse_cursor c = (sapp_mouse_cursor)(SAPP_MOUSECURSOR_CUSTOM_0 + i);
        if (bound[i]) { sapp_unbind_mouse_cursor_image(c); bound[i] = false; }
        u8 *px = raster(&DEFS[i], S);
        float k = S / 32.0f;
        sapp_mouse_cursor got = sapp_bind_mouse_cursor_image(c, &(sapp_image_desc){
            .width = S, .height = S,
            .cursor_hotspot_x = CLAMP((int)roundf(hot_x(i) * k), 0, S - 1), .cursor_hotspot_y = CLAMP((int)roundf(hot_y(i) * k), 0, S - 1),
            .pixels = {px, (size_t)S * S * 4},
        });
        bound[i] = got == c;
        free(px);
    }
}

int cursor_for(int c) {
    static const struct { int ui, slot, fallback; } MAP[] = {
        {SAPP_MOUSECURSOR_DEFAULT, 0, SAPP_MOUSECURSOR_DEFAULT}, {SAPP_MOUSECURSOR_ARROW, 0, SAPP_MOUSECURSOR_ARROW},
        {CUR_SUBSELECT, 1, SAPP_MOUSECURSOR_ARROW}, {CUR_HAND, 2, SAPP_MOUSECURSOR_RESIZE_ALL}, {CUR_GRAB, 3, SAPP_MOUSECURSOR_RESIZE_ALL},
        {SAPP_MOUSECURSOR_POINTING_HAND, 4, SAPP_MOUSECURSOR_POINTING_HAND}, {CUR_ROTATE, 5, SAPP_MOUSECURSOR_POINTING_HAND},
        {SAPP_MOUSECURSOR_RESIZE_ALL, 6, SAPP_MOUSECURSOR_RESIZE_ALL}, {SAPP_MOUSECURSOR_CROSSHAIR, 7, SAPP_MOUSECURSOR_CROSSHAIR},
        {CUR_EYEDROPPER, 8, SAPP_MOUSECURSOR_CROSSHAIR}, {CUR_BUCKET, 9, SAPP_MOUSECURSOR_CROSSHAIR},
        {CUR_ZOOM_IN, 10, SAPP_MOUSECURSOR_CROSSHAIR}, {CUR_ZOOM_OUT, 11, SAPP_MOUSECURSOR_CROSSHAIR},
        {SAPP_MOUSECURSOR_RESIZE_EW, 12, SAPP_MOUSECURSOR_RESIZE_EW}, {SAPP_MOUSECURSOR_RESIZE_NS, 13, SAPP_MOUSECURSOR_RESIZE_NS},
        {SAPP_MOUSECURSOR_RESIZE_NWSE, 14, SAPP_MOUSECURSOR_RESIZE_NWSE}, {SAPP_MOUSECURSOR_RESIZE_NESW, 15, SAPP_MOUSECURSOR_RESIZE_NESW},
    };
    for (int i = 0; i < COUNT(MAP); i++) if (MAP[i].ui == c) return bound[MAP[i].slot] ? SAPP_MOUSECURSOR_CUSTOM_0 + MAP[i].slot : MAP[i].fallback;
    return c < SAPP_MOUSECURSOR_CUSTOM_0 ? c : SAPP_MOUSECURSOR_DEFAULT;
}

bool cursors_sheet(const char *png, float scale) {
    if (!VLINE[1].x && !VLINE[1].y) v_init();
    int S = CLAMP((int)roundf(32 * scale), 24, 128), Z = 4, cell = S * Z + 16, cols = 8;
    int w = cols * cell, h = 2 * cell * 2;
    u8 *img = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) { u8 g = (i / w) < h / 2 ? 0x9a : 0x2a; img[i * 4] = img[i * 4 + 1] = img[i * 4 + 2] = g; img[i * 4 + 3] = 255; }   // light, then dark
    for (int bg = 0; bg < 2; bg++)
        for (int i = 0; i < 16; i++) {
            u8 *px = raster(&DEFS[i], S);
            int ox = (i % cols) * cell + 8, oy = bg * h / 2 + (i / cols) * cell + 8;
            for (int y = 0; y < S * Z; y++)
                for (int x = 0; x < S * Z; x++) {
                    const u8 *s = px + ((size_t)(y / Z) * S + x / Z) * 4;
                    u8 *d = img + ((size_t)(oy + y) * w + ox + x) * 4;
                    for (int c = 0; c < 3; c++) d[c] = (u8)((s[c] * s[3] + d[c] * (255 - s[3])) / 255);
                }
            // the hotspot: a red pixel
            float k = S / 32.0f;
            int hx = (int)roundf(hot_x(i) * k), hy = (int)roundf(hot_y(i) * k);
            for (int y = 0; y < Z; y++) for (int x = 0; x < Z; x++) { u8 *d = img + ((size_t)(oy + hy * Z + y) * w + ox + hx * Z + x) * 4; d[0] = 255; d[1] = 0; d[2] = 0; }
            free(px);
        }
    bool ok = stbi_write_png(png, w, h, 4, img, w * 4) != 0;
    free(img);
    return ok;
}
