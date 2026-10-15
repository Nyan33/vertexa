// Fonts, icons and the logo are embedded into the executable with .incbin, so Vertexa ships as a single .exe.
#include "assets.h"

#define VX_INCBIN(name, file)                                   \
    __asm__(".section .rdata\n"                                 \
            ".global " #name "\n"                               \
            ".balign 16\n" #name ":\n"                          \
            ".incbin \"" VX_ASSETS_DIR "/" file "\"\n"          \
            ".global " #name "_end\n" #name "_end:\n"           \
            ".byte 0\n"                                         \
            ".text\n");                                         \
    extern const unsigned char name[], name##_end[];

VX_INCBIN(vx_font_inter_regular, "fonts/Inter-Regular.ttf")
VX_INCBIN(vx_font_inter_medium, "fonts/Inter-Medium.ttf")
VX_INCBIN(vx_font_inter_semibold, "fonts/Inter-SemiBold.ttf")
VX_INCBIN(vx_font_mono_regular, "fonts/JetBrainsMono-Regular.ttf")
VX_INCBIN(vx_font_mono_semibold, "fonts/JetBrainsMono-SemiBold.ttf")
VX_INCBIN(vx_icons_regular, "icons/Phosphor.ttf")
VX_INCBIN(vx_icons_fill, "icons/Phosphor-Fill.ttf")
VX_INCBIN(vx_logo_png, "brand/vertexa-256.png")

const Blob *asset_fonts(void) {
    static Blob b[FONT_FILE_COUNT];
    if (!b[0].data) {
#define BLOB(i, n) b[i] = (Blob){n, (size_t)(n##_end - n)}
        BLOB(FONT_FILE_INTER_REGULAR, vx_font_inter_regular);
        BLOB(FONT_FILE_INTER_MEDIUM, vx_font_inter_medium);
        BLOB(FONT_FILE_INTER_SEMIBOLD, vx_font_inter_semibold);
        BLOB(FONT_FILE_MONO_REGULAR, vx_font_mono_regular);
        BLOB(FONT_FILE_MONO_SEMIBOLD, vx_font_mono_semibold);
        BLOB(FONT_FILE_ICONS, vx_icons_regular);
        BLOB(FONT_FILE_ICONS_FILL, vx_icons_fill);
#undef BLOB
    }
    return b;
}

const Blob *asset_logo(void) {
    static Blob b;
    if (!b.data) b = (Blob){vx_logo_png, (size_t)(vx_logo_png_end - vx_logo_png)};
    return &b;
}
