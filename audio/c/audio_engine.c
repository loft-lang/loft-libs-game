/* Copyright (c) 2026 Jurjen Stellingwerff
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * The audio library's engine, in C99: clips, voices and the mix, built by loft itself as a
 * `[c] shim` and bound by the `#c` declarations in src/audio.loft.  Pure computation — the
 * engine renders frames into a buffer the caller hands it, and a platform's output (a sound
 * card, the Tanmatsu's I2S stream) is a layer that asks it for frames.  One mix on every
 * target, so a clip sounds the same on a PC and on a 32-bit device, and every rule here is
 * checkable on a machine with no sound device: `ae_render` is the output.
 *
 * The mix, per output frame:
 *   - each playing voice reads its clip at `pos`, linearly interpolated between the two source
 *     frames around it (the neighbour past the last frame is the first when the voice loops,
 *     silence when it does not), and advances `pos` by `clip rate / output rate`;
 *   - the voice's volume scales it, and its pan places it by Web Audio's StereoPannerNode law
 *     (equal-power: a mono clip at the centre is cos(pi/4) on each side);
 *   - the voices are summed, clamped to [-1, 1], and written as 16-bit: round(s * 32767),
 *     halves away from zero.
 * Arithmetic is `float` (the ESP32-P4's FPU is single precision); the pan gains are computed
 * once per pan change, by our own series rather than libm, so they are the same bits on every
 * target.  A non-looping voice ends when its position passes the clip's last frame.
 *
 * Handles: a clip is its index + 1; a voice is `slot + 1 + AE_VOICES * generation`, the
 * generation bumped each time the slot is reused, so a handle to a finished voice acts on
 * nothing rather than on whatever took its place.  0 is "no handle" (loft null). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__wasm__)
/* OGG Vorbis by the vendored stb_vorbis (public domain / MIT).  It needs libm, which loft's
 * freestanding wasm build has not, so there a Vorbis clip does not load (`ae_load` answers 0). */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "vendor/stb_vorbis/stb_vorbis.c"
#define AE_HAVE_VORBIS 1
#else
#define AE_HAVE_VORBIS 0
#endif

#define AE_VOICES 64

typedef struct {
    float *samples; /* interleaved, `channels` per frame */
    int64_t frames;
    int32_t channels; /* 1 or 2 */
    int32_t rate;
} ae_clip;

typedef struct {
    int64_t clip; /* clip handle; 0 when the slot is free */
    int64_t generation;
    double pos;   /* in source frames */
    float volume;
    float pan;
    float g0, g1; /* the pan's two gains (see ae_pan_gains) */
    int looping;
    int owns_clip; /* a play_raw voice frees its clip when it ends */
} ae_voice;

static ae_clip *ae_clips = NULL;
static int64_t ae_clip_count = 0;
static ae_voice ae_voices[AE_VOICES];
static int32_t ae_out_rate = 44100;

/* sin(t) and cos(t) for t in [0, pi/2], by their Taylor series in double: exact at the ends
 * (sin 0 = 0, cos 0 = 1), within 1e-15 elsewhere, and the same bits on every target. */
static double ae_sin(double t) {
    double term = t, sum = t, t2 = t * t;
    int k;
    for (k = 1; k <= 10; k++) {
        term = -term * t2 / ((2.0 * k) * (2.0 * k + 1.0));
        sum += term;
    }
    return sum;
}

static double ae_cos(double t) {
    double term = 1.0, sum = 1.0, t2 = t * t;
    int k;
    for (k = 1; k <= 10; k++) {
        term = -term * t2 / ((2.0 * k - 1.0) * (2.0 * k));
        sum += term;
    }
    return sum;
}

#define AE_HALF_PI 1.5707963267948966

/* StereoPannerNode: a mono source at pan p has x = (p + 1) / 2, gains (cos, sin)(x pi/2).  A
 * stereo source takes x = p + 1 for p <= 0 and x = p above it; the gains are applied in
 * ae_mix.  A pan outside [-1, 1] is held at the edge. */
static void ae_pan_gains(ae_voice *v, int32_t channels) {
    double p = v->pan, x;
    if (p < -1.0) p = -1.0;
    if (p > 1.0) p = 1.0;
    x = channels == 1 ? (p + 1.0) / 2.0 : (p <= 0.0 ? p + 1.0 : p);
    v->g0 = (float) ae_cos(x * AE_HALF_PI);
    v->g1 = (float) ae_sin(x * AE_HALF_PI);
}

static ae_clip *ae_clip_of(int64_t h) {
    if (h < 1 || h > ae_clip_count || ae_clips[h - 1].samples == NULL) return NULL;
    return &ae_clips[h - 1];
}

static ae_voice *ae_voice_of(int64_t h) {
    int64_t slot;
    ae_voice *v;
    if (h < 1) return NULL;
    slot = (h - 1) % AE_VOICES;
    v = &ae_voices[slot];
    if (v->clip == 0 || (h - 1) / AE_VOICES != v->generation) return NULL;
    return v;
}

static int64_t ae_add_clip(float *samples, int64_t frames, int32_t channels, int32_t rate) {
    ae_clip *grown;
    int64_t i;
    for (i = 0; i < ae_clip_count; i++) {
        if (ae_clips[i].samples == NULL) break;
    }
    if (i == ae_clip_count) {
        grown = (ae_clip *) realloc(ae_clips, (size_t) (ae_clip_count + 1) * sizeof(ae_clip));
        if (!grown) {
            free(samples);
            return 0;
        }
        ae_clips = grown;
        ae_clip_count++;
    }
    ae_clips[i].samples = samples;
    ae_clips[i].frames = frames;
    ae_clips[i].channels = channels;
    ae_clips[i].rate = rate;
    return i + 1;
}

static void ae_end(ae_voice *v) {
    if (v->owns_clip) {
        ae_clip *c = ae_clip_of(v->clip);
        if (c) {
            free(c->samples);
            c->samples = NULL;
        }
    }
    v->clip = 0;
    v->owns_clip = 0;
}

/* ── WAV: RIFF/WAVE, PCM 8/16/24/32-bit or IEEE float 32-bit, mono or stereo ── */

static uint32_t ae_u32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static uint16_t ae_u16(const uint8_t *p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static float ae_wav_sample(const uint8_t *p, int fmt, int bits) {
    if (fmt == 3 && bits == 32) {
        float f;
        uint32_t u = ae_u32(p);
        memcpy(&f, &u, sizeof f);
        return f;
    }
    switch (bits) {
    case 8: return ((float) p[0] - 128.0f) / 128.0f;
    case 16: return (float) (int16_t) ae_u16(p) / 32768.0f;
    case 24: {
        int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24);
        return (float) (v / 256) / 8388608.0f;
    }
    default: return (float) (int32_t) ae_u32(p) / 2147483648.0f;
    }
}

static int64_t ae_load_wav(const uint8_t *b, int64_t n) {
    int64_t at = 12, data_at = -1, data_len = 0, frames, i;
    int fmt = 0, channels = 0, bits = 0, rate = 0, width;
    float *out;
    if (n < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) return 0;
    while (at + 8 <= n) {
        uint32_t len = ae_u32(b + at + 4);
        if (memcmp(b + at, "fmt ", 4) == 0 && len >= 16 && at + 8 + 16 <= n) {
            fmt = ae_u16(b + at + 8);
            channels = ae_u16(b + at + 10);
            rate = (int) ae_u32(b + at + 12);
            bits = ae_u16(b + at + 22);
            if (fmt == 0xFFFE && len >= 26 && at + 8 + 26 <= n) fmt = ae_u16(b + at + 32);
        } else if (memcmp(b + at, "data", 4) == 0) {
            data_at = at + 8;
            data_len = (int64_t) len;
            if (data_at + data_len > n) data_len = n - data_at;
        }
        at += 8 + (int64_t) len + (len & 1u);
    }
    if (data_at < 0 || (fmt != 1 && fmt != 3) || channels < 1 || channels > 2 || rate <= 0) return 0;
    if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32) || (fmt == 3 && bits != 32)) return 0;
    width = bits / 8 * channels;
    frames = data_len / width;
    if (frames <= 0) return 0;
    out = (float *) malloc((size_t) frames * (size_t) channels * sizeof(float));
    if (!out) return 0;
    for (i = 0; i < frames * channels; i++) out[i] = ae_wav_sample(b + data_at + i * (bits / 8), fmt, bits);
    return ae_add_clip(out, frames, channels, rate);
}

#if AE_HAVE_VORBIS
static int64_t ae_load_ogg(const uint8_t *b, int64_t n) {
    int err = 0;
    stb_vorbis *f;
    stb_vorbis_info info;
    int64_t frames, got;
    float *out;
    if (n < 4 || memcmp(b, "OggS", 4) != 0) return 0;
    f = stb_vorbis_open_memory(b, (int) n, &err, NULL);
    if (!f) return 0;
    info = stb_vorbis_get_info(f);
    frames = (int64_t) stb_vorbis_stream_length_in_samples(f);
    if (frames <= 0 || info.channels < 1) {
        stb_vorbis_close(f);
        return 0;
    }
    if (info.channels > 2) info.channels = 2; /* the first two channels of a surround file */
    out = (float *) malloc((size_t) frames * (size_t) info.channels * sizeof(float));
    if (!out) {
        stb_vorbis_close(f);
        return 0;
    }
    got = stb_vorbis_get_samples_float_interleaved(f, info.channels, out, (int) (frames * info.channels));
    stb_vorbis_close(f);
    if (got <= 0) {
        free(out);
        return 0;
    }
    return ae_add_clip(out, got, info.channels, (int32_t) info.sample_rate);
}
#endif

/* ── The mix ── */

/* Frame `i` of channel `ch` of a clip; past the end the first frame when looping, else 0. */
static float ae_at(const ae_clip *c, int64_t i, int ch, int looping) {
    if (i >= c->frames) {
        if (!looping) return 0.0f;
        i %= c->frames;
    }
    return c->samples[i * c->channels + (ch < c->channels ? ch : 0)];
}

static void ae_mix(float *acc, int64_t frames) {
    int s;
    for (s = 0; s < AE_VOICES; s++) {
        ae_voice *v = &ae_voices[s];
        ae_clip *c;
        double step;
        int64_t f;
        if (v->clip == 0) continue;
        c = ae_clip_of(v->clip);
        if (!c) {
            v->clip = 0;
            continue;
        }
        step = (double) c->rate / (double) ae_out_rate;
        for (f = 0; f < frames; f++) {
            int64_t i;
            float frac, l, r;
            if (v->pos >= (double) c->frames) {
                if (!v->looping) {
                    ae_end(v);
                    break;
                }
                while (v->pos >= (double) c->frames) v->pos -= (double) c->frames;
            }
            i = (int64_t) v->pos;
            frac = (float) (v->pos - (double) i);
            l = ae_at(c, i, 0, v->looping);
            l = l + (ae_at(c, i + 1, 0, v->looping) - l) * frac;
            if (c->channels == 1) {
                l *= v->volume;
                acc[2 * f] += l * v->g0;
                acc[2 * f + 1] += l * v->g1;
            } else {
                r = ae_at(c, i, 1, v->looping);
                r = r + (ae_at(c, i + 1, 1, v->looping) - r) * frac;
                l *= v->volume;
                r *= v->volume;
                if (v->pan <= 0.0f) {
                    acc[2 * f] += l + r * v->g0;
                    acc[2 * f + 1] += r * v->g1;
                } else {
                    acc[2 * f] += l * v->g0;
                    acc[2 * f + 1] += r + l * v->g1;
                }
            }
            v->pos += step;
        }
    }
}

static int16_t ae_to16(float s) {
    float v;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    v = s * 32767.0f;
    return (int16_t) (v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* ── The bound functions ── */

void ae_init(int64_t rate);
int64_t ae_rate(void);
int64_t ae_vorbis(void);
int64_t ae_load(const uint8_t *bytes, int64_t n);
int64_t ae_load_pcm(const float *samples, int64_t n, int64_t rate);
void ae_unload(int64_t clip);
int64_t ae_clip_frames(int64_t clip);
int64_t ae_clip_rate(int64_t clip);
int64_t ae_clip_channels(int64_t clip);
int64_t ae_play(int64_t clip, const double *p, int64_t n);
int64_t ae_play_raw(const float *samples, int64_t n, int64_t rate, const double *p, int64_t pn);
void ae_stop(int64_t voice);
void ae_stop_all(void);
int64_t ae_playing(int64_t voice);
void ae_set_volume(int64_t voice, const double *p, int64_t n);
void ae_set_pan(int64_t voice, const double *p, int64_t n);
int64_t ae_seek(int64_t voice, const double *p, int64_t n);
void ae_render(int32_t *out, int64_t n);

/* Set the output rate (frames per second) and stop every voice; the clips stay loaded. */
void ae_init(int64_t rate) {
    if (rate > 0 && rate <= 384000) ae_out_rate = (int32_t) rate;
    ae_stop_all();
}

int64_t ae_rate(void) { return ae_out_rate; }

int64_t ae_vorbis(void) { return AE_HAVE_VORBIS; }

/* A clip from a file's bytes: WAV, or OGG Vorbis where the build has it. */
int64_t ae_load(const uint8_t *bytes, int64_t n) {
    if (n >= 12 && memcmp(bytes, "RIFF", 4) == 0) return ae_load_wav(bytes, n);
#if AE_HAVE_VORBIS
    if (n >= 4 && memcmp(bytes, "OggS", 4) == 0) return ae_load_ogg(bytes, n);
#endif
    return 0;
}

/* A mono clip from `n` samples in [-1, 1] at `rate`. */
int64_t ae_load_pcm(const float *samples, int64_t n, int64_t rate) {
    float *copy;
    if (n <= 0 || rate <= 0 || rate > 384000) return 0;
    copy = (float *) malloc((size_t) n * sizeof(float));
    if (!copy) return 0;
    memcpy(copy, samples, (size_t) n * sizeof(float));
    return ae_add_clip(copy, n, 1, (int32_t) rate);
}

/* Unload a clip; every voice playing it stops. */
void ae_unload(int64_t clip) {
    ae_clip *c = ae_clip_of(clip);
    int s;
    if (!c) return;
    for (s = 0; s < AE_VOICES; s++) {
        if (ae_voices[s].clip == clip) ae_end(&ae_voices[s]);
    }
    free(c->samples);
    c->samples = NULL;
}

int64_t ae_clip_frames(int64_t clip) { ae_clip *c = ae_clip_of(clip); return c ? c->frames : 0; }
int64_t ae_clip_rate(int64_t clip) { ae_clip *c = ae_clip_of(clip); return c ? c->rate : 0; }
int64_t ae_clip_channels(int64_t clip) { ae_clip *c = ae_clip_of(clip); return c ? c->channels : 0; }

/* Play a clip: p = [volume, looping (0 / 1), pan, start in seconds].  0 when no slot is free. */
int64_t ae_play(int64_t clip, const double *p, int64_t n) {
    ae_clip *c = ae_clip_of(clip);
    int s;
    if (!c || n < 4) return 0;
    for (s = 0; s < AE_VOICES; s++) {
        ae_voice *v = &ae_voices[s];
        if (v->clip != 0) continue;
        v->generation++;
        v->clip = clip;
        v->volume = (float) p[0];
        v->looping = p[1] != 0.0;
        v->pan = (float) p[2];
        v->pos = p[3] > 0.0 ? p[3] * (double) c->rate : 0.0;
        v->owns_clip = 0;
        if (v->looping) {
            while (v->pos >= (double) c->frames) v->pos -= (double) c->frames;
        }
        ae_pan_gains(v, c->channels);
        return (int64_t) s + 1 + AE_VOICES * v->generation;
    }
    return 0;
}

/* Play `n` mono samples at `rate`, once: p = [volume].  The clip is freed when it ends. */
int64_t ae_play_raw(const float *samples, int64_t n, int64_t rate, const double *p, int64_t pn) {
    double params[4];
    int64_t clip, voice;
    if (pn < 1) return 0;
    clip = ae_load_pcm(samples, n, rate);
    if (clip == 0) return 0;
    params[0] = p[0];
    params[1] = 0.0;
    params[2] = 0.0;
    params[3] = 0.0;
    voice = ae_play(clip, params, 4);
    if (voice == 0) {
        ae_unload(clip);
        return 0;
    }
    ae_voice_of(voice)->owns_clip = 1;
    return voice;
}

void ae_stop(int64_t voice) {
    ae_voice *v = ae_voice_of(voice);
    if (v) ae_end(v);
}

void ae_stop_all(void) {
    int s;
    for (s = 0; s < AE_VOICES; s++) {
        if (ae_voices[s].clip != 0) ae_end(&ae_voices[s]);
    }
}

int64_t ae_playing(int64_t voice) { return ae_voice_of(voice) != NULL; }

void ae_set_volume(int64_t voice, const double *p, int64_t n) {
    ae_voice *v = ae_voice_of(voice);
    if (v && n >= 1) v->volume = (float) p[0];
}

void ae_set_pan(int64_t voice, const double *p, int64_t n) {
    ae_voice *v = ae_voice_of(voice);
    ae_clip *c;
    if (!v || n < 1) return;
    c = ae_clip_of(v->clip);
    v->pan = (float) p[0];
    if (c) ae_pan_gains(v, c->channels);
}

/* Move a voice to `p[0]` seconds into its clip; false for a finished voice or a position past
 * the end of a clip that does not loop. */
int64_t ae_seek(int64_t voice, const double *p, int64_t n) {
    ae_voice *v = ae_voice_of(voice);
    ae_clip *c;
    double pos;
    if (!v || n < 1) return 0;
    c = ae_clip_of(v->clip);
    if (!c) return 0;
    pos = p[0] > 0.0 ? p[0] * (double) c->rate : 0.0;
    if (pos >= (double) c->frames) {
        if (!v->looping) return 0;
        while (pos >= (double) c->frames) pos -= (double) c->frames;
    }
    v->pos = pos;
    return 1;
}

/* Mix the next `n / 2` stereo frames into `out` (L, R interleaved), each a 16-bit value held
 * in 32 bits — loft's `vector<i32>`; a device's output layer reads `ae_mix` / `ae_to16` itself. */
void ae_render(int32_t *out, int64_t n) {
    int64_t frames = n / 2, done = 0;
    float acc[512];
    while (done < frames) {
        int64_t chunk = frames - done, f;
        if (chunk > 256) chunk = 256;
        memset(acc, 0, (size_t) chunk * 2 * sizeof(float));
        ae_mix(acc, chunk);
        for (f = 0; f < chunk * 2; f++) out[done * 2 + f] = ae_to16(acc[f]);
        done += chunk;
    }
}
