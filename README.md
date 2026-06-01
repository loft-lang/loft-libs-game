<!--
Copyright (c) 2026 Jurjen Stellingwerff
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# loft-libs-game — game-system libraries for loft

Multi-package chunk repo for **runtime game services** — time, input,
audio bus, particles, 2D physics.  Each subdirectory is an independent
loft package published to the registry under its own name.

Per the chunked-repo design in
[loft's lib_plans/12-library-extraction/](https://github.com/jjstwerff/loft/blob/main/doc/claude/lib_plans/12-library-extraction/README.md)
§ Chunk grouping, and the 6-chunk topology in
[LAVITION.md](https://github.com/jjstwerff/loft/blob/main/doc/claude/LAVITION.md)
§ Library model.

## Why this chunk exists

These libraries form the **runtime game services** layer: per-frame
state (time), per-frame inputs (`input`), procedural visual effects
(`particles`), and dynamics (`physics_2body`).  They sit between the
hex-world data model ([loft-libs-world](https://github.com/loft-lang/loft-libs-world))
and the rendering / windowing stack ([loft-libs-graphics](https://github.com/loft-lang/loft-libs-graphics))
— consumed by every game, used by the editor itself, but **not**
required for non-game consumers (data processors, asset packers,
servers).

Bundling them together means each release ships a co-versioned set;
games don't have to chase mutually-compatible point versions across
4-5 separate repos.

## Packages

| Subdir | Package | Status |
|---|---|---|
| `time/` | time | ✅ 0.1.0 — frame counter, dt, scheduling |
| `input/` | input | (planned — wraps `graphics::poll_events` into action state + bindings) |
| `particles/` | particles | (planned, lib_plans/future/27) |
| `physics_2body/` | physics_2body | (planned, lib_plans/future/26) |
| `audio_bus/` | audio_bus | (planned — game-side mixer above graphics's raw audio surface) |

## License

LGPL-3.0-or-later for every package in this chunk.  See `LICENSE`.
