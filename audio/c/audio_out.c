/* Copyright (c) 2026 Jurjen Stellingwerff
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * The output layer: a device that asks the engine for frames on its own thread.  Included at
 * the end of audio_engine.c, so it reads the engine's mixer directly; one section per
 * platform, behind the same four bound functions:
 *
 *   ae_out_open(mode)  start the output: 1 when it runs, 0 when this machine has none.
 *                      mode 0 is the platform's own device; mode 1 is a device that plays at
 *                      real-time pace into nothing (the tests' device, on a desktop).
 *   ae_out_close()     stop it.
 *   ae_out_running()   1 while it runs.
 *   ae_out_frames()    the stereo frames the output has taken since it opened.
 *
 * Desktop (Linux, macOS, Windows): miniaudio, 16-bit stereo at the engine's rate.
 * Tanmatsu (ESP-IDF, `ESP_PLATFORM`): a FreeRTOS task writes the mix to the board's I2S
 * channel, which the BSP has configured for 16-bit stereo (`bsp/audio.h`).
 * wasm: none — `ae_out_open` answers 0, and `ae_render` stays the way out. */

/* Mix `frames` stereo frames as 16-bit samples, under the engine lock. */
static void ae_out_fill(int16_t *out, int64_t frames) {
    float acc[512];
    int64_t done = 0;
    AE_LOCK();
    while (done < frames) {
        int64_t chunk = frames - done, f;
        if (chunk > 256) chunk = 256;
        memset(acc, 0, (size_t) chunk * 2 * sizeof(float));
        ae_mix(acc, chunk);
        for (f = 0; f < chunk * 2; f++) out[done * 2 + f] = ae_to16(acc[f]);
        done += chunk;
    }
    AE_UNLOCK();
}

static volatile int ae_out_on = 0;
static volatile int64_t ae_out_taken = 0;

int64_t ae_out_open(int64_t mode);
void ae_out_close(void);
int64_t ae_out_running(void);
int64_t ae_out_frames(void);

#if defined(ESP_PLATFORM)

#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "bsp/audio.h"

static i2s_chan_handle_t ae_i2s = NULL;

static void ae_out_task(void *arg) {
    int16_t buf[512];
    (void) arg;
    while (ae_out_on) {
        size_t written = 0;
        ae_out_fill(buf, 256);
        i2s_channel_write(ae_i2s, buf, sizeof buf, &written, portMAX_DELAY);
        ae_out_taken += (int64_t) (written / 4);
    }
    vTaskDelete(NULL);
}

int64_t ae_out_open(int64_t mode) {
    (void) mode;
    if (ae_out_on) return 1;
    if (!ae_lock) ae_lock = xSemaphoreCreateMutex();
    if (!ae_lock) return 0;
    if (bsp_audio_get_i2s_handle(&ae_i2s) != ESP_OK || ae_i2s == NULL) return 0;
    if (bsp_audio_set_rate((uint32_t) ae_out_rate) != ESP_OK) return 0;
    bsp_audio_set_amplifier(true);
    ae_out_taken = 0;
    ae_out_on = 1;
    if (xTaskCreate(ae_out_task, "loft_audio", 4096, NULL, 5, NULL) != pdPASS) {
        ae_out_on = 0;
        bsp_audio_set_amplifier(false);
        return 0;
    }
    return 1;
}

void ae_out_close(void) {
    if (!ae_out_on) return;
    ae_out_on = 0; /* the task ends after its current buffer */
    bsp_audio_set_amplifier(false);
}

#elif defined(__wasm__)

int64_t ae_out_open(int64_t mode) {
    (void) mode;
    return 0;
}

void ae_out_close(void) { }

#else

static ma_device ae_dev;

static void ae_out_callback(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void) dev;
    (void) in;
    ae_out_fill((int16_t *) out, (int64_t) frames);
    ae_out_taken += (int64_t) frames;
}

int64_t ae_out_open(int64_t mode) {
    ma_device_config cfg;
    ma_result r;
    if (ae_out_on) return 1;
    if (!ae_lock_made) {
        if (ma_mutex_init(&ae_lock) != MA_SUCCESS) return 0;
        ae_lock_made = 1;
    }
    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = (ma_uint32) ae_out_rate;
    cfg.dataCallback = ae_out_callback;
    if (mode == 1) {
        ma_backend null_only = ma_backend_null;
        r = ma_device_init_ex(&null_only, 1, NULL, &cfg, &ae_dev);
    } else {
        r = ma_device_init(NULL, &cfg, &ae_dev);
    }
    if (r != MA_SUCCESS) return 0;
    ae_out_taken = 0;
    if (ma_device_start(&ae_dev) != MA_SUCCESS) {
        ma_device_uninit(&ae_dev);
        return 0;
    }
    ae_out_on = 1;
    return 1;
}

void ae_out_close(void) {
    if (!ae_out_on) return;
    ae_out_on = 0;
    ma_device_uninit(&ae_dev); /* waits for the callback to return */
}

#endif

int64_t ae_out_running(void) { return ae_out_on; }

int64_t ae_out_frames(void) { return ae_out_taken; }
