/* Copyright (c) 2026 Jurjen Stellingwerff
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * screen's macOS window, in one process: opens a window through the shim, shows a frame and
 * reads the image the window's layer was handed back; posts keys and clicks into the
 * application's own AppKit queue (no accessibility grant needed) and reads what `sc_poll` made
 * of them; closes the window.  Every expected value is computed by hand below.
 * `SC_CHECK_BREAK=1` shows another frame than the one checked: the check must then fail.
 *   screen/desktop/mac_check.sh */
#include <stdio.h>
#include "../c/screen.c"

#if defined(SC_COCOA)
typedef void *cg_ref;
typedef struct {
    double x, y;
} cg_point;
static cg_ref (*cg_key)(cg_ref, unsigned short, int);
static cg_ref (*cg_scroll)(cg_ref, uint32_t, uint32_t, int32_t, ...);
static void (*cg_set_type)(cg_ref, uint32_t);
static void (*cg_set_flags)(cg_ref, uint64_t);
static void (*cg_set_location)(cg_ref, cg_point);
static size_t (*cg_image_width)(void *);
static size_t (*cg_image_height)(void *);
static void *(*cg_image_provider)(void *);
static void *(*cg_provider_copy)(void *);
static const uint8_t *(*cf_data_bytes)(void *);
static long (*cf_data_length)(void *);
static long win_number;
static int failed;

static void expect(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed = 1;
}

static void post(sm_id ev) {
    ((void (*)(sm_id, sm_sel, sm_id, signed char)) sm.msg)(sm.app, sm_s("postEvent:atStart:"), ev, 0);
}

static void key(unsigned short code, int press, uint64_t flags) {
    cg_ref e = cg_key(NULL, code, press);
    cg_set_flags(e, flags);
    post(sm_o_p(sm_class("NSEvent"), "eventWithCGEvent:", e));
    sm.cf_release(e);
}

static void modifier(unsigned short code, uint64_t flags) {
    cg_ref e = cg_key(NULL, code, 1);
    cg_set_type(e, 12); /* kCGEventFlagsChanged */
    cg_set_flags(e, flags);
    post(sm_o_p(sm_class("NSEvent"), "eventWithCGEvent:", e));
    sm.cf_release(e);
}

/* A mouse event at (x, y) from the content's top left. */
static void mouse(unsigned long type, int x, int y) {
    sm_point p;
    p.x = (double) x;
    p.y = (double) (sc_bh - y);
    post(((sm_id (*)(sm_id, sm_sel, unsigned long, sm_point, unsigned long, double, long, sm_id, long,
                     long, float)) sm.msg)(
        sm_class("NSEvent"),
        sm_s("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:"
             "clickCount:pressure:"),
        type, p, 0ul, 0.0, win_number, NULL, 0l, 1l, 1.0f));
}

/* A wheel step over the content.  An in-process scroll event has no window, so its
 * `locationInWindow` is the screen point from the bottom; the CoreGraphics location (from the
 * top) is chosen to make that (x, height - y), and the event goes straight to the shim. */
static void wheel(int delta, int x, int y) {
    sm_rect screen;
    cg_point at;
    cg_ref e = cg_scroll(NULL, 1, 1, delta);
    /* The first screen is the one CoreGraphics measures from; `mainScreen` is the key window's. */
    sm_id main_screen = ((sm_id (*)(sm_id, sm_sel, unsigned long)) sm.msg)(
        sm_o(sm_class("NSScreen"), "screens"), sm_s("objectAtIndex:"), 0ul);
#if defined(__x86_64__)
    void (*stret)(void) = (void (*)(void)) dlsym(RTLD_DEFAULT, "objc_msgSend_stret");
    ((void (*)(sm_rect *, sm_id, sm_sel)) stret)(&screen, main_screen, sm_s("frame"));
#else
    screen = ((sm_rect (*)(sm_id, sm_sel)) sm.msg)(main_screen, sm_s("frame"));
#endif
    at.x = (double) x;
    at.y = screen.height - (double) (sc_bh - y);
    cg_set_location(e, at);
    sm_event(sm_o_p(sm_class("NSEvent"), "eventWithCGEvent:", e), 22);
    sm.cf_release(e);
}

static void polls(int n) {
    int i;
    for (i = 0; i < n; i++) sc_poll(0);
}

#define CG_SYM(var, name) (*(void **) (&var) = dlsym(RTLD_DEFAULT, name), var != NULL)

/* The frame, 4 x 3, RGB565: red 63488, green 2016, blue 31, white 65535, grey 0x8410
 * (r 16, g 32, b 16), black 0.  Widened as the X11 window widens it: r * 255 / 31, g * 255 / 63,
 * b * 255 / 31, so grey is 16*255/31 = 131, 32*255/63 = 129, 131: 0x838183. */
static const int32_t frame[12] = {63488, 2016, 31, 65535, 0x8410, 0x8410, 0x8410, 0x8410, 0, 63488, 0, 65535};
static const uint32_t wide[12] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x838183, 0x838183,
                                  0x838183, 0x838183, 0x000000, 0xFF0000, 0x000000, 0xFFFFFF};

int main(void) {
    const uint64_t shift = 1ull << 17;
    int32_t shown[12];
    void *image, *data;
    const uint32_t *px;
    int x, y, bad = 0;
    void *pool;
    if (!sc_open(4, 3, 8, 0, "screen check")) {
        puts("no window");
        return 2;
    }
    if (!(CG_SYM(cg_key, "CGEventCreateKeyboardEvent") && CG_SYM(cg_scroll, "CGEventCreateScrollWheelEvent") &&
          CG_SYM(cg_set_type, "CGEventSetType") && CG_SYM(cg_set_flags, "CGEventSetFlags") &&
          CG_SYM(cg_set_location, "CGEventSetLocation") && CG_SYM(cg_image_width, "CGImageGetWidth") &&
          CG_SYM(cg_image_height, "CGImageGetHeight") && CG_SYM(cg_image_provider, "CGImageGetDataProvider") &&
          CG_SYM(cg_provider_copy, "CGDataProviderCopyData") && CG_SYM(cf_data_bytes, "CFDataGetBytePtr") &&
          CG_SYM(cf_data_length, "CFDataGetLength"))) {
        puts("no CoreGraphics");
        return 2;
    }
    win_number = (long) sm_l(sm.window, "windowNumber");
    expect(sc_width() == 32 && sc_height() == 24, "the screen is 32 x 24");
    expect(sc_has_keys() && sc_pointer(4), "it has keys and a pointer");

    /* The frame, as the window's layer holds it. */
    memcpy(shown, frame, sizeof frame);
    if (getenv("SC_CHECK_BREAK")) shown[0] = 2016, shown[1] = 63488;
    sc_show(shown, 12, 4, 3);
    polls(3);
    pool = sm.pool_push();
    image = sm_o(sm.layer, "contents");
    expect(image != NULL && sm_o(sm_o(sm.window, "contentView"), "layer") == sm.layer,
           "the content view's layer holds an image");
    expect(image && cg_image_width(image) == 32 && cg_image_height(image) == 24, "the image is 32 x 24");
    data = image ? cg_provider_copy(cg_image_provider(image)) : NULL;
    expect(data && cf_data_length(data) == 32 * 24 * 4, "of 4 bytes a pixel");
    px = data ? (const uint32_t *) (const void *) cf_data_bytes(data) : NULL;
    for (y = 0; px && y < 24; y++)
        for (x = 0; x < 32; x++)
            if ((px[y * 32 + x] & 0xFFFFFF) != wide[(y / 8) * 4 + x / 8]) bad++;
    printf("     first %06x last %06x differing %d\n", px ? px[0] & 0xFFFFFF : 0u,
           px ? px[24 * 32 - 1] & 0xFFFFFF : 0u, bad);
    expect(px && bad == 0, "every pixel is its frame value, widened");
    expect(sc_shown(0, 0) == 63488 && sc_shown(31, 23) == 65535, "shown reads the back buffer");
    if (data) sm.cf_release(data);
    sm.pool_pop(pool);

    /* Keys: a; Shift held, b; Left; F1; Return; Delete. */
    pool = sm.pool_push();
    key(0x00, 1, 0);
    polls(3);
    expect(sc_key(97) && !sc_key(98), "a is down");
    key(0x00, 0, 0);
    polls(3);
    expect(!sc_key(97), "a is up");
    modifier(0x38, shift);
    key(0x0B, 1, shift);
    polls(3);
    expect(sc_key(132) && sc_key(98), "Shift and b are down");
    key(0x0B, 0, shift);
    modifier(0x38, 0);
    polls(3);
    expect(!sc_key(132) && !sc_key(98), "Shift and b are up");
    key(0x7B, 1, 0);
    key(0x7A, 1, 0);
    polls(3);
    expect(sc_key(130) && sc_key(135), "Left and F1 are down");
    key(0x7B, 0, 0);
    key(0x7A, 0, 0);
    key(0x24, 1, 0);
    key(0x24, 0, 0);
    key(0x33, 1, 0);
    key(0x33, 0, 0);
    polls(3);
    expect(!sc_key(130) && !sc_key(135) && !sc_key(13) && !sc_key(8), "every key is up");
    printf("     typed:");
    for (x = 0; sc_text()[x]; x++) printf(" %02x", (unsigned char) sc_text()[x]);
    printf("\n");
    expect(strcmp(sc_text(), "aB\r\b") == 0, "typed a, B, Return, a backspace; Left and F1 type nothing");
    sc_text_taken();
    expect(sc_text()[0] == 0, "taken, nothing is typed");

    /* The pointer: a move, a left and a right click, the wheel each way. */
    mouse(5, 10, 20);
    polls(3);
    expect(sc_pointer(0) == 10 && sc_pointer(1) == 20 && sc_pointer(2) == 0, "the pointer is at 10, 20");
    mouse(1, 5, 6);
    polls(3);
    expect(sc_pointer(0) == 5 && sc_pointer(1) == 6 && sc_pointer(2) == 1, "the left button is down at 5, 6");
    mouse(2, 5, 6);
    mouse(3, 7, 8);
    polls(3);
    expect(sc_pointer(2) == 2 && sc_pointer(0) == 7, "the right button is down at 7");
    mouse(4, 7, 8);
    polls(3);
    expect(sc_pointer(2) == 0, "no button is down");
    wheel(1, 7, 8);
    wheel(1, 7, 8);
    wheel(-1, 7, 8);
    expect(sc_pointer(3) == 1 && sc_pointer(3) == 0, "the wheel moved two up and one down, read once");
    sm.pool_pop(pool);

    /* Closing the window ends the screen. */
    expect(sc_poll(0) == 1, "open, poll answers true");
    sm_v(sm.window, "close");
    expect(sc_poll(0) == 0, "its window closed, poll answers false");
    sc_close();
    expect(sc_shown(0, 0) == -1 && !sc_has_keys(), "closed: nothing shown, no keys");
    printf("%s\n", failed ? "FAILED" : "as expected");
    return failed;
}
#else
int main(void) {
    puts("not macOS");
    return 2;
}
#endif
