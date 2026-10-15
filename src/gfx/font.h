// Text: Inter + JetBrains Mono rasterized into one atlas at the current DPI scale.
#pragma once
#include "gfx/render.h"
#include "gfx/icons.h"

typedef enum {
    F_UI,         // Inter 12 regular
    F_UI_MED,     // Inter 12 medium
    F_UI_SEMI,    // Inter 12 semibold
    F_SMALL,      // Inter 11 regular
    F_SMALL_SEMI, // Inter 10 semibold (ruler seconds)
    F_TOOL,       // Inter 12.5 semibold (tool letters)
    F_TITLE,      // Inter 13 semibold
    F_MONO,       // JetBrains Mono 10 regular
    F_MONO_MED,   // JetBrains Mono 11 regular
    F_MONO_SEMI,  // JetBrains Mono 10 semibold
    F_H2,         // Inter 16 semibold (start screen headings)
    F_DISPLAY,    // Inter 28 semibold (splash, start screen)
    F_COUNT
} FontId;

enum { ALIGN_LEFT = -1, ALIGN_CENTER = 0, ALIGN_RIGHT = 1 };

void font_init(float scale);
void font_shutdown(void);
u32 font_view(void);
float font_width(FontId f, const char *s);
float font_cap_height(FontId f);
// baseline-anchored; returns the advance
float font_draw(DrawList *dl, FontId f, float x, float baseline, const char *s, Color c);
// vertically centred on cap height inside r, aligned horizontally
void font_draw_in(DrawList *dl, FontId f, Rect r, int align, const char *s, Color c);
int utf8_next(const char **s);
bool font_covers(const char *s);   // every code point has a glyph in the UI fonts

// icons (Phosphor): sizes 14, 16 and 20 px, outline or filled
enum { ICON_S = 14, ICON_M = 16, ICON_L = 20 };
void icon_draw(DrawList *dl, IconId ic, V2 center, int size, bool fill, Color c);
