<!--
Copyright (c) 2026 Jurjen Stellingwerff
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# audio_bus — one slider, many sounds

```sh
loft install audio_bus
```

```loft
use audio_bus::(mixer, add, play, duck, unduck, MASTER);
use audio;

fn main() {
  audio::init(44100);
  _ = audio::start();                       // the sound device
  m = mixer();
  _ = add(m, "music", MASTER, 0.5);
  _ = add(m, "sfx", MASTER, 0.8);
  _ = add(m, "steps", "sfx", 0.5);          // a bus under a bus

  theme = audio::load("theme.ogg") ?? 0;
  _ = play(m, "music", theme, 1.0, true);   // looping, at 1.0 * 0.5

  // A line of speech starts: pull the music down and put it back after.
  _ = duck(m, "music", 0.15);
  // …later…
  _ = unduck(m, "music");                   // exactly 0.5 again, not 0.4999998
}
```

## Why it exists

`audio::set_volume` moves **one** voice. A game has a music slider, an
effects slider and a master, and every one of them has to reach sounds that were
started at different times by different code. Without a mixer that means the game
keeps the list itself — and then keeps a second one for "which of these is music".

A bus is a number that multiplies, arranged in a tree. A voice is heard at its own
level times the gain of every bus above it, so one write to `music` moves
everything on it and everything under it.

## The one thing this gets right

> **A duck that is lifted restores the level EXACTLY.**

A game ducks the music under speech a thousand times an hour. Restore by dividing
the ducked gain back out and the error accumulates: after a few hundred lines of
dialogue the music sits a few percent from where the player put it, and nothing in
the code says where it went.

So `duck` **remembers** the level it replaced and `unduck` writes that number back.
Nothing is computed on the way home, so nothing can drift — a thousand
duck/unduck pairs on a level of `0.35` (which has no exact binary form) end on the
same bits they started on, and the suite asserts it.

A duck inside a duck keeps the ORIGINAL level to come home to, because the second
speaker starting must not turn the first one's restore into a guess.

## Surface

| | |
|---|---|
| `mixer()` · `add(m, name, parent, volume)` | the tree; `MASTER` is the top |
| `gain(m, bus)` · `volume(m, bus)` · `set_volume(m, bus, v)` | what multiplies, and what one bus contributes |
| `play(m, bus, clip, volume, looping, pan, start)` | `audio::play` on a bus; -1 when it did not start |
| `attach(m, sink, bus, volume)` · `detach(m, sink)` | put a handle you already have on a bus, or take it off |
| `level(m, sink)` | what that voice's sink is set to |
| `duck(m, bus, to)` · `unduck(m, bus)` · `ducked(m, bus)` | remember, replace, restore |
| `pan(m, sink, position)` · `stop(m, sink)` · `stop_bus(m, bus)` · `stop_all(m)` | one voice, one bus, or everything |
| `under(m, bus, ancestor)` | is this bus at or below that one |

Three answers are worth knowing before you need them:

- **A bus nobody declared is silence**, not full volume. `gain` answers `0.0` for
  an unknown name, `play` refuses, and `set_volume` answers false. A typo that
  plays at full volume is one you find in a review; one that goes quiet is one you
  hear.
- **A negative level is clamped to zero.** A negative gain inverts a waveform
  rather than quietening it, which is not what a volume control means anywhere.
- **A parent cycle answers silence** rather than looping forever. A caller can
  build one, so the answer has to be a number.

## What this is not

Not a DSP graph — no filters, no sends, no compressor. A bus is a multiplication.
Everything past that belongs to a sound engine, and a 2-D game does not need one.

Not an owner of playback either: `audio` plays the sounds and this decides how
loud. That is what `attach` is for, and it is also what lets every rule here be
tested on a machine with **no audio device at all** — which matters, because CI has
none, so a suite that needed one would gate nothing.

## Targets

Everything here is pure loft over the `audio` library, so it runs wherever `audio`
does: the interpreter, `--native`, the C backend (32-bit RISC-V and x86 included) and a
Tanmatsu.  The suite reads the mix itself back through `audio::render`, so what a bus
does is measured as heard.

Needs `audio` 0.2.0 or later.  0.1 played through `graphics`; from 0.2 a clip comes from
`audio::load`, so a program's clip handles change with it.

A worked walk through the tree, the duck and what silence means is the guide,
[`docs/01-getting-started.loft`](docs/01-getting-started.loft).

## License

LGPL-3.0-or-later — see [LICENSE](LICENSE).
