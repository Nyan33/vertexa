// Win32: frameless window (custom title bar), WM_POINTER pen input with history, pen feedback off.
#include "platform/platform.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include "sokol/sokol_app.h"

static struct {
    HWND hwnd;
    WNDPROC orig;
    float scale;
    Rect bar, holes[96]; int nholes;
    PenSample q[4096]; int qn;
    bool pen_range, pen_eraser;
} W;

void plat_set_scale(float s) { W.scale = s; }

void plat_set_caption(Rect bar, const Rect *holes, int n) {
    W.bar = bar;
    W.nholes = MIN(n, COUNT(W.holes));
    memcpy(W.holes, holes, sizeof(Rect) * W.nholes);
}

int plat_pen_take(PenSample *out, int cap) {
    int n = MIN(cap, W.qn);
    memcpy(out, W.q, sizeof(PenSample) * n);
    W.qn = 0;
    return n;
}
bool plat_pen_in_range(void) { return W.pen_range; }
bool plat_pen_eraser(void) { return W.pen_eraser; }

void plat_minimize(void) { ShowWindow(W.hwnd, SW_MINIMIZE); }
void plat_toggle_maximize(void) { ShowWindow(W.hwnd, IsZoomed(W.hwnd) ? SW_RESTORE : SW_MAXIMIZE); }
bool plat_maximized(void) { return IsZoomed(W.hwnd) != 0; }
void plat_close(void) { PostMessageW(W.hwnd, WM_CLOSE, 0, 0); }

/* Pen positions. The cursor the artist aims with is moved by the tablet driver, by its own mapping of the
   tablet onto the screens; the positions in WM_POINTER (pixels, and the tablet's finer units) follow
   Windows Ink's mapping, which can differ (several monitors, a part of the tablet, kept proportions), so
   lines landed away from the cursor. The tablet's units are fitted to the cursor instead: a straight line
   per axis, least squares, with older pairs fading so a changed mapping is learnt again. Until the fit has
   seen enough movement the cursor itself is used. */
static FILE *PENLOG;
static int penlog_lines;
void plat_pen_log_open(void) {
    char path[1024];
    if (plat_config_path("pen-log.txt", path, sizeof(path))) PENLOG = plat_fopen(path, "wb");
}
bool plat_pen_logging(void) { return PENLOG != NULL; }
void plat_pen_log(const char *fmt, ...) {
    if (!PENLOG || penlog_lines > 6000) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(PENLOG, fmt, ap);
    va_end(ap);
    fputc('\n', PENLOG);
    fflush(PENLOG);
    penlog_lines++;
}

typedef struct { double w, sh, sp, shh, shp; } PenFit;
static struct { PenFit ax[2]; HANDLE device; } PF;

static void fit_add(PenFit *f, double h, double p) {
    const double k = 0.995;   // older pairs fade
    f->w = f->w * k + 1; f->sh = f->sh * k + h; f->sp = f->sp * k + p; f->shh = f->shh * k + h * h; f->shp = f->shp * k + h * p;
}
static bool fit_map(const PenFit *f, double h, double *out) {
    if (f->w < 20) return false;
    double mh = f->sh / f->w, mp = f->sp / f->w, var = f->shh / f->w - mh * mh;
    if (var <= 1e-6) return false;
    double slope = (f->shp / f->w - mh * mp) / var;
    if (sqrt(var) * fabs(slope) < 30) return false;   // not enough movement seen yet, in pixels
    *out = mp + slope * (h - mh);
    return true;
}

static void push_pen(UINT32 id) {
    UINT32 count = 0;
    if (!GetPointerPenInfoHistory(id, &count, NULL) || !count) return;
    POINTER_PEN_INFO hist[64];
    count = MIN(count, 64);
    if (!GetPointerPenInfoHistory(id, &count, hist)) return;
    if (hist[0].pointerInfo.sourceDevice != PF.device) { PF.device = hist[0].pointerInfo.sourceDevice; memset(PF.ax, 0, sizeof(PF.ax)); }
    POINT cur;
    bool have_cur = GetCursorPos(&cur);
    bool touching = (hist[0].pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    W.pen_eraser = (hist[0].penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
    double tx, ty;
    bool fitted = fit_map(&PF.ax[0], 0, &tx) && fit_map(&PF.ax[1], 0, &ty);
    if (have_cur && (!touching || !fitted)) {   // the newest sample and where the cursor is now (learnt while hovering)
        fit_add(&PF.ax[0], hist[0].pointerInfo.ptHimetricLocation.x, cur.x);
        fit_add(&PF.ax[1], hist[0].pointerInfo.ptHimetricLocation.y, cur.y);
    }
    POINT origin = {0, 0};
    ClientToScreen(W.hwnd, &origin);
    if (PENLOG) {
        static bool once;
        if (!once) {
            once = true;
            RECT wr, cr; GetWindowRect(W.hwnd, &wr); GetClientRect(W.hwnd, &cr);
            RECT dev = {0}, disp = {0};
            GetPointerDeviceRects(hist[0].pointerInfo.sourceDevice, &dev, &disp);
            plat_pen_log("window %ld %ld %ld %ld, client %ld x %ld, client origin %ld %ld, scale %.3f, dpi %u, maximized %d",
                         wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, origin.x, origin.y, W.scale, GetDpiForWindow(W.hwnd), IsZoomed(W.hwnd));
            plat_pen_log("pointer device rect %ld %ld %ld %ld, display rect %ld %ld %ld %ld, screen %d x %d",
                         dev.left, dev.top, dev.right, dev.bottom, disp.left, disp.top, disp.right, disp.bottom,
                         GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        }
        POINTER_INFO *pi = &hist[0].pointerInfo;
        plat_pen_log("pen n=%u flags %08x pixel %ld %ld raw %ld %ld himetric %ld %ld cursor %ld %ld origin %ld %ld pressure %u",
                     count, (unsigned)pi->pointerFlags, pi->ptPixelLocation.x, pi->ptPixelLocation.y, pi->ptPixelLocationRaw.x, pi->ptPixelLocationRaw.y,
                     pi->ptHimetricLocation.x, pi->ptHimetricLocation.y, cur.x, cur.y, origin.x, origin.y, hist[0].pressure);
    }
    for (int i = (int)count - 1; i >= 0; i--) {   // history is newest first
        POINTER_PEN_INFO *p = &hist[i];
        double fx, fy;
        float sx, sy;
        if (fit_map(&PF.ax[0], p->pointerInfo.ptHimetricLocation.x, &fx) && fit_map(&PF.ax[1], p->pointerInfo.ptHimetricLocation.y, &fy)) { sx = (float)fx; sy = (float)fy; }
        else if (i == 0 && have_cur) { sx = (float)cur.x; sy = (float)cur.y; }
        else continue;   // not fitted yet: only the newest sample, at the cursor
        bool contact = (p->pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        PenSample s = {
            .x = (sx - origin.x) / W.scale, .y = (sy - origin.y) / W.scale,
            .pressure = contact ? (p->penMask & PEN_MASK_PRESSURE ? p->pressure / 1024.0f : 1.0f) : 0.0f,
            .tilt = (p->penMask & (PEN_MASK_TILT_X | PEN_MASK_TILT_Y)) ? hypotf((float)p->tiltX, (float)p->tiltY) : 0,
            .eraser = (p->penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0,
        };
        if (!contact) continue;
        if (PENLOG && i == 0) plat_pen_log("  queued %.1f %.1f (logical)", s.x, s.y);
        if (W.qn < COUNT(W.q)) W.q[W.qn++] = s;
    }
}

static LRESULT hit_test(LPARAM lp) {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(W.hwnd, &pt);
    RECT rc; GetClientRect(W.hwnd, &rc);
    int b = (int)(6 * W.scale);
    if (!IsZoomed(W.hwnd)) {
        bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (bo && l) return HTBOTTOMLEFT;
        if (bo && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (bo) return HTBOTTOM;
    }
    V2 p = {pt.x / W.scale, pt.y / W.scale};
    if (rect_has(W.bar, p)) {
        for (int i = 0; i < W.nholes; i++) if (rect_has(W.holes[i], p)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCCALCSIZE:
        if (wp) {
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            if (IsZoomed(h)) {
                int fx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return hit_test(lp);
    case WM_NCACTIVATE: return DefWindowProcW(h, m, wp, -1);   // no default frame repaint
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
        UINT32 id = GET_POINTERID_WPARAM(wp);
        POINTER_INPUT_TYPE type;
        if (GetPointerType(id, &type) && type == PT_PEN) { W.pen_range = m != WM_POINTERUP || IS_POINTER_INRANGE_WPARAM(wp); push_pen(id); }
        break;
    }
    case WM_POINTERLEAVE: W.pen_range = false; break;
    default: break;
    }
    return CallWindowProcW(W.orig, h, m, wp, lp);
}

typedef BOOL(WINAPI *SetFeedbackFn)(HWND, int, DWORD, UINT32, const void *);

void plat_init(float dpi_scale, bool maximize) {
    W.scale = dpi_scale;
    W.hwnd = (HWND)sapp_win32_get_hwnd();
    W.orig = (WNDPROC)SetWindowLongPtrW(W.hwnd, GWLP_WNDPROC, (LONG_PTR)proc);
    // dark, shadowed, frameless
    BOOL dark = TRUE;
    DwmSetWindowAttribute(W.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF border = RGB(0x23, 0x23, 0x29);
    DwmSetWindowAttribute(W.hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    MARGINS mg = {0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(W.hwnd, &mg);
    SetWindowPos(W.hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (maximize) ShowWindow(W.hwnd, SW_MAXIMIZE);
    // artists don't want press-and-hold right clicks, flicks or ripples under the pen
    SetPropW(W.hwnd, L"MicrosoftTabletPenServiceProperty", (HANDLE)(intptr_t)(0x1 | 0x8 | 0x10 | 0x10000));
    SetFeedbackFn fb = (SetFeedbackFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowFeedbackSetting");
    if (fb) { BOOL off = FALSE; for (int f = 1; f <= 11; f++) fb(W.hwnd, f, 0, sizeof(off), &off); }
}

/* ── dialogs (UTF-8 in and out) ── */

#include <commdlg.h>
#include <shlobj.h>

static void to_w(const char *s, wchar_t *w, int cap) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap); }
static void to_u8(const wchar_t *w, char *s, int cap) { WideCharToMultiByte(CP_UTF8, 0, w, -1, s, cap, NULL, NULL); }

static void make_filter(wchar_t *f, int cap, const char *desc, const char *patterns) {
    wchar_t d[256], p[256];
    to_w(desc, d, 256); to_w(patterns, p, 256);
    int n = swprintf(f, cap, L"%ls (%ls)", d, p);
    f[n++] = 0;
    n += swprintf(f + n, cap - n, L"%ls", p);
    f[n++] = 0;
    n += swprintf(f + n, cap - n, L"All files (*.*)");
    f[n++] = 0;
    n += swprintf(f + n, cap - n, L"*.*");
    f[n++] = 0; f[n] = 0;
}

bool plat_open_dialog(char *out, int cap, const char *desc, const char *patterns) {
    wchar_t file[1024] = L"", filter[600];
    make_filter(filter, 600, desc, patterns);
    OPENFILENAMEW o = {sizeof(o)};
    o.hwndOwner = W.hwnd; o.lpstrFilter = filter; o.lpstrFile = file; o.nMaxFile = 1024;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&o)) return false;
    to_u8(file, out, cap);
    return true;
}

bool plat_save_dialog(char *out, int cap, const char *desc, const char *ext, const char *default_name) {
    wchar_t file[1024], filter[600], wext[32];
    char pat[64];
    snprintf(pat, sizeof(pat), "*.%s", ext);
    make_filter(filter, 600, desc, pat);
    to_w(default_name ? default_name : "", file, 1024);
    to_w(ext, wext, 32);
    OPENFILENAMEW o = {sizeof(o)};
    o.hwndOwner = W.hwnd; o.lpstrFilter = filter; o.lpstrFile = file; o.nMaxFile = 1024; o.lpstrDefExt = wext;
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&o)) return false;
    to_u8(file, out, cap);
    return true;
}

bool plat_pick_folder(char *out, int cap, const char *title) {
    wchar_t wt[256], path[MAX_PATH];
    to_w(title, wt, 256);
    BROWSEINFOW bi = {0};
    bi.hwndOwner = W.hwnd; bi.lpszTitle = wt; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi);
    if (!id) return false;
    bool ok = SHGetPathFromIDListW(id, path) != 0;
    CoTaskMemFree(id);
    if (ok) to_u8(path, out, cap);
    return ok;
}

void plat_set_title(const char *title) {
    wchar_t w[256];
    to_w(title, w, 256);
    SetWindowTextW(W.hwnd, w);
}

/* ── files with UTF-8 paths (the CRT's narrow functions use the ANSI code page) ── */

FILE *plat_fopen(const char *path, const char *mode) {
    wchar_t w[1024], m[16];
    to_w(path, w, 1024); to_w(mode, m, 16);
    return _wfopen(w, m);
}

bool plat_mkdirs(const char *path) {
    wchar_t w[1024];
    to_w(path, w, 1024);
    for (wchar_t *c = w + 3; *c; c++) if (*c == L'/' || *c == L'\\') { wchar_t k = *c; *c = 0; CreateDirectoryW(w, NULL); *c = k; }
    CreateDirectoryW(w, NULL);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool plat_replace_file(const char *from, const char *to) {
    wchar_t a[1024], b[1024];
    to_w(from, a, 1024); to_w(to, b, 1024);
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void plat_remove(const char *path) {
    wchar_t w[1024];
    to_w(path, w, 1024);
    DeleteFileW(w);
}

bool plat_config_path(const char *file, char *out, int cap) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) return false;
    char base[1024];
    to_u8(dir, base, sizeof(base));
    snprintf(out, (size_t)cap, "%s\\Vertexa", base);
    if (!plat_mkdirs(out)) return false;
    snprintf(out, (size_t)cap, "%s\\Vertexa\\%s", base, file);
    return true;
}

void plat_open_folder(const char *path) {
    wchar_t w[1024];
    to_w(path, w, 1024);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

// Which GPU Windows gives this exe (Settings > System > Display > Graphics keeps the same per-app value):
// 0 let Windows decide, 1 power saving, 2 high performance. Applies the next time Vertexa starts.
static const wchar_t *GPU_PREF_KEY = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
int plat_gpu_preference(void) {
    wchar_t exe[MAX_PATH], val[64];
    DWORD n = sizeof(val), type = 0;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 0;
    if (RegGetValueW(HKEY_CURRENT_USER, GPU_PREF_KEY, exe, RRF_RT_REG_SZ, &type, val, &n) != ERROR_SUCCESS) return 0;
    const wchar_t *p = wcsstr(val, L"GpuPreference=");
    return p ? CLAMP(_wtoi(p + 14), 0, 2) : 0;
}
bool plat_set_gpu_preference(int pref) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return false;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, GPU_PREF_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    LONG r;
    if (pref <= 0) { r = RegDeleteValueW(k, exe); if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS; }
    else {
        wchar_t v[32];
        swprintf(v, 32, L"GpuPreference=%d;", pref);
        r = RegSetValueExW(k, exe, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

int plat_count_files(const char *dir, const char *pattern) {
    char q[1100];
    snprintf(q, sizeof(q), "%s/%s", dir, pattern);
    wchar_t w[1100];
    to_w(q, w, 1100);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

int plat_list_files(const char *dir, const char *pattern, char (*names)[128], int cap) {
    char q[1100];
    snprintf(q, sizeof(q), "%s/%s", dir, pattern);
    wchar_t w[1100];
    to_w(q, w, 1100);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || n >= cap) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, names[n], 128, NULL, NULL) > 0) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

char **plat_argv_utf8(int *argc) {
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return NULL;
    char **out = calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = malloc((size_t)MAX(len, 1));
        out[i][0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, NULL, NULL);
    }
    LocalFree(w);
    *argc = n;
    return out;
}

void plat_reset_cursor(void) { SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW)); }   // so the app's own cursors can be freed
