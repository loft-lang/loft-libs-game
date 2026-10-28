<!--
Copyright (c) 2026 Jurjen Stellingwerff
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# audio — clips, voices and one mix

The engine every target shares: clips loaded from WAV or OGG Vorbis, up to 64 voices with
volume, looping, pan, start offset and seek, and one mix rendered as 16-bit stereo.  The calls
are `graphics`' audio calls, so a program written against those moves over unchanged.

`audio::start()` plays the mix: the system's sound device on a desktop (miniaudio: ALSA,
PulseAudio, CoreAudio, WASAPI), the board's I2S stream on a Tanmatsu.  Without it,
`audio::render(frames)` answers the next frames of the mix, which is what lets every rule here be
tested on a machine with no sound device.  The output runs on its own thread, and every call
here is safe to make while it plays.

## Install

```sh
loft install audio
```

## API

| Function | Returns | Notes |
|---|---|---|
| `init(rate = 44100)` | — | the output rate; stops every voice, keeps the clips |
| `load(path) -> integer?` | a clip | WAV (PCM 8/16/24/32-bit or float; mono or stereo) or OGG Vorbis; null when it does not decode |
| `load_bytes(bytes) -> integer?` / `load_samples(samples: vector<single>, rate) -> integer?` | a clip | from memory; samples are mono, in [-1, 1] |
| `play(clip, volume, looping = false, pan = 0.0, start = 0.0) -> integer?` | a voice | pan -1 left to 1 right; `start` in seconds; null when every voice is busy |
| `play_raw(samples, rate, volume) -> integer?` | a voice | once, then released |
| `stop(voice)`, `stop_all()`, `playing(voice)` | | a finished voice's handle acts on nothing, even once its slot plays again |
| `set_volume(voice, v)`, `set_pan(voice, p)`, `seek(voice, seconds) -> boolean` | | `seek` fails past the end of a clip that does not loop; a looping clip wraps |
| `render(frames) -> vector<i32>` | `2 * frames` samples | left and right interleaved, each in -32767..32767 |
| `start(silent = false) -> boolean` | whether it runs | the output; false where the machine has none (no device, a browser build); `silent` plays at real-time pace into nothing |
| `stop_output()`, `output_running() -> boolean`, `output_frames() -> integer` | | stopping keeps every voice's place; `output_frames` counts the frames played since `start` |
| `clip_frames`, `clip_rate`, `clip_channels`, `unload`, `rate`, `has_vorbis` | | |

## The mix

Each voice reads its clip with linear interpolation, resampled to the output rate; its volume
scales it and its pan places it by Web Audio's StereoPannerNode law (equal-power: a mono clip
at the centre is 0.7071 on each side); the voices are summed, clamped and written as
`round(s * 32767)`.  The arithmetic is single precision, as a 32-bit device's FPU has it, and
the pan gains are computed by the engine's own series, so a clip renders to the same samples
on every target.

## Targets

| target | state |
|---|---|
| interpreter, `--native` | ✓ render suite, and the output played through miniaudio's null device |
| C backend (`--native-c`) | ✓ both suites match the interpreter on x86-64, 32-bit x86 and 32-bit RISC-V (under qemu) |
| Tanmatsu (ESP-IDF) | the I2S output task, type-checked against the board's BSP headers; not yet run on the device |
| `--native-wasm` / `--html` | the engine builds for wasm32 freestanding, with no output (`start()` is false); OGG Vorbis does not load there (`has_vorbis()` is false) |

## Provenance

miniaudio 0.11.21 (David Reid, public domain or MIT-0) is vendored unmodified in
`c/vendor/miniaudio`; `VENDOR.md` there names its hash.  Only its device layer is compiled.


The engine is `c/audio_engine.c`, a `[c] shim` loft builds itself; OGG Vorbis is the vendored
[stb_vorbis](https://github.com/nothings/stb) (`c/vendor/stb_vorbis`, public domain / MIT).
