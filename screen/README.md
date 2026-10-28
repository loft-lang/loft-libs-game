<!--
Copyright (c) 2026 Jurjen Stellingwerff
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# screen — a small frame on the machine's screen, scaled up, with the keys of its window

```sh
loft install screen
```

A game draws its frame small (with `tiles`: RGB565, 200 x 120 or so) and `screen` shows it
scaled up by a whole number on whatever this machine has, with one API:

| Machine | `open` gives | Keys |
|---|---|---|
| a desktop with X11 (Linux, the BSDs) | a window, `width * scale` by `height * scale` | the window's (`keys()`) |
| a Tanmatsu | the display: the frame rotated into the portrait panel, centred | none here: `input::input_tick_device()` reads the keyboard |
| a browser, Windows, macOS | not yet: `open` answers false | |

```loft
use screen;
use tiles;
use input;

fn main() {
  s = tiles::surface(208, 160) ?? 0;
  if !screen::open(208, 160, 4, "my game") { return; }
  keys = input::input_new(my_bindings());
  window_keys = screen::has_keys();
  if !window_keys { _ = input::device_open(); }
  while screen::poll(15) {                     // waits up to 15 ms; false once closed
    if window_keys {
      keys.input_tick_from_state(screen::keys(), 0.0, 0.0, 0, 0);
    } else {
      keys.input_tick_device();
    }
    // ... update, draw on `s` ...
    screen::show(tiles::pixels(s, 208, 160), 208, 160);
  }
}
```

The keys come from the window where the screen has one, and from the machine's keyboard where
it does not: one program, asking what the machine has, never which machine it is.

## API

| Function | |
|---|---|
| `open(width, height, scale = 1, title = "loft", offscreen = false) -> boolean` | false where there is no screen; `offscreen` keeps only the scaled image, for tests |
| `show(pixels: vector<i32>, width, height)` | RGB565 values, row by row, as `tiles::pixels` answers |
| `poll(wait_ms = 0) -> boolean` | take the window's events, waiting up to `wait_ms`; false once closed |
| `has_keys() -> boolean`, `keys() -> vector<boolean>` | the window's keys by graphics' key codes (`KEY_W` = 119, `KEY_UP` = 128, ...), the form `input_tick_from_state` takes |
| `shown(x, y) -> integer`, `width()`, `height()` | what the screen shows at a pixel (-1 outside), and its size |
| `close()` | |

## How it is built

One C file, `c/screen.c`: the scaling into a back buffer is shared, and one section per
platform puts that buffer on the screen.  X11's library is opened at run time, so building
needs only its headers and a machine with no display just answers `open` false.  The
Tanmatsu section is type-checked against the board's BSP headers; it has not run on the
device yet.  Adding a platform is a new section: [loft's HARDWARE_BACKENDS.md](https://github.com/loft-lang/loft/blob/main/doc/claude/HARDWARE_BACKENDS.md).
