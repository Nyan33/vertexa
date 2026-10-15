// Renders documents (timelines, drawings, symbols, tweens) into draw lists split into GPU passes.
// Shared by the stage and the exporters.
#pragma once
#include "doc/doc.h"
#include "gfx/render.h"

// MSAA color + resolve + depth-stencil, with the views a pass needs
typedef struct {
    sg_image msaa, res, ds;
    sg_view v_c, v_res, v_ds, v_tex;
    int w, h, samples;
} Target;

// surfaces for groups, one per nesting level, plus a copy of the backdrop for the blend shader;
// made on demand at the size of the frame
#define GROUP_MAX 8
typedef struct { Target t[GROUP_MAX]; Target back, tmp; } Pool;   // back, tmp: one sample (backdrop copies, filter passes)

// A frame is a list of segments, each one GPU pass: drawing into a level (0 = the target, 1.. = groups)
// or copying a level into the backdrop texture.
enum { SEG_DRAW, SEG_COPY, SEG_TMP };   // into a level / into the backdrop texture / into the filter scratch
typedef struct { int kind, level; bool clear, resolve; DrawList dl; } Seg;

typedef struct {
    Seg **segs; int nsegs;     // kept between frames, so the draw lists keep their memory
    int w, h;                  // pixels
    u32 tex;                   // a valid texture view for bindings that sample nothing
    Target *main; Pool *pool;
    int level;                 // the surface drawn into now
    int last_at[GROUP_MAX + 1];   // the latest segment drawing into each level
} Frame;

void target_make(Target *t, int w, int h, int samples);
void target_free(Target *t);
void pool_free(Pool *p);

void frame_begin(Frame *F, Target *main, Pool *pool, u32 any_tex);
DrawList *frame_dl(Frame *F);                       // the list currently drawn into
bool frame_push(Frame *F);                          // start an isolated group (false: nested too deep)
void frame_pop(Frame *F, u8 blend, float opacity);  // end it and blend it into what is below
void frame_filter(Frame *F, const Filter *f, float scale, Rect px);   // on the open group, over px (pixels)
void scene_frame_start(void);                       // once per app frame, before any frame is built
int frame_lists(Frame *F, DrawList **out, int cap);
void frame_passes(Frame *F, PipeSet *ps, const float clear[4]);

// document rendering
void render_timeline(Frame *F, Doc *d, Timeline *t, int frame, Mat m, float alpha, Color tint, int depth, bool export_mode);
void render_items(Frame *F, Doc *d, Drawing *dr, int off, Mat m, float alpha, Color tint, int depth);
int symbol_frame(Doc *d, const Item *inst, int off);
bool drawing_bounds(Doc *d, Drawing *dr, int off, Mat m, float b[4], int depth);   // b: x0, y0, x1, y1 (grown)
bool item_bounds_m(Doc *d, const Item *it, int off, Mat m, float b[4], int depth);
Layer *tl_layer(Timeline *t, int id);
bool tl_hidden(Timeline *t, const Layer *l);
u32 bitmap_texture(int id);
// a stroke; one drawn with image stamps gets a group of its own where the stamps keep the larger value
// instead of piling up, so a stroke is as opaque as its colour however densely it is stamped (as in CSP)
void render_stroke(Frame *F, const Item *it, Mat m, Color c);   // the bitmap's texture view (made on first use), 0 if there is no such bitmap
