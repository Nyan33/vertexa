// Application state shared by the views (stage, timeline, panels) and the tools.
#pragma once
#include "doc/doc.h"
#include "brush/brush.h"
#include "ui/ui.h"
#include "render/scene.h"

typedef struct {
    char name[48];
    float size, smoothing;
    Curve curve;
    u8 tip; float angle, aspect;
    float taper_s, taper_e;       // taper at the start / end, % (100 = nine tip diameters)
    float opacity;                // %
    float fit;                    // 0..100: smoothing of the curve on release
    char texture[48];             // paper grain (app/textures.c), "" = none
    float tex_density, tex_scale; // %
    bool user;                    // the user's (made, duplicated or imported): editable; built-ins are not
} BrushPreset;

// brush textures (app/textures.c): built-in grains and the user's images in %APPDATA%/Vertexa/textures
int texture_count(void);
const char *texture_name(int i);
bool texture_is_user(int i);
int texture_find(const char *name);         // index, -1 if there is none by that name
int texture_bitmap(const char *name);       // its bitmap in the bank (made on first use), 0 if none
bool texture_import(const char *path, char *name, int cap);

enum { TRIM_TO_INTERSECTION, TRIM_WHOLE };
enum { SELM_STROKES, SELM_SECTIONS };            // what a click picks
enum { SELB_CUT, SELB_TOUCHED };                 // what a drag box / lasso picks
enum { SELT_ALL, SELT_LINES, SELT_FILLS };       // which vectors the selection tools see

// keyboard shortcuts (app/keys.c)
typedef struct { u16 key; u8 mods; } KeyChord;   // sokol key code + MOD_* bits; key 0 = unbound
enum {
    ACT_NEW, ACT_OPEN, ACT_SAVE, ACT_SAVE_AS, ACT_EXPORT, ACT_IMPORT_IMAGE, ACT_CLOSE, ACT_NEXT_TAB, ACT_PREV_TAB, ACT_SETTINGS,
    ACT_UNDO, ACT_REDO, ACT_CUT, ACT_COPY, ACT_PASTE, ACT_PASTE_IN_PLACE, ACT_DUPLICATE, ACT_DELETE, ACT_SELECT_ALL,
    ACT_SYMBOL, ACT_BREAK, ACT_SWAP_COLORS, ACT_NUDGE_LEFT, ACT_NUDGE_RIGHT, ACT_NUDGE_UP, ACT_NUDGE_DOWN,
    ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_FIT, ACT_ACTUAL, ACT_ONION,
    ACT_PLAY, ACT_PREV, ACT_NEXT, ACT_FIRST, ACT_LAST, ACT_FRAME, ACT_REMOVE_FRAMES, ACT_KEYFRAME, ACT_BLANK_KEY, ACT_CLEAR_KEY,
    ACT_LOOP, ACT_NEW_LAYER,
    ACT_SMALLER, ACT_BIGGER, ACT_OBJECT_DRAW,
    ACT_TOOL_SELECT, ACT_TOOL_SUBSELECT, ACT_TOOL_TRANSFORM, ACT_TOOL_LASSO, ACT_TOOL_PEN, ACT_TOOL_TEXT, ACT_TOOL_LINE,
    ACT_TOOL_RECT, ACT_TOOL_OVAL, ACT_TOOL_PENCIL, ACT_TOOL_BRUSH, ACT_TOOL_ERASER, ACT_TOOL_TRIM, ACT_TOOL_BUCKET,
    ACT_TOOL_EYEDROPPER, ACT_TOOL_BONE, ACT_TOOL_HAND, ACT_TOOL_ZOOM,
    ACT_SHORTCUTS,
    ACT_COUNT
};
extern KeyChord KEYMAP[ACT_COUNT][2];
void keys_load(void);
bool act(int a);                            // pressed this frame (exact modifiers)
const char *act_keys(int a);                // first binding as text ("Ctrl+S"), NULL if unbound
const char *act_name(int a);
const char *act_group(int a);
const char *chord_text(KeyChord c, char *buf, int cap);
bool chord_eq(KeyChord a, KeyChord b);
bool act_is_default(int a);
int keys_bind(int a, int slot, KeyChord c);    // returns the action that lost this chord, or -1
void keys_reset(int a);                     // -1: all

enum { SET_GENERAL, SET_PERFORMANCE, SET_KEYS, SET_LAYOUT, SET_THEME, SET_COUNT };

// where the window's parts go (Settings > Layout; profiles in %APPDATA%/Vertexa/layouts.txt)
typedef struct {
    char name[48];
    bool panel_left;              // colors and the Brushes / Library / Properties panel on the left
    bool tools_right;             // the tools column on the right of the stage
    bool timeline_top;            // the timeline above the stage (Animate's Classic workspace)
    bool color_below;             // the color wheel under the panel's tabs instead of above them
    float panel_w, tl_h;
} Layout;

// an open document (a tab in the title bar)
typedef struct DocTab {
    Doc *doc; u32 saved_version, rec_version;
    int uid;                      // stays with the tab when tabs are reordered
    History hist;                 // its undo steps
    float view[3]; bool fitted;   // its stage zoom and offset
} DocTab;

typedef struct {
    Doc *doc;                     // the document shown (the current tab's)
    DocTab **tabs; int tab;       // open documents; tab = the one shown, -1 = none (start screen)
    char tool;                    // 'V', 'B', 'C', ...
    // brush
    BrushPreset *presets;         // built-ins first, then the user's
    int preset;
    float brush_size, brush_smooth;
    bool brush_pressure, brush_tilt, object_draw;
    int brush_mode;               // 0 paint normal, 2 paint behind
    u8 tip; float tip_angle, tip_aspect;
    float taper_s, taper_e, brush_opacity, brush_fit;
    char texture[48]; float tex_density, tex_scale;
    Curve curve;
    float eraser_size; int eraser_mode;
    float pencil_size; int pencil_mode;
    // trim edge
    int trim_mode; bool trim_all_layers;
    // selection
    int sel_mode, sel_box, sel_target;
    // shapes and fills
    float shape_width; bool shape_fill, shape_line;
    int bucket_gap;               // 0 none, 1 small, 2 medium, 3 large
    // color
    Color fill, stroke;
    float alpha;
    int color_target;             // 0 = fill, 1 = stroke
    float hue, sat, val;
    Color swatches[40]; int nswatches;
    // playback
    bool playing; double play_t;
    // layout
    Layout layout;
    int right_tab;
    float tl_h;                   // the timeline's height (the layout keeps it too)
    // feedback
    char toast[192]; double toast_until;
    bool settings;                // the Settings window is open
    bool home;                    // the start screen is showing
    // preferences (Settings; %APPDATA%/Vertexa/settings.txt)
    int autosave_min;             // 0 = off; saves documents that already have a file
    int recovery_min;             // 0 = off; spare copy in %APPDATA%/Vertexa/recovery
    int undo_steps;
    float ui_zoom;                // interface size on top of the system DPI scale
    bool pen_eraser_tool;         // the pen's eraser end uses the current tool instead of erasing
    bool size_fixed;              // brush / pencil / eraser sizes in screen pixels instead of document pixels
    bool gpu_low;                 // low memory: no stage anti-aliasing, 75 % stage resolution
    int msaa;                     // stage anti-aliasing samples (1, 2, 4, 8)
    int stage_res;                // stage resolution, % of the display
    bool stats;                   // performance overlay on the stage
    u32 saved_version;
} App;

extern App app;

void app_init(void);
void app_frame(void);
void app_toast(const char *fmt, ...);
void app_commit(void);
void app_undo(void);
void app_redo(void);
void app_play(bool on);
void app_step(int df);
Color app_color(void);
void app_set_color(Color c);
void app_apply_preset(int i);
void app_add_preset(const BrushPreset *p);   // appends a user preset and saves the list
int app_insert_preset(int at, const BrushPreset *p);   // a user preset at index at; returns it
void app_remove_preset(int i);
void app_move_preset(int from, int to);
void app_presets_save(void);                // %APPDATA%/Vertexa/brushes.txt, the list in its order
void app_export_preset(int i);              // save dialog, .vxbrush
void app_brush_name(char *out, int cap, const char *base);   // base, or base 2, 3, ... when taken
void app_import_brushes(void);
void brush_new(void);                       // a user brush from the current settings, opened for editing
bool brush_editing(void);                   // the Brushes tab shows a brush's settings
void settings_load(void);                   // theme from %APPDATA%/Vertexa/themes.txt
void settings_window(void);                 // modal Settings window (Edit > Settings, Ctrl+,)
void settings_open(int page);
void layout_tick(void);                     // the layout in use is saved once a drag of its edges ends
void prefs_changed(void);                   // apply and save the preferences
void recovery_cleanup(void);
void app_autosave_due(void);                // self-tests: the auto-save and recovery timers fire now                // on a normal exit: this session's recovery copy is not needed
void stage_stats(float *cpu_ms, int *tris, int *samples, float *res);
float st_size_unit(void);                   // drawing units per brush-size pixel (document or screen pixels)              // file dialog for .sut
const char *tool_name(char t);
IconId tool_icon(char t);
bool app_tool_paints(char t);
void app_set_doc(Doc *d);                 // shows a document in a new tab (or the untouched one)
void app_switch_tab(int i);
void app_close_tab(int i);                 // asks first when it has unsaved changes
bool app_tab_modified(int i);
bool app_quit_requested(void);             // false: something is unsaved, the app asks and quits after
bool app_open_path(const char *path);     // .vxa, .fla, .xfl, .sut, sounds, images
bool app_is_image(const char *path);
bool app_import_image(const char *path, V2 screen);   // into the Library and onto the stage (x < 0: its middle)
bool app_save(bool save_as);
void app_new(void);

// views
void stage_init(void);
void stage_ui(Rect r);
int stage_lists(DrawList **out, int cap);   // draw lists to upload this frame
void stage_render(void);                    // offscreen passes (after upload)
void stage_fit(void);
void stage_zoom_by(float k);
void stage_zoom_to(float z, V2 scene);
void stage_pan(float dx, float dy);
bool *stage_camera_view(void);              // looking through the camera (zoom pill toggle)
void stage_zoom_100(void);
void stage_view_get(float v[3], bool *fitted);
void stage_view_set(const float v[3], bool fitted);   // v[0] == 0: fit
void stage_demo(void);
Rect stage_rect(void);
V2 stage_to_screen(V2 doc_point);
void timeline_ui(Rect r);
void timeline_shortcuts(void);
void timeline_op(int which);                // 0 frame, 1 remove frames, 2 key, 3 blank key, 4 clear key, 5/6/7 classic/shape/no tween
void context_bar(Rect r);
void color_panel(Rect r);
void right_panel(Rect r);

// editing space helpers (stage.c): the current timeline's space (scene, or the symbol being edited)
V2 st_local(V2 screen);
V2 st_screen(V2 local);
float st_unit(void);                       // editing units per screen pixel
Drawing *st_drawing(bool create);          // active layer's drawing at the current frame
const char *st_cannot_edit(void);          // why the active layer can't be edited (NULL if it can)
void st_invalidate(void);                  // force a stage re-render

// tools
void trim_tool(UiResp *re);
void trim_overlay(void);
#include "geom/geom.h"
float cut_step(void);                                                        // centerline sampling used for cuts
bool stroke_section_at(const Path *P, const Item *it, V2 p, float *a, float *b);   // part around p between intersections
Item stroke_piece(const Path *P, const Item *it, float a, float b);         // part between arc lengths (wraps when closed)
void select_tool(UiResp *re, char tool);    // V, A, Q, L
void select_overlay(void);
void select_render(DrawList *dl, Mat m);    // hatch over the selected vectors (stage image)
u32 select_signature(void);                 // changes whenever that hatch would
bool select_info(float b[4], V2 *pivot);    // selection bounds and transformation point (self-tests)
void select_shortcuts(void);
void select_clear(void);
int select_count(void);
Item *select_first(void);
void select_all(void);
void select_delete(void);
bool select_recolor(Color c, int target, bool commit);   // the color panel changed (target 0 fill, 1 stroke)
void select_copy(bool cut);
void select_paste(bool in_place);
void select_convert_to_symbol(void);
void select_break_apart(void);
void shape_tool(UiResp *re, char tool);     // N, R, O
bool shape_preview(Item **out, int *n);      // items to draw live while dragging
void bucket_click(V2 local);
void library_place(int sym, V2 screen);      // drop a symbol instance on the stage (x < 0: its middle)
void bitmap_place(int id, V2 screen);        // a bitmap from the Library, the same way
void symbol_edit(int sym);                   // edit a symbol from the library

// export / import
bool export_png_sequence(const char *dir, char *msg, int cap);
// clips: one animation each - a symbol, or a frame label range of the scene (export.c)
typedef struct { int sym, f0, f1; char name[64], key[64]; } ClipSrc;
int export_clip_sources(Doc *d, const int *syms, int nsyms, ClipSrc *out, int cap);
ExportClip *export_clip_settings(Doc *d, const char *key, const char *name);
int export_clip_len(const ClipSrc *c);
bool export_clip_bounds(Doc *d, const ClipSrc *cs, int n, float b[4]);
void export_render_clip(Frame *F, Doc *d, const ClipSrc *c, int frame, Mat m, float alpha);
bool export_clips(const ClipSrc *cs, int n, int format, const char *dest, char *msg, int cap);
// the Export Symbols window (exporter.c)
void exporter_open(const int *syms, int n);
bool exporter_is_open(void);
void exporter_window(void);
int exporter_lists(DrawList **out, int cap);
void exporter_render(void);
ClipSrc *exporter_clips(int *n);
void exporter_select(int i);
void exporter_nudge(float dx, float dy, bool whole);
int library_selection(int *out, int cap);   // symbols picked in the Library, in the order picked
void library_forget(void);                  // another document is shown
int export_lists(DrawList **out, int cap);   // offscreen frame of a running export
void export_render(void);
void export_after_render(void);
bool export_busy(void);
bool export_thumbnail(const char *png);
// start screen (app/home.c)
void home_frame(Rect r);                    // recent animations, a new one
void home_prepare(void);                    // reads the recent list and its thumbnails
void home_thumb_ready(const char *png);
void recent_add(const char *path);          // an opened or saved document goes to the top of the list
void splash_draw(float alpha);
Doc *import_xfl(const char *path, char *err, int cap);
bool import_sut(const char *path, char *msg, int cap);

// platform dialogs (platform/win32.c)
bool plat_open_dialog(char *out, int cap, const char *filter_desc, const char *patterns);
bool plat_save_dialog(char *out, int cap, const char *filter_desc, const char *ext, const char *default_name);
bool plat_pick_folder(char *out, int cap, const char *title);
void plat_set_title(const char *title);
