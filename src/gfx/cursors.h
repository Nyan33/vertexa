// Mouse cursors: Vertexa's own set (the V arrow, Mac-like hands, tool cursors), drawn at the display's
// scale and bound to sokol's custom cursor slots.
#pragma once
#include "base.h"

// cursors the UI asks for besides sokol's own SAPP_MOUSECURSOR_* (ui_cursor takes either)
enum { CUR_HAND = 100, CUR_GRAB, CUR_EYEDROPPER, CUR_BUCKET, CUR_ZOOM_IN, CUR_ZOOM_OUT, CUR_ROTATE, CUR_SUBSELECT };

void cursors_init(float scale);        // (re)draws the set for this scale and binds it
int cursor_for(int ui_cursor);         // the sapp_mouse_cursor that shows a UI cursor
bool cursors_sheet(const char *png, float scale);   // every cursor on a grey sheet, enlarged (self-tests)
