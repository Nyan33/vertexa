// Vector brush: input stabilizer, pressure curve, tapers, Flash-like curve snap on release,
// and GPU geometry for the area a convex tip (round / oval / square) sweeps along the stroke.
#pragma once
#include "doc/doc.h"
#include "gfx/render.h"

enum { TAPER_NONE, TAPER_START, TAPER_END, TAPER_BOTH };

// pressure -> size: points sorted by x, the first at x = 0 and the last at x = 1; smooth between them
#define CURVE_MAX 12
typedef struct { int n; float x[CURVE_MAX], y[CURVE_MAX]; } Curve;
float curve_eval(const Curve *c, float p);   // monotone cubic through the points (no overshoot)
bool curve_insert(Curve *c, float x, float y, int *at);
void curve_remove(Curve *c, int i);           // interior points only

typedef struct {
    float size;          // tip diameter in document units
    float smoothing;     // 0..100
    bool pressure, tilt;
    Curve curve;
    u8 tip; float angle, aspect;
    u8 taper;
    Color color;
    u8 erase;
    float unit;          // document units per screen pixel
} BrushOpts;

typedef struct {
    BrushOpts o;
    SPt *pts;
    float sx, sy, sp;
    bool started;
    float lx, ly, lp, lt;
    float k, min_step;
} Builder;


void bb_begin(Builder *b, BrushOpts o);
void bb_add(Builder *b, float x, float y, float pressure, float tilt);
void bb_live(Builder *b, Stroke *out);    // preview of the stroke so far (out->pts is scratch, do not free)
Stroke bb_finish(Builder *b);            // final stroke (owns its points)
void bb_free(Builder *b);

// stencil triangles of the swept area + the cover quad; m maps drawing space to framebuffer pixels
void brush_draw(DrawList *dl, const Stroke *st, Mat m, Color color, bool erase);
void fill_draw(DrawList *dl, const Item *it, Mat m, Color color, bool erase);
// a fill covered with a texture: uvm maps drawing space to texture coordinates (0..1 = one tile)
void fill_draw_tex(DrawList *dl, const Item *it, Mat m, u32 tex, Mat uvm, Color tint, u8 smp, int kind, const float p[4]);
void brush_cover_style(int kind, float k1);
void brush_union_begin(void);                       // following draws only fill the stencil (fills first, then strokes)...
void brush_union_end(DrawList *dl, Color color);    // ...and one cover shades their union   // how the next covers are shaded (K_STRIPES for selection); K_SOLID is the default
SPt *brush_resample(const Stroke *st, float step);   // dense centerline (scratch array, valid until the next call)
