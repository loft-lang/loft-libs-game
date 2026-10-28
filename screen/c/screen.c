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
 *   Elsewhere (wasm, Windows, macOS): none yet — sc_open answers 0.
 *
 * Mode 1 of sc_open is OFF-SCREEN on every platform: the back buffer only, which is how the
 * scaling is tested where there is no display.
 *
 * Key codes are graphics' (`KEY_W` = 119, `KEY_UP` = 128, ...), as input's are. */
#if !defined(_POSIX_C_SOURCE) && !defined(_WIN32) && !defined(ESP_PLATFORM)
#define _POSIX_C_SOURCE 200809L
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

/* ── Shared: the back buffer, one RGB565 value per screen pixel ── */

static uint16_t *sc_back = NULL;
static int sc_bw = 0, sc_bh = 0;    /* the back buffer's size */
static int sc_fw = 0, sc_fh = 0;    /* the frame's size */
static int sc_scale = 1;
static int sc_open_mode = -1;       /* -1 closed, 0 on screen, 1 off-screen */
static unsigned char sc_keys[256];
static int sc_have_keys = 0;
static int sc_alive = 0;

/* Scale the frame `px` (w x h, RGB565) by sc_scale into the back buffer, centred; what the
 * frame does not cover is black. */
static void sc_scale_into_back(const int32_t *px, int w, int h) {
    int ox = (sc_bw - w * sc_scale) / 2, oy = (sc_bh - h * sc_scale) / 2, y;
    memset(sc_back, 0, (size_t) sc_bw * (size_t) sc_bh * sizeof(uint16_t));
    for (y = 0; y < h; y++) {
        int x, k, by = oy + y * sc_scale;
        uint16_t *row;
        if (by < 0 || by + sc_scale > sc_bh) continue;
        row = sc_back + (size_t) by * (size_t) sc_bw;
        for (x = 0; x < w; x++) {
            uint16_t c = (uint16_t) px[(size_t) y * (size_t) w + (size_t) x];
            int bx = ox + x * sc_scale;
            if (bx < 0 || bx + sc_scale > sc_bw) continue;
            for (k = 0; k < sc_scale; k++) row[bx + k] = c;
        }
        for (k = 1; k < sc_scale; k++) memcpy(row + (size_t) k * (size_t) sc_bw, row, (size_t) sc_bw * sizeof(uint16_t));
    }
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

int64_t sc_open(int64_t w, int64_t h, int64_t scale, int64_t mode, const char *title) {
    if (w <= 0 || h <= 0 || scale <= 0 || w * scale > 8192 || h * scale > 8192) return 0;
    sc_close();
    sc_fw = (int) w;
    sc_fh = (int) h;
    sc_scale = (int) scale;
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
    sc_open_mode = -1;
    sc_alive = 0;
    sc_have_keys = 0;
    free(sc_back);
    sc_back = NULL;
}

void sc_show(const int32_t *px, int64_t n, int64_t w, int64_t h) {
    if (sc_open_mode < 0 || w <= 0 || h <= 0 || n < w * h) return;
    if (w != sc_fw || h != sc_fh) {
        sc_fw = (int) w;
        sc_fh = (int) h;
    }
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
    if (!sc_back || x < 0 || y < 0 || x >= sc_bw || y >= sc_bh) return -1;
    return sc_back[(size_t) y * (size_t) sc_bw + (size_t) x];
}

int64_t sc_width(void) { return sc_bw; }

int64_t sc_height(void) { return sc_bh; }

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

static void sc_platform_wait(int wait_ms) {
    if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

static void sc_platform_close(void) {
    free(sc_panel);
    sc_panel = NULL;
}

#elif !defined(__wasm__) && !defined(_WIN32) && !defined(__APPLE__) && defined(__has_include)
#if __has_include(<X11/Xlib.h>)
#define SC_X11 1
#endif
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
    SC_SYM(Pending); SC_SYM(NextEvent); SC_SYM(LookupKeysym); SC_SYM(DestroyWindow);
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
    X.p_SelectInput(sc_dpy, sc_win, ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask | FocusChangeMask);
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

#elif !defined(ESP_PLATFORM)

/* ── No screen here yet (the browser, Windows, macOS): sc_open answers 0. ── */
static int sc_platform_open(const char *title) {
    (void) title;
    return 0;
}

static void sc_platform_present(void) { }

static void sc_platform_wait(int wait_ms) { (void) wait_ms; }

static void sc_platform_close(void) { }

#endif
