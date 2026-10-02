<!--
Copyright (c) 2026 Jurjen Stellingwerff
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# fixstep — a fixed simulation step, in exact integer time

```sh
loft install fixstep
```

Pure-loft, **zero dependencies**.  A simulation advances by a whole
number of fixed steps; this owns the arithmetic that decides *how many*,
and nothing about what a step does.

## Why it exists, counted rather than argued

Before it, two loft games had **nine** independent implementations of
*do not lose a fraction* between them, and one was wrong in a way no
test in either repo could see: a mover that truncated its progress and
carried nothing covered **180 / 120 / 180 / 0 / 0 / 0 / 0** units a
minute against a true 180 as the timestep was swept, and stopped moving
entirely below a 250 ms step.

Every one of those nine existed because there was nothing to consume.
So the admission test here is not a size budget but its opposite:
***could a consumer be tempted to hand-roll this?***  If yes it belongs
in — built once, tested once, and never argued about again.

## The invariant

> **Every duration is an exact integer count of one chosen base unit,
> and every rate is consumed by an integer accumulator that carries its
> remainder.  Floating point appears only where something is drawn.**

`ease` is the one module outside that invariant and inside the package;
it says so itself, and it is here because both games wrote it and one
got it wrong.

## Three types, and picking the wrong one is silent

| you have | you want | why not the others |
|---|---|---|
| wall time, and a simulation to step | `TickClock` | — |
| a rate, and whole units to spend | `Bank` | a bank's remainder is load-bearing **for ever** |
| a duration that fires **once** | `Timer` | a one-shot's remainder dies at its boundary |

A one-shot built on a bank fires a second time with nobody re-arming it,
and carries its residue into the next arming — a 5 s cooldown costs 8
steps the first time and **7** the second.  Measured, in `@FIX-011`.

## A door per use case — and the test *is* the example

Every public door has a test named for its case.  A snippet in a doc
comment rots (nothing compiles it); a test nobody links to is invisible.
The pair is the deliverable, so each function carries an
`// Example: @FIX-0NN` line and each tag names a test in `tests/`.

| the case | the door | example |
|---|---|---|
| a frame loop (GL, browser) | `clock_advance(clk, elapsed)` | `@FIX-001` |
| a script, a test, a turn commit | `clock_step(clk, n)` | `@FIX-002` |
| a server pump owning the wall | `clock_pump(clk, now)` | `@FIX-003` |
| a loop that may be backgrounded | `clock_advance_capped(clk, dt, max)` | `@FIX-004` |
| drawing between two steps | `clock_alpha(clk)` | `@FIX-005` |
| a rollback re-simulation | `clock_restore` + `clock_step` | `@FIX-006` |
| pause, slow-mo, fast-forward | `clock_set_rate(clk, num, den)` | `@FIX-007` |
| a slow layer over a fast one | `clock_drive(slow, fast, n)` | `@FIX-008` |
| a mover spending a rate | `bank_gain(b, rate, elapsed, whole)` | `@FIX-009` |
| asking without spending | `bank_most(rate, elapsed, whole)` | `@FIX-010` |
| a cooldown, a countdown, a lull | `timer_arm` + `timer_spend` | `@FIX-011` |
| an eased value | `approach(now, want, k, dt)` | `@FIX-012` |
| an eased angle, the short way | `approach_angle` / `wrap_delta` | `@FIX-013` |

Nothing is a mode flag on another door.  A count and a duration look
like one question with a switch, and they are not: through a float
accumulator, `n × step` seconds comes back `n − 1` for **602 of the
first 1000 `n`**.

## The base unit is the consumer's choice

`clock_new(step)` counts whatever you hand it.  What the library
promises is the **identity** — `clock_advance(clk, n × step) ==
clock_step(clk, n)` for all `n` — never a unit.

Pick the coarsest unit in which your step is a whole number.  A 2/3-second
step is **not** a whole number of microseconds, so its first consumer
counts thirds of a microsecond (3 000 000 to the second) and its step is
exactly 2 000 000 of them.

## What is deliberately not here

- **No tick body.**  What a step *does* is the game's.
- **No callbacks.**  This never calls your tick; it answers *how many*.
- **No interpolation policy.**  `clock_alpha` says *where between two
  steps we are*, not *what to draw there* — the three usual answers are
  priced on three different axes and which wins depends on how long the
  step is.
- **No netcode or snapshots.**  Rollback rewinds game state, which this
  cannot hold without owning the game.  It owns the timing half exactly.
- **No rest tolerance on `approach`.**  An exponential approach is
  asymptotic; what counts as *arrived* is a per-valve decision.

## Verified targets

| target | status |
|---|---|
| interpreter | ✅ 13/13, **warning-clean under `LOFT_DENY_WARNINGS=1`** |
| `--native` | ✅ 13/13, identical results |
| `--native-wasm` | ✅ a clock program builds and answers what the interpreter answers under wasmtime |
| `--html` | ✅ **measured, not argued** — a browser page's answers are identical to the interpreter's, line for line. See below |

### `--html`, measured

The package is pure loft with no `#native` bindings, and its one call that could
plausibly differ in a browser is `ease_fraction` (`1 − exp(−k·dt)`; `exp` is a builtin).
So the browser column is a measurement: moros's `probe/e1/ease_html.loft` prints the
composition identity, the four `k` its camera eases at after 120 fixed ticks,
`ease_fraction` including a paused frame, and the angle half, on the interpreter and
as an `--html` page driven headless — **all 11 lines identical**, including
`compose one 0.9975212478 sixty 0.9975212478 spread 0`.

```sh
loft test                                        # interpreter
loft test --native                               # compiled Rust
LOFT_DENY_WARNINGS=1 loft --interpret --tests tests   # what CI runs
```

⚠ **The package carries no `.allow_warnings`, deliberately.**  `clock_step`
does not read its `_clk`, which is its contract rather than an oversight, so
the parameter is underscored to say so at the one site that means it — where
an `.allow_warnings` file would have switched the gate off for every future
warning as well.

The worked-example linkage — every `// Example: @FIX-0NN` naming a test that carries
it — is checked by loft's examples gate (`make examples-preflight REPO=<this repo>` in a
loft checkout), the same gate the library CI runs.

A guide: [docs/01-getting-started.loft](docs/01-getting-started.loft).
