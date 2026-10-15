// Keyboard shortcuts: every action has up to two bindings, defaults below, the user's changes in
// %APPDATA%/Vertexa/keys.txt (one "action=key:mods key:mods" line per changed action).
#include "app/app.h"
#include "platform/platform.h"
#include "sokol/sokol_app.h"

enum { C = MOD_CTRL, S = MOD_SHIFT, A = MOD_ALT };
#define K(k) SAPP_KEYCODE_##k

typedef struct { const char *id, *name, *group; KeyChord def[2]; bool shift_free; } ActionDef;

static const ActionDef ACTS[ACT_COUNT] = {
    [ACT_NEW] = {"new", "New document", "File", {{K(N), C}}},
    [ACT_OPEN] = {"open", "Open", "File", {{K(O), C}}},
    [ACT_SAVE] = {"save", "Save", "File", {{K(S), C}}},
    [ACT_SAVE_AS] = {"save_as", "Save as", "File", {{K(S), C | S}}},
    [ACT_EXPORT] = {"export_symbols", "Export symbols", "File", {{K(E), C | S}}},
    [ACT_IMPORT_IMAGE] = {"import_image", "Import image", "File", {{K(R), C}}},
    [ACT_CLOSE] = {"close", "Close document", "File", {{K(W), C}, {K(F4), C}}},
    [ACT_NEXT_TAB] = {"next_tab", "Next document", "File", {{K(TAB), C}}},
    [ACT_PREV_TAB] = {"prev_tab", "Previous document", "File", {{K(TAB), C | S}}},
    [ACT_SETTINGS] = {"settings", "Settings", "File", {{K(COMMA), C}}},
    [ACT_UNDO] = {"undo", "Undo", "Edit", {{K(Z), C}}},
    [ACT_REDO] = {"redo", "Redo", "Edit", {{K(Z), C | S}, {K(Y), C}}},
    [ACT_CUT] = {"cut", "Cut", "Edit", {{K(X), C}}},
    [ACT_COPY] = {"copy", "Copy", "Edit", {{K(C), C}}},
    [ACT_PASTE] = {"paste", "Paste", "Edit", {{K(V), C}}},
    [ACT_PASTE_IN_PLACE] = {"paste_in_place", "Paste in place", "Edit", {{K(V), C | S}}},
    [ACT_DUPLICATE] = {"duplicate", "Duplicate", "Edit", {{K(D), C}}},
    [ACT_DELETE] = {"delete", "Delete", "Edit", {{K(DELETE), 0}, {K(BACKSPACE), 0}}},
    [ACT_SELECT_ALL] = {"select_all", "Select all", "Edit", {{K(A), C}}},
    [ACT_SYMBOL] = {"symbol", "Convert to symbol", "Modify", {{K(F8), 0}}},
    [ACT_BREAK] = {"break_apart", "Break apart", "Modify", {{K(B), C}}},
    [ACT_SWAP_COLORS] = {"swap_colors", "Swap fill and stroke", "Modify", {{K(X), 0}}},
    [ACT_NUDGE_LEFT] = {"nudge_left", "Nudge left (Shift: 10 px)", "Modify", {{K(LEFT), 0}}, true},
    [ACT_NUDGE_RIGHT] = {"nudge_right", "Nudge right", "Modify", {{K(RIGHT), 0}}, true},
    [ACT_NUDGE_UP] = {"nudge_up", "Nudge up", "Modify", {{K(UP), 0}}, true},
    [ACT_NUDGE_DOWN] = {"nudge_down", "Nudge down", "Modify", {{K(DOWN), 0}}, true},
    [ACT_ZOOM_IN] = {"zoom_in", "Zoom in", "View", {{K(EQUAL), C}, {K(KP_ADD), C}}},
    [ACT_ZOOM_OUT] = {"zoom_out", "Zoom out", "View", {{K(MINUS), C}, {K(KP_SUBTRACT), C}}},
    [ACT_FIT] = {"fit", "Fit to window", "View", {{K(0), C}}},
    [ACT_ACTUAL] = {"actual_size", "Actual size", "View", {{K(1), C}}},
    [ACT_ONION] = {"onion", "Onion skin", "View", {{K(O), A | S}}},
    [ACT_PLAY] = {"play", "Play / stop", "Timeline", {{K(ENTER), 0}, {K(KP_ENTER), 0}}},
    [ACT_PREV] = {"prev_frame", "Previous frame", "Timeline", {{K(COMMA), 0}}},
    [ACT_NEXT] = {"next_frame", "Next frame", "Timeline", {{K(PERIOD), 0}}},
    [ACT_FIRST] = {"first_frame", "First frame", "Timeline", {{K(HOME), 0}}},
    [ACT_LAST] = {"last_frame", "Last frame", "Timeline", {{K(END), 0}}},
    [ACT_FRAME] = {"insert_frame", "Insert frame", "Timeline", {{K(F5), 0}}},
    [ACT_REMOVE_FRAMES] = {"remove_frames", "Remove frames", "Timeline", {{K(F5), S}}},
    [ACT_KEYFRAME] = {"keyframe", "Keyframe", "Timeline", {{K(F6), 0}}},
    [ACT_BLANK_KEY] = {"blank_keyframe", "Blank keyframe", "Timeline", {{K(F7), 0}}},
    [ACT_CLEAR_KEY] = {"clear_keyframe", "Clear keyframe", "Timeline", {{K(F6), S}}},
    [ACT_LOOP] = {"loop", "Loop playback", "Timeline", {{K(L), A | S}}},
    [ACT_NEW_LAYER] = {"new_layer", "New layer", "Timeline", {{K(N), C | S}}},
    [ACT_SMALLER] = {"smaller", "Smaller brush", "Drawing", {{K(LEFT_BRACKET), 0}}},
    [ACT_BIGGER] = {"bigger", "Bigger brush", "Drawing", {{K(RIGHT_BRACKET), 0}}},
    [ACT_OBJECT_DRAW] = {"object_drawing", "Object drawing", "Drawing", {{K(J), 0}}},
    [ACT_TOOL_SELECT] = {"tool_select", "Selection", "Tools", {{K(V), 0}}},
    [ACT_TOOL_SUBSELECT] = {"tool_subselect", "Subselection", "Tools", {{K(A), 0}}},
    [ACT_TOOL_TRANSFORM] = {"tool_transform", "Free transform", "Tools", {{K(Q), 0}}},
    [ACT_TOOL_LASSO] = {"tool_lasso", "Lasso", "Tools", {{K(L), 0}}},
    [ACT_TOOL_PEN] = {"tool_pen", "Pen", "Tools", {{K(P), 0}}},
    [ACT_TOOL_TEXT] = {"tool_text", "Text", "Tools", {{K(T), 0}}},
    [ACT_TOOL_LINE] = {"tool_line", "Line", "Tools", {{K(N), 0}}},
    [ACT_TOOL_RECT] = {"tool_rect", "Rectangle", "Tools", {{K(R), 0}}},
    [ACT_TOOL_OVAL] = {"tool_oval", "Oval", "Tools", {{K(O), 0}}},
    [ACT_TOOL_PENCIL] = {"tool_pencil", "Pencil", "Tools", {{K(Y), 0}}},
    [ACT_TOOL_BRUSH] = {"tool_brush", "Brush", "Tools", {{K(B), 0}}},
    [ACT_TOOL_ERASER] = {"tool_eraser", "Eraser", "Tools", {{K(E), 0}}},
    [ACT_TOOL_TRIM] = {"tool_trim", "Trim edge", "Tools", {{K(C), 0}}},
    [ACT_TOOL_BUCKET] = {"tool_bucket", "Paint bucket", "Tools", {{K(K), 0}}},
    [ACT_TOOL_EYEDROPPER] = {"tool_eyedropper", "Eyedropper", "Tools", {{K(I), 0}}},
    [ACT_TOOL_BONE] = {"tool_bone", "Bone", "Tools", {{K(M), 0}}},
    [ACT_TOOL_HAND] = {"tool_hand", "Hand", "Tools", {{K(H), 0}}},
    [ACT_TOOL_ZOOM] = {"tool_zoom", "Zoom", "Tools", {{K(Z), 0}}},
    [ACT_SHORTCUTS] = {"shortcuts", "Keyboard shortcuts", "Help", {{K(SLASH), S}}},
};

KeyChord KEYMAP[ACT_COUNT][2];

const char *act_name(int a) { return ACTS[a].name; }
const char *act_group(int a) { return ACTS[a].group; }

bool act(int a) {
    u32 mods = ui.in.mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT);
    for (int i = 0; i < 2; i++) {
        KeyChord c = KEYMAP[a][i];
        if (!c.key || !ui.in.key_pressed[c.key]) continue;
        if (mods == c.mods || (ACTS[a].shift_free && (mods & ~MOD_SHIFT) == c.mods)) return true;
    }
    return false;
}

static const char *key_name(int k, char *tmp) {
    if (k >= SAPP_KEYCODE_A && k <= SAPP_KEYCODE_Z) { tmp[0] = (char)('A' + k - SAPP_KEYCODE_A); tmp[1] = 0; return tmp; }
    if (k >= SAPP_KEYCODE_0 && k <= SAPP_KEYCODE_9) { tmp[0] = (char)('0' + k - SAPP_KEYCODE_0); tmp[1] = 0; return tmp; }
    if (k >= SAPP_KEYCODE_F1 && k <= SAPP_KEYCODE_F12) { sprintf(tmp, "F%d", k - SAPP_KEYCODE_F1 + 1); return tmp; }
    if (k >= SAPP_KEYCODE_KP_0 && k <= SAPP_KEYCODE_KP_9) { sprintf(tmp, "Num %d", k - SAPP_KEYCODE_KP_0); return tmp; }
    switch (k) {
    case SAPP_KEYCODE_SPACE: return "Space";
    case SAPP_KEYCODE_ENTER: return "Enter";
    case SAPP_KEYCODE_KP_ENTER: return "Num Enter";
    case SAPP_KEYCODE_TAB: return "Tab";
    case SAPP_KEYCODE_BACKSPACE: return "Backspace";
    case SAPP_KEYCODE_DELETE: return "Del";
    case SAPP_KEYCODE_INSERT: return "Ins";
    case SAPP_KEYCODE_HOME: return "Home";
    case SAPP_KEYCODE_END: return "End";
    case SAPP_KEYCODE_PAGE_UP: return "PgUp";
    case SAPP_KEYCODE_PAGE_DOWN: return "PgDn";
    case SAPP_KEYCODE_LEFT: return "\xE2\x86\x90";
    case SAPP_KEYCODE_RIGHT: return "\xE2\x86\x92";
    case SAPP_KEYCODE_UP: return "\xE2\x86\x91";
    case SAPP_KEYCODE_DOWN: return "\xE2\x86\x93";
    case SAPP_KEYCODE_COMMA: return ",";
    case SAPP_KEYCODE_PERIOD: return ".";
    case SAPP_KEYCODE_SLASH: return "/";
    case SAPP_KEYCODE_BACKSLASH: return "\\";
    case SAPP_KEYCODE_SEMICOLON: return ";";
    case SAPP_KEYCODE_APOSTROPHE: return "'";
    case SAPP_KEYCODE_GRAVE_ACCENT: return "`";
    case SAPP_KEYCODE_MINUS: return "-";
    case SAPP_KEYCODE_EQUAL: return "=";
    case SAPP_KEYCODE_LEFT_BRACKET: return "[";
    case SAPP_KEYCODE_RIGHT_BRACKET: return "]";
    case SAPP_KEYCODE_KP_ADD: return "Num +";
    case SAPP_KEYCODE_KP_SUBTRACT: return "Num -";
    case SAPP_KEYCODE_KP_MULTIPLY: return "Num *";
    case SAPP_KEYCODE_KP_DIVIDE: return "Num /";
    case SAPP_KEYCODE_KP_DECIMAL: return "Num .";
    case SAPP_KEYCODE_ESCAPE: return "Esc";
    }
    sprintf(tmp, "Key %d", k);
    return tmp;
}

const char *chord_text(KeyChord c, char *buf, int cap) {
    if (!c.key) { snprintf(buf, (size_t)cap, "%s", ""); return buf; }
    char tmp[16];
    if (c.key == SAPP_KEYCODE_SLASH && c.mods == MOD_SHIFT) { snprintf(buf, (size_t)cap, "?"); return buf; }
    snprintf(buf, (size_t)cap, "%s%s%s%s", c.mods & MOD_CTRL ? "Ctrl+" : "", c.mods & MOD_SHIFT ? "Shift+" : "", c.mods & MOD_ALT ? "Alt+" : "", key_name(c.key, tmp));
    return buf;
}

// the first binding as text, for menus and tooltips (a few rotating buffers, valid for this frame)
const char *act_keys(int a) {
    static char ring[8][32];
    static int next;
    char *b = ring[next++ & 7];
    KeyChord c = KEYMAP[a][0].key ? KEYMAP[a][0] : KEYMAP[a][1];
    chord_text(c, b, 32);
    return b[0] ? b : NULL;
}

bool chord_eq(KeyChord a, KeyChord b) { return a.key == b.key && a.mods == b.mods; }
bool act_is_default(int a) { return chord_eq(KEYMAP[a][0], ACTS[a].def[0]) && chord_eq(KEYMAP[a][1], ACTS[a].def[1]); }

/* ── file ── */

static void keys_save(void) {
    char path[1024];
    if (!plat_config_path("keys.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# Vertexa shortcuts changed from the defaults: action=key:mods key:mods (sokol key codes; mods 1 shift, 2 ctrl, 4 alt)\n");
    for (int a = 0; a < ACT_COUNT; a++)
        if (!act_is_default(a)) fprintf(f, "%s=%d:%d %d:%d\n", ACTS[a].id, KEYMAP[a][0].key, KEYMAP[a][0].mods, KEYMAP[a][1].key, KEYMAP[a][1].mods);
    fclose(f);
}

void keys_load(void) {
    for (int a = 0; a < ACT_COUNT; a++) { KEYMAP[a][0] = ACTS[a].def[0]; KEYMAP[a][1] = ACTS[a].def[1]; }
    char path[1024];
    if (!plat_config_path("keys.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int k0, m0, k1, m1;
        if (sscanf(eq + 1, "%d:%d %d:%d", &k0, &m0, &k1, &m1) != 4) continue;
        for (int a = 0; a < ACT_COUNT; a++) {
            if (strcmp(ACTS[a].id, line)) continue;
            KEYMAP[a][0] = (KeyChord){(u16)CLAMP(k0, 0, 511), (u8)(m0 & 7)};
            KEYMAP[a][1] = (KeyChord){(u16)CLAMP(k1, 0, 511), (u8)(m1 & 7)};
        }
    }
    fclose(f);
}

// binds slot i of action a; the chord leaves any other action that had it. Returns that action or -1.
int keys_bind(int a, int i, KeyChord c) {
    int taken = -1;
    if (c.key)
        for (int b = 0; b < ACT_COUNT; b++)
            for (int j = 0; j < 2; j++)
                if ((b != a || j != i) && chord_eq(KEYMAP[b][j], c)) { KEYMAP[b][j] = (KeyChord){0}; if (b != a) taken = b; }
    KEYMAP[a][i] = c;
    keys_save();
    return taken;
}
void keys_reset(int a) {
    if (a >= 0) { KEYMAP[a][0] = ACTS[a].def[0]; KEYMAP[a][1] = ACTS[a].def[1]; }
    else for (int b = 0; b < ACT_COUNT; b++) { KEYMAP[b][0] = ACTS[b].def[0]; KEYMAP[b][1] = ACTS[b].def[1]; }
    keys_save();
}
