// Bitmap bank: images for the whole session, shared by every document and undo snapshot (like sounds).
// Pixels are kept premultiplied, ready for the GPU; the image file is kept (or made, as PNG) for saving.
#include "doc/doc.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

static Bitmap **BANK;   // stable pointers
static int next_bitmap = 1;

Bitmap *bitmap_get(int id) {
    for (ptrdiff_t i = 0; i < arrlen(BANK); i++) if (BANK[i]->id == id) return BANK[i];
    return NULL;
}

static int add(const char *name, int w, int h, u8 *px, bool smooth) {
    Bitmap *b = calloc(1, sizeof(Bitmap));
    b->id = next_bitmap++;
    snprintf(b->name, sizeof(b->name), "%s", name && name[0] ? name : "Bitmap");
    b->w = w; b->h = h; b->px = px; b->smooth = smooth;
    arrput(BANK, b);
    return b->id;
}

int bitmap_add_pixels(const char *name, int w, int h, u8 *premul_rgba, bool smooth) {
    if (!premul_rgba || w <= 0 || h <= 0) { free(premul_rgba); return 0; }
    return add(name, w, h, premul_rgba, smooth);
}

int bitmap_add_file(const char *name, const u8 *data, u32 size, char *err, int cap) {
    int w, h, n;
    u8 *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!rgba) { snprintf(err, (size_t)cap, "Can't read this image (%s)", stbi_failure_reason()); return 0; }
    if ((size_t)w * h > (size_t)16384 * 16384) { stbi_image_free(rgba); snprintf(err, (size_t)cap, "The image is too big"); return 0; }
    u8 *px = malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        u8 *s = rgba + i * 4, *d = px + i * 4;
        d[0] = (u8)((s[0] * s[3] + 127) / 255); d[1] = (u8)((s[1] * s[3] + 127) / 255); d[2] = (u8)((s[2] * s[3] + 127) / 255); d[3] = s[3];
    }
    stbi_image_free(rgba);
    int id = add(name, w, h, px, true);
    Bitmap *b = bitmap_get(id);
    // PNG and JPEG are saved as they came; anything else becomes a PNG when saved
    bool keep = size >= 4 && ((data[0] == 0x89 && data[1] == 'P') || (data[0] == 0xff && data[1] == 0xd8));
    if (keep) { b->data = malloc(size); memcpy(b->data, data, size); b->size = size; }
    return id;
}

static void grow(void *ctx, void *data, int size) {
    Bitmap *b = ctx;
    b->data = realloc(b->data, b->size + (u32)size);
    memcpy(b->data + b->size, data, (size_t)size);
    b->size += (u32)size;
}

const u8 *bitmap_encoded(Bitmap *b, u32 *size) {
    if (!b->data) {   // straight alpha for the PNG
        u8 *st = malloc((size_t)b->w * b->h * 4);
        for (size_t i = 0; i < (size_t)b->w * b->h; i++) {
            const u8 *s = b->px + i * 4; u8 *d = st + i * 4;
            int a = s[3];
            for (int c = 0; c < 3; c++) d[c] = a ? (u8)MIN(255, (s[c] * 255 + a / 2) / a) : 0;
            d[3] = (u8)a;
        }
        b->size = 0;
        stbi_write_png_to_func(grow, b, b->w, b->h, 4, st, b->w * 4);
        free(st);
    }
    *size = b->size;
    return b->data;
}
