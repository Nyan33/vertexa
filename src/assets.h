#pragma once
#include <stddef.h>

typedef struct { const unsigned char *data; size_t size; } Blob;

enum { FONT_FILE_INTER_REGULAR, FONT_FILE_INTER_MEDIUM, FONT_FILE_INTER_SEMIBOLD, FONT_FILE_MONO_REGULAR, FONT_FILE_MONO_SEMIBOLD, FONT_FILE_ICONS, FONT_FILE_ICONS_FILL, FONT_FILE_COUNT };
const Blob *asset_fonts(void);
const Blob *asset_logo(void);   // brand/vertexa-256.png
