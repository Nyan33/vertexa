// Settings: General (auto-save, recovery copies, undo, interface size, pen), Performance (GPU mode,
// anti-aliasing, stage resolution, overlay), Keyboard (every shortcut, two bindings each), Layout (where
// the panels go, kept as profiles) and Theme.
// Files in %APPDATA%/Vertexa: settings.txt (preferences), keys.txt (shortcuts, see keys.c), layouts.txt
// (layout profiles, written like the themes) and themes.txt
// (built-in and user presets plus the theme in use, marked "@"; colors are role=RRGGBB pairs, so files
// written by older builds keep working when roles are added).
#include "app/app.h"
#include "platform/platform.h"
#include "sokol/sokol_app.h"
#include <ctype.h>

typedef struct { char name[48]; u32 rgb[TC_COUNT]; } ThemePreset;

static const struct { const char *key, *label; } ROLES[TC_COUNT] = {
    [TC_BG] = {"bg", "Window"}, [TC_S1] = {"s1", "Panels"}, [TC_S2] = {"s2", "Fields and hover"},
    [TC_S3] = {"s3", "Menus and buttons"}, [TC_S4] = {"s4", "Selected and tooltips"}, [TC_PASTE] = {"paste", "Around the stage"},
    [TC_LINE] = {"line", "Dividers"}, [TC_LINE2] = {"line2", "Outlines"},
    [TC_TX] = {"tx", "Text"}, [TC_TX2] = {"tx2", "Secondary text"}, [TC_TX3] = {"tx3", "Hints"},
    [TC_AC] = {"ac", "Accent"}, [TC_LOOP] = {"loop", "Loop range"}, [TC_PREV] = {"prev", "Onion skin before"},
    [TC_NEXT] = {"next", "Onion skin after"}, [TC_TWEEN_C] = {"tween_c", "Classic tween"}, [TC_TWEEN_S] = {"tween_s", "Shape tween"},
    [TC_SEL] = {"sel", "Selection"},
};
static bool group_end(int i) { return i == TC_PASTE || i == TC_LINE2 || i == TC_TX3; }

//                 bg        s1        s2        s3        s4        paste     line      line2     tx        tx2       tx3       ac        loop      prev      next      tween_c   tween_s   sel
static const ThemePreset BUILTIN[] = {
    {"Graphite", {0x0e0e10, 0x151518, 0x1b1b1f, 0x232328, 0x2c2c33, 0x0a0a0c, 0x232329, 0x303037, 0xececf1, 0xa2a2ad, 0x6b6b77, 0xff5a36, 0xf2b544, 0x4c8dff, 0x35c47a, 0x3a3468, 0x1d4a3c, 0x4c8dff}},
    {"Midnight", {0x0b0d14, 0x11141d, 0x161a25, 0x1d2230, 0x272d3e, 0x07080d, 0x1b2030, 0x2a3144, 0xe6eaf5, 0x9ba4ba, 0x636c82, 0x7c8cff, 0xf2b544, 0x4ca3ff, 0x35c47a, 0x343a72, 0x1b4a48, 0x5aa9ff}},
    {"Animate", {0x1d1d1d, 0x262626, 0x2e2e2e, 0x383838, 0x444444, 0x141414, 0x1a1a1a, 0x4a4a4a, 0xf0f0f0, 0xb8b8b8, 0x838383, 0x2d8ceb, 0xf2b544, 0x4c8dff, 0x35c47a, 0x4a3f7a, 0x2e5a3a, 0x3399ff}},
};

static struct {
    ThemePreset *user;          // stb_ds array
    char name[48];              // preset the current colors started from
    int role;                   // color being edited
    float h, s, v; Color shown;
    bool dirty;                 // changes not written to disk yet
    int naming;                 // 1 = start typing a preset name, 2 = typing
    char newname[48];
} S;

/* ── presets ── */

static const ThemePreset *find(const char *name) {
    for (int i = 0; i < COUNT(BUILTIN); i++) if (!strcmp(BUILTIN[i].name, name)) return &BUILTIN[i];
    for (ptrdiff_t i = 0; i < arrlen(S.user); i++) if (!strcmp(S.user[i].name, name)) return &S.user[i];
    return NULL;
}
static int user_index(const char *name) {
    for (ptrdiff_t i = 0; i < arrlen(S.user); i++) if (!strcmp(S.user[i].name, name)) return (int)i;
    return -1;
}
static bool matches(const ThemePreset *p) {
    for (int i = 0; i < TC_COUNT; i++) if (THEME[i] != hex(p->rgb[i])) return false;
    return true;
}
static void apply(const ThemePreset *p) {
    for (int i = 0; i < TC_COUNT; i++) THEME[i] = hex(p->rgb[i]);
    snprintf(S.name, sizeof(S.name), "%s", p->name);
    S.dirty = true;
}
static void current_as(ThemePreset *p, const char *name) {
    snprintf(p->name, sizeof(p->name), "%s", name);
    for (int i = 0; i < TC_COUNT; i++) p->rgb[i] = col_rgb(THEME[i]);
}

/* ── file ── */

static void write_line(FILE *f, const char *prefix, const char *name, const u32 *rgb) {
    fprintf(f, "%s%s\t", prefix, name);
    for (int i = 0; i < TC_COUNT; i++) fprintf(f, "%s=%06X%s", ROLES[i].key, rgb[i], i + 1 < TC_COUNT ? " " : "\n");
}

static void save(void) {
    S.dirty = false;
    char path[1024];
    if (!plat_config_path("themes.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# Vertexa themes: name<TAB>role=RRGGBB ... ; the line starting with @ is the theme in use\n");
    ThemePreset cur;
    current_as(&cur, S.name);
    write_line(f, "@", cur.name, cur.rgb);
    for (ptrdiff_t i = 0; i < arrlen(S.user); i++) write_line(f, "", S.user[i].name, S.user[i].rgb);
    fclose(f);
}

static void parse_colors(char *s, u32 *rgb) {
    for (char *tok = strtok(s, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        unsigned v;
        if (sscanf(eq + 1, "%x", &v) != 1) continue;
        for (int i = 0; i < TC_COUNT; i++) if (!strcmp(ROLES[i].key, tok)) rgb[i] = v & 0xffffff;
    }
}

static void themes_load(void) {
    snprintf(S.name, sizeof(S.name), "%s", BUILTIN[0].name);
    S.role = TC_AC;
    char path[1024];
    if (!plat_config_path("themes.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        bool current = line[0] == '@';
        const char *name = current ? line + 1 : line;
        if (!name[0]) continue;
        const ThemePreset *seed = current ? find(name) : NULL;
        ThemePreset p = seed ? *seed : BUILTIN[0];   // roles missing from the file (older builds) keep the preset's colors
        snprintf(p.name, sizeof(p.name), "%s", name);
        parse_colors(tab + 1, p.rgb);
        if (current) { for (int i = 0; i < TC_COUNT; i++) THEME[i] = hex(p.rgb[i]); snprintf(S.name, sizeof(S.name), "%s", p.name); }
        else if (!find(p.name)) arrput(S.user, p);
    }
    fclose(f);
    if (!find(S.name)) snprintf(S.name, sizeof(S.name), "%s", BUILTIN[0].name);
}

static void save_preset(char *name) {
    // trim, keep built-in names for the built-ins
    while (*name == ' ') memmove(name, name + 1, strlen(name));
    for (size_t n = strlen(name); n && name[n - 1] == ' '; n--) name[n - 1] = 0;
    if (!name[0]) return;
    char final[48];
    snprintf(final, sizeof(final), "%s", name);
    for (int i = 0; i < COUNT(BUILTIN); i++) if (!strcmp(BUILTIN[i].name, final)) { snprintf(final, sizeof(final), "%.40s (mine)", name); break; }
    ThemePreset p;
    current_as(&p, final);
    int k = user_index(final);
    if (k >= 0) S.user[k] = p; else arrput(S.user, p);
    snprintf(S.name, sizeof(S.name), "%s", final);
    save();
    app_toast("Theme \xE2\x80\x9C%s\xE2\x80\x9D saved", final);
}

/* ── layouts ── */

static const Layout LBUILTIN[] = {
    {"Vertexa", 0, 0, 0, 0, 300, 316},
    {"Animate Classic", 0, 1, 1, 0, 300, 260},
    {"Left-handed", 1, 1, 0, 0, 300, 316},
    {"Compact", 0, 0, 0, 1, 264, 220},
};

static struct {
    Layout *user;               // stb_ds array
    Layout saved;               // the layout in use as last written
    int naming;                 // 1 = start typing a profile name, 2 = typing
    char newname[48];
} LY;

static const Layout *lfind(const char *name) {
    for (int i = 0; i < COUNT(LBUILTIN); i++) if (!strcmp(LBUILTIN[i].name, name)) return &LBUILTIN[i];
    for (ptrdiff_t i = 0; i < arrlen(LY.user); i++) if (!strcmp(LY.user[i].name, name)) return &LY.user[i];
    return NULL;
}
static int luser_index(const char *name) {
    for (ptrdiff_t i = 0; i < arrlen(LY.user); i++) if (!strcmp(LY.user[i].name, name)) return (int)i;
    return -1;
}
static bool lsame(const Layout *a, const Layout *b) {   // the arrangement, names aside
    return a->panel_left == b->panel_left && a->tools_right == b->tools_right && a->timeline_top == b->timeline_top &&
           a->color_below == b->color_below && fabsf(a->panel_w - b->panel_w) < 0.5f && fabsf(a->tl_h - b->tl_h) < 0.5f;
}
static void lapply(const Layout *p) { app.layout = *p; app.tl_h = p->tl_h; }

static void lwrite(FILE *f, const char *prefix, const Layout *p) {
    fprintf(f, "%s%s\tpanel=%s tools=%s timeline=%s color=%s width=%g height=%g\n", prefix, p->name, p->panel_left ? "left" : "right",
            p->tools_right ? "right" : "left", p->timeline_top ? "top" : "bottom", p->color_below ? "bottom" : "top", p->panel_w, p->tl_h);
}
static void layouts_save(void) {
    app.layout.tl_h = app.tl_h;
    LY.saved = app.layout;
    char path[1024];
    if (!plat_config_path("layouts.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# Vertexa layouts: name<TAB>key=value ... ; the line starting with @ is the layout in use\n");
    lwrite(f, "@", &app.layout);
    for (ptrdiff_t i = 0; i < arrlen(LY.user); i++) lwrite(f, "", &LY.user[i]);
    fclose(f);
}
static void lparse(char *s, Layout *p) {
    for (char *tok = strtok(s, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(tok, "panel")) p->panel_left = !strcmp(v, "left");
        else if (!strcmp(tok, "tools")) p->tools_right = !strcmp(v, "right");
        else if (!strcmp(tok, "timeline")) p->timeline_top = !strcmp(v, "top");
        else if (!strcmp(tok, "color")) p->color_below = !strcmp(v, "bottom");
        else if (!strcmp(tok, "width")) p->panel_w = clampf((float)atof(v), 260, 420);
        else if (!strcmp(tok, "height")) p->tl_h = clampf((float)atof(v), 150, 900);
    }
}
static void layouts_load(void) {
    lapply(&LBUILTIN[0]);
    char path[1024];
    FILE *f = plat_config_path("layouts.txt", path, sizeof(path)) ? plat_fopen(path, "rb") : NULL;
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        bool current = line[0] == '@';
        const char *name = current ? line + 1 : line;
        if (!name[0]) continue;
        Layout p = LBUILTIN[0];
        snprintf(p.name, sizeof(p.name), "%s", name);
        lparse(tab + 1, &p);
        if (current) lapply(&p);
        else if (!lfind(p.name)) arrput(LY.user, p);
    }
    if (f) fclose(f);
    if (!lfind(app.layout.name)) snprintf(app.layout.name, sizeof(app.layout.name), "%s", LBUILTIN[0].name);
    LY.saved = app.layout;
}
void layout_tick(void) {
    app.layout.tl_h = app.tl_h;
    if (!ui.in.down[0] && (!lsame(&app.layout, &LY.saved) || strcmp(app.layout.name, LY.saved.name))) layouts_save();
}
static void lsave_profile(char *name) {
    while (*name == ' ') memmove(name, name + 1, strlen(name));
    for (size_t n = strlen(name); n && name[n - 1] == ' '; n--) name[n - 1] = 0;
    for (char *c = name; *c; c++) if (*c == '\t') *c = ' ';
    if (!name[0]) return;
    char final[48];
    snprintf(final, sizeof(final), "%s", name);
    for (int i = 0; i < COUNT(LBUILTIN); i++) if (!strcmp(LBUILTIN[i].name, final)) { snprintf(final, sizeof(final), "%.40s (mine)", name); break; }
    Layout p = app.layout;
    snprintf(p.name, sizeof(p.name), "%s", final);
    int k = luser_index(final);
    if (k >= 0) LY.user[k] = p; else arrput(LY.user, p);
    snprintf(app.layout.name, sizeof(app.layout.name), "%s", final);
    layouts_save();
    app_toast("Layout \xE2\x80\x9C%s\xE2\x80\x9D saved", final);
}

/* ── preferences file ── */

static struct {
    int page;
    bool prefs_dirty;
    int cap_act, cap_slot;        // Keyboard: binding waiting for keys (-1: none)
    float key_scroll;
    char filter[48];
    char gpu[128]; double vram; bool gpu_known;
} P = {.cap_act = -1};

static void prefs_defaults(void) {
    app.autosave_min = 0; app.recovery_min = 2; app.undo_steps = 100; app.ui_zoom = 1;
    app.pen_eraser_tool = false; app.size_fixed = false; app.gpu_low = false; app.msaa = 8; app.stage_res = 100; app.stats = false;
}

static void prefs_save(void) {
    P.prefs_dirty = false;
    char path[1024];
    if (!plat_config_path("settings.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# Vertexa settings\nautosave=%d\nrecovery=%d\nundo=%d\nui_zoom=%g\npen_eraser_tool=%d\nsize_fixed=%d\ngpu_low=%d\nmsaa=%d\nstage_res=%d\nstats=%d\n",
            app.autosave_min, app.recovery_min, app.undo_steps, app.ui_zoom, app.pen_eraser_tool, app.size_fixed, app.gpu_low, app.msaa, app.stage_res, app.stats);
    fclose(f);
}

static void prefs_load(void) {
    prefs_defaults();
    char path[1024];
    if (!plat_config_path("settings.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64]; float v;
        if (line[0] == '#' || sscanf(line, "%63[^=]=%f", key, &v) != 2) continue;
        if (!strcmp(key, "autosave")) app.autosave_min = CLAMP((int)v, 0, 60);
        else if (!strcmp(key, "recovery")) app.recovery_min = CLAMP((int)v, 0, 60);
        else if (!strcmp(key, "undo")) app.undo_steps = CLAMP((int)v, 10, 2000);
        else if (!strcmp(key, "ui_zoom")) app.ui_zoom = clampf(v, 0.75f, 2);
        else if (!strcmp(key, "pen_eraser_tool")) app.pen_eraser_tool = v != 0;
        else if (!strcmp(key, "gpu_low")) app.gpu_low = v != 0;
        else if (!strcmp(key, "size_fixed")) app.size_fixed = v != 0;
        else if (!strcmp(key, "msaa")) app.msaa = (int)v == 1 || (int)v == 2 || (int)v == 4 ? (int)v : 8;
        else if (!strcmp(key, "stage_res")) app.stage_res = CLAMP((int)v, 25, 100);
        else if (!strcmp(key, "stats")) app.stats = v != 0;
    }
    fclose(f);
}

void prefs_changed(void) { hist_set_limit(app.undo_steps); P.prefs_dirty = true; }

void settings_load(void) {
    prefs_load();
    hist_set_limit(app.undo_steps);
    themes_load();
    layouts_load();
    char dir[1024];
    if (plat_config_path("recovery", dir, sizeof(dir)) && plat_count_files(dir, "*.vxa") > 0)
        app_toast("Recovery copies from an earlier session are kept, see Settings > General > Open folder");
}

void settings_open(int page) { app.settings = true; P.page = CLAMP(page, 0, SET_COUNT - 1); }

/* ── layout helpers ── */

static float RY;   // layout cursor of the page

// a setting: title and a one-line description on the left, its control (cw wide) on the right
static Rect row(Rect c, const char *title, const char *desc, float cw) {
    font_draw_in(ui.dl, F_UI_MED, rect(c.x, RY, c.w - cw - 24, 18), ALIGN_LEFT, title, C_TX);
    if (desc) font_draw_in(ui.dl, F_UI, rect(c.x, RY + 21, c.w - cw - 24, 16), ALIGN_LEFT, desc, C_TX3);
    Rect ctl = {c.x + c.w - cw, RY + 4, cw, 30};
    RY += 62;
    dl_rect(ui.dl, rect(c.x, RY - 14, c.w, 1), C_LINE);
    return ctl;
}
static int pick(const int *vals, int n, int v) { for (int i = 0; i < n; i++) if (vals[i] == v) return i; return 0; }
static void dim(Rect r) { dl_rrect(ui.dl, r, 7, col_a(C_S2, 0xb0)); }

static bool close_button(Rect b) {
    UiResp re = ui_interact(ui_id("settings-close"), b);
    if (re.hover) dl_rrect(ui.dl, b, 6, C_S3);
    float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    Color c = re.hover ? C_TX : C_TX3;
    dl_line(ui.dl, v2(cx - 5, cy - 5), v2(cx + 5, cy + 5), 1.4f, c);
    dl_line(ui.dl, v2(cx + 5, cy - 5), v2(cx - 5, cy + 5), 1.4f, c);
    return re.clicked;
}

/* ── General ── */

static void general_page(Rect c) {
    static const char *every[] = {"Off", "1 min", "2 min", "5 min", "10 min"};
    static const int every_v[] = {0, 1, 2, 5, 10};
    Rect ctl = row(c, "Auto-save", "Saves documents that already have a file", 300);
    int i = pick(every_v, 5, app.autosave_min), ni = ui_segmented(ui_id("pref-autosave"), ctl, every, 5, i);
    if (ni != i) { app.autosave_min = every_v[ni]; prefs_changed(); }

    ctl = row(c, "Recovery copies", "A spare copy in case Vertexa closes unexpectedly", 300);
    Rect seg = {ctl.x, ctl.y, ctl.w - 96, ctl.h};
    i = pick(every_v, 4, app.recovery_min); ni = ui_segmented(ui_id("pref-recovery"), seg, every, 4, i);
    if (ni != i) { app.recovery_min = every_v[ni]; prefs_changed(); }
    if (ui_text_button(ui_id("pref-recovery-open"), rect(ctl.x + ctl.w - 90, ctl.y, 90, 30), "Open folder", false)) {
        char dir[1024];
        if (plat_config_path("recovery", dir, sizeof(dir)) && plat_mkdirs(dir)) plat_open_folder(dir);
    }

    static const char *undo[] = {"50", "100", "200", "400"};
    static const int undo_v[] = {50, 100, 200, 400};
    ctl = row(c, "Undo steps", "More steps keep more of your history in memory", 300);
    i = pick(undo_v, 4, app.undo_steps); ni = ui_segmented(ui_id("pref-undo"), ctl, undo, 4, i);
    if (ni != i) { app.undo_steps = undo_v[ni]; prefs_changed(); }

    static const char *sizes[] = {"90%", "100%", "110%", "125%", "150%"};
    static const int sizes_v[] = {90, 100, 110, 125, 150};
    ctl = row(c, "Interface size", "On top of the Windows display scaling", 300);
    i = pick(sizes_v, 5, (int)roundf(app.ui_zoom * 100)); ni = ui_segmented(ui_id("pref-zoom"), ctl, sizes, 5, i);
    if (ni != i) { app.ui_zoom = sizes_v[ni] / 100.0f; prefs_changed(); }

    static const char *bsize[] = {"Follows zoom", "Fixed on screen"};
    ctl = row(c, "Brush size", "Follows zoom: a stroke is as wide in the drawing at any zoom", 300);
    ni = ui_segmented(ui_id("pref-bsize"), ctl, bsize, 2, app.size_fixed);
    if (ni != app.size_fixed) { app.size_fixed = ni; prefs_changed(); }

    static const char *eraser[] = {"Erases", "Current tool"};
    ctl = row(c, "Pen eraser end", "What turning the pen around does", 300);
    ni = ui_segmented(ui_id("pref-eraser"), ctl, eraser, 2, app.pen_eraser_tool);
    if (ni != app.pen_eraser_tool) { app.pen_eraser_tool = ni; prefs_changed(); }
}

/* ── Performance ── */

static void perf_page(Rect c) {
    if (!P.gpu_known) {
        P.gpu_known = true;
        if (!plat_gpu_info(P.gpu, sizeof(P.gpu), &P.vram)) { snprintf(P.gpu, sizeof(P.gpu), "Unknown"); P.vram = 0; }
    }
    char desc[200];
    snprintf(desc, sizeof(desc), "%s, %.1f GB video memory", P.gpu, P.vram);
    static int pref = -1;
    if (pref < 0) pref = plat_gpu_preference();
    static const char *gp[] = {"Windows decides", "Power saving", "High performance"};
    Rect gctl = row(c, "Graphics card", desc, 340);
    int np = ui_segmented(ui_id("pref-gpu-pick"), gctl, gp, 3, pref);
    if (np != pref) {
        if (plat_set_gpu_preference(np)) { pref = np; app_toast("Restart Vertexa to switch graphics cards"); }
        else app_toast("Windows didn't accept the graphics card choice");
    }

    static const char *mode[] = {"Full", "Low memory"};
    Rect ctl = row(c, "GPU mode", "Low memory: no anti-aliasing, the stage at 75%", 300);
    int ni = ui_segmented(ui_id("pref-gpu"), ctl, mode, 2, app.gpu_low);
    if (ni != app.gpu_low) { app.gpu_low = ni; prefs_changed(); }

    static const char *aa[] = {"Off", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97"};
    static const int aa_v[] = {1, 2, 4, 8};
    ctl = row(c, "Stage anti-aliasing", "Smoother edges while you work (exports use 8\xC3\x97)", 300);
    int i = pick(aa_v, 4, app.msaa);
    ni = ui_segmented(ui_id("pref-msaa"), ctl, aa, 4, app.gpu_low ? 0 : i);
    if (app.gpu_low) dim(ctl);
    else if (ni != i) { app.msaa = aa_v[ni]; prefs_changed(); }

    static const char *res[] = {"50%", "75%", "100%"};
    static const int res_v[] = {50, 75, 100};
    ctl = row(c, "Stage resolution", "Lower is lighter on 4K screens and weak graphics cards", 300);
    i = pick(res_v, 3, app.stage_res);
    ni = ui_segmented(ui_id("pref-res"), ctl, res, 3, app.gpu_low ? 1 : i);
    if (app.gpu_low) dim(ctl);
    else if (ni != i) { app.stage_res = res_v[ni]; prefs_changed(); }

    static const char *stats[] = {"Hide", "Show"};
    ctl = row(c, "Performance overlay", "Frame rate and stage timings in the corner of the stage", 300);
    ni = ui_segmented(ui_id("pref-stats"), ctl, stats, 2, app.stats);
    if (ni != app.stats) { app.stats = ni; prefs_changed(); }

    font_draw_in(ui.dl, F_UI, rect(c.x, RY, c.w, 18), ALIGN_LEFT,
                 "The graphics card choice is the same one Windows keeps in Settings \xE2\x80\xBA Display \xE2\x80\xBA Graphics.", C_TX3);
}

/* ── Keyboard ── */

static bool is_modifier(int k) { return k >= SAPP_KEYCODE_LEFT_SHIFT && k <= SAPP_KEYCODE_MENU; }
static bool contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((u8)*a) == tolower((u8)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static void capture_keys(void) {
    if (P.cap_act < 0) return;
    for (int k = 1; k < 512; k++) {
        if (!ui.in.key_pressed[k] || is_modifier(k)) continue;
        u8 mods = (u8)(ui.in.mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT));
        if (k == SAPP_KEYCODE_ESCAPE && !mods) { P.cap_act = -1; return; }
        KeyChord c = {(u16)k, mods};
        if ((k == SAPP_KEYCODE_BACKSPACE || k == SAPP_KEYCODE_DELETE) && !mods) c = (KeyChord){0};
        int lost = keys_bind(P.cap_act, P.cap_slot, c);
        char t[32];
        if (lost >= 0) app_toast("%s was taken from \xE2\x80\x9C%s\xE2\x80\x9D", chord_text(c, t, sizeof(t)), act_name(lost));
        P.cap_act = -1;
        ui.in.key_pressed[k] = false;   // not an Esc that closes the window
        return;
    }
}

static void keys_page(Rect c) {
    capture_keys();
    // search and reset
    UiId fid = ui_id("key-filter");
    Rect fr = {c.x, RY, 280, 30};
    ui_text_field(fid, fr, P.filter, sizeof(P.filter), NULL);
    if (!P.filter[0] && ui.focus != fid) font_draw_in(ui.dl, F_UI, rect(fr.x + 10, fr.y, 200, 30), ALIGN_LEFT, "Search actions or keys", C_TX3);
    if (ui.focus == fid) snprintf(P.filter, sizeof(P.filter), "%s", ui.edit);   // filter while typing
    if (ui_text_button(ui_id("keys-reset-all"), rect(c.x + c.w - 120, RY, 120, 30), "Reset all", false)) { keys_reset(-1); P.cap_act = -1; app_toast("Shortcuts are back to the defaults"); }
    RY += 30 + 10;
    font_draw_in(ui.dl, F_UI, rect(c.x, RY, c.w, 16), ALIGN_LEFT, "Click a shortcut and press the new keys. Backspace removes it, Esc cancels", C_TX3);
    RY += 16 + 12;

    Rect list = {c.x - 8, RY, c.w + 16, c.y + c.h - RY};
    float content = 0;
    const char *grp = NULL;
    for (int a = 0; a < ACT_COUNT; a++) {
        char k0[32], k1[32];
        chord_text(KEYMAP[a][0], k0, 32); chord_text(KEYMAP[a][1], k1, 32);
        if (!contains_ci(act_name(a), P.filter) && !contains_ci(k0, P.filter) && !contains_ci(k1, P.filter) && !contains_ci(act_group(a), P.filter)) continue;
        if (!grp || strcmp(grp, act_group(a))) { grp = act_group(a); content += 30; }
        content += 32;
    }
    if (rect_has(list, ui.in.mouse) && ui.in.wheel_y != 0) P.key_scroll -= ui.in.wheel_y * 64;
    P.key_scroll = clampf(P.key_scroll, 0, MAX(0, content - list.h));
    dl_push_clip(ui.dl, list);
    float y = list.y - P.key_scroll;
    grp = NULL;
    bool clicked_chip = false;
    for (int a = 0; a < ACT_COUNT; a++) {
        char k[2][32];
        chord_text(KEYMAP[a][0], k[0], 32); chord_text(KEYMAP[a][1], k[1], 32);
        if (!contains_ci(act_name(a), P.filter) && !contains_ci(k[0], P.filter) && !contains_ci(k[1], P.filter) && !contains_ci(act_group(a), P.filter)) continue;
        if (!grp || strcmp(grp, act_group(a))) {
            grp = act_group(a);
            font_draw_in(ui.dl, F_SMALL_SEMI, rect(c.x, y + 10, 200, 16), ALIGN_LEFT, grp, C_TX3);
            y += 30;
        }
        Rect rr = {c.x - 8, y, c.w + 16, 30};
        if (rr.y + rr.h >= list.y && rr.y <= list.y + list.h) {
            bool hov = rect_has(rr, ui.in.mouse) && rect_has(list, ui.in.mouse) && ui_mouse_free();   // highlight only: the chips take the clicks
            if (hov) dl_rrect(ui.dl, rr, 6, C_S3);
            font_draw_in(ui.dl, F_UI, rect(c.x, y, 300, 30), ALIGN_LEFT, act_name(a), hov ? C_TX : C_TX2);
            for (int s = 0; s < 2; s++) {
                Rect ch = {c.x + c.w - 260 + s * 134, y + 3, 126, 24};
                UiResp cr = ui_interact(ui_idi(s ? "key-chip1" : "key-chip0", a), ch);
                bool cap = P.cap_act == a && P.cap_slot == s;
                if (cap) { dl_rrect(ui.dl, ch, 6, C_AC2); dl_rrect_border(ui.dl, ch, 6, 1, C_AC); }
                else if (k[s][0]) dl_rrect(ui.dl, ch, 6, cr.hover ? C_S4 : C_S1);
                else dl_rrect_border(ui.dl, ch, 6, 1, cr.hover ? C_LINE2 : C_LINE);
                const char *label = cap ? "Press keys\xE2\x80\xA6" : k[s][0] ? k[s] : cr.hover ? "Add" : "";
                font_draw_in(ui.dl, cap ? F_UI : F_MONO_MED, ch, ALIGN_CENTER, label, cap ? C_TX : k[s][0] ? C_TX : C_TX3);
                if (cr.clicked) { P.cap_act = a; P.cap_slot = s; ui.focus = 0; clicked_chip = true; }
            }
            if (!act_is_default(a)) {
                Rect rb = {c.x + c.w - 260 - 32, y + 3, 24, 24};
                UiId rid = ui_idi("key-reset", a);
                UiResp rre = ui_interact(rid, rb);
                if (rre.hover) dl_rrect(ui.dl, rb, 6, C_S4);
                icon_draw(ui.dl, IC_UNDO, v2(rb.x + 12, rb.y + 12), ICON_S, false, rre.hover ? C_TX : C_TX3);
                ui_tooltip(rid, rb, "Back to the default");
                if (rre.clicked) { keys_reset(a); P.cap_act = -1; }
            }
        }
        y += 32;
    }
    dl_pop_clip(ui.dl);
    if (content > list.h) {
        float th = list.h * list.h / content, ty = list.y + (list.h - th) * P.key_scroll / (content - list.h);
        dl_rrect(ui.dl, rect(list.x + list.w + 4, ty, 3, th), 1.5f, hexa(0xffffff, 0x22));
    }
    if (P.cap_act >= 0 && ui.in.pressed[0] && !clicked_chip) P.cap_act = -1;   // clicking elsewhere cancels
}

/* ── Theme ── */

static void theme_page(Rect c) {
    const float ROW = 26, GAP = 10;
    float list_h = TC_COUNT * ROW + 3 * GAP;
    float x = c.x, y = RY;
    Rect drop = {x, y, 240, 30};
    UiId nid = ui_id("theme-name");
    const ThemePreset *base = find(S.name);
    if (!base) base = &BUILTIN[0];
    bool is_user = user_index(S.name) >= 0;
    float bx = x + 240 + 8;
    if (S.naming) {
        if (S.naming == 1) { ui_focus_text(nid, S.newname); S.naming = 2; }
        if (ui_text_field(nid, drop, S.newname, sizeof(S.newname), NULL)) { save_preset(S.newname); S.naming = 0; }
        else if (ui.focus != nid) S.naming = 0;   // Esc
        font_draw_in(ui.dl, F_UI, rect(bx + 4, y, 300, 30), ALIGN_LEFT, "Type a name. Enter saves, Esc cancels", C_TX3);
    } else {
        if (ui_text_button(ui_id("theme-save"), rect(bx, y, 92, 30), "Save as\xE2\x80\xA6", false)) {
            snprintf(S.newname, sizeof(S.newname), "%s", is_user ? S.name : "");   // re-saving one's own preset overwrites it
            S.naming = 1;
        }
        ui_tooltip(ui_id("theme-save"), rect(bx, y, 92, 30), "Save these colors as a preset");
        bx += 92 + 4;
        if (is_user && ui_text_button(ui_id("theme-del"), rect(bx, y, 72, 30), "Delete", false)) {
            int k = user_index(S.name);
            char gone[48]; snprintf(gone, sizeof(gone), "%s", S.name);
            arrdel(S.user, k);
            snprintf(S.name, sizeof(S.name), "%s", BUILTIN[0].name);
            save();
            app_toast("Theme \xE2\x80\x9C%s\xE2\x80\x9D deleted", gone);
        }
    }
    Rect reset = {c.x + c.w - 150, y, 150, 30};
    if (ui_text_button(ui_id("theme-reset"), reset, "Reset to original", false) && !matches(&BUILTIN[0])) apply(&BUILTIN[0]);
    ui_tooltip(ui_id("theme-reset"), reset, "Back to the Graphite colors");

    // roles
    y += 30 + 18;
    Rect list = {x, y, 356, list_h};
    float ry = list.y;
    for (int i = 0; i < TC_COUNT; i++) {
        Rect rw = {list.x, ry, list.w, ROW - 2};
        UiResp re = ui_interact(ui_idi("theme-role", i), rw);
        bool on = S.role == i;
        if (on) dl_rrect(ui.dl, rw, 6, C_S4);
        else if (re.hover) dl_rrect(ui.dl, rw, 6, C_S3);
        V2 cc = {rw.x + 15, rw.y + rw.h / 2};
        dl_circle(ui.dl, cc, 7.5f, THEME[i]);
        dl_circle_border(ui.dl, cc, 7.5f, 1, hexa(0xffffff, 0x30));
        font_draw_in(ui.dl, F_UI, rect(rw.x + 32, rw.y, 220, rw.h), ALIGN_LEFT, ROLES[i].label, on || re.hover ? C_TX : C_TX2);
        char hx[8]; snprintf(hx, sizeof(hx), "%06X", col_rgb(THEME[i]));
        font_draw_in(ui.dl, F_MONO, rect(rw.x, rw.y, rw.w - 12, rw.h), ALIGN_RIGHT, hx, on ? C_TX2 : C_TX3);
        if (re.pressed) S.role = i;
        ry += ROW + (group_end(i) ? GAP : 0);
    }

    // picker for the selected role
    float px = list.x + list.w + 28, pw = c.x + c.w - px;
    Color *col = &THEME[S.role];
    if (*col != S.shown) { color_to_hsv(*col, &S.h, &S.s, &S.v); S.shown = *col; }
    V2 wc = {roundf(px + pw / 2), list.y + 104};
    if (ui_color_wheel(ui_id("theme-wheel"), wc, 104, 87, &S.h, &S.s, &S.v)) { *col = color_from_hsv(S.h, S.s, S.v); S.shown = *col; S.dirty = true; }
    float fy = list.y + 104 * 2 + 20;
    font_draw_in(ui.dl, F_UI_MED, rect(px, fy, pw, 18), ALIGN_CENTER, ROLES[S.role].label, C_TX);
    fy += 28;
    static char hexbuf[16];
    UiId hid = ui_id("theme-hex");
    if (ui.focus != hid) snprintf(hexbuf, sizeof(hexbuf), "%06X", col_rgb(*col));
    Rect hf = {px, fy, pw - 96 - 8, 30};
    if (ui_text_field(hid, hf, hexbuf, sizeof(hexbuf), "#")) {
        unsigned v; char *s = hexbuf; if (*s == '#') s++;
        if (strlen(s) == 3) { char e[7] = {s[0], s[0], s[1], s[1], s[2], s[2], 0}; if (sscanf(e, "%x", &v) == 1) { *col = hex(v); S.dirty = true; } }
        else if (sscanf(s, "%x", &v) == 1) { *col = hex(v & 0xffffff); S.dirty = true; }
    }
    Rect rc = {px + pw - 96, fy, 96, 30};
    if (ui_text_button(ui_id("theme-role-reset"), rc, "Reset", false)) { *col = hex(base->rgb[S.role]); S.dirty = true; }
    ui_tooltip(ui_id("theme-role-reset"), rc, "This color as in the selected preset");

    // the preset dropdown goes last so its list is drawn over everything else
    int n = 0, sel = 0;
    const char *items[40];
    static char edited[64];
    bool changed = !matches(base);
    for (int i = 0; i < COUNT(BUILTIN) && n < 40; i++) items[n++] = BUILTIN[i].name;
    for (ptrdiff_t i = 0; i < arrlen(S.user) && n < 40; i++) items[n++] = S.user[i].name;
    for (int i = 0; i < n; i++) if (!strcmp(items[i], base->name)) sel = i;
    if (changed) { snprintf(edited, sizeof(edited), "%s (edited)", items[sel]); items[sel] = edited; }
    if (!S.naming) {
        int ns = ui_dropdown(ui_id("theme-preset"), drop, items, n, sel);
        if (ns != sel) apply(ns < COUNT(BUILTIN) ? &BUILTIN[ns] : &S.user[ns - COUNT(BUILTIN)]);
    }
}

/* ── Layout ── */

static int nearest(const int *vals, int n, float v) {
    int k = 0;
    for (int i = 1; i < n; i++) if (fabsf(vals[i] - v) < fabsf(vals[k] - v)) k = i;
    return k;
}

static void layout_page(Rect c) {
    Layout *L = &app.layout;
    // profile: pick, save as, delete (the user's)
    float x = c.x, y = RY;
    Rect drop = {x, y, 240, 30};
    UiId nid = ui_id("layout-name");
    const Layout *base = lfind(L->name);
    if (!base) base = &LBUILTIN[0];
    bool is_user = luser_index(L->name) >= 0;
    float bx = x + 240 + 8;
    if (LY.naming) {
        if (LY.naming == 1) { ui_focus_text(nid, LY.newname); LY.naming = 2; }
        if (ui_text_field(nid, drop, LY.newname, sizeof(LY.newname), NULL)) { lsave_profile(LY.newname); LY.naming = 0; }
        else if (ui.focus != nid) LY.naming = 0;   // Esc
        font_draw_in(ui.dl, F_UI, rect(bx + 4, y, 300, 30), ALIGN_LEFT, "Type a name. Enter saves, Esc cancels", C_TX3);
    } else {
        if (ui_text_button(ui_id("layout-save"), rect(bx, y, 92, 30), "Save as\xE2\x80\xA6", false)) {
            snprintf(LY.newname, sizeof(LY.newname), "%s", is_user ? L->name : "");
            LY.naming = 1;
        }
        ui_tooltip(ui_id("layout-save"), rect(bx, y, 92, 30), "Save this layout as a profile");
        bx += 92 + 4;
        if (is_user && ui_text_button(ui_id("layout-del"), rect(bx, y, 72, 30), "Delete", false)) {
            char gone[48]; snprintf(gone, sizeof(gone), "%s", L->name);
            arrdel(LY.user, luser_index(L->name));
            snprintf(L->name, sizeof(L->name), "%s", LBUILTIN[0].name);
            layouts_save();
            app_toast("Layout \xE2\x80\x9C%s\xE2\x80\x9D deleted", gone);
        }
        if (is_user) bx += 72 + 4;
        if (!lsame(L, base) && ui_text_button(ui_id("layout-revert"), rect(bx, y, 72, 30), "Revert", false)) lapply(base);
        ui_tooltip(ui_id("layout-revert"), rect(bx, y, 72, 30), "Back to the profile as saved");
    }
    RY += 30 + 30;
    dl_rect(ui.dl, rect(c.x, RY - 14, c.w, 1), C_LINE);

    static const char *sides[] = {"Left", "Right"};
    Rect ctl = row(c, "Panels", "Colors, brushes, the library and properties", 300);
    int ni = ui_segmented(ui_id("layout-panel"), ctl, sides, 2, !L->panel_left);
    L->panel_left = ni == 0;

    ctl = row(c, "Tools", "The tools column, beside the stage", 300);
    ni = ui_segmented(ui_id("layout-tools"), ctl, sides, 2, L->tools_right);
    L->tools_right = ni == 1;

    static const char *tl[] = {"Above the stage", "Below the stage"};
    ctl = row(c, "Timeline", "Above the stage as in Animate's Classic workspace", 300);
    ni = ui_segmented(ui_id("layout-timeline"), ctl, tl, 2, !L->timeline_top);
    L->timeline_top = ni == 0;

    static const char *cw[] = {"Above the tabs", "Below the tabs"};
    ctl = row(c, "Color wheel", "Where the colors sit in the panel", 300);
    ni = ui_segmented(ui_id("layout-color"), ctl, cw, 2, L->color_below);
    L->color_below = ni == 1;

    static const char *widths[] = {"264", "300", "340", "380"};
    static const int widths_v[] = {264, 300, 340, 380};
    ctl = row(c, "Panel width", "Or drag the panel's edge", 300);
    int i = nearest(widths_v, 4, L->panel_w);
    ni = ui_segmented(ui_id("layout-width"), ctl, widths, 4, i);
    if (ni != i) L->panel_w = (float)widths_v[ni];

    static const char *heights[] = {"Short", "Medium", "Tall"};
    static const int heights_v[] = {220, 316, 420};
    ctl = row(c, "Timeline height", "Or drag the timeline's edge", 300);
    i = nearest(heights_v, 3, app.tl_h);
    ni = ui_segmented(ui_id("layout-height"), ctl, heights, 3, i);
    if (ni != i) app.tl_h = (float)heights_v[ni];
    L->tl_h = app.tl_h;

    // the profile dropdown goes last so its list is drawn over everything else
    int n = 0, sel = 0;
    const char *items[40];
    static char edited[64];
    for (int k = 0; k < COUNT(LBUILTIN) && n < 40; k++) items[n++] = LBUILTIN[k].name;
    for (ptrdiff_t k = 0; k < arrlen(LY.user) && n < 40; k++) items[n++] = LY.user[k].name;
    for (int k = 0; k < n; k++) if (!strcmp(items[k], base->name)) sel = k;
    if (!lsame(L, base)) { snprintf(edited, sizeof(edited), "%s (edited)", items[sel]); items[sel] = edited; }
    if (!LY.naming) {
        int ns = ui_dropdown(ui_id("layout-profile"), drop, items, n, sel);
        if (ns != sel) lapply(ns < COUNT(LBUILTIN) ? &LBUILTIN[ns] : &LY.user[ns - COUNT(LBUILTIN)]);
    }
}

/* ── the window ── */

void settings_window(void) {
    if (!app.settings) return;
    // Esc first leaves a text field, a preset name or a key capture; only a later Esc closes the window
    bool esc_closes = ui.in.key_pressed[SAPP_KEYCODE_ESCAPE] && !ui.focus && !ui.popup && !S.naming && !LY.naming && P.cap_act < 0;
    ui_overlay(true);
    const float W = 900, NAV = 200;
    float H = MIN(690, ui.h - 24);
    Rect r = {roundf((ui.w - W) / 2), roundf(MAX(12, (ui.h - H) / 2)), W, H};
    dl_rect(ui.dl, rect(0, 0, ui.w, ui.h), hexa(0x000000, 0x5a));
    dl_shadow(ui.dl, rect(r.x, r.y + 16, r.w, r.h), 12, 40, hexa(0x000000, 0xa0));
    dl_rrect(ui.dl, r, 12, C_S2);
    dl_push_clip(ui.dl, rect(r.x, r.y, NAV, r.h));
    dl_rrect(ui.dl, r, 12, C_S1);
    dl_pop_clip(ui.dl);
    dl_rect(ui.dl, rect(r.x + NAV, r.y, 1, r.h), C_LINE);
    dl_rrect_border(ui.dl, r, 12, 1, C_LINE2);

    // sections
    font_draw(ui.dl, F_TITLE, r.x + 22, r.y + 38, "Settings", C_TX);
    static const struct { IconId ic; const char *name; } PAGES[SET_COUNT] = {
        {IC_SLIDERS, "General"}, {IC_CPU, "Performance"}, {IC_KEYBOARD, "Keyboard"}, {IC_LAYOUT, "Layout"}, {IC_PALETTE, "Theme"},
    };
    for (int i = 0; i < SET_COUNT; i++) {
        Rect b = {r.x + 10, r.y + 62 + i * 38, NAV - 20, 34};
        UiResp re = ui_interact(ui_idi("settings-page", i), b);
        bool on = P.page == i;
        if (on) dl_rrect(ui.dl, b, 7, C_S3); else if (re.hover) dl_rrect(ui.dl, b, 7, C_S2);
        icon_draw(ui.dl, PAGES[i].ic, v2(b.x + 20, b.y + 17), ICON_M, false, on ? C_AC : re.hover ? C_TX : C_TX2);
        font_draw_in(ui.dl, F_UI_MED, rect(b.x + 40, b.y, b.w - 44, b.h), ALIGN_LEFT, PAGES[i].name, on || re.hover ? C_TX : C_TX2);
        if (re.clicked && !on) { P.page = i; P.cap_act = -1; S.naming = LY.naming = 0; ui.focus = 0; }
    }
    font_draw_in(ui.dl, F_SMALL, rect(r.x + 22, r.y + r.h - 34, NAV - 30, 16), ALIGN_LEFT, "Vertexa Alpha", C_TX3);

    // the page
    Rect c = {r.x + NAV + 32, r.y + 26, W - NAV - 64, H - 50};
    bool close = close_button(rect(r.x + r.w - 46, r.y + 16, 30, 28));
    font_draw(ui.dl, F_TITLE, c.x, c.y + 14, PAGES[P.page].name, C_TX);
    RY = c.y + 44;
    dl_push_clip(ui.dl, rect(c.x - 12, c.y + 30, c.w + 24, c.h - 30 + 12));
    switch (P.page) {
    case SET_GENERAL: general_page(c); break;
    case SET_PERFORMANCE: perf_page(c); break;
    case SET_KEYS: keys_page(c); break;
    case SET_LAYOUT: layout_page(c); break;
    default: theme_page(c); break;
    }
    dl_pop_clip(ui.dl);
    ui_overlay(false);

    if (!ui.in.down[0] && !ui.focus) {   // after a drag ends, not on every frame of it
        if (S.dirty) save();
        if (P.prefs_dirty) prefs_save();
    }
    if (close || esc_closes) {
        if (S.dirty) save();
        if (P.prefs_dirty) prefs_save();
        app.settings = false;
        S.naming = LY.naming = 0; P.cap_act = -1;
    }
}
