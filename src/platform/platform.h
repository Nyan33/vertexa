// OS integration: frameless window with our own title bar, tablet pen input, window capture.
#pragma once
#include "base.h"
#include <stdio.h>

typedef struct { float x, y, pressure, tilt; bool eraser; } PenSample;   // x, y in logical px

void plat_init(float dpi_scale, bool maximize);
void plat_set_scale(float dpi_scale);
int plat_pen_take(PenSample *out, int cap);  // samples received since the last call
bool plat_pen_in_range(void);                // pen hovering or touching
bool plat_pen_eraser(void);

void plat_set_caption(Rect bar, const Rect *holes, int n);  // logical px; holes are clickable controls
void plat_minimize(void);
void plat_toggle_maximize(void);
bool plat_maximized(void);
void plat_close(void);

bool plat_capture_png(const char *path);     // call after the swapchain pass
bool plat_open_dialog(char *out, int cap, const char *filter_desc, const char *patterns);
bool plat_save_dialog(char *out, int cap, const char *filter_desc, const char *ext, const char *default_name);
bool plat_pick_folder(char *out, int cap, const char *title);
void plat_set_title(const char *title);
void plat_reset_cursor(void);
char **plat_argv_utf8(int *argc);                 // the command line as UTF-8 (main() gets the ANSI code page)

// files with UTF-8 paths
FILE *plat_fopen(const char *path, const char *mode);
bool plat_mkdirs(const char *dir);                        // creates every missing folder of the path
bool plat_replace_file(const char *from, const char *to); // atomic rename over an existing file
void plat_remove(const char *path);
bool plat_config_path(const char *file, char *out, int cap);
void plat_open_folder(const char *path);                  // in Explorer
int plat_count_files(const char *dir, const char *pattern);
void plat_pen_log_open(void);                 // --pen-log: pen messages and where they land, in %APPDATA%/Vertexa/pen-log.txt
bool plat_pen_logging(void);
void plat_pen_log(const char *fmt, ...);
int plat_list_files(const char *dir, const char *pattern, char (*names)[128], int cap);   // file names (UTF-8), not folders
int plat_gpu_preference(void);              // Windows per-app GPU choice: 0 default, 1 power saving, 2 high performance
bool plat_set_gpu_preference(int pref);     // takes effect on the next start   // %APPDATA%/Vertexa/<file>, folder created
#include "sokol/sokol_gfx.h"
bool plat_read_image(sg_image img, int w, int h, u8 *out_rgba);   // RGBA8 render target -> CPU
bool plat_gpu_info(char *name, int cap, double *vram_gb);          // the adapter sokol_gfx runs on
