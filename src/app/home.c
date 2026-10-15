// Start screen and splash. A splash while Vertexa gets ready, then recent animations (with a picture of
// their first frame) and a new one in a size that fits FNF or video. Recent files are listed in
// %APPDATA%/Vertexa/recent.txt (time<TAB>path), their pictures are %APPDATA%/Vertexa/thumbs/<hash>.png.
#include "app/app.h"
#include "gfx/logo.h"
#include "platform/platform.h"
#include "stb/stb_image.h"
#include "sokol/sokol_app.h"
#include <time.h>
#include <ctype.h>

typedef struct { char path[512]; long long when; bool missing, tried; sg_image img; sg_view view; int tw, th; } Recent;

static struct {
    Recent *items;
    bool loaded;
    int preset;
    char cw[8], ch[8], cfps[8];
    float scroll;
    int menu;   // recent item with its menu open
} H = {.cw = "1280", .ch = "720", .cfps = "24"};

static const struct { const char *name; int w, h, fps; } PRESETS[] = {
    {"FNF character", 1280, 720, 24}, {"Full HD", 1920, 1080, 24}, {"Square", 1080, 1080, 24}, {"Custom", 0, 0, 0},
};

/* ── the list ── */

static u64 hash_path(const char *s) { u64 h = 1469598103934665603ull; for (; *s; s++) { h ^= (u8)(*s == '/' ? '\\' : tolower((u8)*s)); h *= 1099511628211ull; } return h; }
static bool thumb_path(const char *doc, char *out, int cap) {
    char dir[1024];
    if (!plat_config_path("thumbs", dir, sizeof(dir)) || !plat_mkdirs(dir)) return false;
    snprintf(out, (size_t)cap, "%s\\%016llx.png", dir, (unsigned long long)hash_path(doc));
    return true;
}

static void free_thumb(Recent *r) {
    if (r->view.id) sg_destroy_view(r->view);
    if (r->img.id) sg_destroy_image(r->img);
    r->view.id = r->img.id = 0; r->tried = false;
}

static void save_list(void) {
    char path[1024];
    if (!plat_config_path("recent.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    for (ptrdiff_t i = 0; i < arrlen(H.items); i++) fprintf(f, "%lld\t%s\n", H.items[i].when, H.items[i].path);
    fclose(f);
}

static void load_list(void) {
    for (ptrdiff_t i = 0; i < arrlen(H.items); i++) free_thumb(&H.items[i]);
    arrsetlen(H.items, 0);
    char path[1024];
    if (!plat_config_path("recent.txt", path, sizeof(path))) return;
    FILE *f = plat_fopen(path, "rb");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof(line), f) && arrlen(H.items) < 24) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        Recent r = {0};
        r.when = atoll(line);
        snprintf(r.path, sizeof(r.path), "%s", tab + 1);
        for (char *c = r.path; *c; c++) if (*c == '\r' || *c == '\n') *c = 0;
        if (!r.path[0]) continue;
        FILE *t = plat_fopen(r.path, "rb");
        r.missing = !t;
        if (t) fclose(t);
        arrput(H.items, r);
    }
    fclose(f);
}

void recent_add(const char *path) {
    if (!H.loaded) { load_list(); H.loaded = true; }
    for (ptrdiff_t i = arrlen(H.items) - 1; i >= 0; i--) if (!_stricmp(H.items[i].path, path)) { free_thumb(&H.items[i]); arrdel(H.items, i); }
    Recent r = {0};
    snprintf(r.path, sizeof(r.path), "%s", path);
    r.when = (long long)time(NULL);
    arrins(H.items, 0, r);
    while (arrlen(H.items) > 24) { free_thumb(&arrlast(H.items)); arrpop(H.items); }
    save_list();
    char png[1100];   // its picture: the first frame, rendered in the background
    if (thumb_path(path, png, sizeof(png))) export_thumbnail(png);
}

void home_prepare(void) { load_list(); H.loaded = true; }

void home_thumb_ready(const char *png) {
    for (ptrdiff_t i = 0; i < arrlen(H.items); i++) {
        char p[1100];
        if (thumb_path(H.items[i].path, p, sizeof(p)) && !_stricmp(p, png)) free_thumb(&H.items[i]);   // reload it
    }
}

static void load_thumb(Recent *r) {
    if (r->tried) return;
    r->tried = true;
    char png[1100];
    if (!thumb_path(r->path, png, sizeof(png))) return;
    FILE *f = plat_fopen(png, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *data = malloc(n > 0 ? (size_t)n : 1);
    size_t got = fread(data, 1, (size_t)MAX(n, 0), f);
    fclose(f);
    int w, h, c;
    u8 *px = stbi_load_from_memory(data, (int)got, &w, &h, &c, 4);
    free(data);
    if (!px) return;
    for (int i = 0; i < w * h; i++) { u8 *p = px + i * 4; for (int k = 0; k < 3; k++) p[k] = (u8)(p[k] * p[3] / 255); }   // premultiplied
    r->img = sg_make_image(&(sg_image_desc){.width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8, .data.mip_levels[0] = {px, (size_t)w * h * 4}});
    r->view = sg_make_view(&(sg_view_desc){.texture.image = r->img});
    r->tw = w; r->th = h;
    stbi_image_free(px);
}

static void when_text(long long when, char *out, int cap) {
    long long d = (long long)time(NULL) - when;
    if (d < 60) snprintf(out, (size_t)cap, "just now");
    else if (d < 3600) snprintf(out, (size_t)cap, "%lld min ago", d / 60);
    else if (d < 86400) snprintf(out, (size_t)cap, "%lld h ago", d / 3600);
    else if (d < 86400 * 7) snprintf(out, (size_t)cap, "%lld day%s ago", d / 86400, d / 86400 == 1 ? "" : "s");
    else {
        time_t t = (time_t)when;
        struct tm *tm = localtime(&t);
        strftime(out, (size_t)cap, "%d %b %Y", tm);
    }
}

/* ── the screen ── */

static bool big_button(UiId id, Rect r, const char *label, IconId ic, bool primary) {
    UiResp re = ui_interact(id, r);
    Color bg = primary ? (re.hover ? col_mix(C_AC, hex(0xffffff), 0.12f) : C_AC) : re.hover ? C_S3 : C_S2;
    dl_rrect(ui.dl, r, 8, bg);
    if (!primary) dl_rrect_border(ui.dl, r, 8, 1, C_LINE2);
    Color tc = primary ? hex(0xffffff) : re.hover ? C_TX : C_TX2;
    icon_draw(ui.dl, ic, v2(r.x + 22, r.y + r.h / 2), ICON_M, false, tc);
    font_draw_in(ui.dl, F_UI_MED, rect(r.x + 42, r.y, r.w - 50, r.h), ALIGN_LEFT, label, tc);
    return re.clicked;
}

static void new_document(void) {
    int w = PRESETS[H.preset].w, h = PRESETS[H.preset].h, fps = PRESETS[H.preset].fps;
    if (!w) { w = CLAMP(atoi(H.cw), 16, 8192); h = CLAMP(atoi(H.ch), 16, 8192); fps = CLAMP(atoi(H.cfps), 1, 120); }
    Doc *d = doc_new();
    d->w = w; d->h = h; d->fps = fps;
    d->loop_b = fps; d->scene.len = MAX(d->scene.len, fps);
    app_set_doc(d);
}

void home_frame(Rect r) {
    if (!H.loaded) home_prepare();
    dl_rect(ui.dl, r, C_BG);
    float pad = 56, lx = r.x + pad, lw = 300, y = r.y + 52;

    // who we are
    int mh = (int)roundf(56 * ui.scale);
    float mw = logo_mark_width(mh) / ui.scale;
    dl_image(ui.dl, rect(lx, y, mw, mh / ui.scale), logo_mark_view(mh), hex(0xffffff), 0, 0, 1, 1);
    font_draw(ui.dl, F_DISPLAY, lx + mw + 16, y + 36, "Vertexa", C_TX);
    float tw = font_width(F_DISPLAY, "Vertexa");
    Rect pill = {lx + mw + 16 + tw + 10, y + 16, font_width(F_SMALL_SEMI, "ALPHA") + 14, 18};
    dl_rrect(ui.dl, pill, 9, C_AC2);
    dl_rrect_border(ui.dl, pill, 9, 1, C_AC3);
    font_draw_in(ui.dl, F_SMALL_SEMI, pill, ALIGN_CENTER, "ALPHA", C_AC);
    y += 56 + 34;

    // start
    if (big_button(ui_id("home-new"), rect(lx, y, lw, 44), "New animation", IC_PLUS, true)) new_document();
    y += 44 + 10;
    if (big_button(ui_id("home-open"), rect(lx, y, lw, 40), "Open\xE2\x80\xA6", IC_OPEN, false)) {
        char path[512];
        if (plat_open_dialog(path, sizeof(path), "Vertexa and Flash documents", "*.vxa;*.fla;*.xfl")) app_open_path(path);
    }
    y += 40 + 8;
    if (big_button(ui_id("home-import"), rect(lx, y, lw, 40), "Import FLA / XFL\xE2\x80\xA6", IC_IMPORT, false)) {
        char path[512];
        if (plat_open_dialog(path, sizeof(path), "Flash / Animate documents", "*.fla;*.xfl")) app_open_path(path);
    }
    y += 40 + 34;

    // the size of a new animation
    font_draw_in(ui.dl, F_UI_SEMI, rect(lx, y, lw, 16), ALIGN_LEFT, "New animation size", C_TX2);
    y += 16 + 10;
    for (int i = 0; i < COUNT(PRESETS); i++) {
        Rect row = {lx, y, lw, 36};
        UiResp re = ui_interact(ui_idi("home-preset", i), row);
        bool on = H.preset == i;
        if (on) { dl_rrect(ui.dl, row, 7, C_S2); dl_rrect_border(ui.dl, row, 7, 1, C_LINE2); }
        else if (re.hover) dl_rrect(ui.dl, row, 7, C_S1);
        V2 c = {row.x + 18, row.y + 18};
        dl_circle_border(ui.dl, c, 6, 1.5f, on ? C_AC : C_TX3);
        if (on) dl_circle(ui.dl, c, 3, C_AC);
        font_draw_in(ui.dl, F_UI, rect(row.x + 34, row.y, 140, row.h), ALIGN_LEFT, PRESETS[i].name, on || re.hover ? C_TX : C_TX2);
        if (PRESETS[i].w) {
            char s[48]; snprintf(s, sizeof(s), "%d \xC3\x97 %d, %d fps", PRESETS[i].w, PRESETS[i].h, PRESETS[i].fps);
            font_draw_in(ui.dl, F_MONO, rect(row.x, row.y, row.w - 12, row.h), ALIGN_RIGHT, s, C_TX3);
        }
        if (re.clicked) H.preset = i;
        y += 38;
    }
    if (PRESETS[H.preset].w == 0) {   // custom: width, height, frame rate
        float fx = lx + 34;
        ui_text_field(ui_id("home-w"), rect(fx, y, 74, 30), H.cw, sizeof(H.cw), "W");
        font_draw_in(ui.dl, F_UI, rect(fx + 74, y, 20, 30), ALIGN_CENTER, "\xC3\x97", C_TX3);
        ui_text_field(ui_id("home-h"), rect(fx + 94, y, 74, 30), H.ch, sizeof(H.ch), "H");
        ui_text_field(ui_id("home-fps"), rect(fx + 178, y, 88, 30), H.cfps, sizeof(H.cfps), "fps");
    }

    // recent
    float rx = lx + lw + 72, rw = r.x + r.w - pad - rx, ry = r.y + 52;
    dl_rect(ui.dl, rect(rx - 36, r.y + 40, 1, r.h - 80), C_LINE);
    font_draw(ui.dl, F_H2, rx, ry + 20, "Recent", C_TX);
    if (arrlen(H.items)) {
        char n[16]; snprintf(n, sizeof(n), "%d", (int)arrlen(H.items));
        font_draw(ui.dl, F_MONO, rx + font_width(F_H2, "Recent") + 10, ry + 19, n, C_TX3);
    }
    ry += 44;
    Rect area = {rx, ry, rw, r.y + r.h - ry - 24};
    if (!arrlen(H.items)) {
        font_draw_in(ui.dl, F_UI_MED, rect(area.x, area.y + 40, area.w, 20), ALIGN_LEFT, "Nothing here yet", C_TX2);
        font_draw_in(ui.dl, F_UI, rect(area.x, area.y + 64, area.w, 18), ALIGN_LEFT, "Animations you open or save show up here, with a picture of their first frame.", C_TX3);
        return;
    }
    const float CW = 248, TH = 140, CH = TH + 58, GAP = 20;
    int cols = MAX(1, (int)((area.w + GAP) / (CW + GAP)));
    int rows = (int)((arrlen(H.items) + cols - 1) / cols);
    float content = rows * (CH + GAP);
    if (rect_has(area, ui.in.mouse) && ui_mouse_free() && ui.in.wheel_y) H.scroll -= ui.in.wheel_y * 60;
    H.scroll = clampf(H.scroll, 0, MAX(0, content - area.h));
    dl_push_clip(ui.dl, rect(area.x - 6, area.y - 6, area.w + 12, area.h + 12));
    int open = -1, remove = -1;
    for (int i = 0; i < (int)arrlen(H.items); i++) {
        Recent *it = &H.items[i];
        Rect card = {area.x + (i % cols) * (CW + GAP), area.y + (i / cols) * (CH + GAP) - H.scroll, CW, CH};
        if (card.y + card.h < area.y - 6 || card.y > area.y + area.h + 6) continue;
        UiId id = ui_idi("recent", i);
        UiResp re = ui_interact(id, card);
        Rect th = {card.x, card.y, CW, TH};
        dl_rrect(ui.dl, th, 8, C_S1);
        if (!it->missing) load_thumb(it);
        if (it->view.id) {   // the picture, fitted into the card
            float k = MIN((CW - 12) / it->tw, (TH - 12) / it->th), w = it->tw * k, h = it->th * k;   // framed inside the card
            dl_image(ui.dl, rect(th.x + (CW - w) / 2, th.y + (TH - h) / 2, w, h), it->view.id, it->missing ? hexa(0xffffff, 0x60) : hex(0xffffff), 0, 0, 1, 1);
        } else {
            const char *dot = strrchr(it->path, '.');
            char ext[8]; snprintf(ext, sizeof(ext), "%s", dot ? dot + 1 : "");
            for (char *c = ext; *c; c++) *c = (char)toupper((u8)*c);
            icon_draw(ui.dl, IC_FILM, v2(th.x + CW / 2, th.y + TH / 2 - 8), ICON_L, false, C_TX3);
            font_draw_in(ui.dl, F_MONO, rect(th.x, th.y + TH / 2 + 8, CW, 16), ALIGN_CENTER, ext, C_TX3);
        }
        dl_rrect_border(ui.dl, th, 8, re.hover ? 2 : 1, re.hover ? C_AC : C_LINE);
        const char *name = it->path;
        for (const char *c = it->path; *c; c++) if (*c == '/' || *c == '\\') name = c + 1;
        font_draw_in(ui.dl, F_UI_MED, rect(card.x + 2, card.y + TH + 10, CW - 4, 18), ALIGN_LEFT, name, it->missing ? C_TX3 : re.hover ? C_TX : C_TX2);
        char meta[96];
        if (it->missing) snprintf(meta, sizeof(meta), "File not found");
        else when_text(it->when, meta, sizeof(meta));
        font_draw_in(ui.dl, F_SMALL, rect(card.x + 2, card.y + TH + 30, CW - 4, 16), ALIGN_LEFT, meta, C_TX3);
        ui_tooltip(id, card, it->path);
        if (re.clicked) { if (it->missing) remove = i; else open = i; }
        if (re.right) { H.menu = i; ui_open_popup(ui_id("recent-menu"), rect(ui.in.mouse.x, ui.in.mouse.y, 1, 1)); }
    }
    dl_pop_clip(ui.dl);
    if (ui_popup_begin(ui_id("recent-menu"), rect(ui.popup_anchor.x, ui.popup_anchor.y, 220, 10 + 28 * 3 + 9))) {
        Recent *it = H.menu >= 0 && H.menu < arrlen(H.items) ? &H.items[H.menu] : NULL;
        if (ui_menu_item("Open", NULL, it && !it->missing, false)) open = H.menu;
        if (ui_menu_item("Show in Folder", NULL, it && !it->missing, false)) {
            char dir[512]; snprintf(dir, sizeof(dir), "%s", it->path);
            char *sl = strrchr(dir, '\\'); if (!sl) sl = strrchr(dir, '/');
            if (sl) { *sl = 0; plat_open_folder(dir); }
        }
        ui_menu_sep();
        if (ui_menu_item("Remove from the List", NULL, it != NULL, false)) remove = H.menu;
        ui_popup_end();
    }
    if (remove >= 0) { free_thumb(&H.items[remove]); arrdel(H.items, remove); save_list(); }
    else if (open >= 0) { char p[512]; snprintf(p, sizeof(p), "%s", H.items[open].path); app_open_path(p); }
}

/* ── splash ── */

void splash_draw(float alpha) {
    if (alpha <= 0) return;
    ui_overlay(true);
    Rect full = {0, 0, ui.w, ui.h};
    dl_rect(ui.dl, full, col_alpha(C_BG, alpha));
    int mh = (int)roundf(96 * ui.scale);
    float mw = logo_mark_width(mh) / ui.scale, cx = roundf(ui.w / 2), cy = roundf(ui.h / 2) - 40;
    dl_image(ui.dl, rect(cx - mw / 2, cy - 96, mw, mh / ui.scale), logo_mark_view(mh), col_alpha(hex(0xffffff), alpha), 0, 0, 1, 1);
    float tw = font_width(F_DISPLAY, "Vertexa");
    font_draw(ui.dl, F_DISPLAY, cx - tw / 2, cy + 44, "Vertexa", col_alpha(C_TX, alpha));
    Rect pill = {cx - (font_width(F_SMALL_SEMI, "ALPHA") + 14) / 2, cy + 58, font_width(F_SMALL_SEMI, "ALPHA") + 14, 18};
    dl_rrect(ui.dl, pill, 9, col_alpha(C_AC2, alpha));
    font_draw_in(ui.dl, F_SMALL_SEMI, pill, ALIGN_CENTER, "ALPHA", col_alpha(C_AC, alpha));
    // a slim running bar while it loads
    Rect bar = {cx - 80, cy + 104, 160, 2};
    dl_rect(ui.dl, bar, col_alpha(C_S3, alpha));
    float t = (float)fmod(ui.time * 0.9, 1.0), x0 = bar.x + bar.w * clampf(t * 1.4f - 0.4f, 0, 1), x1 = bar.x + bar.w * clampf(t * 1.4f, 0, 1);
    if (x1 > x0) dl_rect(ui.dl, rect(x0, bar.y, x1 - x0, bar.h), col_alpha(C_AC, alpha));
    ui_overlay(false);
}
