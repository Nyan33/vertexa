// Common types and helpers used everywhere.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "stb/stb_ds.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

#define VX_PI 3.14159265358979323846f
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct { float x, y; } V2;
typedef struct { float x, y, w, h; } Rect;

static inline V2 v2(float x, float y) { return (V2){x, y}; }
static inline Rect rect(float x, float y, float w, float h) { return (Rect){x, y, w, h}; }
static inline bool rect_has(Rect r, V2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }
static inline Rect rect_inset(Rect r, float dx, float dy) { return (Rect){r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy}; }
static inline Rect rect_isect(Rect a, Rect b) {
    float x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y), x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    return (Rect){x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0)};
}
// RectCut layout: slice a side off a rectangle and return it
static inline Rect cut_left(Rect *r, float a) { Rect o = {r->x, r->y, a, r->h}; r->x += a; r->w -= a; return o; }
static inline Rect cut_right(Rect *r, float a) { Rect o = {r->x + r->w - a, r->y, a, r->h}; r->w -= a; return o; }
static inline Rect cut_top(Rect *r, float a) { Rect o = {r->x, r->y, r->w, a}; r->y += a; r->h -= a; return o; }
static inline Rect cut_bottom(Rect *r, float a) { Rect o = {r->x, r->y + r->h - a, r->w, a}; r->h -= a; return o; }

// colors are 0xAABBGGRR (byte order R,G,B,A in memory) to match the vertex format
typedef u32 Color;
static inline Color rgba(u8 r, u8 g, u8 b, u8 a) { return (Color)r | ((Color)g << 8) | ((Color)b << 16) | ((Color)a << 24); }
static inline Color hex(u32 rgb) { return rgba((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255); }
static inline Color hexa(u32 rgb, u8 a) { return rgba((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
static inline Color col_alpha(Color c, float a) { u32 al = (u32)((c >> 24) * CLAMP(a, 0.f, 1.f) + 0.5f); return (c & 0x00ffffffu) | (al << 24); }
static inline Color col_mix(Color a, Color b, float t) {   // per channel, alpha included
    Color o = 0;
    for (int k = 0; k < 32; k += 8) { float x = ((a >> k) & 255) + (((float)((b >> k) & 255)) - ((a >> k) & 255)) * t; o |= (Color)(x < 0 ? 0 : x > 255 ? 255 : (int)(x + 0.5f)) << k; }
    return o;
}
static inline Color col_a(Color c, u8 a) { return (c & 0x00ffffffu) | ((Color)a << 24); }
static inline u32 col_rgb(Color c) { return ((c & 255) << 16) | (((c >> 8) & 255) << 8) | ((c >> 16) & 255); }

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// 2D affine: x' = a*x + c*y + tx, y' = b*x + d*y + ty
typedef struct { float a, b, c, d, tx, ty; } Mat;
static inline Mat mat_id(void) { return (Mat){1, 0, 0, 1, 0, 0}; }
static inline Mat mat_tr(float x, float y) { return (Mat){1, 0, 0, 1, x, y}; }
static inline Mat mat_scale(float sx, float sy) { return (Mat){sx, 0, 0, sy, 0, 0}; }
static inline Mat mat_rot(float r) { float c = cosf(r), s = sinf(r); return (Mat){c, s, -s, c, 0, 0}; }
static inline Mat mat_mul(Mat m, Mat n) {   // apply n, then m
    return (Mat){m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                 m.a * n.tx + m.c * n.ty + m.tx, m.b * n.tx + m.d * n.ty + m.ty};
}
static inline V2 mat_ap(Mat m, V2 p) { return (V2){m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty}; }
static inline V2 mat_ap_vec(Mat m, V2 p) { return (V2){m.a * p.x + m.c * p.y, m.b * p.x + m.d * p.y}; }
static inline float mat_det(Mat m) { return m.a * m.d - m.b * m.c; }
static inline float mat_scalef(Mat m) { return sqrtf(fabsf(mat_det(m))); }
static inline Mat mat_inv(Mat m) {
    float det = mat_det(m);
    if (fabsf(det) < 1e-12f) return mat_id();
    float id = 1 / det;
    Mat r = {m.d * id, -m.b * id, -m.c * id, m.a * id, 0, 0};
    r.tx = -(r.a * m.tx + r.c * m.ty); r.ty = -(r.b * m.tx + r.d * m.ty);
    return r;
}
static inline bool mat_eq(Mat m, Mat n) { return !memcmp(&m, &n, sizeof(Mat)); }

void vx_log(const char *fmt, ...);
