// 2D renderer: draw lists of SDF-shaded primitives, uploaded once per frame and drawn on the GPU.
#pragma once
#include "base.h"
#include "sokol/sokol_gfx.h"

typedef struct { float x, y, u, v; Color col; float p[4]; float k[4]; } Vtx;

enum { K_SOLID, K_GLYPH, K_IMAGE, K_RRECT, K_RING, K_SV, K_CAPSULE, K_CHECKER, K_SHADOW, K_STRIPES,   // K_STRIPES: k1 = period in px
       K_GRADIENT,   // uv = gradient space (-1..1); p.x radial, p.y spread (0 pad, 1 reflect, 2 repeat); tex = 256 x 1 ramp
       K_IMAGE_FX,   // image * colour (rgb multipliers, alpha) + p.rgb * its alpha (colour effect offsets)
       K_GRAIN };    // colour with a texture's grain: alpha * lerp(1, the texture's lightness over white, p.x)
// pipeline variant a draw command uses (stage passes use the stencil ones)
// group compositing (premultiplied): MULTIPLY then MULTIPLY2 is an exact multiply in two draws, COPY
// overwrites, COMPOSITE runs the blend shader on tex (the group) and tex2 (a copy of the backdrop)
enum { PIPE_NORMAL, PIPE_STENCIL, PIPE_COVER, PIPE_ERASE, PIPE_MULTIPLY, PIPE_SCREEN, PIPE_ADD, PIPE_STENCIL_EO,
       PIPE_MULTIPLY2, PIPE_SUBTRACT, PIPE_ALPHA, PIPE_ERASE_OUT, PIPE_COPY, PIPE_COMPOSITE, PIPE_FILTER, PIPE_COUNT };
// filter shader passes (PIPE_FILTER; parameters in the command's uniform block, see FX_P)
enum { FXM_BLUR = 1, FXM_ALPHA = 3, FXM_OUTER = 4, FXM_INNER = 5, FXM_ALPHA_INV = 6, FXM_MATRIX = 7, FXM_COPY = 8 };
#define FX_P 36   // floats per filter uniform block: mode sigma step strength | dir off | colour | flags taps | matrix rows, offsets
// blend shader modes (vertex k.x of a PIPE_COMPOSITE quad; k.y = opacity)
enum { CB_NORMAL, CB_DARKEN, CB_LIGHTEN, CB_DIFFERENCE, CB_OVERLAY, CB_HARDLIGHT, CB_INVERT };

enum { SMP_LINEAR, SMP_NEAREST, SMP_MIP, SMP_REPEAT };   // smooth, pixel art, smooth with mipmaps (bitmaps), tiled (bitmap fills)
typedef struct { u32 first, count; Rect clip; u32 tex, tex2; u8 pipe, smp; int ub; } DrawCmd;   // ub: filter uniforms (index into DrawList.ub)

typedef struct DrawList {
    Vtx *v;           // stb_ds arrays
    u32 *idx;
    DrawCmd *cmd;
    Rect *clips;
    u32 tex, tex2;    // current texture view ids (tex2: the backdrop of a PIPE_COMPOSITE draw)
    u8 pipe;          // current pipeline variant
    u8 smp;           // SMP_*
    float *ub; int ubi;   // filter uniform blocks (FX_P floats each), the current one
    float scale;      // logical -> framebuffer pixels
    u32 base_v, base_i; // set by gfx_upload
} DrawList;

typedef struct { sg_pipeline p[PIPE_COUNT]; } PipeSet;

void gfx_init(void);
void gfx_shutdown(void);
PipeSet gfx_make_pipes(int sample_count, sg_pixel_format color, sg_pixel_format depth);
void gfx_free_pipes(PipeSet *ps);
PipeSet *gfx_swapchain_pipes(void);
void gfx_upload(DrawList **lists, int n);
void gfx_draw(DrawList *dl, PipeSet *ps, int fb_w, int fb_h);
sg_sampler gfx_linear_sampler(void);

void dl_reset(DrawList *dl, float scale, Rect full);
void dl_push_clip(DrawList *dl, Rect r);   // logical units, intersected with the current clip
void dl_pop_clip(DrawList *dl);
Rect dl_clip(DrawList *dl);
void dl_set_tex(DrawList *dl, u32 view);
void dl_set_pipe(DrawList *dl, u8 pipe);
void dl_set_tex2(DrawList *dl, u32 view);
void dl_set_sampler(DrawList *dl, u8 smp);

void dl_rect(DrawList *dl, Rect r, Color c);                       // crisp, snapped to pixels
void dl_rrect(DrawList *dl, Rect r, float radius, Color c);        // anti-aliased rounded rect
void dl_rrect_border(DrawList *dl, Rect r, float radius, float width, Color c);
void dl_circle(DrawList *dl, V2 c, float r, Color col);
void dl_circle_border(DrawList *dl, V2 c, float r, float width, Color col);
void dl_line(DrawList *dl, V2 a, V2 b, float width, Color c);      // anti-aliased capsule
void dl_tri(DrawList *dl, V2 a, V2 b, V2 c, Color col);
void dl_image(DrawList *dl, Rect r, u32 view, Color tint, float u0, float v0, float u1, float v1);
void dl_hue_ring(DrawList *dl, V2 c, float r_out, float r_in);
void dl_sv_square(DrawList *dl, Rect r, float radius, float hue);
void dl_checker(DrawList *dl, Rect r, float radius);
void dl_shadow(DrawList *dl, Rect r, float radius, float blur, Color c);   // soft drop shadow
// raw: a quad already in framebuffer pixels (used by the font code and the stage)
void dl_quad_px(DrawList *dl, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c, int kind, float k1);
// raw triangles in framebuffer pixels (stage geometry)
u32 dl_vtx_px(DrawList *dl, float x, float y, Color c);
u32 dl_vtx_k(DrawList *dl, float x, float y, Color c, int kind, float k1);
u32 dl_vtx_uv(DrawList *dl, float x, float y, float u, float v, Color c, int kind);   // textured triangles
u32 dl_vtx_uvp(DrawList *dl, float x, float y, float u, float v, Color c, int kind, const float p[4]);
void dl_filter_quad(DrawList *dl, Rect px, u32 t0, u32 t1, const float P[FX_P]);   // a PIPE_FILTER pass over px
void dl_idx(DrawList *dl, u32 a, u32 b, u32 c);
