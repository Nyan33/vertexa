// Immediate-mode UI core: input, ids, hot/active tracking, popups and the common widgets.
#pragma once
#include "gfx/render.h"
#include "gfx/font.h"
#include "ui/theme.h"

typedef u64 UiId;

enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };
enum { MB_LEFT, MB_RIGHT, MB_MIDDLE };

typedef struct {
    V2 mouse, prev;
    bool down[3], pressed[3], released[3];
    bool dbl;              // double click this frame
    float wheel_x, wheel_y;
    u32 mods;
    bool key_down[512], key_pressed[512];
    int chars[32], nchars;
    bool mouse_inside;
} Input;

typedef struct {
    bool hover, pressed, clicked, held, released, dbl, right;
    V2 delta;              // mouse movement while held
} UiResp;

typedef struct {
    Input in;
    float scale, w, h;
    double time, dt;
    UiId hot, active, focus;
    UiId hot_next;
    V2 press_pos;
    DrawList base, over;
    DrawList *dl;
    // rectangles that swallow the mouse (open popups) — from the previous frame
    Rect blockers[8]; int nblockers;
    Rect next_blockers[8]; int next_nblockers;
    bool in_overlay;
    bool in_popup;         // drawing the open popup (it is above everything, modal windows included)
    bool modal;            // a modal window is open: only the overlay layer gets the mouse
    UiId popup;            // currently open popup (menu / dropdown)
    Rect popup_anchor;
    bool popup_opened_now;
    float popup_h;         // measured height of the open popup (from the previous frame)
    int cursor;
    const char *tip; Rect tip_rect; UiId tip_id; double tip_since;
    // text field editing
    char edit[64]; int edit_len;
    // raw mouse positions received this frame (for drawing)
    V2 msamples[256]; int nmsamples;
    bool hide_cursor;
} Ui;

extern Ui ui;

UiId ui_id(const char *s);
UiId ui_idi(const char *s, int i);

void ui_begin(float w, float h, float scale, double dt);
void ui_end(void);
void ui_overlay(bool on);          // draw into the overlay layer (popups, tooltips)
bool ui_mouse_free(void);          // mouse not swallowed by a popup
UiResp ui_interact(UiId id, Rect r);
void ui_tooltip(UiId id, Rect r, const char *text);

// popups
void ui_open_popup(UiId id, Rect anchor);
bool ui_popup_is_open(UiId id);
void ui_close_popup(void);
bool ui_popup_begin(UiId id, Rect r);   // returns true while open; draws the panel
void ui_popup_end(void);
bool ui_menu_item(const char *label, const char *shortcut, bool enabled, bool checked);
bool ui_menu_item_ic(int icon, const char *label, const char *shortcut, bool enabled, bool checked);
void ui_menu_sep(void);
void ui_menu_header(const char *label);

// widgets (styled as in the mockup)
bool ui_chip(const char *label, Rect r, bool on);                  // toggle chip
bool ui_text_button(UiId id, Rect r, const char *label, bool on);   // flat button
float ui_slider(UiId id, Rect r, float v, float lo, float hi);      // compact track; returns new value
int ui_segmented(UiId id, Rect r, const char **items, int n, int sel);
int ui_dropdown(UiId id, Rect r, const char **items, int n, int sel);
bool ui_text_field(UiId id, Rect r, char *buf, int cap, const char *prefix);
void ui_focus_text(UiId id, const char *text);   // start editing a text field from code
// hue ring around a saturation / value square; returns true while the color changes
bool ui_color_wheel(UiId id, V2 center, float ro, float ri, float *h, float *s, float *v);
void color_to_hsv(Color c, float *h, float *s, float *v);   // h is kept for greys
Color color_from_hsv(float h, float s, float v);

// small helpers
float ui_label(Rect r, FontId f, const char *s, Color c, int align);
void ui_cursor(int sapp_cursor);
