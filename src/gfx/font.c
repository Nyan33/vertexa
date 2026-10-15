#include "gfx/font.h"
#include "assets.h"
#include "stb/stb_rect_pack.h"
#include "stb/stb_truetype.h"

typedef struct { int first, count; } CpRange;
static const CpRange RANGES[] = {
    {0x20, 0x5F}, {0xA0, 0x60}, {0x400, 0x60}, {0x2010, 0x30}, {0x2190, 0x04}, {0x2212, 0x01},
    {0x25B2, 0x0D}, {0x2713, 0x03}, {0x00, 0x00},
};
#define NRANGES (COUNT(RANGES) - 1)

typedef struct {
    int file;
    float size;
    stbtt_packedchar *chars[NRANGES];
    stbtt_fontinfo info;
    float px_scale;   // stbtt scale for kerning
    float cap;        // cap height in framebuffer pixels
} Face;

static const struct { int file; float size; } SPEC[F_COUNT] = {
    [F_UI] = {FONT_FILE_INTER_REGULAR, 12},
    [F_UI_MED] = {FONT_FILE_INTER_MEDIUM, 12},
    [F_UI_SEMI] = {FONT_FILE_INTER_SEMIBOLD, 12},
    [F_SMALL] = {FONT_FILE_INTER_REGULAR, 11},
    [F_SMALL_SEMI] = {FONT_FILE_INTER_SEMIBOLD, 10},
    [F_TOOL] = {FONT_FILE_INTER_SEMIBOLD, 12.5f},
    [F_TITLE] = {FONT_FILE_INTER_SEMIBOLD, 13},
    [F_MONO] = {FONT_FILE_MONO_REGULAR, 10},
    [F_MONO_MED] = {FONT_FILE_MONO_REGULAR, 11},
    [F_MONO_SEMI] = {FONT_FILE_MONO_SEMIBOLD, 10},
    [F_H2] = {FONT_FILE_INTER_SEMIBOLD, 16},
    [F_DISPLAY] = {FONT_FILE_INTER_SEMIBOLD, 28},
};

typedef struct { int size; bool fill; stbtt_packedchar chars[IC_COUNT]; } IconFace;
static const int ICON_SIZES[3] = {ICON_S, ICON_M, ICON_L};

static struct {
    Face face[F_COUNT];
    IconFace icons[6];
    float scale;
    int aw, ah;
    sg_image img;
    sg_view view;
} FT;

void font_shutdown(void) {
    for (int f = 0; f < F_COUNT; f++) for (int r = 0; r < NRANGES; r++) { free(FT.face[f].chars[r]); FT.face[f].chars[r] = NULL; }
    if (FT.view.id) sg_destroy_view(FT.view);
    if (FT.img.id) sg_destroy_image(FT.img);
    FT.view.id = FT.img.id = 0;
}

void font_init(float scale) {
    font_shutdown();
    FT.scale = scale;
    FT.aw = FT.ah = scale > 1.5f ? 4096 : 2048;
    u8 *pix = calloc((size_t)FT.aw * FT.ah, 1);
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, pix, FT.aw, FT.ah, 0, 1, NULL);
    stbtt_PackSetOversampling(&pc, 1, 1);
    for (int f = 0; f < F_COUNT; f++) {
        Face *fc = &FT.face[f];
        fc->file = SPEC[f].file;
        fc->size = SPEC[f].size * scale;
        const Blob *b = &asset_fonts()[fc->file];
        stbtt_pack_range pr[NRANGES];
        for (int r = 0; r < NRANGES; r++) {
            fc->chars[r] = calloc(RANGES[r].count, sizeof(stbtt_packedchar));
            pr[r] = (stbtt_pack_range){.font_size = STBTT_POINT_SIZE(fc->size), .first_unicode_codepoint_in_range = RANGES[r].first,
                                       .num_chars = RANGES[r].count, .chardata_for_range = fc->chars[r]};
        }
        if (!stbtt_PackFontRanges(&pc, b->data, 0, pr, NRANGES)) vx_log("font atlas full (font %d)", f);
        stbtt_InitFont(&fc->info, b->data, stbtt_GetFontOffsetForIndex(b->data, 0));
        fc->px_scale = stbtt_ScaleForMappingEmToPixels(&fc->info, fc->size);
        int x0, y0, x1, y1;
        if (stbtt_GetCodepointBox(&fc->info, 'H', &x0, &y0, &x1, &y1)) fc->cap = y1 * fc->px_scale; else fc->cap = fc->size * 0.7f;
    }
    // icons: only the glyphs the UI uses
    static int cps[IC_COUNT];
    for (int i = 0; i < IC_COUNT; i++) cps[i] = ICON_CODEPOINTS[i];
    for (int i = 0; i < 6; i++) {
        IconFace *ic = &FT.icons[i];
        ic->size = ICON_SIZES[i % 3]; ic->fill = i >= 3;
        stbtt_pack_range pr = {.font_size = STBTT_POINT_SIZE(ic->size * scale), .array_of_unicode_codepoints = cps, .num_chars = IC_COUNT, .chardata_for_range = ic->chars};
        const Blob *b = &asset_fonts()[ic->fill ? FONT_FILE_ICONS_FILL : FONT_FILE_ICONS];
        if (!stbtt_PackFontRanges(&pc, b->data, 0, &pr, 1)) vx_log("icon atlas full");
    }
    stbtt_PackEnd(&pc);
    FT.img = sg_make_image(&(sg_image_desc){
        .width = FT.aw, .height = FT.ah, .pixel_format = SG_PIXELFORMAT_R8,
        .data.mip_levels[0] = {pix, (size_t)FT.aw * FT.ah}, .label = "vx-font-atlas",
    });
    FT.view = sg_make_view(&(sg_view_desc){.texture.image = FT.img});
    free(pix);
}

u32 font_view(void) { return FT.view.id; }

int utf8_next(const char **s) {
    const u8 *p = (const u8 *)*s;
    int c = *p;
    if (!c) return 0;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n == 2) c = ((c & 31) << 6) | (p[1] & 63);
    else if (n == 3) c = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
    else if (n == 4) c = ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
    for (int i = 0; i < n; i++) if (!p[i]) { n = i; break; }
    *s += n ? n : 1;
    return c;
}

static stbtt_packedchar *glyph(Face *fc, int cp) {
    for (int r = 0; r < NRANGES; r++)
        if (cp >= RANGES[r].first && cp < RANGES[r].first + RANGES[r].count) return &fc->chars[r][cp - RANGES[r].first];
    return &fc->chars[0]['?' - 0x20];
}

bool font_covers(const char *s) {
    int cp;
    while ((cp = utf8_next(&s))) {
        bool ok = false;
        for (int r = 0; r < NRANGES && !ok; r++) ok = cp >= RANGES[r].first && cp < RANGES[r].first + RANGES[r].count;
        if (!ok) return false;
    }
    return true;
}

float font_width(FontId f, const char *s) {
    Face *fc = &FT.face[f];
    float w = 0;
    int prev = 0, cp;
    while ((cp = utf8_next(&s))) {
        if (prev) w += stbtt_GetCodepointKernAdvance(&fc->info, prev, cp) * fc->px_scale;
        w += roundf(glyph(fc, cp)->xadvance);
        prev = cp;
    }
    return w / FT.scale;
}

float font_cap_height(FontId f) { return FT.face[f].cap / FT.scale; }

float font_draw(DrawList *dl, FontId f, float x, float baseline, const char *s, Color c) {
    Face *fc = &FT.face[f];
    float sc = dl->scale;
    float px = roundf(x * sc), by = roundf(baseline * sc), start = px;
    // light text on dark backgrounds reads thin without hinting: boost coverage a little
    float lum = ((c & 255) * 0.3f + ((c >> 8) & 255) * 0.59f + ((c >> 16) & 255) * 0.11f) / 255.0f;
    float gamma = lum > 0.5f ? 0.78f : 1.0f;
    u32 old = dl->tex;
    dl_set_tex(dl, FT.view.id);
    int prev = 0, cp;
    while ((cp = utf8_next(&s))) {
        if (prev) px += roundf(stbtt_GetCodepointKernAdvance(&fc->info, prev, cp) * fc->px_scale);
        stbtt_packedchar *g = glyph(fc, cp);
        if (g->x1 > g->x0) {
            float gx = px + roundf(g->xoff), gy = by + roundf(g->yoff);
            dl_quad_px(dl, gx, gy, gx + (g->x1 - g->x0), gy + (g->y1 - g->y0),
                       (float)g->x0 / FT.aw, (float)g->y0 / FT.ah, (float)g->x1 / FT.aw, (float)g->y1 / FT.ah, c, K_GLYPH, gamma);
        }
        px += roundf(g->xadvance);
        prev = cp;
    }
    dl_set_tex(dl, old);
    return (px - start) / sc;
}

void font_draw_in(DrawList *dl, FontId f, Rect r, int align, const char *s, Color c) {
    float w = font_width(f, s), x = r.x;
    if (align == ALIGN_CENTER) x = r.x + (r.w - w) / 2;
    else if (align == ALIGN_RIGHT) x = r.x + r.w - w;
    float base = r.y + (r.h + font_cap_height(f)) / 2;
    font_draw(dl, f, x, base, s, c);
}

void icon_draw(DrawList *dl, IconId id, V2 center, int size, bool fill, Color c) {
    int si = size <= ICON_S ? 0 : size <= ICON_M ? 1 : 2;
    stbtt_packedchar *g = &FT.icons[si + (fill ? 3 : 0)].chars[id];
    if (g->x1 <= g->x0) return;
    float sc = dl->scale, w = (float)(g->x1 - g->x0), h = (float)(g->y1 - g->y0);
    float x = roundf(center.x * sc - w / 2), y = roundf(center.y * sc - h / 2);
    float lum = ((c & 255) * 0.3f + ((c >> 8) & 255) * 0.59f + ((c >> 16) & 255) * 0.11f) / 255.0f;
    u32 old = dl->tex;
    dl_set_tex(dl, FT.view.id);
    dl_quad_px(dl, x, y, x + w, y + h, (float)g->x0 / FT.aw, (float)g->y0 / FT.ah, (float)g->x1 / FT.aw, (float)g->y1 / FT.ah, c, K_GLYPH, lum > 0.5f ? 0.85f : 1.0f);
    dl_set_tex(dl, old);
}
