/* Copyright (c) 2026 Jurjen Stellingwerff
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * screen: show a small RGB565 frame on the machine's screen, scaled up by a whole number, and
 * read the keys of the window it is shown in.  The scaling into the back buffer is shared C;
 * one section per platform puts the back buffer on the screen:
 *
 *   X11 (Linux, BSDs): a window; libX11 is opened at run time, so building needs only its
 *        headers and running without a display just answers "no screen".
 *   Tanmatsu (ESP_PLATFORM): the BSP's display, the frame rotated into the portrait panel.
 *   A browser page (`--native-c --html`, where loft defines LOFT_BROWSER): the page's canvas
 *        through loft's `loft_gl` imports, the frame scaled up by the GPU.
 *   macOS: a window through AppKit, by the Objective-C runtime; libobjc and AppKit are opened
 *        at run time, so building needs no SDK.
 *   Elsewhere (a WASI module, Windows): none yet — sc_open answers 0.
 *
 * Mode 1 of sc_open is OFF-SCREEN on every platform: the back buffer only, which is how the
 * scaling is tested where there is no display.
 *
 * Key codes are graphics' (`KEY_W` = 119, `KEY_UP` = 128, ...), as input's are. */
#if !defined(_POSIX_C_SOURCE) && !defined(_WIN32) && !defined(ESP_PLATFORM)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1 /* RTLD_DEFAULT, pthread_main_np, usleep beside _POSIX_C_SOURCE */
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t sc_open(int64_t w, int64_t h, int64_t scale, int64_t mode, const char *title);
void sc_close(void);
void sc_show(const int32_t *px, int64_t n, int64_t w, int64_t h);
int64_t sc_poll(int64_t wait_ms);
int64_t sc_has_keys(void);
int64_t sc_key(int64_t code);
int64_t sc_shown(int64_t x, int64_t y);
int64_t sc_width(void);
int64_t sc_height(void);
int64_t sc_pointer(int64_t which);
const char *sc_text(void);
void sc_text_taken(void);
int64_t sc_program_keys(void);

/* ── Shared: the back buffer, one RGB565 value per screen pixel ── */

static uint16_t *sc_back = NULL;
static int sc_bw = 0, sc_bh = 0;    /* the back buffer's size */
static int sc_fw = 0, sc_fh = 0;    /* the frame's size */
static int sc_scale = 1;            /* the scale the frame is shown at */
static int sc_want_scale = 1;       /* the scale asked for: a frame too large for it is shown at
                                     * the largest whole scale that fits, a display-sized one 1:1 */
static int sc_fill = 0;             /* the screen fills the display (a page: the whole window) */
static int sc_open_mode = -1;       /* -1 closed, 0 on screen, 1 off-screen */
static unsigned char sc_keys[256];
static int sc_have_keys = 0;
/* The pointer, where the screen has one: its pixel, its buttons (1 left, 2 right, 4 middle), the
 * wheel's notches since `poll` last answered, and the text typed since then, UTF-8. */
static int sc_px = 0, sc_py = 0, sc_buttons = 0, sc_wheel = 0, sc_have_pointer = 0;
static char sc_typed[256];
static int sc_typed_n = 0;

static void sc_type(const char *t, int n) {
    if (n <= 0 || sc_typed_n + n >= (int) sizeof sc_typed) return;
    memcpy(sc_typed + sc_typed_n, t, (size_t) n);
    sc_typed_n += n;
    sc_typed[sc_typed_n] = 0;
}
static int sc_alive = 0;

/* Scale a frame (w x h RGB565 values, from `px32` or `px16`, whichever is not null) by sc_scale
 * into the back buffer, centred; what the frame does not cover is black. */
static void sc_scale_from(const int32_t *px32, const uint16_t *px16, int w, int h) {
    int ox = (sc_bw - w * sc_scale) / 2, oy = (sc_bh - h * sc_scale) / 2, y;
    memset(sc_back, 0, (size_t) sc_bw * (size_t) sc_bh * sizeof(uint16_t));
    for (y = 0; y < h; y++) {
        int x, k, by = oy + y * sc_scale;
        uint16_t *row;
        if (by < 0 || by + sc_scale > sc_bh) continue;
        row = sc_back + (size_t) by * (size_t) sc_bw;
        for (x = 0; x < w; x++) {
            size_t at = (size_t) y * (size_t) w + (size_t) x;
            uint16_t c = px16 ? px16[at] : (uint16_t) px32[at];
            int bx = ox + x * sc_scale;
            if (bx < 0 || bx + sc_scale > sc_bw) continue;
            for (k = 0; k < sc_scale; k++) row[bx + k] = c;
        }
        for (k = 1; k < sc_scale; k++) memcpy(row + (size_t) k * (size_t) sc_bw, row, (size_t) sc_bw * sizeof(uint16_t));
    }
}

/* The frame `px` scaled into the back buffer. */
static void sc_scale_into_back(const int32_t *px, int w, int h) {
    sc_scale_from(px, NULL, w, h);
}

static int sc_make_back(int w, int h) {
    free(sc_back);
    sc_back = (uint16_t *) calloc((size_t) w * (size_t) h, sizeof(uint16_t));
    if (!sc_back) return 0;
    sc_bw = w;
    sc_bh = h;
    return 1;
}

/* The platform's half: open a screen of the back buffer's size, put the back buffer on it,
 * wait for events, close. */
static int sc_platform_open(const char *title);
static void sc_platform_present(void);
static void sc_platform_wait(int wait_ms);
static void sc_platform_close(void);
/* A machine that shows the frame itself rather than the back buffer — the Tanmatsu's panel,
 * written straight from the frame and turned; a page, whose GPU scales the frame — keeps the
 * last frame and where its scaled picture sits on the screen, and `shown` reads that.  The back
 * buffer then serves off-screen mode only. */
#if defined(ESP_PLATFORM) || (defined(__wasm__) && defined(LOFT_BROWSER))
#define SC_FRAME_PATH 1
static uint16_t *sc_frame = NULL;
static int sc_frame_w = 0, sc_frame_h = 0, sc_ox = 0, sc_oy = 0;

/* Keep frame `px`: 1 when its size (and so where it sits) changed, 0 when not, -1 when there is
 * no memory for it. */
static int sc_keep_frame(const int32_t *px, int w, int h) {
    size_t i, n = (size_t) w * (size_t) h;
    int changed = 0;
    if (w != sc_frame_w || h != sc_frame_h || !sc_frame) {
        uint16_t *f = (uint16_t *) realloc(sc_frame, n * sizeof(uint16_t));
        if (!f) return -1;
        sc_frame = f;
        sc_frame_w = w;
        sc_frame_h = h;
        sc_ox = (sc_bw - w * sc_scale) / 2;
        sc_oy = (sc_bh - h * sc_scale) / 2;
        changed = 1;
    }
    for (i = 0; i < n; i++) sc_frame[i] = (uint16_t) px[i];
    return changed;
}

static void sc_drop_frame(void) {
    free(sc_frame);
    sc_frame = NULL;
    sc_frame_w = sc_frame_h = 0;
}

/* The value shown at screen pixel (x, y): the kept frame's, black around it, -1 off the screen. */
static int64_t sc_frame_shown(int64_t x, int64_t y) {
    long fx, fy;
    if (x < 0 || y < 0 || x >= sc_bw || y >= sc_bh) return -1;
    if (!sc_frame) return 0;
    fx = (long) x - sc_ox;
    fy = (long) y - sc_oy;
    if (fx < 0 || fy < 0) return 0;
    fx /= sc_scale;
    fy /= sc_scale;
    if (fx >= sc_frame_w || fy >= sc_frame_h) return 0;
    /* A block cut by the screen's edge is not drawn, as on every other platform. */
    if (sc_ox + fx * sc_scale < 0 || sc_oy + fy * sc_scale < 0) return 0;
    if (sc_ox + (fx + 1) * sc_scale > sc_bw || sc_oy + (fy + 1) * sc_scale > sc_bh) return 0;
    return sc_frame[(size_t) fy * (size_t) sc_frame_w + (size_t) fx];
}

/* The platform shows the kept frame; `changed` says its size or place changed since the last. */
static void sc_platform_present_frame(int changed);
#endif

int64_t sc_open(int64_t w, int64_t h, int64_t scale, int64_t mode, const char *title) {
    if (w <= 0 || h <= 0 || scale <= 0 || w * scale > 8192 || h * scale > 8192) return 0;
    sc_close();
    sc_fw = (int) w;
    sc_fh = (int) h;
    sc_scale = sc_want_scale = (int) scale;
    sc_fill = mode == 2;
    memset(sc_keys, 0, sizeof sc_keys);
    if (!sc_make_back((int) (w * scale), (int) (h * scale))) return 0;
    if (mode == 1) {
        sc_open_mode = 1;
        sc_alive = 1;
        return 1;
    }
    if (!sc_platform_open(title)) {
        free(sc_back);
        sc_back = NULL;
        return 0;
    }
    sc_open_mode = 0;
    sc_alive = 1;
    return 1;
}

void sc_close(void) {
    if (sc_open_mode == 0) sc_platform_close();
#if defined(SC_FRAME_PATH)
    sc_drop_frame();
#endif
    sc_open_mode = -1;
    sc_alive = 0;
    sc_have_keys = 0;
    sc_have_pointer = 0;
    sc_buttons = sc_wheel = 0;
    sc_typed_n = 0;
    free(sc_back);
    sc_back = NULL;
}

void sc_show(const int32_t *px, int64_t n, int64_t w, int64_t h) {
    int k = sc_want_scale;
    if (sc_open_mode < 0 || w <= 0 || h <= 0 || n < w * h) return;
    if (w != sc_fw || h != sc_fh) {
        sc_fw = (int) w;
        sc_fh = (int) h;
    }
    /* A frame drawn at the screen's own resolution is shown as it is. */
    while (k > 1 && (w * k > sc_bw || h * k > sc_bh)) k--;
    if (k != sc_scale) {
        sc_scale = k;
#if defined(SC_FRAME_PATH)
        sc_frame_w = sc_frame_h = 0;   /* where the frame sits moves with its scale */
#endif
    }
#if defined(SC_FRAME_PATH)
    if (sc_open_mode == 0) {
        int changed = sc_keep_frame(px, (int) w, (int) h);
        if (changed >= 0) sc_platform_present_frame(changed);
        return;
    }
#endif
    sc_scale_into_back(px, (int) w, (int) h);
    if (sc_open_mode == 0) sc_platform_present();
}

int64_t sc_poll(int64_t wait_ms) {
    if (sc_open_mode == 0) sc_platform_wait((int) (wait_ms < 0 ? 0 : wait_ms));
    return sc_alive;
}

int64_t sc_has_keys(void) { return sc_open_mode == 0 && sc_have_keys; }

int64_t sc_key(int64_t code) { return code > 0 && code < 256 && sc_keys[code]; }

int64_t sc_shown(int64_t x, int64_t y) {
#if defined(SC_FRAME_PATH)
    if (sc_open_mode == 0) return sc_frame_shown(x, y);
#endif
    if (!sc_back || x < 0 || y < 0 || x >= sc_bw || y >= sc_bh) return -1;
    return sc_back[(size_t) y * (size_t) sc_bw + (size_t) x];
}

int64_t sc_width(void) { return sc_bw; }

/* 0 the pointer's x, 1 its y, 2 its buttons, 3 the wheel's notches since the last call (then 0),
 * 4 whether this screen has a pointer at all. */
int64_t sc_pointer(int64_t which) {
    int w;
    switch (which) {
    case 0: return sc_px;
    case 1: return sc_py;
    case 2: return sc_buttons;
    case 3: w = sc_wheel; sc_wheel = 0; return w;
    case 4: return sc_open_mode == 0 && sc_have_pointer;
    }
    return 0;
}

/* The text typed since it was last taken. */
const char *sc_text(void) { sc_typed[sc_typed_n] = 0; return sc_typed; }

void sc_text_taken(void) { sc_typed_n = 0; sc_typed[0] = 0; }

int64_t sc_height(void) { return sc_bh; }

/* Whether this screen's keys are the PROGRAM's, whichever of its windows is in front: macOS has
 * one event queue for the application and this screen drains it; an X11 window has its own. */
int64_t sc_program_keys(void) {
#if defined(__APPLE__) && !defined(__wasm__) && !defined(ESP_PLATFORM)
    return sc_open_mode == 0 && sc_have_keys;
#else
    return 0;
#endif
}

#if defined(ESP_PLATFORM)

/* ── Tanmatsu: the BSP display.  The panel is portrait and the frame landscape, so each back
 * buffer pixel is written rotated by the panel's default rotation. ── */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/display.h"

static uint8_t *sc_panel = NULL;
static size_t sc_ph = 0, sc_pv = 0;
static bsp_display_color_format_t sc_fmt;
static bsp_display_endianness_t sc_endian;
static bsp_display_rotation_t sc_rot;

static int sc_platform_open(const char *title) {
    size_t bpp;
    (void) title;
    if (bsp_display_get_parameters(&sc_ph, &sc_pv, &sc_fmt, &sc_endian) != ESP_OK) return 0;
    if (sc_fmt != BSP_DISPLAY_COLOR_FORMAT_16_565RGB && sc_fmt != BSP_DISPLAY_COLOR_FORMAT_24_888RGB) return 0;
    bpp = sc_fmt == BSP_DISPLAY_COLOR_FORMAT_16_565RGB ? 2 : 3;
    sc_rot = bsp_display_get_default_rotation();
    sc_panel = (uint8_t *) calloc(sc_ph * sc_pv, bpp);
    if (!sc_panel) return 0;
    /* The landscape screen the frame is centred in: the panel turned by its rotation. */
    if (sc_rot == BSP_DISPLAY_ROTATION_90 || sc_rot == BSP_DISPLAY_ROTATION_270) {
        if (!sc_make_back((int) sc_pv, (int) sc_ph)) return 0;
    } else {
        if (!sc_make_back((int) sc_ph, (int) sc_pv)) return 0;
    }
    return 1;
}

/* The back buffer turned into the panel, pixel by pixel: the reference the row path below
 * matches, and the path for a panel in 24-bit colour. */
static void sc_platform_present(void) {
    int x, y;
    int bpp = sc_fmt == BSP_DISPLAY_COLOR_FORMAT_16_565RGB ? 2 : 3;
    for (y = 0; y < sc_bh; y++) {
        for (x = 0; x < sc_bw; x++) {
            uint16_t c = sc_back[(size_t) y * (size_t) sc_bw + (size_t) x];
            size_t px, py, at;
            switch (sc_rot) {
            case BSP_DISPLAY_ROTATION_90: px = sc_ph - 1 - (size_t) y; py = (size_t) x; break;
            case BSP_DISPLAY_ROTATION_270: px = (size_t) y; py = sc_pv - 1 - (size_t) x; break;
            case BSP_DISPLAY_ROTATION_180: px = sc_ph - 1 - (size_t) x; py = sc_pv - 1 - (size_t) y; break;
            default: px = (size_t) x; py = (size_t) y; break;
            }
            at = (py * sc_ph + px) * (size_t) bpp;
            if (bpp == 2) {
                if (sc_endian == BSP_DISPLAY_ENDIAN_BIG) {
                    sc_panel[at] = (uint8_t) (c >> 8);
                    sc_panel[at + 1] = (uint8_t) c;
                } else {
                    sc_panel[at] = (uint8_t) c;
                    sc_panel[at + 1] = (uint8_t) (c >> 8);
                }
            } else {
                uint8_t r = (uint8_t) (((c >> 11) & 31) * 255 / 31), g = (uint8_t) (((c >> 5) & 63) * 255 / 63),
                        b = (uint8_t) ((c & 31) * 255 / 31);
                sc_panel[at] = sc_endian == BSP_DISPLAY_ENDIAN_BIG ? r : b;
                sc_panel[at + 1] = g;
                sc_panel[at + 2] = sc_endian == BSP_DISPLAY_ENDIAN_BIG ? b : r;
            }
        }
    }
    bsp_display_blit(0, 0, sc_ph, sc_pv, sc_panel);
}

static int sc_panel_clean = 0;      /* the panel outside the frame is black and blitted */
static uint16_t *sc_line = NULL;    /* one landscape row or column, sc_ph long */

/* A pixel as the panel stores it. */
static uint16_t sc_wire(uint16_t c) {
    return sc_endian == BSP_DISPLAY_ENDIAN_BIG ? (uint16_t) ((c >> 8) | (c << 8)) : c;
}

/* `n` frame values, `stride` apart from `src`, each written `s` times into `line` (`len` long)
 * from `at` on; a block that does not fit whole is left black, as `sc_scale_into_back` leaves
 * it. */
static void sc_fill_line(uint16_t *line, long len, long at, const uint16_t *src, size_t stride, int n, int s) {
    int k, j;
    memset(line, 0, (size_t) len * sizeof(uint16_t));
    for (k = 0; k < n; k++) {
        long b = at + (long) k * s;
        uint16_t c;
        if (b < 0 || b + s > len) continue;
        c = sc_wire(src[(size_t) k * stride]);
        for (j = 0; j < s; j++) line[b + j] = c;
    }
}

/* The panel from the frame, a panel row at a time — the same pixels `sc_platform_present`
 * writes from the back buffer.  A panel row is one landscape column (90, 270) or row (0,
 * 180): one column or row of the frame stretched by the scale, so the `s` panel rows one
 * frame column or row makes are the same and the first is copied.  Only the rows the frame
 * covers are written and blitted; the rest stay black from when the frame's size last changed. */
static void sc_present_rows(void) {
    int s = sc_scale;
    int turned = sc_rot == BSP_DISPLAY_ROTATION_90 || sc_rot == BSP_DISPLAY_ROTATION_270;
    /* Landscape line `l` is panel row `l` (0, 90) or `sc_pv - 1 - l` (180, 270); along it the
     * landscape order is the panel's (0, 270) or its reverse (90, 180). */
    int row_back = sc_rot == BSP_DISPLAY_ROTATION_180 || sc_rot == BSP_DISPLAY_ROTATION_270;
    int along_back = sc_rot == BSP_DISPLAY_ROTATION_90 || sc_rot == BSP_DISPLAY_ROTATION_180;
    long len = (long) sc_ph, at = turned ? sc_oy : sc_ox;
    long across = turned ? (long) sc_bw : (long) sc_bh, from = turned ? sc_ox : sc_oy;
    int n_along = turned ? sc_frame_h : sc_frame_w, n_across = turned ? sc_frame_w : sc_frame_h;
    size_t stride = turned ? (size_t) sc_frame_w : 1;
    long first = -1, last = -1, k, x;
    uint16_t *panel = (uint16_t *) sc_panel;
    for (k = 0; k < n_across; k++) {
        long b = from + k * s, j;
        const uint16_t *src = turned ? sc_frame + k : sc_frame + (size_t) k * (size_t) sc_frame_w;
        uint16_t *row0 = NULL;
        if (b < 0 || b + s > across) continue;
        sc_fill_line(sc_line, len, at, src, stride, n_along, s);
        for (j = 0; j < s; j++) {
            long l = b + j, py = row_back ? (long) sc_pv - 1 - l : l;
            uint16_t *row = panel + (size_t) py * sc_ph;
            if (row0) {
                memcpy(row, row0, sc_ph * sizeof(uint16_t));
            } else {
                if (along_back) for (x = 0; x < len; x++) row[x] = sc_line[len - 1 - x];
                else memcpy(row, sc_line, sc_ph * sizeof(uint16_t));
                row0 = row;
            }
            if (first < 0 || py < first) first = py;
            if (py > last) last = py;
        }
    }
    if (!sc_panel_clean) {
        bsp_display_blit(0, 0, sc_ph, sc_pv, sc_panel);
        sc_panel_clean = 1;
    } else if (first >= 0) {
        bsp_display_blit(0, (size_t) first, sc_ph, (size_t) last + 1, panel + (size_t) first * sc_ph);
    }
}

static void sc_platform_present_frame(int changed) {
    if (changed) {
        /* A new size can leave the old picture outside the new one: start from black. */
        memset(sc_panel, 0, sc_ph * sc_pv * (sc_fmt == BSP_DISPLAY_COLOR_FORMAT_16_565RGB ? 2 : 3));
        sc_panel_clean = 0;
    }
    if (sc_fmt != BSP_DISPLAY_COLOR_FORMAT_16_565RGB) {
        sc_scale_from(NULL, sc_frame, sc_frame_w, sc_frame_h);
        sc_platform_present();
        return;
    }
    if (!sc_line) sc_line = (uint16_t *) malloc(sc_ph * sizeof(uint16_t));
    if (!sc_line) return;
    sc_present_rows();
}

static void sc_platform_wait(int wait_ms) {
    if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

/* Closing leaves the panel black, so what comes next (the launcher, another screen) starts
 * from nothing. */
static void sc_platform_close(void) {
    if (sc_panel) {
        memset(sc_panel, 0, sc_ph * sc_pv * (sc_fmt == BSP_DISPLAY_COLOR_FORMAT_16_565RGB ? 2 : 3));
        bsp_display_blit(0, 0, sc_ph, sc_pv, sc_panel);
        bsp_display_wait(100);
    }
    free(sc_panel);
    sc_panel = NULL;
    free(sc_line);
    sc_line = NULL;
    sc_panel_clean = 0;
}

#elif !defined(__wasm__) && !defined(_WIN32) && !defined(__APPLE__) && defined(__has_include)
#if __has_include(<X11/Xlib.h>)
#define SC_X11 1
#endif
#endif
#if defined(__APPLE__) && !defined(__wasm__) && !defined(ESP_PLATFORM)
#define SC_COCOA 1
#endif

#if defined(SC_X11)

/* ── X11: a window, libX11 opened at run time ── */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <sys/select.h>
#include <sys/time.h>

static struct {
    void *lib;
    Display *(*p_OpenDisplay)(const char *);
    int (*p_CloseDisplay)(Display *);
    Window (*p_CreateSimpleWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
    int (*p_StoreName)(Display *, Window, const char *);
    int (*p_SelectInput)(Display *, Window, long);
    int (*p_MapWindow)(Display *, Window);
    Atom (*p_InternAtom)(Display *, const char *, Bool);
    Status (*p_SetWMProtocols)(Display *, Window, Atom *, int);
    GC (*p_CreateGC)(Display *, Drawable, unsigned long, XGCValues *);
    int (*p_FreeGC)(Display *, GC);
    XImage *(*p_CreateImage)(Display *, Visual *, unsigned, int, int, char *, unsigned, unsigned, int, int);
    int (*p_PutImage)(Display *, Drawable, GC, XImage *, int, int, int, int, unsigned, unsigned);
    int (*p_Flush)(Display *);
    int (*p_Pending)(Display *);
    int (*p_NextEvent)(Display *, XEvent *);
    KeySym (*p_LookupKeysym)(XKeyEvent *, int);
    int (*p_LookupString)(XKeyEvent *, char *, int, KeySym *, XComposeStatus *);
    int (*p_DestroyWindow)(Display *, Window);
    int (*p_ConnectionNumber)(Display *);
    int (*p_DefaultScreen)(Display *);
    Window (*p_RootWindow)(Display *, int);
    Visual *(*p_DefaultVisual)(Display *, int);
    int (*p_DefaultDepth)(Display *, int);
    int (*p_Free)(void *);
} X;

static Display *sc_dpy = NULL;
static Window sc_win;
static GC sc_gc;
static XImage *sc_img = NULL;
static uint32_t *sc_pixels = NULL;
static Atom sc_wm_delete;

static int sc_load_x11(void) {
    if (X.lib) return 1;
    X.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!X.lib) return 0;
#define SC_SYM(name) *(void **) (&X.p_##name) = dlsym(X.lib, "X" #name); if (!X.p_##name) return 0
    SC_SYM(OpenDisplay); SC_SYM(CloseDisplay); SC_SYM(CreateSimpleWindow); SC_SYM(StoreName);
    SC_SYM(SelectInput); SC_SYM(MapWindow); SC_SYM(InternAtom); SC_SYM(SetWMProtocols);
    SC_SYM(CreateGC); SC_SYM(FreeGC); SC_SYM(CreateImage); SC_SYM(PutImage); SC_SYM(Flush);
    SC_SYM(Pending); SC_SYM(NextEvent); SC_SYM(LookupKeysym); SC_SYM(LookupString); SC_SYM(DestroyWindow);
    SC_SYM(ConnectionNumber); SC_SYM(DefaultScreen); SC_SYM(RootWindow); SC_SYM(DefaultVisual);
    SC_SYM(DefaultDepth); SC_SYM(Free);
#undef SC_SYM
    return 1;
}

/* An X keysym as graphics' key code; 0 for a key graphics has no code for. */
static int sc_code(KeySym k) {
    if (k >= XK_a && k <= XK_z) return (int) k;
    if (k >= XK_A && k <= XK_Z) return (int) (k - XK_A + XK_a);
    if (k >= XK_0 && k <= XK_9) return (int) k;
    if (k >= XK_F1 && k <= XK_F12) return 135 + (int) (k - XK_F1);
    switch (k) {
    case XK_space: return 32;
    case XK_Return: case XK_KP_Enter: return 13;
    case XK_Escape: return 27;
    case XK_BackSpace: return 8;
    case XK_Tab: return 9;
    case XK_Up: return 128;
    case XK_Down: return 129;
    case XK_Left: return 130;
    case XK_Right: return 131;
    case XK_Shift_L: case XK_Shift_R: return 132;
    case XK_Control_L: case XK_Control_R: return 133;
    case XK_Delete: return 147;
    case XK_Insert: return 148;
    case XK_Home: return 149;
    case XK_End: return 150;
    case XK_Prior: return 151;
    case XK_Next: return 152;
    case XK_bracketleft: return 91;
    case XK_bracketright: return 93;
    default: return 0;
    }
}

static int sc_platform_open(const char *title) {
    int scr;
    if (!sc_load_x11()) return 0;
    sc_dpy = X.p_OpenDisplay(NULL);
    if (!sc_dpy) return 0;
    scr = X.p_DefaultScreen(sc_dpy);
    if (X.p_DefaultDepth(sc_dpy, scr) < 24) {
        X.p_CloseDisplay(sc_dpy);
        sc_dpy = NULL;
        return 0;
    }
    sc_win = X.p_CreateSimpleWindow(sc_dpy, X.p_RootWindow(sc_dpy, scr), 0, 0, (unsigned) sc_bw, (unsigned) sc_bh, 0, 0, 0);
    X.p_StoreName(sc_dpy, sc_win, title && *title ? title : "loft");
    X.p_SelectInput(sc_dpy, sc_win, ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask |
                                        FocusChangeMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    sc_wm_delete = X.p_InternAtom(sc_dpy, "WM_DELETE_WINDOW", False);
    X.p_SetWMProtocols(sc_dpy, sc_win, &sc_wm_delete, 1);
    sc_gc = X.p_CreateGC(sc_dpy, sc_win, 0, NULL);
    sc_pixels = (uint32_t *) calloc((size_t) sc_bw * (size_t) sc_bh, sizeof(uint32_t));
    if (!sc_pixels) return 0;
    sc_img = X.p_CreateImage(sc_dpy, X.p_DefaultVisual(sc_dpy, scr), 24, ZPixmap, 0, (char *) sc_pixels,
                           (unsigned) sc_bw, (unsigned) sc_bh, 32, 0);
    X.p_MapWindow(sc_dpy, sc_win);
    X.p_Flush(sc_dpy);
    sc_have_keys = 1;
    sc_have_pointer = 1;
    return 1;
}

static void sc_platform_present(void) {
    size_t i, n = (size_t) sc_bw * (size_t) sc_bh;
    for (i = 0; i < n; i++) {
        uint16_t c = sc_back[i];
        uint32_t r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        sc_pixels[i] = (r << 16) | (g << 8) | b;
    }
    X.p_PutImage(sc_dpy, sc_win, sc_gc, sc_img, 0, 0, 0, 0, (unsigned) sc_bw, (unsigned) sc_bh);
    X.p_Flush(sc_dpy);
}

/* Handle every queued event, then wait up to `wait_ms` for more. */
static void sc_platform_wait(int wait_ms) {
    for (;;) {
        while (sc_dpy && X.p_Pending(sc_dpy)) {
            XEvent e;
            X.p_NextEvent(sc_dpy, &e);
            if (e.type == KeyPress || e.type == KeyRelease) {
                int code = sc_code(X.p_LookupKeysym(&e.xkey, 0));
                if (code > 0) sc_keys[code] = e.type == KeyPress;
                if (e.type == KeyPress) {
                    /* What the key types, in Latin-1, widened to UTF-8. */
                    char t[16], u[32];
                    int n = X.p_LookupString(&e.xkey, t, (int) sizeof t, NULL, NULL), i, k = 0;
                    for (i = 0; i < n; i++) {
                        unsigned char ch = (unsigned char) t[i];
                        if (ch < 128) u[k++] = (char) ch;
                        else { u[k++] = (char) (0xC0 | (ch >> 6)); u[k++] = (char) (0x80 | (ch & 63)); }
                    }
                    sc_type(u, k);
                }
            } else if (e.type == ButtonPress || e.type == ButtonRelease) {
                /* X's buttons: 1 left, 2 middle, 3 right, 4 and 5 the wheel. */
                int b = e.xbutton.button == 1 ? 1 : e.xbutton.button == 3 ? 2 : e.xbutton.button == 2 ? 4 : 0;
                sc_px = e.xbutton.x;
                sc_py = e.xbutton.y;
                if (b) sc_buttons = e.type == ButtonPress ? sc_buttons | b : sc_buttons & ~b;
                if (e.type == ButtonPress && e.xbutton.button == 4) sc_wheel++;
                if (e.type == ButtonPress && e.xbutton.button == 5) sc_wheel--;
            } else if (e.type == MotionNotify) {
                sc_px = e.xmotion.x;
                sc_py = e.xmotion.y;
            } else if (e.type == Expose) {
                sc_platform_present();
            } else if (e.type == FocusOut) {
                memset(sc_keys, 0, sizeof sc_keys); /* keys let go while away never release */
            } else if (e.type == ClientMessage && (Atom) e.xclient.data.l[0] == sc_wm_delete) {
                sc_alive = 0;
            } else if (e.type == DestroyNotify) {
                sc_alive = 0;
            }
        }
        if (wait_ms <= 0 || !sc_alive || !sc_dpy) return;
        {
            fd_set fds;
            struct timeval tv;
            int fd = X.p_ConnectionNumber(sc_dpy);
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            tv.tv_sec = wait_ms / 1000;
            tv.tv_usec = (wait_ms % 1000) * 1000;
            if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) return;
            wait_ms = 0; /* handle what arrived, then answer */
        }
    }
}

static void sc_platform_close(void) {
    if (!sc_dpy) return;
    if (sc_img) {
        sc_img->data = NULL; /* the pixels are ours */
        X.p_Free(sc_img);
        sc_img = NULL;
    }
    free(sc_pixels);
    sc_pixels = NULL;
    X.p_FreeGC(sc_dpy, sc_gc);
    X.p_DestroyWindow(sc_dpy, sc_win);
    X.p_CloseDisplay(sc_dpy);
    sc_dpy = NULL;
}

#elif defined(__wasm__) && defined(LOFT_BROWSER)

/* ── A browser page: the canvas, through the page's `loft_gl` imports (loft's
 * doc/loft-gl-wasm.js).  The kept frame goes up as one texture, its RGB565 widened exactly as
 * the X11 window widens it, and the GPU draws it scaled by nearest neighbour where its picture
 * sits, so the page shows the pixels a desktop window shows.  `poll` hands the browser a frame
 * (`gg_present` suspends the program until the next one) and takes the page's key events,
 * which arrive in graphics' key codes. ── */
#define SC_IMPORT(name) __attribute__((import_module("loft_gl"), import_name(#name)))
SC_IMPORT(gg_js_open) int sc_js_open(int width, int height, int fullscreen);
SC_IMPORT(gg_js_size) int sc_js_size(int which);
SC_IMPORT(gg_present) void sc_js_present(void);
SC_IMPORT(gg_js_event) int sc_js_event(int *out, char *text, int cap);
SC_IMPORT(gg_glViewport) void sc_glViewport(int, int, int, int);
SC_IMPORT(gg_glClearColor) void sc_glClearColor(float, float, float, float);
SC_IMPORT(gg_glClear) void sc_glClear(unsigned);
SC_IMPORT(gg_glCreateShader) unsigned sc_glCreateShader(unsigned);
SC_IMPORT(gg_glShaderSource) void sc_glShaderSource(unsigned, int, const char *const *, const int *);
SC_IMPORT(gg_glCompileShader) void sc_glCompileShader(unsigned);
SC_IMPORT(gg_glCreateProgram) unsigned sc_glCreateProgram(void);
SC_IMPORT(gg_glAttachShader) void sc_glAttachShader(unsigned, unsigned);
SC_IMPORT(gg_glLinkProgram) void sc_glLinkProgram(unsigned);
SC_IMPORT(gg_glUseProgram) void sc_glUseProgram(unsigned);
SC_IMPORT(gg_glGetUniformLocation) int sc_glGetUniformLocation(unsigned, const char *);
SC_IMPORT(gg_glUniform1i) void sc_glUniform1i(int, int);
SC_IMPORT(gg_glUniform4f) void sc_glUniform4f(int, float, float, float, float);
SC_IMPORT(gg_glGenTextures) void sc_glGenTextures(int, unsigned *);
SC_IMPORT(gg_glBindTexture) void sc_glBindTexture(unsigned, unsigned);
SC_IMPORT(gg_glActiveTexture) void sc_glActiveTexture(unsigned);
SC_IMPORT(gg_glTexImage2D) void sc_glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
SC_IMPORT(gg_glTexSubImage2D) void sc_glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
SC_IMPORT(gg_glTexParameteri) void sc_glTexParameteri(unsigned, unsigned, int);
SC_IMPORT(gg_glGenVertexArrays) void sc_glGenVertexArrays(int, unsigned *);
SC_IMPORT(gg_glBindVertexArray) void sc_glBindVertexArray(unsigned);
SC_IMPORT(gg_glDrawArrays) void sc_glDrawArrays(unsigned, int, int);

#define SC_GL_TEXTURE_2D 0x0DE1
#define SC_GL_TEXTURE0 0x84C0
#define SC_GL_RGBA 0x1908
#define SC_GL_UNSIGNED_BYTE 0x1401
#define SC_GL_NEAREST 0x2600
#define SC_GL_CLAMP_TO_EDGE 0x812F
#define SC_GL_MAG_FILTER 0x2800
#define SC_GL_MIN_FILTER 0x2801
#define SC_GL_WRAP_S 0x2802
#define SC_GL_WRAP_T 0x2803
#define SC_GL_COLOR_BUFFER_BIT 0x4000
#define SC_GL_TRIANGLE_STRIP 0x0005
#define SC_GL_VERTEX_SHADER 0x8B31
#define SC_GL_FRAGMENT_SHADER 0x8B30

static unsigned sc_prog = 0, sc_tex = 0, sc_vao = 0;
static int sc_u_rect = -1;
static unsigned char *sc_rgba = NULL;
static int sc_tex_w = 0, sc_tex_h = 0;

/* A quad over `u_rect` (left, top, right, bottom in clip space), its corners from the vertex
 * number alone, the frame's first row at its top. */
static const char *const sc_vs =
    "#version 330 core\n"
    "uniform vec4 u_rect;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  v_uv = c;\n"
    "  gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);\n"
    "}\n";
static const char *const sc_fs =
    "#version 330 core\n"
    "uniform sampler2D u_frame;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = texture(u_frame, v_uv); }\n";

static unsigned sc_shader(unsigned kind, const char *src) {
    unsigned sh = sc_glCreateShader(kind);
    sc_glShaderSource(sh, 1, &src, NULL);
    sc_glCompileShader(sh);
    return sh;
}

static int sc_platform_open(const char *title) {
    (void) title;
    if (!sc_js_open(sc_bw, sc_bh, sc_fill)) return 0;
    /* Filling the window, the screen is the canvas's size in device pixels. */
    if (sc_fill && !sc_make_back(sc_js_size(0), sc_js_size(1))) return 0;
    if (!sc_prog) {
        sc_prog = sc_glCreateProgram();
        sc_glAttachShader(sc_prog, sc_shader(SC_GL_VERTEX_SHADER, sc_vs));
        sc_glAttachShader(sc_prog, sc_shader(SC_GL_FRAGMENT_SHADER, sc_fs));
        sc_glLinkProgram(sc_prog);
        sc_glGenTextures(1, &sc_tex);
        sc_glGenVertexArrays(1, &sc_vao);
    }
    sc_glUseProgram(sc_prog);
    sc_u_rect = sc_glGetUniformLocation(sc_prog, "u_rect");
    sc_glUniform1i(sc_glGetUniformLocation(sc_prog, "u_frame"), 0);
    sc_glActiveTexture(SC_GL_TEXTURE0);
    sc_glBindTexture(SC_GL_TEXTURE_2D, sc_tex);
    sc_glTexParameteri(SC_GL_TEXTURE_2D, SC_GL_MAG_FILTER, SC_GL_NEAREST);
    sc_glTexParameteri(SC_GL_TEXTURE_2D, SC_GL_MIN_FILTER, SC_GL_NEAREST);
    sc_glTexParameteri(SC_GL_TEXTURE_2D, SC_GL_WRAP_S, SC_GL_CLAMP_TO_EDGE);
    sc_glTexParameteri(SC_GL_TEXTURE_2D, SC_GL_WRAP_T, SC_GL_CLAMP_TO_EDGE);
    sc_tex_w = sc_tex_h = 0;
    sc_have_keys = 1;
    sc_have_pointer = 1;
    return 1;
}

/* Draw the kept frame: widened into RGBA, uploaded, and drawn over where its picture sits on the
 * back buffer's screen; the rest of the canvas black. */
static void sc_platform_present_frame(int changed) {
    size_t i, n = (size_t) sc_frame_w * (size_t) sc_frame_h;
    float l, t, r, b;
    (void) changed;
    if (sc_frame_w != sc_tex_w || sc_frame_h != sc_tex_h) {
        unsigned char *p = (unsigned char *) realloc(sc_rgba, n * 4);
        if (!p) return;
        sc_rgba = p;
    }
    for (i = 0; i < n; i++) {
        uint16_t c = sc_frame[i];
        sc_rgba[i * 4] = (unsigned char) (((c >> 11) & 31) * 255 / 31);
        sc_rgba[i * 4 + 1] = (unsigned char) (((c >> 5) & 63) * 255 / 63);
        sc_rgba[i * 4 + 2] = (unsigned char) ((c & 31) * 255 / 31);
        sc_rgba[i * 4 + 3] = 255;
    }
    if (sc_frame_w != sc_tex_w || sc_frame_h != sc_tex_h) {
        sc_glTexImage2D(SC_GL_TEXTURE_2D, 0, SC_GL_RGBA, sc_frame_w, sc_frame_h, 0, SC_GL_RGBA, SC_GL_UNSIGNED_BYTE, sc_rgba);
        sc_tex_w = sc_frame_w;
        sc_tex_h = sc_frame_h;
    } else {
        sc_glTexSubImage2D(SC_GL_TEXTURE_2D, 0, 0, 0, sc_frame_w, sc_frame_h, SC_GL_RGBA, SC_GL_UNSIGNED_BYTE, sc_rgba);
    }
    sc_glViewport(0, 0, sc_js_size(0), sc_js_size(1));
    sc_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    sc_glClear(SC_GL_COLOR_BUFFER_BIT);
    l = -1.0f + 2.0f * (float) sc_ox / (float) sc_bw;
    r = -1.0f + 2.0f * (float) (sc_ox + sc_frame_w * sc_scale) / (float) sc_bw;
    t = 1.0f - 2.0f * (float) sc_oy / (float) sc_bh;
    b = 1.0f - 2.0f * (float) (sc_oy + sc_frame_h * sc_scale) / (float) sc_bh;
    sc_glUseProgram(sc_prog);
    sc_glBindVertexArray(sc_vao);
    sc_glUniform4f(sc_u_rect, l, t, r, b);
    sc_glDrawArrays(SC_GL_TRIANGLE_STRIP, 0, 4);
}

/* The back-buffer path is off-screen mode's alone here. */
static void sc_platform_present(void) { }

/* The page's events since the last call: a key down or up by graphics' code, and a window
 * resized (the screen takes the canvas's new size; `width` and `height` answer it). */
static void sc_take_events(void) {
    int v[5];
    char text[64];
    int tag;
    while ((tag = sc_js_event(v, text, (int) sizeof text)) != 0) {
        if ((tag == 1 || tag == 2) && v[0] > 0 && v[0] < 256) sc_keys[v[0]] = tag == 1;
        if (tag == 3) sc_type(text, v[0] < (int) sizeof text ? v[0] : (int) sizeof text);
        if (tag == 4 || tag == 5) {
            sc_px = v[1];
            sc_py = v[2];
            sc_buttons = tag == 4 ? sc_buttons | v[0] : sc_buttons & ~v[0];
        }
        if (tag == 6) {
            sc_px = v[0];
            sc_py = v[1];
        }
        if (tag == 7) sc_wheel += v[0];
        if (tag == 8 && v[0] > 0 && v[1] > 0 && sc_make_back(v[0], v[1])) sc_frame_w = sc_frame_h = 0;
    }
}

/* Take what happened, and with a wait hand the browser a frame: the program is suspended until
 * the next one, which is the page's pace (a wait shorter than a frame is a frame). */
static void sc_platform_wait(int wait_ms) {
    sc_take_events();
    if (wait_ms > 0) {
        sc_js_present();
        sc_take_events();
    }
}

static void sc_platform_close(void) {
    sc_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    sc_glClear(SC_GL_COLOR_BUFFER_BIT);
    free(sc_rgba);
    sc_rgba = NULL;
    sc_tex_w = sc_tex_h = 0;
}

#elif defined(SC_COCOA)

/* ── macOS: a window through AppKit, by the Objective-C runtime.  libobjc and AppKit are opened
 * at run time and every message goes through `objc_msgSend` cast to the method's own type, so
 * this builds with loft's fixed flags and no SDK, as graphics' window does.  The back buffer is
 * widened exactly as the X11 window widens it and handed to the content view's layer as one
 * image, magnified by nearest neighbour where the display has more pixels than points.  AppKit
 * makes and drives a window on the main thread: every job below runs there. ── */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

typedef void *sm_id;
typedef void *sm_sel;
typedef struct {
    double x, y;
} sm_point;
typedef struct {
    double x, y, width, height;
} sm_rect;

static struct {
    int loaded; /* 1 bound, -1 could not be */
    sm_id (*get_class)(const char *);
    sm_sel (*selector)(const char *);
    void (*msg)(void); /* objc_msgSend, cast at each call */
    void *(*pool_push)(void);
    void (*pool_pop)(void *);
    void (*dispatch_sync_f)(void *, void *, void (*)(void *));
    void *main_queue;
    void *(*run_loop_main)(void);
    unsigned char (*run_loop_waiting)(void *);
    int main_served;
    void *(*space_rgb)(void);
    void *(*data_create)(void *, const uint8_t *, long);
    void *(*provider_create)(void *);
    void *(*image_create)(size_t, size_t, size_t, size_t, size_t, void *, uint32_t, void *,
                          const double *, unsigned char, int32_t);
    void (*cf_release)(void *);
    void *space;
    sm_id app, mode, window, layer;
    int was_key;
} sm;

static sm_sel sm_s(const char *n) { return sm.selector(n); }
static sm_id sm_class(const char *n) { return sm.get_class(n); }
static sm_id sm_o(sm_id o, const char *s) { return ((sm_id (*)(sm_id, sm_sel)) sm.msg)(o, sm_s(s)); }
static sm_id sm_o_p(sm_id o, const char *s, const void *a) {
    return ((sm_id (*)(sm_id, sm_sel, const void *)) sm.msg)(o, sm_s(s), a);
}
static void sm_v(sm_id o, const char *s) { ((void (*)(sm_id, sm_sel)) sm.msg)(o, sm_s(s)); }
static void sm_v_o(sm_id o, const char *s, sm_id a) {
    ((void (*)(sm_id, sm_sel, sm_id)) sm.msg)(o, sm_s(s), a);
}
static void sm_v_l(sm_id o, const char *s, long a) { /* an integer, enum or BOOL argument */
    ((void (*)(sm_id, sm_sel, long)) sm.msg)(o, sm_s(s), a);
}
static unsigned long sm_l(sm_id o, const char *s) {
    return ((unsigned long (*)(sm_id, sm_sel)) sm.msg)(o, sm_s(s));
}
static int sm_b(sm_id o, const char *s) { /* a BOOL: only its low byte is defined */
    return ((signed char (*)(sm_id, sm_sel)) sm.msg)(o, sm_s(s)) != 0;
}
static sm_id sm_str(const char *t) { return sm_o_p(sm_class("NSString"), "stringWithUTF8String:", t); }

#define SM_SYM(field, name) (*(void **) (&sm.field) = dlsym(RTLD_DEFAULT, name), sm.field != NULL)

static int sm_load(void) {
    void **mode;
    if (sm.loaded) return sm.loaded > 0;
    sm.loaded = -1;
    if (!dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL) ||
        !dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW | RTLD_GLOBAL))
        return 0;
    if (!(SM_SYM(get_class, "objc_getClass") && SM_SYM(selector, "sel_registerName") &&
          SM_SYM(msg, "objc_msgSend") && SM_SYM(pool_push, "objc_autoreleasePoolPush") &&
          SM_SYM(pool_pop, "objc_autoreleasePoolPop") && SM_SYM(dispatch_sync_f, "dispatch_sync_f") &&
          SM_SYM(main_queue, "_dispatch_main_q") && SM_SYM(run_loop_main, "CFRunLoopGetMain") &&
          SM_SYM(run_loop_waiting, "CFRunLoopIsWaiting") &&
          SM_SYM(space_rgb, "CGColorSpaceCreateDeviceRGB") && SM_SYM(data_create, "CFDataCreate") &&
          SM_SYM(provider_create, "CGDataProviderCreateWithCFData") &&
          SM_SYM(image_create, "CGImageCreate") && SM_SYM(cf_release, "CFRelease")))
        return 0;
    mode = (void **) dlsym(RTLD_DEFAULT, "NSDefaultRunLoopMode");
    if (!mode || !*mode) return 0;
    sm.mode = *mode;
    sm.loaded = 1;
    return 1;
}

/* Run `job` on the main thread and wait for it: here when this is the main thread (the
 * interpreter, the C backend), else through the main queue, which loft's native `main` drains.
 * 0 when the main thread is not in its run loop, so nothing would run the job. */
static int sm_on_main(void (*job)(void *), void *arg) {
    int i;
    if (pthread_main_np()) {
        job(arg);
        return 1;
    }
    for (i = 0; !sm.main_served && i < 200; i++) {
        if (sm.run_loop_waiting(sm.run_loop_main())) sm.main_served = 1;
        else usleep(10000);
    }
    if (!sm.main_served) return 0;
    sm.dispatch_sync_f(sm.main_queue, arg, job);
    return 1;
}

/* A virtual key code as graphics' key code, the keys the X11 window knows; 0 for any other. */
static int sm_code(unsigned kc, sm_id ev) {
    static const unsigned char fkeys[12] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61,
                                            0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};
    const char *chars;
    int i;
    for (i = 0; i < 12; i++)
        if (kc == fkeys[i]) return 135 + i;
    switch (kc) {
    case 0x31: return 32;             /* Space */
    case 0x24: case 0x4C: return 13;  /* Return, keypad Enter */
    case 0x35: return 27;             /* Escape */
    case 0x33: return 8;              /* Delete, which is Backspace */
    case 0x30: return 9;              /* Tab */
    case 0x7E: return 128;
    case 0x7D: return 129;
    case 0x7B: return 130;
    case 0x7C: return 131;
    case 0x38: case 0x3C: return 132; /* Shift */
    case 0x3B: case 0x3E: return 133; /* Control */
    case 0x75: return 147;            /* forward delete */
    case 0x72: return 148;            /* Help, where Insert is */
    case 0x73: return 149;
    case 0x77: return 150;
    case 0x74: return 151;
    case 0x79: return 152;
    default: break;
    }
    if (!ev) return 0;
    chars = (const char *) sm_o(sm_o(ev, "charactersIgnoringModifiers"), "UTF8String");
    if (!chars || chars[1] != 0) return 0;
    if (chars[0] >= 'a' && chars[0] <= 'z') return chars[0];
    if (chars[0] >= 'A' && chars[0] <= 'Z') return chars[0] + 32;
    if ((chars[0] >= '0' && chars[0] <= '9') || chars[0] == '[' || chars[0] == ']') return chars[0];
    return 0;
}

static void sm_event(sm_id ev, unsigned long type) {
    sm_point p;
    int x, y, bit = 0, inside;
    if (type == 10 || type == 11) { /* key down, key up */
        unsigned kc = (unsigned) sm_l(ev, "keyCode");
        int code = sm_code(kc, ev);
        if (code > 0) sc_keys[code] = type == 10;
        if (type == 10) {
            /* What the key types: AppKit's, except Delete (a backspace, as X11 types it), forward
             * delete, and the keys that type nothing (U+F700-U+F8FF here). */
            const char *t = (const char *) sm_o(sm_o(ev, "characters"), "UTF8String");
            int n = t ? (int) strlen(t) : 0;
            if (kc == 0x33) t = "\b", n = 1;
            else if (kc == 0x75) t = "\x7f", n = 1;
            else if (n == 3 && (unsigned char) t[0] == 0xEF && (unsigned char) t[1] >= 0x9C &&
                     (unsigned char) t[1] <= 0xA3)
                n = 0;
            sc_type(t, n);
        }
        return;
    }
    if (type == 12) { /* the modifier state changed: Shift and Control are keys here */
        unsigned kc = (unsigned) sm_l(ev, "keyCode");
        unsigned long flags = sm_l(ev, "modifierFlags");
        if (kc == 0x38 || kc == 0x3C) sc_keys[132] = (flags & (1ul << 17)) != 0;
        if (kc == 0x3B || kc == 0x3E) sc_keys[133] = (flags & (1ul << 18)) != 0;
        return;
    }
    /* The pointer, in pixels from the content's top left (AppKit's y runs up). */
    p = ((sm_point (*)(sm_id, sm_sel)) sm.msg)(ev, sm_s("locationInWindow"));
    x = (int) floor(p.x);
    y = (int) floor((double) sc_bh - p.y);
    inside = x >= 0 && y >= 0 && x < sc_bw && y < sc_bh;
    switch (type) {
    case 1: case 2: bit = 1; break;                                  /* left down, up */
    case 3: case 4: bit = 2; break;                                  /* right down, up */
    case 25: case 26: bit = sm_l(ev, "buttonNumber") == 2 ? 4 : 0; break; /* other down, up */
    case 5: case 6: case 7: case 27:                                 /* moved, dragged */
        if (!inside && !sc_buttons) return; /* over the title bar */
        sc_px = x;
        sc_py = y;
        return;
    case 22: { /* the wheel */
        double dy = ((double (*)(sm_id, sm_sel)) sm.msg)(ev, sm_s("scrollingDeltaY"));
        if (inside && dy != 0.0) sc_wheel += dy > 0.0 ? 1 : -1;
        return;
    }
    default: return;
    }
    if (!bit) return;
    if (type == 1 || type == 3 || type == 25) {
        if (!inside) return; /* the title bar: the window's, not the program's */
        sc_buttons |= bit;
    } else {
        sc_buttons &= ~bit;
    }
    sc_px = x;
    sc_py = y;
}

static sm_id sm_next(sm_id until) {
    return ((sm_id (*)(sm_id, sm_sel, unsigned long, sm_id, sm_id, signed char)) sm.msg)(
        sm.app, sm_s("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ul, until, sm.mode, 1);
}

/* Take every queued event.  Keys are answered here and not passed on (AppKit beeps at a key no
 * view takes); everything else goes on to AppKit, which moves and closes the window. */
static void sm_drain(sm_id first) {
    sm_id past = sm_o(sm_class("NSDate"), "distantPast"), ev = first;
    for (;;) {
        unsigned long type;
        if (!ev) ev = sm_next(past);
        if (!ev) break;
        type = sm_l(ev, "type");
        sm_event(ev, type);
        if (type != 10 && type != 11) sm_v_o(sm.app, "sendEvent:", ev);
        ev = NULL;
    }
    sm_v(sm.app, "updateWindows");
}

typedef struct {
    const char *title;
    int wait_ms, ok;
} sm_job;

static void sm_open_main(void *arg) {
    sm_job *job = (sm_job *) arg;
    void *pool = sm.pool_push();
    sm_rect content;
    sm_id view;
    if (!sm.app) {
        sm.app = sm_o(sm_class("NSApplication"), "sharedApplication");
        if (sm.app) {
            sm_v_l(sm.app, "setActivationPolicy:", 0); /* a regular app: a Dock icon, key windows */
            sm_v(sm.app, "finishLaunching");
        }
    }
    content.x = content.y = 0.0;
    content.width = (double) sc_bw;
    content.height = (double) sc_bh;
    if (sm.app)
        sm.window = ((sm_id (*)(sm_id, sm_sel, sm_rect, unsigned long, unsigned long, signed char)) sm.msg)(
            sm_o(sm_class("NSWindow"), "alloc"), sm_s("initWithContentRect:styleMask:backing:defer:"),
            content, 1ul | 2ul | 4ul /* titled, closable, miniaturizable */, 2ul /* buffered */, 0);
    if (sm.window) {
        sm_v_l(sm.window, "setReleasedWhenClosed:", 0);
        sm_v_o(sm.window, "setTitle:", sm_str(job->title && *job->title ? job->title : "loft"));
        sm_v_l(sm.window, "setAcceptsMouseMovedEvents:", 1);
        sm_v(sm.window, "center");
        view = sm_o(sm.window, "contentView");
        sm_v_l(view, "setWantsLayer:", 1);
        sm.layer = sm_o(view, "layer");
        sm_v_o(sm.layer, "setMagnificationFilter:", sm_str("nearest"));
        sm_v_o(sm.window, "makeKeyAndOrderFront:", NULL);
        sm_v_l(sm.app, "activateIgnoringOtherApps:", 1);
        if (!sm.space) sm.space = sm.space_rgb();
        sm.was_key = 0;
        job->ok = sm.layer != NULL && sm.space != NULL;
    }
    sm.pool_pop(pool);
}

static int sc_platform_open(const char *title) {
    sm_job job;
    job.title = title;
    job.wait_ms = 0;
    job.ok = 0;
    if (!sm_load() || !sm_on_main(sm_open_main, &job)) return 0;
    if (!job.ok) {
        sc_platform_close();
        return 0;
    }
    sc_have_keys = 1;
    sc_have_pointer = 1;
    return 1;
}

static void sm_present_main(void *arg) {
    size_t i, n = (size_t) sc_bw * (size_t) sc_bh;
    uint32_t *px = (uint32_t *) malloc(n * sizeof(uint32_t));
    void *pool, *data, *provider, *image;
    (void) arg;
    if (!px || !sm.layer) {
        free(px);
        return;
    }
    for (i = 0; i < n; i++) {
        uint16_t c = sc_back[i];
        uint32_t r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        px[i] = (r << 16) | (g << 8) | b;
    }
    pool = sm.pool_push();
    data = sm.data_create(NULL, (const uint8_t *) px, (long) (n * sizeof(uint32_t)));
    free(px);
    provider = data ? sm.provider_create(data) : NULL;
    /* 8 bits a component, 32 a pixel, the first byte skipped, little-endian: 0x00RRGGBB words. */
    image = provider ? sm.image_create((size_t) sc_bw, (size_t) sc_bh, 8, 32, (size_t) sc_bw * 4, sm.space,
                                       6u | (2u << 12), provider, NULL, 0, 0)
                     : NULL;
    if (image) {
        sm_v_o(sm.layer, "setContents:", (sm_id) image);
        sm_v(sm_class("CATransaction"), "flush");
        sm.cf_release(image);
    }
    if (provider) sm.cf_release(provider);
    if (data) sm.cf_release(data);
    sm.pool_pop(pool);
}

static void sc_platform_present(void) { sm_on_main(sm_present_main, NULL); }

/* Handle every queued event, then wait up to `wait_ms` for more, as the X11 window does. */
static void sm_wait_main(void *arg) {
    sm_job *job = (sm_job *) arg;
    void *pool;
    int key;
    if (!sm.window) return;
    pool = sm.pool_push();
    sm_drain(NULL);
    if (job->wait_ms > 0 && sc_alive) {
        sm_id until = ((sm_id (*)(sm_id, sm_sel, double)) sm.msg)(
            sm_class("NSDate"), sm_s("dateWithTimeIntervalSinceNow:"), (double) job->wait_ms / 1000.0);
        sm_id ev = sm_next(until);
        if (ev) sm_drain(ev);
    }
    key = sm_b(sm.window, "isKeyWindow");
    if (sm.was_key && !key) memset(sc_keys, 0, sizeof sc_keys); /* keys let go while away never release */
    sm.was_key = key;
    if (!sm_b(sm.window, "isVisible") && !sm_b(sm.window, "isMiniaturized")) sc_alive = 0; /* closed */
    sm.pool_pop(pool);
}

static void sc_platform_wait(int wait_ms) {
    sm_job job;
    job.title = NULL;
    job.wait_ms = wait_ms;
    job.ok = 0;
    sm_on_main(sm_wait_main, &job);
}

static void sm_close_main(void *arg) {
    void *pool = sm.pool_push();
    (void) arg;
    if (sm.window) {
        sm_v(sm.window, "close");
        sm_v(sm.window, "release");
        sm_drain(NULL); /* the window leaves the screen now, not at the next poll */
    }
    sm.window = sm.layer = NULL;
    sm.pool_pop(pool);
}

static void sc_platform_close(void) {
    if (sm.loaded > 0) sm_on_main(sm_close_main, NULL);
}

#elif !defined(ESP_PLATFORM)

/* ── No screen here yet (a WASI module, Windows): sc_open answers 0. ── */
static int sc_platform_open(const char *title) {
    (void) title;
    return 0;
}

static void sc_platform_present(void) { }

static void sc_platform_wait(int wait_ms) { (void) wait_ms; }

static void sc_platform_close(void) { }

#endif
