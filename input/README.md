<!--
Copyright (c) 2026 Jurjen Stellingwerff
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# input — action bindings and per-tick input state for loft games

```sh
loft install input
```

Game code should ask *"is the player jumping?"*, not *"is key 32 down?"*.
This library wraps what a `graphics` window reports (`Window.key_down`,
`mouse_x` / `mouse_y`, `mouse_buttons`, and the frame's `Wheel` events) into
three things a frame loop actually wants:

- **Action bindings** — a name (`"jump"`) to one or more key codes; any of
  them fires it.
- **Edge detection** — `is_action_just_pressed` / `_just_released` are true
  for exactly the one tick a binding changed, built by keeping the previous
  tick's key table beside this one's.
- **Axes** — a pair of keys reading `-1.0` / `0.0` / `+1.0`, with both keys
  down cancelling to centre (the Unity / Godot convention).

```loft
use graphics::*;    // the key codes: KEY_SPACE, KEY_LEFT, …
use input::*;

bindings = Bindings{
  bnd_actions: [
    ActionBinding{ab_name: "jump", ab_keys: [KEY_SPACE, KEY_UP]},
  ],
  bnd_axes: [
    AxisBinding{ax_name: "horizontal", ax_neg: KEY_LEFT, ax_pos: KEY_RIGHT},
  ]
};
state = input_new(bindings);

// ...once per frame, and only once, after `w.poll()`:
evs = w.events();            // the frame's events stay yours: input only reads them
state.input_tick(w, evs);
if state.is_action_just_pressed("jump") { player.jump(); }
vx = state.get_axis("horizontal");
```

A guide: [docs/01-getting-started.loft](docs/01-getting-started.loft).

## The four contracts a signature does not carry

Each links to a test that demonstrates it and is run by CI — the code below
the link is the documentation, so it cannot go stale.

| contract | worked example |
|---|---|
| **Tick once per frame.** The *tick* consumes an edge, not the query — so queries are free and repeatable, and a second tick in one frame swallows the press. An already-held key fires its edge on the first tick, by design. | [`@INP-001`](tests/02-worked-examples.loft) |
| **A name is a bare `text` that is never declared.** An unknown action reads `false` — what an unpressed one reads; an unknown axis reads `0.0` — what a centred one reads. A typo is a control that silently never works. | [`@INP-002`](tests/02-worked-examples.loft) |
| **A mouse-button argument is a mask tested with `& != 0`,** so `MB_LEFT \| MB_RIGHT` means *either*, and its `just_pressed` misses the second button because the mask was already non-zero. Pass one constant per call. | [`@INP-003`](tests/02-worked-examples.loft) |
| **A rebind inherits the key tables.** That is what stops a key held across a remap from being stranded — and it means an action bound onto an already-held key fires a rising edge the player never made for it. | [`@INP-004`](tests/02-worked-examples.loft) |

## Headless use

`input_tick_from_state(keys, mx, my, buttons, wheel)` advances the state from
a caller-supplied snapshot instead of polling `graphics` — for tests, replays,
and network rollback.  It runs the same edge-detection code the polled path
does, so every example above is exercised in CI with no GL context.

Two differences from `input_tick` worth knowing: the snapshot fills **every**
key code it carries, while `input_tick` polls only the keys the current
bindings name; and the snapshot is whatever the caller says, so it is also
how you replay input that never came from a keyboard.

## The machine's own keys

`device_open()` and then `input_tick_device(state)` each tick read the keyboard
of the machine itself, with no window: the Tanmatsu's keys (through its board
support package), or the terminal the program runs in (raw mode, until
`device_close()` or the end of the program).  The key codes and the bindings
are the same as `input_tick`'s.

The Tanmatsu reports which keys are held.  A terminal reports key PRESSES, so
there a key is down for the tick its press arrived in, and again on each of the
terminal's repeats while it is held — enough for a turn-based or puzzle game,
not for holding a key down in an action game.  `device_feed(bytes)` hands the
terminal reader bytes as if typed (an arrow is ESC `[` A..D), which is how the
suite tests it with no terminal.  `is_key_down(state, code)` reads one key,
bound or not.

## API

- **Build**: `input_new(bindings)`, `input_set_bindings(state, bindings)`.
- **Advance**: `input_tick(state, window, events)` (a `graphics` window and the
  frame's events),
  `input_tick_device(state)` (the machine's own keys, after `device_open()`),
  `input_tick_from_state(state, keys, mx, my, buttons, wheel)`.
- **Keys**: `is_key_down(state, code)`; `device_open() -> boolean`,
  `device_close()`, `device_feed(bytes)`.
- **Actions**: `is_action_pressed`, `is_action_just_pressed`,
  `is_action_just_released`.
- **Axis**: `get_axis(state, name) -> float`.
- **Mouse**: `mouse_x`, `mouse_y`, `mouse_wheel`, `mouse_button_down`,
  `mouse_button_just_pressed`, `mouse_button_just_released`; masks
  `MB_LEFT`, `MB_RIGHT`, `MB_MIDDLE`.

Key codes come from `graphics` (`KEY_SPACE` = 32, `KEY_A` = 97, …), so
`use graphics::*;` brings them in; a binding
referencing a code outside `0..256` is ignored at query time rather than
faulting.

## Moving from 0.2

0.3 reads graphics 0.10's `Window` (graphics 0.10 has no `gl_key_pressed`), so
`input_tick` takes the window and the frame's events:
`state.input_tick()` becomes `state.input_tick(w, w.events())` — or, when the
game reads the events too, take them once and hand the same vector to both.
The wheel is the sum of the `Wheel` deltas in it.  A program locked to
graphics 0.9 stays on input 0.2.

Licensed LGPL-3.0-or-later.
