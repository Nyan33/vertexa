// Document model: timelines (scene + symbols), layers, keyframes, drawings and their items.
#pragma once
#include "base.h"

enum { LT_VECTOR, LT_RASTER, LT_CAMERA, LT_AUDIO, LT_FOLDER, LT_MASK, LT_GUIDE, LT_REFERENCE, LT_COUNT };
enum { TW_NONE, TW_CLASSIC, TW_SHAPE };
enum { TIP_ROUND, TIP_OVAL, TIP_SQUARE };
// Animate's blend modes, in its order. Alpha and Erase work inside a group with Layer blending.
enum { BLEND_NORMAL, BLEND_LAYER, BLEND_DARKEN, BLEND_MULTIPLY, BLEND_LIGHTEN, BLEND_SCREEN, BLEND_OVERLAY, BLEND_HARDLIGHT,
       BLEND_ADD, BLEND_SUBTRACT, BLEND_DIFFERENCE, BLEND_INVERT, BLEND_ALPHA, BLEND_ERASE, BLEND_COUNT };
extern const char *BLEND_NAMES[BLEND_COUNT];
enum { IT_STROKE, IT_FILL, IT_INSTANCE, IT_BITMAP };
enum { ITF_LINEAR = 1, ITF_CLOSED = 2 };          // stroke flags: straight segments (shapes), closed outline
enum { SYM_GRAPHIC, SYM_MOVIECLIP };
enum { LOOP_LOOP, LOOP_ONCE, LOOP_SINGLE };

extern const char *LAYER_TYPE_NAMES[LT_COUNT];

typedef struct { float x, y, r; } SPt;

// Animate's filters on a symbol instance, applied in order. Blur sizes are box widths in pixels,
// quality 1..3 is how many times the box runs; strength 1 = 100 %.
enum { FX_BLUR, FX_GLOW, FX_SHADOW, FX_ADJUST, FX_COUNT };
enum { FXF_INNER = 1, FXF_KNOCKOUT = 2, FXF_HIDE = 4, FXF_OFF = 8 };
typedef struct {
    u8 type, quality, flags, pad;
    float bx, by, strength, angle, dist;   // angle in degrees
    Color color;
    float adj[4];                          // adjust colour: brightness, contrast, saturation (-100..100), hue (-180..180)
} Filter;
extern const char *FILTER_NAMES[FX_COUNT];

// colour effect (Animate's Color in Properties): c' = c * mul + add, 0..1 units; kind tells the panel what it is
enum { CX_NONE, CX_BRIGHTNESS, CX_TINT, CX_ADVANCED };
typedef struct { u8 kind; float amount; Color tint; float mul[4], add[4]; } ColorFx;
void colorfx_update(ColorFx *c);   // mul / add from kind, amount and tint (brightness, tint)

// gradient fills: stops by position 0..1; m maps gradient space (-819.2..819.2 across) into the drawing
enum { GR_NONE, GR_LINEAR, GR_RADIAL };
enum { SP_PAD, SP_REFLECT, SP_REPEAT };
typedef struct { float pos; Color c; } GStop;

// One thing in a drawing, in paint order:
//  - stroke: centerline nodes with radius, drawn as the area its tip sweeps
//  - fill: closed contours (even-odd), e.g. from the paint bucket or shape tools
//  - instance: a placed symbol
//  - bitmap: a placed image (sym = bitmap id, m maps its pixels into the drawing)
typedef struct {
    u8 kind, erase, tip, texture, flags, loop;
    u8 blend;                // instance / bitmap blend mode
    Color color;
    float angle, aspect;     // stroke tip
    SPt *pts;                // stroke centerline (stb_ds)
    V2 *poly;                // fill contours, concatenated (stb_ds)
    int *starts;             // fill contour start indices (stb_ds)
    int sym, first;          // instance: symbol id, first frame; bitmap: bitmap id; fill: a bitmap to fill with
    Mat m;                   // instance / bitmap placement; fill: bitmap or gradient matrix
    float alpha;             // instance color effect
    ColorFx cx;              // instance colour effect (brightness, tint, advanced)
    Filter *filters;         // instance filters (stb_ds)
    u8 grad, spread;         // fill: gradient kind, spread
    GStop *stops;            // fill: gradient stops (stb_ds)
    float bx0, by0, bx1, by1;   // bounds in drawing space (strokes and fills)
} Item;

typedef Item Stroke;   // brush code still speaks about strokes

typedef struct { Item *items; bool used; } Drawing;

// d: drawing index, -1 = blank. cam (camera layers): centre x, y, zoom, rotation; zoom 0 = the whole stage
typedef struct { int f; int d; u8 tw; char label[32]; float cam[4]; } Key;

typedef struct {
    int id;
    char name[48];
    u8 type;
    Color color;          // label color
    int parent;           // folder id or 0
    Key *keys;            // sorted by f
    int len;              // last frame with content
    bool hidden, locked, collapsed;
    bool transparent;     // seen through (Animate's Shift-click on the eye)
    u8 see_pct;           // how much: Animate's alphaPercent (0 = the default, 50)
    float opacity;        // 0..1
    u8 blend;
    int sound;            // audio layers: sound id in the session's sound bank (0 = none)
    int sound_frame;      // the frame the sound starts on
} Layer;

typedef struct { Layer *layers; int len, active, frame; } Timeline;
typedef struct { int id; char name[64]; u8 type; Timeline tl; } Symbol;

typedef struct { bool on; int r0, r1, f0, f1; } Sel;  // r = visible row index

// Export Symbols settings, kept with the document. A clip is one animation: a symbol ("sym:<id>") or,
// without symbols, a frame label range of the scene ("label:<name>").
enum { XF_SPARROW, XF_VSLICE, XF_PSYCH };
typedef struct { char key[64]; char anim[48]; char prefix[64]; int fps; bool loop; float off[2]; } ExportClip;
typedef struct { int format; char id[48]; float scale, pos[2]; ExportClip *clips; } ExportSettings;

typedef struct Doc {
    char name[64];
    char path[512];
    int w, h, fps;
    bool loop_on; int loop_a, loop_b;
    bool onion_on; int onion_prev, onion_next;
    Timeline scene;
    Symbol *symbols;
    int edit[8], nedit;   // symbols being edited in place (empty = the scene)
    Mat edit_m[8];        // where each edited symbol sits in its parent's space
    Drawing *drawings;
    int *bitmaps;         // the bitmaps in this document's library (ids in the bitmap bank)
    int next_id;
    Sel sel;
    ExportSettings xp;
    u32 version;          // bumped on every content change
} Doc;

Doc *doc_new(void);
void doc_free(Doc *d);
Doc *doc_clone(const Doc *d);
void doc_touch(Doc *d);

// the timeline being edited (scene or the innermost symbol being edited)
Timeline *doc_tl(Doc *d);
Symbol *doc_symbol(Doc *d, int id);
Timeline *doc_symbol_tl(Doc *d, int id);
int doc_editing(Doc *d);                       // symbol id being edited, 0 = scene
void doc_enter_symbol(Doc *d, int id, Mat m);
Mat doc_edit_matrix(Doc *d);                   // editing space -> scene space
void doc_exit_symbol(Doc *d);                 // one level up
int doc_symbol_len(Doc *d, int id);

Layer *doc_layer(Doc *d, int id);
int doc_layer_index(Doc *d, int id);
Layer *doc_active(Doc *d);
Layer *doc_parent(Doc *d, const Layer *l);
int doc_depth(Doc *d, const Layer *l);
bool doc_hidden(Doc *d, const Layer *l);
bool doc_locked(Doc *d, const Layer *l);
bool doc_drawable(const Layer *l);
int doc_rows(Doc *d, int *out, int cap);       // indices of visible (not collapsed away) layers
int doc_targets(Doc *d, Layer **out, int cap); // layers an edit applies to (selection or active), folders expanded

Key *doc_key_for(Layer *l, int f);             // keyframe whose span covers f
Key *doc_key_at(Layer *l, int f);
int doc_key_index_for(Layer *l, int f);
int doc_span_end(Layer *l, int ki);
int doc_content_end(Doc *d);

int doc_new_drawing(Doc *d);
int doc_clone_drawing(Doc *d, int src);
Drawing *doc_drawing_for_edit(Doc *d, Layer *l, int f);
Drawing *doc_drawing_at(Doc *d, Layer *l, int f);   // NULL when blank / empty frame
void item_free(Item *it);
Item item_copy(const Item *it);
void item_bounds(Item *it);
void stroke_free(Stroke *s);
void stroke_bounds(Stroke *s);

Layer *doc_add_layer(Doc *d, u8 type, const char *name);   // above the active layer (inside its folder)
void doc_delete_layer(Doc *d, int id);
bool doc_delete_symbol(Doc *d, int id);   // false: still used somewhere
void doc_move_frames(Doc *d, Layer **ls, int n, int f0, int f1, int df);
bool doc_move_layers(Doc *d, const int *ids, int n, int ref, int where);   // where: 0 above, 1 into (folder), 2 below
int doc_add_symbol(Doc *d, const char *name, u8 type);     // empty symbol with one layer

void doc_insert_frames(Doc *d, Layer **ls, int n, int f, int count);
void doc_remove_frames(Doc *d, Layer **ls, int n, int f0, int f1);
void doc_insert_key(Doc *d, Layer **ls, int n, int f, bool blank);
void doc_clear_key(Doc *d, Layer **ls, int n, int f);
void doc_set_tween(Doc *d, Layer **ls, int n, int f0, int f1, u8 kind);

// files
bool doc_save(Doc *d, const char *path);
void hist_set_limit(int n);

// camera: the first visible camera layer of the scene frames the output (export, "view through camera")
Layer *doc_camera_layer(Doc *d);
bool doc_camera_at(Doc *d, int frame, float cam[4]);   // false: no camera
void doc_camera_default(const Doc *d, float cam[4]);
Mat doc_camera_matrix(const Doc *d, const float cam[4]);   // stage -> output frame

// sounds live in a bank for the whole session (undo snapshots share them); see audio/audio.c
typedef struct { int id; char name[64]; u8 *data; u32 size; float *pcm; int nframes, rate; float *peaks; int npeaks; } Sound;
#define SOUND_PEAK 256   // samples per waveform peak
int sound_add(const char *name, const u8 *data, u32 size, char *err, int cap);   // decodes; 0 on failure
Sound *sound_get(int id);
Doc *doc_load(const char *path, char *err, int errcap);

// bitmaps live in a bank for the whole session too (doc/bitmap.c); px is premultiplied RGBA
typedef struct {
    int id; char name[64]; int w, h;
    u8 *px;
    u8 *data; u32 size;   // the image file kept for saving (PNG or JPEG); made from px on first use
    bool smooth;          // smoothing when scaled (off: pixel art)
    u32 img, view;        // GPU texture, made by the renderer on first use
} Bitmap;
int bitmap_add_file(const char *name, const u8 *data, u32 size, char *err, int cap);   // PNG, JPEG, BMP, GIF, TGA, PSD
int bitmap_add_pixels(const char *name, int w, int h, u8 *premul_rgba, bool smooth);     // takes the pixels
Bitmap *bitmap_get(int id);
const u8 *bitmap_encoded(Bitmap *b, u32 *size);
int doc_add_bitmap(Doc *d, int id);          // into the library (once); returns id
bool doc_delete_bitmap(Doc *d, int id);      // false: still placed somewhere
static inline bool item_placed(const Item *it) { return it->kind == IT_INSTANCE || it->kind == IT_BITMAP; }

// undo / redo (whole-document snapshots), kept per document tab
typedef struct History { Doc **undo, **redo; } History;
void hist_bind(History *h);   // the steps hist_* work on (NULL: a spare set)
void hist_free(History *h);
void hist_commit(Doc *d);
Doc *hist_undo(Doc *d);   // returns the document to use (or NULL if nothing to undo)
Doc *hist_redo(Doc *d);
void hist_clear(void);
