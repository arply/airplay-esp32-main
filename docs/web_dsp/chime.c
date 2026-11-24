#include "chime.h"

#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Embedded via EMBED_FILES "arply_chime.pcm" (raw s16le, mono, 44.1 kHz)
extern const uint8_t chime_pcm_start[] asm("_binary_arply_chime_pcm_start");
extern const uint8_t chime_pcm_end[]   asm("_binary_arply_chime_pcm_end");

#define SAMPLE_RATE   44100
#define CHUNK_FRAMES  256
#define FADE_FRAMES   (SAMPLE_RATE / 100)   // 10 ms safety fade-in/out

static esp_err_t write_silence(i2s_chan_handle_t tx, int frames)
{
    int16_t buf[CHUNK_FRAMES * 2] = {0};
    size_t written;
    while (frames > 0) {
        int n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        esp_err_t err = i2s_channel_write(tx, buf, n * 2 * sizeof(int16_t),
                                          &written, portMAX_DELAY);
        if (err != ESP_OK) return err;
        frames -= n;
    }
    return ESP_OK;
}

esp_err_t chime_play(i2s_chan_handle_t tx, int mute_gpio, int mute_active_lvl, float gain)
{
    const int16_t *pcm = (const int16_t *)chime_pcm_start;
    const int total = (chime_pcm_end - chime_pcm_start) / sizeof(int16_t);

    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    const int32_t g_q15 = (int32_t)(gain * 32767.0f);

    // 1. Clock the DAC with zeros, then unmute and let it settle -> no pop
    esp_err_t err = write_silence(tx, SAMPLE_RATE / 20);           // 50 ms
    if (err != ESP_OK) return err;
    if (mute_gpio >= 0) gpio_set_level(mute_gpio, !mute_active_lvl);
    err = write_silence(tx, SAMPLE_RATE / 20);                     // 50 ms
    if (err != ESP_OK) return err;

    // 2. Stream the chime, mono -> stereo, with gain and edge fades
    int16_t buf[CHUNK_FRAMES * 2];
    size_t written;
    for (int pos = 0; pos < total; pos += CHUNK_FRAMES) {
        int n = total - pos < CHUNK_FRAMES ? total - pos : CHUNK_FRAMES;
        for (int i = 0; i < n; i++) {
            int idx = pos + i;
            int32_t s = ((int32_t)pcm[idx] * g_q15) >> 15;
            if (idx < FADE_FRAMES)          s = s * idx / FADE_FRAMES;
            if (total - idx < FADE_FRAMES)  s = s * (total - idx) / FADE_FRAMES;
            buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
        }
        err = i2s_channel_write(tx, buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        if (err != ESP_OK) return err;
    }

    // 3. Trailing silence so the DMA buffers drain, then mute again
    err = write_silence(tx, SAMPLE_RATE / 10);                     // 100 ms
    if (mute_gpio >= 0) gpio_set_level(mute_gpio, mute_active_lvl);
    return err;
}
