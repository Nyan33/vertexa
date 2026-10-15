// The Vertexa logo: title bar texture and window / taskbar icon.
#pragma once
#include "base.h"
#include "sokol/sokol_gfx.h"

u32 logo_view(int px);        // premultiplied texture view, px x px (rebuilt when the size changes); 0 if missing
void logo_window_icon(void);  // sets the window and taskbar icon
u32 logo_mark_view(int h);    // the V with its anchors alone (in-app), h px tall, premultiplied
int logo_mark_width(int h);
