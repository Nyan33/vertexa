// Main window: layout, title bar with menus, tools, global shortcuts, playback, toasts, help.
#include "app/app.h"
#include "platform/platform.h"
#include "gfx/logo.h"
#include "audio/audio.h"
#include "sokol/sokol_app.h"
#include <stdarg.h>

App app;

#define C3(y0, hx, hy, y1) {3, {0, hx, 1}, {y0, hy, y1}}
// name, size, smoothing, pressure curve, tip, angle, roundness, taper start / end %, opacity %, curve fit,
// texture, its density % and scale %
static const BrushPreset BUILTIN[] = {
    {"Flash Classic", 14, 50, C3(0.25f, 0.475f, 0.52f, 1), TIP_ROUND, 0, 1, 0, 0, 100, 50, "", 60, 100},
    {"Animate Fluid", 16, 60, C3(0.04f, 0.45f, 0.40f, 1), TIP_ROUND, 0, 1, 0, 50, 100, 60, "", 60, 100},
    {"Taper Ink", 10, 55, C3(0.0f, 0.425f, 0.525f, 1), TIP_ROUND, 0, 1, 50, 50, 100, 55, "", 60, 100},
    {"G-Pen", 9, 40, C3(0.08f, 0.475f, 0.445f, 1), TIP_OVAL, 35, 0.38f, 50, 50, 100, 40, "", 60, 100},
    {"Marker", 24, 30, C3(0.85f, 0.50f, 0.92f, 1), TIP_SQUARE, 20, 1, 0, 0, 100, 30, "", 60, 100},
    {"Sketch", 4, 20, C3(0.35f, 0.50f, 0.64f, 1), TIP_ROUND, 0, 1, 0, 0, 100, 20, "", 60, 100},
    {"Pencil", 5, 25, C3(0.3f, 0.5f, 0.62f, 1), TIP_ROUND, 0, 1, 20, 30, 90, 25, "Pencil", 75, 60},
    {"Chalk", 18, 30, C3(0.5f, 0.5f, 0.75f, 1), TIP_ROUND, 0, 1, 0, 10, 100, 30, "Chalk", 85, 100},
    {"Wash", 30, 55, C3(0.4f, 0.5f, 0.7f, 1), TIP_OVAL, 30, 0.7f, 30, 40, 55, 55, "Watercolor", 55, 150},
};
#undef C3

/* brushes: %APPDATA%/Vertexa/brushes.txt keeps the whole list in order. A built-in is a line "@Name",
   the user's brushes are one tab-separated line each; an exported brush (.vxbrush) is the same line */

// the old taper field (1 start, 2 end) stays for earlier builds; what came later is key=value after a tab,
// then the texture's name after another
static void preset_line(FILE *f, const BrushPreset *p) {
    int taper = (p->taper_s > 0 ? TAPER_START : 0) | (p->taper_e > 0 ? TAPER_END : 0);
    fprintf(f, "%s\t%g %g %d %g %g %d |", p->name, p->size, p->smoothing, p->tip, p->angle, p->aspect, taper);
    for (int k = 0; k < p->curve.n; k++) fprintf(f, " %g,%g", p->curve.x[k], p->curve.y[k]);
    fprintf(f, "\topacity=%g fit=%g taper_start=%g taper_end=%g density=%g scale=%g image=%d spacing=%g flags=%d width_light=%g width_heavy=%g\t%s\t%s\n",
            p->opacity, p->fit, p->taper_s, p->taper_e, p->tex_density, p->tex_scale, p->img_mode, p->img_spacing, p->img_flags,
            p->img_w0, p->img_w1,
            p->texture, p->tip_image);
}
#define PRESET_HEADER "# Vertexa brushes: name<TAB>size smoothing tip angle aspect taper | pressure curve points x,y<TAB>key=value ...<TAB>texture<TAB>tip image\n"

void app_presets_save(void) {
    char path[1024];
    if (!plat_config_path("brushes.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    fprintf(f, PRESET_HEADER "# @Name: a built-in brush, in its place in the list\n");
    for (ptrdiff_t i = 0; i < arrlen(app.presets); i++) {
        BrushPreset *p = &app.presets[i];
        if (p->user) preset_line(f, p);
        else fprintf(f, "@%s\n", p->name);
    }
    fclose(f);
}

static bool parse_preset(char *line, BrushPreset *p) {
    char *tab = strchr(line, '\t');
    if (!tab || tab == line) return false;
    *tab = 0;
    memset(p, 0, sizeof(*p));
    p->user = true;
    char *ext = strchr(tab + 1, '\t'), *tex = NULL, *img = NULL;
    if (ext) { *ext++ = 0; tex = strchr(ext, '\t'); if (tex) { *tex++ = 0; img = strchr(tex, '\t'); if (img) *img++ = 0; } }
    int tip = 0, taper = 0;
    char *bar = strchr(tab + 1, '|');
    if (bar) {
        if (sscanf(tab + 1, "%f %f %d %f %f %d", &p->size, &p->smoothing, &tip, &p->angle, &p->aspect, &taper) != 6) return false;
        float lastx = -1;
        for (char *t = strtok(bar + 1, " \t\r\n"); t && p->curve.n < CURVE_MAX; t = strtok(NULL, " \t\r\n")) {
            float x, y;
            if (sscanf(t, "%f,%f", &x, &y) != 2 || x <= lastx) continue;
            p->curve.x[p->curve.n] = clampf(x, 0, 1); p->curve.y[p->curve.n] = clampf(y, 0, 1); p->curve.n++;
            lastx = x;
        }
        if (p->curve.n < 2) return false;
        p->curve.x[0] = 0; p->curve.x[p->curve.n - 1] = 1;
    } else {   // the earlier one-handle format: y0, handle x, handle y, y1
        float c[4];
        if (sscanf(tab + 1, "%f %f %f %f %f %f %d %f %f %d", &p->size, &p->smoothing, &c[0], &c[1], &c[2], &c[3], &tip, &p->angle, &p->aspect, &taper) != 10) return false;
        p->curve = (Curve){3, {0, clampf(c[1], 0.05f, 0.95f), 1}, {clampf(c[0], 0, 1), clampf(c[2], 0, 1), clampf(c[3], 0, 1)}};
    }
    snprintf(p->name, sizeof(p->name), "%s", line);
    p->size = clampf(p->size, 1, 400); p->smoothing = clampf(p->smoothing, 0, 100);
    p->tip = (u8)CLAMP(tip, 0, TIP_SQUARE);
    p->aspect = clampf(p->aspect, 0.05f, 1);
    p->taper_s = taper & TAPER_START ? 50 : 0; p->taper_e = taper & TAPER_END ? 50 : 0;
    p->opacity = 100; p->fit = p->smoothing; p->tex_density = 60; p->tex_scale = 100; p->img_spacing = 80; p->img_w0 = p->img_w1 = 100;
    for (char *t = ext ? strtok(ext, " \r\n") : NULL; t; t = strtok(NULL, " \r\n")) {
        char key[32]; float v;
        if (sscanf(t, "%31[^=]=%f", key, &v) != 2) continue;
        if (!strcmp(key, "opacity")) p->opacity = clampf(v, 1, 100);
        else if (!strcmp(key, "fit")) p->fit = clampf(v, 0, 100);
        else if (!strcmp(key, "taper_start")) p->taper_s = clampf(v, 0, 100);
        else if (!strcmp(key, "taper_end")) p->taper_e = clampf(v, 0, 100);
        else if (!strcmp(key, "density")) p->tex_density = clampf(v, 0, 100);
        else if (!strcmp(key, "scale")) p->tex_scale = clampf(v, 10, 400);
        else if (!strcmp(key, "image")) p->img_mode = (u8)CLAMP((int)v, 0, IMG_ALONG);
        else if (!strcmp(key, "spacing")) p->img_spacing = clampf(v, 1, 500);
        else if (!strcmp(key, "flags")) p->img_flags = (u8)((int)v & 7);
        else if (!strcmp(key, "width_light")) p->img_w0 = clampf(v, 2, 300);
        else if (!strcmp(key, "width_heavy")) p->img_w1 = clampf(v, 2, 300);
    }
    if (tex) { tex[strcspn(tex, "\r\n")] = 0; snprintf(p->texture, sizeof(p->texture), "%s", tex); }
    if (img) { img[strcspn(img, "\r\n")] = 0; snprintf(p->tip_image, sizeof(p->tip_image), "%s", img); }
    if (!p->tip_image[0]) p->img_mode = IMG_NONE;
    return true;
}

static void presets_load(void) {
    bool placed[COUNT(BUILTIN)] = {0}, any = false;
    char path[1024];
    FILE *f = plat_config_path("brushes.txt", path, sizeof(path)) ? plat_fopen(path, "rb") : NULL;
    char line[1024];
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '@') {
            line[strcspn(line, "\r\n")] = 0;
            for (int i = 0; i < COUNT(BUILTIN); i++)
                if (!placed[i] && !strcmp(line + 1, BUILTIN[i].name)) { arrput(app.presets, BUILTIN[i]); placed[i] = any = true; }
            continue;
        }
        BrushPreset p;
        if (line[0] != '#' && parse_preset(line, &p)) arrput(app.presets, p);
    }
    if (f) fclose(f);
    // files from before the list had an order: the built-ins first; a built-in a newer version added: last
    for (int i = COUNT(BUILTIN) - 1; i >= 0 && !any; i--) arrins(app.presets, 0, BUILTIN[i]);
    for (int i = 0; i < COUNT(BUILTIN) && any; i++) if (!placed[i]) arrput(app.presets, BUILTIN[i]);
}

static bool name_taken(const char *s) {
    for (ptrdiff_t i = 0; i < arrlen(app.presets); i++) if (!strcmp(app.presets[i].name, s)) return true;
    return false;
}
void app_brush_name(char *out, int cap, const char *base) {   // base, or "base 2", "base 3", ... if taken
    snprintf(out, (size_t)cap, "%s", base);
    for (int n = 2; name_taken(out) && n < 1000; n++) snprintf(out, (size_t)cap, "%.40s %d", base, n);
}

int app_insert_preset(int at, const BrushPreset *p) {
    BrushPreset q = *p;
    q.user = true;
    for (char *c = q.name; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
    if (!q.name[0]) snprintf(q.name, sizeof(q.name), "Brush");
    at = CLAMP(at, 0, (int)arrlen(app.presets));
    arrins(app.presets, at, q);
    if (app.preset >= at) app.preset++;
    app_presets_save();
    return at;
}
void app_add_preset(const BrushPreset *p) { app_insert_preset((int)arrlen(app.presets), p); }

void app_remove_preset(int i) {
    if (i < 0 || i >= arrlen(app.presets) || !app.presets[i].user) return;
    arrdel(app.presets, i);
    if (app.preset == i) app_apply_preset(MIN(i, (int)arrlen(app.presets) - 1));
    else if (app.preset > i) app.preset--;
    app_presets_save();
}

void app_move_preset(int from, int to) {   // to: the index it ends up at
    int n = (int)arrlen(app.presets);
    if (from < 0 || from >= n) return;
    to = CLAMP(to, 0, n - 1);
    if (to == from) return;
    BrushPreset p = app.presets[from];
    arrdel(app.presets, from);
    arrins(app.presets, to, p);
    if (app.preset == from) app.preset = to;
    else if (from < app.preset && to >= app.preset) app.preset--;
    else if (from > app.preset && to <= app.preset) app.preset++;
    app_presets_save();
}

void app_export_preset(int i) {
    if (i < 0 || i >= arrlen(app.presets)) return;
    char path[512];
    if (!plat_save_dialog(path, sizeof(path), "Vertexa brush", "vxbrush", app.presets[i].name)) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) { app_toast("Couldn't write the brush file"); return; }
    fprintf(f, PRESET_HEADER);
    preset_line(f, &app.presets[i]);
    fclose(f);
    app_toast("Exported %s", app.presets[i].name);
}

static bool import_vxbrush(const char *path) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) { app_toast("Couldn't open the brush file"); return false; }
    char line[1024];
    int first = -1, n = 0;
    while (fgets(line, sizeof(line), f)) {
        BrushPreset p;
        if (line[0] == '#' || !parse_preset(line, &p)) continue;
        char name[48];
        app_brush_name(name, sizeof(name), p.name);
        memcpy(p.name, name, sizeof(name));
        int at = app_insert_preset((int)arrlen(app.presets), &p);
        if (first < 0) first = at;
        n++;
    }
    fclose(f);
    if (!n) { app_toast("No brushes in this file"); return false; }
    app_apply_preset(first);
    app.right_tab = 0;
    if (!app_tool_paints(app.tool) || app.tool == 'E') app.tool = 'B';
    if (n > 1) app_toast("Imported %d brushes", n);
    else app_toast("Imported %s", app.presets[first].name);
    return true;
}

static const struct { char k; const char *name; bool grp; IconId icon; bool fill; int act; } TOOLS[] = {
    {'V', "Selection", 0, IC_SELECT, 1, ACT_TOOL_SELECT}, {'A', "Subselection", 0, IC_SELECT, 0, ACT_TOOL_SUBSELECT}, {'Q', "Free Transform", 1, IC_TRANSFORM, 0, ACT_TOOL_TRANSFORM}, {'L', "Lasso", 1, IC_LASSO, 0, ACT_TOOL_LASSO}, {0},
    {'P', "Pen", 1, IC_PEN, 0, ACT_TOOL_PEN}, {'T', "Text", 0, IC_TEXT, 0, ACT_TOOL_TEXT}, {'N', "Line", 0, IC_LINE, 0, ACT_TOOL_LINE}, {'R', "Rectangle", 0, IC_RECT, 0, ACT_TOOL_RECT}, {'O', "Oval", 0, IC_OVAL, 0, ACT_TOOL_OVAL}, {0},
    {'Y', "Pencil", 0, IC_PENCIL, 0, ACT_TOOL_PENCIL}, {'B', "Brush", 0, IC_BRUSH, 0, ACT_TOOL_BRUSH}, {'E', "Eraser", 0, IC_ERASER, 0, ACT_TOOL_ERASER}, {'C', "Trim Edge", 0, IC_TRIM, 0, ACT_TOOL_TRIM}, {'K', "Paint Bucket", 0, IC_BUCKET, 0, ACT_TOOL_BUCKET}, {'I', "Eyedropper", 0, IC_EYEDROPPER, 0, ACT_TOOL_EYEDROPPER}, {0},
    {'M', "Bone", 0, IC_BONE, 0, ACT_TOOL_BONE}, {'H', "Hand", 0, IC_HAND, 0, ACT_TOOL_HAND}, {'Z', "Zoom", 0, IC_ZOOM, 0, ACT_TOOL_ZOOM},
};

IconId tool_icon(char t) {
    for (int i = 0; i < COUNT(TOOLS); i++) if (TOOLS[i].k == t) return TOOLS[i].icon;
    return IC_SELECT;
}

const char *tool_name(char t) {
    for (int i = 0; i < COUNT(TOOLS); i++) if (TOOLS[i].k == t) return TOOLS[i].name;
    return "Tool";
}
bool app_tool_paints(char t) { return t == 'B' || t == 'Y' || t == 'E'; }

void app_apply_preset(int i) {
    if (i < 0 || i >= arrlen(app.presets)) return;
    const BrushPreset *p = &app.presets[i];
    app.preset = i;
    app.brush_size = p->size; app.brush_smooth = p->smoothing;
    app.curve = p->curve;
    app.tip = p->tip; app.tip_angle = p->angle; app.tip_aspect = p->aspect;
    app.taper_s = p->taper_s; app.taper_e = p->taper_e; app.brush_opacity = p->opacity; app.brush_fit = p->fit;
    snprintf(app.texture, sizeof(app.texture), "%s", p->texture);
    app.tex_density = p->tex_density; app.tex_scale = p->tex_scale;
    snprintf(app.tip_image, sizeof(app.tip_image), "%s", p->tip_image);
    app.img_mode = p->img_mode; app.img_flags = p->img_flags; app.img_spacing = p->img_spacing > 0 ? p->img_spacing : 80;
    app.img_w0 = p->img_w0 > 0 ? p->img_w0 : 100; app.img_w1 = p->img_w1 > 0 ? p->img_w1 : 100;
}

void app_init(void) {
    app.tab = -1;
    app_set_doc(doc_new());
    app.tool = 'B';
    settings_load();
    keys_load();
    presets_load();
    app_apply_preset(0);
    app.brush_pressure = true; app.object_draw = true;
    app.eraser_size = 24; app.pencil_size = 2; app.pencil_mode = 0;
    app.fill = hex(0x141414); app.stroke = hex(0x141414); app.alpha = 1;
    static const u32 sw[] = {0x141414, 0xffffff, 0xffd2ad, 0xf7a07c, 0xff6f5e, 0xc2304a, 0xffb347, 0xffe066, 0x3ddc97, 0x1b998b, 0x46c2ff, 0x2f6fdb, 0x8b7bff, 0x2b2f5c, 0x7d8590};
    for (int i = 0; i < COUNT(sw); i++) app.swatches[app.nswatches++] = hex(sw[i]);
    app.shape_width = 2; app.shape_fill = true; app.shape_line = true;
    app.bucket_gap = 1;
    app.trim_mode = TRIM_TO_INTERSECTION;
}

void app_toast(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    vsnprintf(app.toast, sizeof(app.toast), fmt, a);
    va_end(a);
    app.toast_until = ui.time + 2.6;
}

void app_commit(void) { hist_commit(app.doc); }
void app_undo(void) { Doc *d = hist_undo(app.doc); if (d) { app.doc = d; select_clear(); } else app_toast("Nothing to undo"); }
void app_redo(void) { Doc *d = hist_redo(app.doc); if (d) { app.doc = d; select_clear(); } else app_toast("Nothing to redo"); }

/* ── files ── */

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

/* ── document tabs ── */

// Each open document is a tab with its own undo steps and view. app.doc and app.saved_version are the
// shown tab's; the others keep theirs in their DocTab until they are shown again.
static Doc *scratch;   // stands in for app.doc while no document is open (the start screen)

static void tab_store(void) {
    if (app.tab < 0) return;
    DocTab *t = app.tabs[app.tab];
    t->doc = app.doc; t->saved_version = app.saved_version;
    stage_view_get(t->view, &t->fitted);
}
static void tab_load(int i) {
    DocTab *t = app.tabs[i];
    app.tab = i; app.doc = t->doc; app.saved_version = t->saved_version;
    hist_bind(&t->hist);
    select_clear();
    library_forget();
    stage_view_set(t->view, t->fitted);
}
static Doc *tab_doc(int i) { return i == app.tab ? app.doc : app.tabs[i]->doc; }
bool app_tab_modified(int i) {
    Doc *d = tab_doc(i);
    return d->version != (i == app.tab ? app.saved_version : app.tabs[i]->saved_version);
}
// a new document nobody has touched gives its tab to the next one opened
static bool tab_pristine(int i) {
    Doc *d = tab_doc(i);
    return !d->path[0] && !app_tab_modified(i) && !arrlen(app.tabs[i]->hist.undo);
}

void app_switch_tab(int i) {
    if (i < 0 || i >= arrlen(app.tabs)) return;
    if (i == app.tab && !app.home) return;
    if (app.playing) app_play(false);
    app.home = false;
    if (i == app.tab) return;
    tab_store();
    tab_load(i);
}

static void recovery_remove(Doc *d);
static void close_now(int i) {
    if (app.playing) app_play(false);
    tab_store();
    DocTab *t = app.tabs[i];
    recovery_remove(t->doc);
    hist_free(&t->hist);
    doc_free(t->doc);
    free(t);
    arrdel(app.tabs, i);
    int was = app.tab;
    app.tab = -1;   // nothing to store any more
    int n = (int)arrlen(app.tabs);
    if (!n) {
        if (!scratch) scratch = doc_new();
        app.doc = scratch; app.saved_version = scratch->version;
        hist_bind(NULL);
        select_clear();
        library_forget();
        app.home = true;
        home_prepare();
        return;
    }
    tab_load(was == i ? MIN(i, n - 1) : was > i ? was - 1 : was);
}

static void open_doc(Doc *d, bool reuse) {
    if (app.playing) app_play(false);
    app.home = false;
    tab_store();
    if (reuse && app.tab >= 0 && tab_pristine(app.tab)) {   // an opened file takes the untouched tab
        DocTab *t = app.tabs[app.tab];
        hist_free(&t->hist);
        doc_free(t->doc);
        int uid = t->uid;
        memset(t, 0, sizeof(*t));
        t->doc = d; t->saved_version = d->version; t->uid = uid;
    } else {
        static int uids;
        DocTab *t = calloc(1, sizeof(DocTab));
        t->doc = d; t->saved_version = d->version; t->uid = ++uids;
        arrput(app.tabs, t);
        app.tab = (int)arrlen(app.tabs) - 1;
    }
    tab_load(app.tab);
}
void app_set_doc(Doc *d) { open_doc(d, true); }

void app_new(void) {
    Doc *d = doc_new();
    // Untitled, Untitled 2, ...
    for (int k = 2; k < 1000; k++) {
        bool taken = false;
        for (int i = 0; i < arrlen(app.tabs); i++) taken |= !strcmp(tab_doc(i)->name, d->name);
        if (!taken) break;
        snprintf(d->name, sizeof(d->name), "Untitled %d", k);
    }
    open_doc(d, false);
}

// closing: a tab with unsaved changes asks first (so does quitting, for each of them)
static struct { bool open, quitting; int tab; } ASK;

void app_close_tab(int i) {
    if (i < 0 || i >= arrlen(app.tabs)) return;
    if (!app_tab_modified(i)) { close_now(i); return; }
    app_switch_tab(i);
    ASK.open = true; ASK.tab = i; ASK.quitting = false;
}

// false: something is unsaved, the app asks about it and quits afterwards
bool app_quit_requested(void) {
    for (int i = 0; i < arrlen(app.tabs); i++) if (app_tab_modified(i)) {
        app_switch_tab(i);
        ASK.open = true; ASK.tab = i; ASK.quitting = true;
        return false;
    }
    return true;
}

static void ask_next(void) {   // after an answer while quitting: the next unsaved tab, or quit
    ASK.open = false;
    if (ASK.quitting && app_quit_requested()) sapp_quit();
}

static void save_prompt(void) {
    if (!ASK.open) return;
    if (ASK.tab >= arrlen(app.tabs)) { ASK.open = false; return; }
    ui_overlay(true);
    dl_rect(ui.dl, rect(0, 0, ui.w, ui.h), hexa(0x000000, 0x80));
    Rect w = {roundf((ui.w - 440) / 2), roundf((ui.h - 168) / 2), 440, 168};
    dl_shadow(ui.dl, rect(w.x, w.y + 10, w.w, w.h), 12, 28, hexa(0x000000, 0xa0));
    dl_rrect(ui.dl, w, 12, C_S1);
    dl_rrect_border(ui.dl, w, 12, 1, C_LINE2);
    char t[160];
    snprintf(t, sizeof(t), "Save changes to \"%s\"?", tab_doc(ASK.tab)->name);
    font_draw_in(ui.dl, F_H2, rect(w.x + 24, w.y + 20, w.w - 48, 28), ALIGN_LEFT, t, C_TX);
    font_draw_in(ui.dl, F_UI, rect(w.x + 24, w.y + 52, w.w - 48, 22), ALIGN_LEFT,
                 ASK.quitting ? "The changes are lost if you quit without saving." : "The changes are lost if you close it without saving.", C_TX2);
    Rect bsave = {w.x + w.w - 24 - 96, w.y + w.h - 24 - 34, 96, 34};
    Rect bcancel = {bsave.x - 8 - 88, bsave.y, 88, 34};
    Rect bdrop = {w.x + 24, bsave.y, 112, 34};
    UiResp rs = ui_interact(ui_id("ask-save"), bsave);
    dl_rrect(ui.dl, bsave, 8, rs.hover ? col_mix(C_AC, hex(0xffffff), 0.12f) : C_AC);
    font_draw_in(ui.dl, F_UI_MED, bsave, ALIGN_CENTER, "Save", hex(0xffffff));
    bool cancel = ui_text_button(ui_id("ask-cancel"), bcancel, "Cancel", false);
    bool drop = ui_text_button(ui_id("ask-drop"), bdrop, "Don't Save", false);
    ui_overlay(false);
    if (ui.in.key_pressed[SAPP_KEYCODE_ESCAPE]) cancel = true;
    bool save = rs.clicked || ui.in.key_pressed[SAPP_KEYCODE_ENTER] || ui.in.key_pressed[SAPP_KEYCODE_KP_ENTER];
    if (cancel) { ASK.open = false; ASK.quitting = false; return; }
    if (save) {
        app_switch_tab(ASK.tab);
        if (!app_save(false)) return;   // the save dialog was cancelled: keep asking
        if (ASK.quitting) { ask_next(); return; }
        ASK.open = false;
        close_now(ASK.tab);
        return;
    }
    if (drop) {
        if (ASK.quitting) { tab_store(); app.tabs[ASK.tab]->saved_version = tab_doc(ASK.tab)->version; if (ASK.tab == app.tab) app.saved_version = app.doc->version; ask_next(); return; }
        ASK.open = false;
        close_now(ASK.tab);
    }
}

static bool import_audio(const char *path) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) { app_toast("Can't open %s", base_name(path)); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *data = malloc(n > 0 ? (size_t)n : 1);
    size_t got = fread(data, 1, (size_t)MAX(n, 0), f);
    fclose(f);
    char name[64], err[128] = "";
    snprintf(name, sizeof(name), "%s", base_name(path));
    int id = sound_add(name, data, (u32)got, err, sizeof(err));
    free(data);
    if (!id) { app_toast("%s: %s", name, err); return false; }
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    app_commit();
    Doc *d = app.doc;
    Layer *act = doc_active(d);
    Layer *l = act && act->type == LT_AUDIO && !act->sound ? act : doc_add_layer(d, LT_AUDIO, name);
    if (!arrlen(l->keys)) { Key k = {.f = 1, .d = -1}; arrput(l->keys, k); }
    l->sound = id;
    Sound *so = sound_get(id);
    double secs = so->nframes / (double)so->rate;
    l->len = MAX(l->len, l->keys[0].f + (int)ceil(secs * d->fps) - 1);
    doc_tl(d)->len = MAX(doc_tl(d)->len, l->len);
    doc_tl(d)->active = l->id;
    doc_touch(d);
    app_toast("Added %s (%d:%02d)", so->name, (int)secs / 60, (int)secs % 60);
    return true;
}

bool app_is_image(const char *path) {
    const char *dot = strrchr(path, '.');
    static const char *ext[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd"};
    for (int i = 0; dot && i < COUNT(ext); i++) if (!_stricmp(dot, ext[i])) return true;
    return false;
}

// an image goes into the Library and onto the stage (screen x < 0: the middle of the stage)
bool app_import_image(const char *path, V2 screen) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) { app_toast("Can't open %s", base_name(path)); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *data = malloc(n > 0 ? (size_t)n : 1);
    size_t got = fread(data, 1, (size_t)MAX(n, 0), f);
    fclose(f);
    char err[128] = "";
    int id = bitmap_add_file(base_name(path), data, (u32)got, err, sizeof(err));
    free(data);
    if (!id) { app_toast("%s: %s", base_name(path), err); return false; }
    if (app.tab < 0) app_new();   // from the start screen: into a new document
    doc_add_bitmap(app.doc, id);
    bitmap_place(id, screen);
    Bitmap *b = bitmap_get(id);
    app_toast("Imported %s (%d \xC3\x97 %d)", b->name, b->w, b->h);
    return true;
}

bool app_open_path(const char *path) {
    for (int i = 0; i < arrlen(app.tabs); i++) if (!_stricmp(tab_doc(i)->path, path)) { app_switch_tab(i); return true; }
    const char *dot = strrchr(path, '.');
    char err[200] = "";
    if (dot && (!_stricmp(dot, ".fla") || !_stricmp(dot, ".xfl"))) {
        Doc *d = import_xfl(path, err, sizeof(err));
        if (!d) { app_toast("%s", err); return false; }
        app_set_doc(d);
        recent_add(path);
        app.saved_version = d->version - 1;   // imported, not saved yet
        if (err[0]) app_toast("Imported %s. %s", base_name(path), err);
        else app_toast("Imported %s", base_name(path));
        return true;
    }
    if (dot && (!_stricmp(dot, ".wav") || !_stricmp(dot, ".mp3") || !_stricmp(dot, ".ogg"))) return import_audio(path);
    if (app_is_image(path)) return app_import_image(path, v2(-1, -1));
    if (dot && !_stricmp(dot, ".vxbrush")) return import_vxbrush(path);
    if (dot && (!_stricmp(dot, ".sut") || !_stricmp(dot, ".sutg"))) {
        bool ok = import_sut(path, err, sizeof(err));
        app_toast("%s", err);
        return ok;
    }
    Doc *d = doc_load(path, err, sizeof(err));
    if (!d) { app_toast("%s: %s", base_name(path), err); return false; }
    char name[64];
    snprintf(name, sizeof(name), "%s", base_name(path));
    char *e = strrchr(name, '.'); if (e) *e = 0;
    snprintf(d->name, sizeof(d->name), "%s", name);
    app_set_doc(d);
    recent_add(path);
    return true;
}

bool app_save(bool save_as) {
    Doc *d = app.doc;
    char path[512];
    if (save_as || !d->path[0]) {
        if (!plat_save_dialog(path, sizeof(path), "Vertexa document", "vxa", d->name)) return false;
    } else snprintf(path, sizeof(path), "%s", d->path);
    if (!doc_save(d, path)) { app_toast("Couldn't save %s", base_name(path)); return false; }
    recent_add(path);
    snprintf(d->path, sizeof(d->path), "%s", path);
    char name[64];
    snprintf(name, sizeof(name), "%s", base_name(path));
    char *e = strrchr(name, '.'); if (e) *e = 0;
    snprintf(d->name, sizeof(d->name), "%s", name);
    app.saved_version = d->version;
    app_toast("Saved %s", base_name(path));
    return true;
}

static void open_dialog(void) {
    char path[512];
    if (plat_open_dialog(path, sizeof(path), "Vertexa and Flash documents", "*.vxa;*.fla;*.xfl")) app_open_path(path);
}
static void import_dialog(int kind) {
    char path[512];
    if (kind == 0 && plat_open_dialog(path, sizeof(path), "Flash / Animate documents", "*.fla;*.xfl")) app_open_path(path);
    if (kind == 1 && plat_open_dialog(path, sizeof(path), "Brushes (Vertexa, Clip Studio Paint)", "*.vxbrush;*.sut")) app_open_path(path);
    if (kind == 2 && plat_open_dialog(path, sizeof(path), "Sound files", "*.wav;*.mp3;*.ogg")) app_open_path(path);
    if (kind == 3 && plat_open_dialog(path, sizeof(path), "Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga;*.psd")) app_open_path(path);
}
void app_import_brushes(void) { import_dialog(1); }
static void export_dialog(int kind) {
    char path[512], msg[300] = "";
    bool ok = false;
    if (kind == 0) { if (!plat_pick_folder(path, sizeof(path), "Export PNG sequence to")) return; ok = export_png_sequence(path, msg, sizeof(msg)); }
    else { int ids[64], n = library_selection(ids, 64); exporter_open(ids, n); return; }   // the symbols picked in the Library
    app_toast("%s", msg[0] ? msg : ok ? "Exported" : "Export failed");
}

Color app_color(void) { return app.color_target ? app.stroke : app.fill; }
void app_set_color(Color c) { c |= 0xff000000u; if (app.color_target) app.stroke = c; else app.fill = c; }

void app_play(bool on) {
    Doc *d = app.doc;
    app.playing = on;
    app.play_t = 0;
    if (on && d->loop_on && (doc_tl(d)->frame < d->loop_a || doc_tl(d)->frame > d->loop_b)) doc_tl(d)->frame = d->loop_a;
    if (on && !d->loop_on && doc_tl(d)->frame >= doc_tl(d)->len) doc_tl(d)->frame = 1;
    if (on) audio_play(d, doc_tl(d)->frame);   // sound, if the scene has any, becomes the clock
    else audio_stop();
}
void app_step(int df) { if (app.playing) app_play(false); doc_tl(app.doc)->frame = MAX(1, doc_tl(app.doc)->frame + df); }

static void tick_playback(void) {
    if (!app.playing) return;
    Doc *d = app.doc;
    if (audio_playing() && !d->nedit) {   // the frame follows what is being heard
        int f = 1 + (int)floor(audio_time() * d->fps + 1e-6);
        if (d->loop_on && f > d->loop_b) { doc_tl(d)->frame = d->loop_a; audio_play(d, d->loop_a); }
        else if (!d->loop_on && f > doc_tl(d)->len) { doc_tl(d)->frame = doc_tl(d)->len; app_play(false); }
        else doc_tl(d)->frame = MAX(1, f);
        return;
    }
    app.play_t += ui.dt;
    double spf = 1.0 / d->fps;
    while (app.play_t >= spf) {
        app.play_t -= spf;
        if (d->loop_on) doc_tl(d)->frame = doc_tl(d)->frame >= d->loop_b ? d->loop_a : doc_tl(d)->frame + 1;
        else if (doc_tl(d)->frame >= doc_tl(d)->len) { app_play(false); break; }
        else doc_tl(d)->frame++;
    }
}

/* ── title bar & menus ── */

static void win_glyph(Rect c, int kind, Color col) {
    float cx = roundf(c.x + c.w / 2), cy = roundf(c.y + c.h / 2);
    if (kind == 0) dl_rect(ui.dl, rect(cx - 5, cy, 10, 1), col);
    else if (kind == 1) {
        if (plat_maximized()) { dl_rrect_border(ui.dl, rect(cx - 5, cy - 3, 8, 8), 1, 1, col); dl_rect(ui.dl, rect(cx - 3, cy - 5, 8, 1), col); dl_rect(ui.dl, rect(cx + 4, cy - 5, 1, 8), col); }
        else dl_rrect_border(ui.dl, rect(cx - 5, cy - 5, 10, 10), 1, 1, col);
    } else { dl_line(ui.dl, v2(cx - 5, cy - 5), v2(cx + 5, cy + 5), 1.1f, col); dl_line(ui.dl, v2(cx + 5, cy - 5), v2(cx - 5, cy + 5), 1.1f, col); }
}


static void menu_contents(int m) {
    Doc *d = app.doc;
    switch (m) {
    case 0:
        if (ui_menu_item("New", act_keys(ACT_NEW), true, false)) app_new();
        if (ui_menu_item("Open\xE2\x80\xA6", act_keys(ACT_OPEN), true, false)) open_dialog();
        if (ui_menu_item("Save", act_keys(ACT_SAVE), app.tab >= 0, false)) app_save(false);
        if (ui_menu_item("Save As\xE2\x80\xA6", act_keys(ACT_SAVE_AS), app.tab >= 0, false)) app_save(true);
        if (ui_menu_item("Close", act_keys(ACT_CLOSE), app.tab >= 0, false)) app_close_tab(app.tab);
        ui_menu_sep();
        if (ui_menu_item("Import FLA / XFL\xE2\x80\xA6", NULL, true, false)) import_dialog(0);
        if (ui_menu_item("Import Image\xE2\x80\xA6", act_keys(ACT_IMPORT_IMAGE), true, false)) import_dialog(3);
        if (ui_menu_item("Import Audio\xE2\x80\xA6", NULL, true, false)) import_dialog(2);
        if (ui_menu_item("Import Clip Studio Brush\xE2\x80\xA6", NULL, true, false)) import_dialog(1);
        ui_menu_sep();
        if (ui_menu_item("Export PNG sequence\xE2\x80\xA6", NULL, true, false)) export_dialog(0);
        if (ui_menu_item("Export Symbols (Sparrow, FNF)\xE2\x80\xA6", act_keys(ACT_EXPORT), true, false)) export_dialog(1);
        ui_menu_sep();
        if (ui_menu_item("Exit", "Alt+F4", true, false)) plat_close();
        break;
    case 1:
        if (ui_menu_item("Undo", act_keys(ACT_UNDO), true, false)) app_undo();
        if (ui_menu_item("Redo", act_keys(ACT_REDO), true, false)) app_redo();
        ui_menu_sep();
        if (ui_menu_item("Cut", act_keys(ACT_CUT), select_count() > 0, false)) select_copy(true);
        if (ui_menu_item("Copy", act_keys(ACT_COPY), select_count() > 0, false)) select_copy(false);
        if (ui_menu_item("Paste", act_keys(ACT_PASTE), true, false)) select_paste(false);
        if (ui_menu_item("Paste in Place", act_keys(ACT_PASTE_IN_PLACE), true, false)) select_paste(true);
        if (ui_menu_item("Duplicate", act_keys(ACT_DUPLICATE), select_count() > 0, false)) { select_copy(false); select_paste(false); }
        if (ui_menu_item("Delete", act_keys(ACT_DELETE), select_count() > 0, false)) select_delete();
        ui_menu_sep();
        if (ui_menu_item("Select All", act_keys(ACT_SELECT_ALL), true, false)) { app.tool = 'V'; select_all(); }
        ui_menu_sep();
        if (ui_menu_item("Settings\xE2\x80\xA6", act_keys(ACT_SETTINGS), true, false)) settings_open(SET_GENERAL);
        break;
    case 2:
        if (ui_menu_item("Zoom In", act_keys(ACT_ZOOM_IN), true, false)) stage_zoom_by(1.25f);
        if (ui_menu_item("Zoom Out", act_keys(ACT_ZOOM_OUT), true, false)) stage_zoom_by(0.8f);
        if (ui_menu_item("Fit to Window", act_keys(ACT_FIT), true, false)) stage_fit();
        if (ui_menu_item("Actual Size", act_keys(ACT_ACTUAL), true, false)) stage_zoom_100();
        ui_menu_sep();
        if (ui_menu_item("Onion Skin", act_keys(ACT_ONION), true, d->onion_on)) d->onion_on = !d->onion_on;
        break;
    case 3: {
        if (ui_menu_item("New Layer", act_keys(ACT_NEW_LAYER), true, false)) { app_commit(); Layer *l = doc_add_layer(app.doc, LT_VECTOR, NULL); doc_tl(app.doc)->active = l->id; }
        if (ui_menu_item("New Folder", NULL, true, false)) { app_commit(); Layer *l = doc_add_layer(app.doc, LT_FOLDER, NULL); doc_tl(app.doc)->active = l->id; }
        ui_menu_sep();
        if (ui_menu_item("Frame", act_keys(ACT_FRAME), true, false)) timeline_op(0);
        if (ui_menu_item("Keyframe", act_keys(ACT_KEYFRAME), true, false)) timeline_op(2);
        if (ui_menu_item("Blank Keyframe", act_keys(ACT_BLANK_KEY), true, false)) timeline_op(3);
        ui_menu_sep();
        if (ui_menu_item("Classic Tween", NULL, true, false)) timeline_op(5);
        if (ui_menu_item("Shape Tween", NULL, true, false)) timeline_op(6);
        break;
    }
    case 4:
        if (ui_menu_item("Convert to Symbol", act_keys(ACT_SYMBOL), select_count() > 0, false)) select_convert_to_symbol();
        if (ui_menu_item("Break Apart", act_keys(ACT_BREAK), select_count() > 0, false)) select_break_apart();
        if (ui_menu_item("Back to Scene", "Esc", d->nedit > 0, false)) { while (d->nedit) doc_exit_symbol(d); select_clear(); }
        ui_menu_sep();
        if (ui_menu_item("Swap Fill and Stroke", act_keys(ACT_SWAP_COLORS), true, false)) { Color t = app.fill; app.fill = app.stroke; app.stroke = t; }
        break;
    case 5:
        if (ui_menu_item(app.playing ? "Stop" : "Play", act_keys(ACT_PLAY), true, false)) app_play(!app.playing);
        if (ui_menu_item("Loop Playback", act_keys(ACT_LOOP), true, d->loop_on)) d->loop_on = !d->loop_on;
        ui_menu_sep();
        if (ui_menu_item("Previous Frame", act_keys(ACT_PREV), true, false)) app_step(-1);
        if (ui_menu_item("Next Frame", act_keys(ACT_NEXT), true, false)) app_step(1);
        if (ui_menu_item("First Frame", act_keys(ACT_FIRST), true, false)) doc_tl(d)->frame = 1;
        if (ui_menu_item("Last Frame", act_keys(ACT_LAST), true, false)) doc_tl(d)->frame = doc_tl(d)->len;
        break;
    case 6:
        if (ui_menu_item("Keyboard Shortcuts", act_keys(ACT_SHORTCUTS), true, false)) settings_open(SET_KEYS);
        if (ui_menu_item("About Vertexa", NULL, true, false)) app_toast("Vertexa Alpha. Vector animation drawn on the GPU");
        break;
    }
}

static const int MENU_ITEMS[] = {13, 10, 6, 8, 5, 7, 2};
static const int MENU_SEPS[] = {4, 3, 1, 2, 1, 1, 0};

static void tab_close_glyph(Rect b, Color c) {
    float cx = roundf(b.x + b.w / 2), cy = roundf(b.y + b.h / 2);
    dl_line(ui.dl, v2(cx - 3.5f, cy - 3.5f), v2(cx + 3.5f, cy + 3.5f), 1.2f, c);
    dl_line(ui.dl, v2(cx + 3.5f, cy - 3.5f), v2(cx - 3.5f, cy + 3.5f), 1.2f, c);
}

static void title_bar(Rect r) {
    dl_rect(ui.dl, r, C_BG);
    dl_rect(ui.dl, rect(r.x, r.y + r.h - 1, r.w, 1), C_LINE);
    Rect holes[96]; int nh = 0;
    // the logo and the name: back to the start screen
    int mh = (int)roundf(22 * ui.scale);
    float mw = logo_mark_width(mh) / ui.scale;
    Rect home = {r.x + 6, r.y + 4, 8 + mw + 9 + font_width(F_TITLE, "vertexa") + 8, r.h - 8};
    UiResp hr = ui_interact(ui_id("home"), home);
    if (hr.hover && !app.home) dl_rrect(ui.dl, home, 6, C_S2);
    float x = home.x + 8;
    dl_image(ui.dl, rect(x, r.y + 7, mw, mh / ui.scale), logo_mark_view(mh), hex(0xffffff), 0, 0, 1, 1);
    x += mw + 9;
    font_draw(ui.dl, F_TITLE, x, r.y + 23, "vertexa", C_TX);
    ui_tooltip(ui_id("home"), home, "Start screen");
    if (hr.clicked && !app.home) { if (app.playing) app_play(false); app.home = true; home_prepare(); }
    holes[nh++] = home;
    x = home.x + home.w + 6;
    static const char *menus[] = {"File", "Edit", "View", "Insert", "Modify", "Control", "Help"};
    for (int i = 0; i < COUNT(menus); i++) {
        float w = font_width(F_UI, menus[i]) + 16;
        Rect m = {x, r.y + 6, w, 24};
        UiId id = ui_idi("menu", i);
        UiResp re = ui_interact(id, m);
        bool open = ui_popup_is_open(id);
        // moving across the bar while a menu is open switches menus
        if (re.hover && ui.popup && !open) {
            for (int j = 0; j < COUNT(menus); j++) if (ui_popup_is_open(ui_idi("menu", j))) { ui_open_popup(id, m); open = true; }
        }
        if (open || re.hover) dl_rrect(ui.dl, m, 5, open ? C_S3 : C_S2);
        font_draw_in(ui.dl, F_UI, m, ALIGN_CENTER, menus[i], open || re.hover ? C_TX : C_TX2);
        if (re.pressed) { if (open) ui_close_popup(); else ui_open_popup(id, m); }
        if (ui_popup_begin(id, rect(m.x, m.y + m.h + 4, 270, 10 + 28 * MENU_ITEMS[i] + 9 * MENU_SEPS[i]))) { menu_contents(i); ui_popup_end(); }
        holes[nh++] = m;
        x += w + 2;
    }
    // one tab per open document; drag to reorder, middle-click or the cross closes
    x += 12;
    int n = (int)arrlen(app.tabs);
    float room = r.x + r.w - 46 * 3 - 48 - x;   // keep some bar to drag the window by
    float want[64], total = 0;
    char names[64][72];
    for (int i = 0; i < n && i < 64; i++) {
        snprintf(names[i], sizeof(names[i]), "%s%s", tab_doc(i)->name, app_tab_modified(i) ? "*" : "");
        want[i] = clampf(font_width(F_UI_MED, names[i]) + 16 + 30, 96, 240);
        total += want[i] + 2;
    }
    float k = total > room && total > 0 ? MAX(0.3f, room / total) : 1;
    static int drag = -1; static float drag_dx;
    int close = -1, menu_tab = -1;
    dl_push_clip(ui.dl, rect(r.x, r.y, r.w - 46 * 3, r.h));
    for (int i = 0; i < n && i < 64; i++) {
        float tw = floorf(want[i] * k);
        Rect tab = {x, r.y + 6, tw, r.h - 6};
        UiId id = ui_idi("doctab", app.tabs[i]->uid);   // follows the tab while it is dragged along
        UiResp re = ui_interact(id, tab);
        bool on = i == app.tab && !app.home;
        Rect xb = {tab.x + tab.w - 26, tab.y + (tab.h - 20) / 2, 20, 20};
        UiId xid = ui_idi("doctab-x", app.tabs[i]->uid);
        bool show_x = on || re.hover;
        UiResp xr = show_x ? ui_interact(xid, xb) : (UiResp){0};
        if (on) {
            dl_rrect(ui.dl, rect(tab.x, tab.y, tab.w, tab.h + 8), 7, C_S1);
            dl_rrect_border(ui.dl, rect(tab.x, tab.y, tab.w, tab.h + 8), 7, 1, C_LINE2);
            dl_rect(ui.dl, rect(tab.x + 1, r.y + r.h - 1, tab.w - 2, 1), C_S1);
        } else if (re.hover || drag == i) dl_rrect(ui.dl, rect(tab.x, tab.y, tab.w, tab.h + 8), 7, C_S2);
        dl_push_clip(ui.dl, rect(tab.x + 12, tab.y, tab.w - 12 - (show_x ? 28 : 8), tab.h));
        font_draw_in(ui.dl, F_UI_MED, rect(tab.x + 12, tab.y, 400, tab.h), ALIGN_LEFT, names[i], on || re.hover ? C_TX : C_TX2);
        dl_pop_clip(ui.dl);
        if (show_x) {
            if (xr.hover) dl_rrect(ui.dl, xb, 5, C_S3);
            tab_close_glyph(xb, xr.hover ? C_TX : C_TX3);
            if (xr.clicked) close = i;
        }
        static char tip[520];
        if (re.hover) { snprintf(tip, sizeof(tip), "%s", tab_doc(i)->path[0] ? tab_doc(i)->path : tab_doc(i)->name); ui_tooltip(id, tab, tip); }
        if (re.pressed && !xr.hover) { app_switch_tab(i); drag = i; drag_dx = 0; }
        if (rect_has(tab, ui.in.mouse) && ui.in.pressed[MB_MIDDLE] && ui_mouse_free()) close = i;
        if (re.right) { menu_tab = i; ui_open_popup(ui_id("tab-menu"), rect(ui.in.mouse.x, ui.in.mouse.y, 1, 1)); }
        // reordering: past the middle of a neighbour, swap with it
        if (drag == i && re.held) {
            drag_dx += re.delta.x;
            if (drag_dx > tw / 2 && i + 1 < n) { DocTab *q = app.tabs[i]; app.tabs[i] = app.tabs[i + 1]; app.tabs[i + 1] = q; if (app.tab == i) app.tab = i + 1; else if (app.tab == i + 1) app.tab = i; drag = i + 1; drag_dx -= tw; }
            else if (drag_dx < -tw / 2 && i > 0) { DocTab *q = app.tabs[i]; app.tabs[i] = app.tabs[i - 1]; app.tabs[i - 1] = q; if (app.tab == i) app.tab = i - 1; else if (app.tab == i - 1) app.tab = i; drag = i - 1; drag_dx += tw; }
        }
        holes[nh++] = tab;
        x += tw + 2;
    }
    if (drag >= 0 && !ui.in.down[0]) drag = -1;
    dl_pop_clip(ui.dl);
    static int menu_for = -1;
    if (menu_tab >= 0) menu_for = menu_tab;
    if (ui_popup_begin(ui_id("tab-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 220, 10 + 28 * 3))) {
        if (ui_menu_item("Close", act_keys(ACT_CLOSE), menu_for >= 0 && menu_for < arrlen(app.tabs), false)) close = menu_for;
        if (ui_menu_item("Close Others", NULL, arrlen(app.tabs) > 1, false)) {
            Doc *keep = tab_doc(menu_for);
            for (int i = (int)arrlen(app.tabs) - 1; i >= 0; i--) if (tab_doc(i) != keep && !app_tab_modified(i)) close_now(i);
        }
        if (ui_menu_item("Close Saved", NULL, true, false)) {
            for (int i = (int)arrlen(app.tabs) - 1; i >= 0; i--) if (!app_tab_modified(i)) close_now(i);
        }
        ui_popup_end();
    }
    if (close >= 0) app_close_tab(close);
    // window title: the document and the app, plain
    static char last_title[160];
    char title[160];
    if (app.home || app.tab < 0) snprintf(title, sizeof(title), "Vertexa");
    else snprintf(title, sizeof(title), "%s%s - Vertexa", app.doc->name, app_tab_modified(app.tab) ? "*" : "");
    if (strcmp(title, last_title)) { plat_set_title(title); snprintf(last_title, sizeof(last_title), "%s", title); }
    // window buttons
    float bx = r.x + r.w - 46 * 3;
    for (int i = 0; i < 3; i++) {
        Rect b = {bx + i * 46, r.y, 46, r.h - 1};
        UiResp re = ui_interact(ui_idi("winbtn", i), b);
        if (re.hover) dl_rect(ui.dl, b, i == 2 ? hex(0xc42b1c) : C_S2);
        win_glyph(b, i, re.hover ? hex(0xffffff) : C_TX3);
        if (re.clicked) { if (i == 0) plat_minimize(); else if (i == 1) plat_toggle_maximize(); else plat_close(); }
        holes[nh++] = b;
    }
    plat_set_caption(r, holes, nh);
}

/* ── tools column ── */

static void tools_column(Rect r) {
    static float scroll;
    dl_rect(ui.dl, r, C_S1);
    dl_rect(ui.dl, rect(app.layout.tools_right ? r.x : r.x + r.w - 1, r.y, 1, r.h), C_LINE);
    float total = 20;
    for (int i = 0; i < COUNT(TOOLS); i++) total += TOOLS[i].k ? 34 : 13;
    if (rect_has(r, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y) scroll -= ui.in.wheel_y * 34;
    scroll = clampf(scroll, 0, MAX(0, total - r.h));
    dl_push_clip(ui.dl, r);
    float y = r.y + 10 - scroll;
    for (int i = 0; i < COUNT(TOOLS); i++) {
        if (!TOOLS[i].k) { dl_rect(ui.dl, rect(r.x + 15, y + 6, 22, 1), C_LINE2); y += 13; continue; }
        Rect b = {r.x + 8, y, 36, 32};
        UiId id = ui_idi("tool", TOOLS[i].k);
        UiResp re = ui_interact(id, b);
        bool on = app.tool == TOOLS[i].k;
        if (on) dl_rrect(ui.dl, b, 7, C_AC); else if (re.hover) dl_rrect(ui.dl, b, 7, C_S3);
        Color tc = on ? hex(0xffffff) : re.hover ? C_TX : C_TX2;
        icon_draw(ui.dl, TOOLS[i].icon, v2(b.x + b.w / 2, b.y + b.h / 2), ICON_L, TOOLS[i].fill, tc);
        if (TOOLS[i].grp) dl_tri(ui.dl, v2(b.x + b.w - 3, b.y + b.h - 7), v2(b.x + b.w - 3, b.y + b.h - 3), v2(b.x + b.w - 7, b.y + b.h - 3), col_alpha(tc, 0.45f));
        static char tip[64];
        if (ui.hot_next == id) { const char *k = act_keys(TOOLS[i].act); snprintf(tip, sizeof(tip), "%s%s%s", TOOLS[i].name, k ? "  " : "", k ? k : ""); ui_tooltip(id, b, tip); }
        if (re.clicked) app.tool = TOOLS[i].k;
        y += 34;
    }
    dl_pop_clip(ui.dl);
}

/* ── overlays ── */

static void toast_overlay(Rect stage) {
    if (ui.time > app.toast_until || !app.toast[0]) return;
    float a = clampf((float)(app.toast_until - ui.time) / 0.3f, 0, 1);
    float w = font_width(F_UI, app.toast) + 28;
    Rect r = {roundf(stage.x + (stage.w - w) / 2), stage.y + stage.h - 56, w, 32};
    ui_overlay(true);
    dl_shadow(ui.dl, rect(r.x, r.y + 6, r.w, r.h), 8, 14, hexa(0x000000, (u8)(140 * a)));
    dl_rrect(ui.dl, r, 8, col_alpha(C_S4, a));
    font_draw_in(ui.dl, F_UI, rect_inset(r, 14, 0), ALIGN_LEFT, app.toast, col_alpha(C_TX, a));
    ui_overlay(false);
}

/* ── shortcuts ── */

static void shortcuts(void) {
    Input *in = &ui.in;
    if (ui.focus || app.settings || exporter_is_open() || ASK.open) return;
    if (act(ACT_NEXT_TAB) && arrlen(app.tabs)) app_switch_tab(app.home ? MAX(app.tab, 0) : (app.tab + 1) % (int)arrlen(app.tabs));
    if (act(ACT_PREV_TAB) && arrlen(app.tabs)) app_switch_tab(app.home ? MAX(app.tab, 0) : (app.tab + (int)arrlen(app.tabs) - 1) % (int)arrlen(app.tabs));
    if (act(ACT_IMPORT_IMAGE)) import_dialog(3);
    if (app.home) {
        if (act(ACT_NEW)) app_new();
        if (act(ACT_OPEN)) open_dialog();
        if (act(ACT_SETTINGS)) settings_open(SET_GENERAL);
        if (act(ACT_SHORTCUTS)) settings_open(SET_KEYS);
        return;
    }
    if (act(ACT_UNDO)) app_undo();
    if (act(ACT_REDO)) app_redo();
    if (act(ACT_NEW)) app_new();
    if (act(ACT_SAVE)) app_save(false);
    if (act(ACT_SAVE_AS)) app_save(true);
    if (act(ACT_CLOSE)) { app_close_tab(app.tab); return; }
    if (act(ACT_EXPORT)) export_dialog(1);
    if (act(ACT_OPEN)) open_dialog();
    if (act(ACT_SETTINGS)) settings_open(SET_GENERAL);
    if (act(ACT_SHORTCUTS)) settings_open(SET_KEYS);
    if (act(ACT_NEW_LAYER)) { app_commit(); Layer *l = doc_add_layer(app.doc, LT_VECTOR, NULL); doc_tl(app.doc)->active = l->id; }
    if (act(ACT_FIT)) stage_fit();
    if (act(ACT_ACTUAL)) stage_zoom_100();
    if (act(ACT_ZOOM_IN)) stage_zoom_by(1.25f);
    if (act(ACT_ZOOM_OUT)) stage_zoom_by(0.8f);
    if (act(ACT_ONION)) app.doc->onion_on = !app.doc->onion_on;
    if (act(ACT_LOOP)) app.doc->loop_on = !app.doc->loop_on;
    for (int i = 0; i < COUNT(TOOLS); i++) if (TOOLS[i].k && act(TOOLS[i].act)) app.tool = TOOLS[i].k;
    float *size = app.tool == 'E' ? &app.eraser_size : app.tool == 'Y' ? &app.pencil_size : &app.brush_size;
    if (act(ACT_SMALLER)) *size = MAX(1, roundf(*size / 1.15f));
    if (act(ACT_BIGGER)) *size = MIN(400, roundf(*size * 1.15f + 0.5f));
    if (act(ACT_SWAP_COLORS)) { Color t = app.fill; app.fill = app.stroke; app.stroke = t; }
    if (act(ACT_OBJECT_DRAW)) app.object_draw = !app.object_draw;
    if (in->key_pressed[SAPP_KEYCODE_ESCAPE] && !in->mods) app.doc->sel.on = false;
    select_shortcuts();
    timeline_shortcuts();
}

/* ── auto-save and recovery copies ── */

static bool recovery_path(Doc *d, char *out, int cap) {
    char dir[1024];
    if (!plat_config_path("recovery", dir, sizeof(dir)) || !plat_mkdirs(dir)) return false;
    char name[64];
    snprintf(name, sizeof(name), "%s", d->name[0] ? d->name : "Untitled");
    for (char *c = name; *c; c++) if (strchr("\\/:*?\"<>|", *c) || (u8)*c < 0x20) *c = '-';
    snprintf(out, (size_t)cap, "%s\\%s.vxa", dir, name);
    return true;
}

static void recovery_remove(Doc *d) {
    char path[1100];
    if (d && recovery_path(d, path, sizeof(path))) plat_remove(path);
}
void recovery_cleanup(void) { for (int i = 0; i < arrlen(app.tabs); i++) recovery_remove(tab_doc(i)); }

static double t_save, t_rec;
void app_autosave_due(void) { t_save = t_rec = 1e9; }   // self-tests: run both timers now

static void autosave_tick(void) {
    bool busy = ui.in.down[0] || export_busy();
    bool save_now = app.autosave_min > 0 && (t_save += ui.dt) >= app.autosave_min * 60.0 && !busy;
    bool rec_now = app.recovery_min > 0 && (t_rec += ui.dt) >= app.recovery_min * 60.0 && !busy;
    if (!save_now && !rec_now) return;
    if (save_now) t_save = 0;
    if (rec_now) t_rec = 0;
    tab_store();
    for (int i = 0; i < arrlen(app.tabs); i++) {   // every open document, not just the one shown
        DocTab *t = app.tabs[i];
        Doc *d = t->doc;
        bool modified = d->version != t->saved_version;
        if (save_now && modified && d->path[0] && doc_save(d, d->path)) { t->saved_version = d->version; modified = false; app_toast("Auto-saved %s", base_name(d->path)); }
        char path[1100];
        if (rec_now && modified && d->version != t->rec_version && recovery_path(d, path, sizeof(path)) && doc_save(d, path)) t->rec_version = d->version;
    }
    if (app.tab >= 0) app.saved_version = app.tabs[app.tab]->saved_version;
}

/* ── frame ── */

void app_frame(void) {
    ui.modal = app.settings || exporter_is_open() || ASK.open;   // the base layer gets no mouse while these are open
    tick_playback();
    autosave_tick();
    layout_tick();
    shortcuts();
    Rect full = rect(0, 0, ui.w, ui.h);
    dl_rect(ui.dl, full, C_BG);
    Rect title = cut_top(&full, M_TITLE_H);
    if (app.home) {
        home_frame(full);
        title_bar(title);
        toast_overlay(full);
        settings_window();
        save_prompt();
        return;
    }
    // where the parts go: Settings > Layout
    const Layout *L = &app.layout;
    app.layout.panel_w = clampf(L->panel_w, 260, 420);
    Rect right = L->panel_left ? cut_left(&full, L->panel_w) : cut_right(&full, L->panel_w);
    app.tl_h = clampf(app.tl_h, 150, MAX(150, ui.h - 260));
    Rect tl = L->timeline_top ? cut_top(&full, app.tl_h) : cut_bottom(&full, app.tl_h);
    Rect tools = L->tools_right ? cut_right(&full, M_TOOLS_W) : cut_left(&full, M_TOOLS_W);
    Rect ctx = cut_top(&full, M_CTX_H);
    Rect stage = full;

    stage_ui(stage);
    context_bar(ctx);
    tools_column(tools);
    timeline_ui(tl);
    if (L->timeline_top) dl_rect(ui.dl, rect(tl.x, tl.y + tl.h - 1, tl.w, 1), C_LINE);

    // the timeline's and the panel's inner edges drag
    Rect split = {tl.x, L->timeline_top ? tl.y + tl.h - 3 : tl.y - 3, tl.w, 6};
    UiResp sp = ui_interact(ui_id("tl-split"), split);
    if (sp.hover || sp.held) ui_cursor(SAPP_MOUSECURSOR_RESIZE_NS);
    if (sp.held) app.tl_h += L->timeline_top ? sp.delta.y : -sp.delta.y;
    Rect psplit = {L->panel_left ? right.x + right.w - 3 : right.x - 3, right.y, 6, right.h};
    UiResp pp = ui_interact(ui_id("panel-split"), psplit);
    if (pp.hover || pp.held) ui_cursor(SAPP_MOUSECURSOR_RESIZE_EW);
    if (pp.held) app.layout.panel_w = clampf(L->panel_w + (L->panel_left ? pp.delta.x : -pp.delta.x), 260, 420);

    dl_rect(ui.dl, right, C_S1);
    dl_rect(ui.dl, rect(L->panel_left ? right.x + right.w - 1 : right.x, right.y, 1, right.h), C_LINE);
    if (!L->panel_left) right.x += 1;
    right.w -= 1;
    int per_row = MAX(1, (int)((right.w - 32 + 7) / 27));
    int rows = (app.nswatches + 1 + per_row - 1) / per_row;
    float col_h = 16 + 4 + 204 + 16 + 30 + 14 + rows * 27 - 7 + 16;
    Rect col = L->color_below ? cut_bottom(&right, col_h) : cut_top(&right, col_h);
    if (L->color_below) dl_rect(ui.dl, rect(col.x, col.y, col.w, 1), C_LINE);
    color_panel(col);
    right_panel(right);

    title_bar(title);
    exporter_window();
    toast_overlay(stage);
    settings_window();
    save_prompt();
}
