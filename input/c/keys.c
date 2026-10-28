/* Copyright (c) 2026 Jurjen Stellingwerff
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * input's key source: which keys are down, read from the machine itself rather than from a
 * graphics window.  One section per platform behind the same bound functions; key codes are
 * `graphics`' (`KEY_W` = 119, `KEY_UP` = 128, ...), so a binding means the same key everywhere.
 *
 *   ik_open()         start the source: 1 when this machine has one, 0 when not.
 *   ik_close()        give the terminal back.
 *   ik_poll()         take this tick's state: afterwards ik_down answers for it.
 *   ik_down(code)     1 when key `code` is down this tick.
 *   ik_feed(b, n)     hand the terminal parser bytes as if typed (tests, replays); on every
 *                     platform, so the parser is testable where there is no terminal.
 *
 * Tanmatsu (ESP-IDF, `ESP_PLATFORM`): the BSP's polled key state (`bsp_input_read_scancode`,
 * `bsp_input_read_navigation_key`) for each code, so a held key is down until it is let go.
 * A terminal (POSIX termios, the Windows console): a terminal sends key PRESSES, not states, so
 * a key is down for the tick its bytes arrived in, and a held key is down again on each of the
 * terminal's repeats.  wasm: no source.
 */
#if !defined(_POSIX_C_SOURCE) && !defined(_WIN32) && !defined(ESP_PLATFORM)
#define _POSIX_C_SOURCE 200809L /* termios, read, isatty under a strict -std=c99 */
#endif
#include <stdint.h>
#include <string.h>

#define IK_KEYS 256

/* graphics' key codes */
#define K_BACKSPACE 8
#define K_TAB 9
#define K_ENTER 13
#define K_ESCAPE 27
#define K_UP 128
#define K_DOWN 129
#define K_LEFT 130
#define K_RIGHT 131
#define K_SHIFT 132
#define K_CTRL 133
#define K_F1 135
#define K_DELETE 147
#define K_INSERT 148
#define K_HOME 149
#define K_END 150
#define K_PAGEUP 151
#define K_PAGEDOWN 152

static unsigned char ik_now[IK_KEYS];
static unsigned char ik_buf[512];
static int ik_len = 0;

int64_t ik_open(void);
void ik_close(void);
void ik_poll(void);
int64_t ik_down(int64_t code);
void ik_feed(const uint8_t *b, int64_t n);

static void ik_press(int code) {
    if (code > 0 && code < IK_KEYS) ik_now[code] = 1;
}

void ik_feed(const uint8_t *b, int64_t n) {
    int64_t i;
    for (i = 0; i < n && ik_len < (int) sizeof ik_buf; i++) ik_buf[ik_len++] = b[i];
}

/* The number in `ESC [ <n> ~` for F5..F12, Insert, Delete, Page Up and Page Down. */
static int ik_tilde(int n) {
    switch (n) {
    case 1: case 7: return K_HOME;
    case 2: return K_INSERT;
    case 3: return K_DELETE;
    case 4: case 8: return K_END;
    case 5: return K_PAGEUP;
    case 6: return K_PAGEDOWN;
    case 15: return K_F1 + 4;
    case 17: return K_F1 + 5;
    case 18: return K_F1 + 6;
    case 19: return K_F1 + 7;
    case 20: return K_F1 + 8;
    case 21: return K_F1 + 9;
    case 23: return K_F1 + 10;
    case 24: return K_F1 + 11;
    default: return 0;
    }
}

/* The letter after `ESC [` or `ESC O`: the arrows, Home, End, and F1..F4. */
static int ik_final(int c) {
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'P': return K_F1;
    case 'Q': return K_F1 + 1;
    case 'R': return K_F1 + 2;
    case 'S': return K_F1 + 3;
    default: return 0;
    }
}

/* Turn the bytes a terminal sent into key presses.  A lone ESC (nothing after it in the same
 * read) is the Escape key; ESC followed by `[` or `O` is a sequence. */
static void ik_parse(void) {
    int i = 0;
    while (i < ik_len) {
        int c = ik_buf[i++];
        if (c == 27) {
            if (i < ik_len && (ik_buf[i] == '[' || ik_buf[i] == 'O')) {
                int n = 0, j = i + 1;
                while (j < ik_len && ik_buf[j] >= '0' && ik_buf[j] <= '9') n = n * 10 + (ik_buf[j++] - '0');
                while (j < ik_len && ik_buf[j] == ';') { /* a modifier: `ESC [ 1 ; 5 A` */
                    j++;
                    while (j < ik_len && ik_buf[j] >= '0' && ik_buf[j] <= '9') j++;
                }
                if (j < ik_len) {
                    ik_press(ik_buf[j] == '~' ? ik_tilde(n) : ik_final(ik_buf[j]));
                    i = j + 1;
                    continue;
                }
                i = ik_len; /* a sequence cut off by the read: dropped */
                continue;
            }
            ik_press(K_ESCAPE);
        } else if (c == '\r' || c == '\n') {
            ik_press(K_ENTER);
        } else if (c == 127 || c == 8) {
            ik_press(K_BACKSPACE);
        } else if (c == '\t') {
            ik_press(K_TAB);
        } else if (c >= 1 && c <= 26) { /* Ctrl + a letter */
            ik_press(K_CTRL);
            ik_press('a' + c - 1);
        } else if (c >= 'A' && c <= 'Z') {
            ik_press(K_SHIFT);
            ik_press(c - 'A' + 'a');
        } else if (c >= 32 && c < 127) {
            ik_press(c);
        }
    }
    ik_len = 0;
}

#if defined(ESP_PLATFORM)

#include "bsp/input.h"

/* graphics' code -> the BSP's scancode (letters, digits, space, Enter, ...) or navigation key
 * (arrows, Home, End, Page Up / Down).  0 where the board has no such key. */
static int ik_scancode(int code) {
    static const unsigned char letters[26] = {
        BSP_INPUT_SCANCODE_A, BSP_INPUT_SCANCODE_B, BSP_INPUT_SCANCODE_C, BSP_INPUT_SCANCODE_D,
        BSP_INPUT_SCANCODE_E, BSP_INPUT_SCANCODE_F, BSP_INPUT_SCANCODE_G, BSP_INPUT_SCANCODE_H,
        BSP_INPUT_SCANCODE_I, BSP_INPUT_SCANCODE_J, BSP_INPUT_SCANCODE_K, BSP_INPUT_SCANCODE_L,
        BSP_INPUT_SCANCODE_M, BSP_INPUT_SCANCODE_N, BSP_INPUT_SCANCODE_O, BSP_INPUT_SCANCODE_P,
        BSP_INPUT_SCANCODE_Q, BSP_INPUT_SCANCODE_R, BSP_INPUT_SCANCODE_S, BSP_INPUT_SCANCODE_T,
        BSP_INPUT_SCANCODE_U, BSP_INPUT_SCANCODE_V, BSP_INPUT_SCANCODE_W, BSP_INPUT_SCANCODE_X,
        BSP_INPUT_SCANCODE_Y, BSP_INPUT_SCANCODE_Z};
    if (code >= 'a' && code <= 'z') return letters[code - 'a'];
    if (code >= '1' && code <= '9') return BSP_INPUT_SCANCODE_1 + (code - '1');
    if (code == '0') return BSP_INPUT_SCANCODE_0;
    if (code >= K_F1 && code <= K_F1 + 9) return BSP_INPUT_SCANCODE_F1 + (code - K_F1);
    switch (code) {
    case ' ': return BSP_INPUT_SCANCODE_SPACE;
    case K_ESCAPE: return BSP_INPUT_SCANCODE_ESC;
    case K_ENTER: return BSP_INPUT_SCANCODE_ENTER;
    case K_TAB: return BSP_INPUT_SCANCODE_TAB;
    case K_BACKSPACE: return BSP_INPUT_SCANCODE_BACKSPACE;
    case K_SHIFT: return BSP_INPUT_SCANCODE_LEFTSHIFT;
    case K_CTRL: return BSP_INPUT_SCANCODE_LEFTCTRL;
    case K_F1 + 10: return BSP_INPUT_SCANCODE_F11;
    case K_F1 + 11: return BSP_INPUT_SCANCODE_F12;
    case '[': return BSP_INPUT_SCANCODE_LEFTBRACE;
    case ']': return BSP_INPUT_SCANCODE_RIGHTBRACE;
    case '-': return BSP_INPUT_SCANCODE_MINUS;
    case '=': return BSP_INPUT_SCANCODE_EQUAL;
    case ',': return BSP_INPUT_SCANCODE_COMMA;
    case '.': return BSP_INPUT_SCANCODE_DOT;
    case '/': return BSP_INPUT_SCANCODE_SLASH;
    case ';': return BSP_INPUT_SCANCODE_SEMICOLON;
    default: return 0;
    }
}

static int ik_navigation(int code) {
    switch (code) {
    case K_UP: return BSP_INPUT_NAVIGATION_KEY_UP;
    case K_DOWN: return BSP_INPUT_NAVIGATION_KEY_DOWN;
    case K_LEFT: return BSP_INPUT_NAVIGATION_KEY_LEFT;
    case K_RIGHT: return BSP_INPUT_NAVIGATION_KEY_RIGHT;
    case K_HOME: return BSP_INPUT_NAVIGATION_KEY_HOME;
    case K_END: return BSP_INPUT_NAVIGATION_KEY_END;
    case K_PAGEUP: return BSP_INPUT_NAVIGATION_KEY_PGUP;
    case K_PAGEDOWN: return BSP_INPUT_NAVIGATION_KEY_PGDN;
    default: return 0;
    }
}

int64_t ik_open(void) { return 1; }

void ik_close(void) { }

void ik_poll(void) {
    int code;
    memset(ik_now, 0, sizeof ik_now);
    for (code = 1; code < IK_KEYS; code++) {
        int nav = ik_navigation(code), sc = ik_scancode(code);
        bool state = false;
        if (nav) {
            bsp_input_read_navigation_key((bsp_input_navigation_key_t) nav, &state);
        } else if (sc) {
            bsp_input_read_scancode((bsp_input_scancode_t) sc, &state);
        }
        if (state) ik_now[code] = 1;
    }
    ik_parse(); /* fed bytes count too */
}

#elif defined(__wasm__)

int64_t ik_open(void) { return 0; }

void ik_close(void) { }

void ik_poll(void) {
    memset(ik_now, 0, sizeof ik_now);
    ik_parse();
}

#elif defined(_WIN32)

#include <conio.h>

static int ik_open_done = 0;

int64_t ik_open(void) {
    ik_open_done = 1;
    return 1;
}

void ik_close(void) { ik_open_done = 0; }

/* The console's second byte after a 0 / 0xE0 prefix. */
static int ik_console(int c) {
    switch (c) {
    case 72: return K_UP;
    case 80: return K_DOWN;
    case 75: return K_LEFT;
    case 77: return K_RIGHT;
    case 71: return K_HOME;
    case 79: return K_END;
    case 73: return K_PAGEUP;
    case 81: return K_PAGEDOWN;
    case 82: return K_INSERT;
    case 83: return K_DELETE;
    case 133: return K_F1 + 10;
    case 134: return K_F1 + 11;
    default: return c >= 59 && c <= 68 ? K_F1 + (c - 59) : 0;
    }
}

void ik_poll(void) {
    memset(ik_now, 0, sizeof ik_now);
    while (ik_open_done && _kbhit()) {
        int c = _getch();
        if (c == 0 || c == 0xE0) {
            ik_press(ik_console(_getch()));
        } else if (ik_len < (int) sizeof ik_buf) {
            ik_buf[ik_len++] = (unsigned char) c;
        }
    }
    ik_parse();
}

#else

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>

static struct termios ik_saved;
static int ik_raw = 0;

void ik_close(void) {
    if (!ik_raw) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &ik_saved);
    ik_raw = 0;
}

static void ik_restore_at_exit(void) { ik_close(); }

int64_t ik_open(void) {
    struct termios t;
    static int registered = 0;
    if (ik_raw) return 1;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &ik_saved) != 0) return 0;
    t = ik_saved;
    t.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0; /* a read answers at once, with whatever is there */
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return 0;
    ik_raw = 1;
    if (!registered) {
        atexit(ik_restore_at_exit);
        registered = 1;
    }
    return 1;
}

void ik_poll(void) {
    memset(ik_now, 0, sizeof ik_now);
    while (ik_raw && ik_len < (int) sizeof ik_buf) {
        ssize_t got = read(STDIN_FILENO, ik_buf + ik_len, sizeof ik_buf - (size_t) ik_len);
        if (got <= 0) break;
        ik_len += (int) got;
    }
    ik_parse();
}

#endif

int64_t ik_down(int64_t code) { return code > 0 && code < IK_KEYS && ik_now[code]; }
