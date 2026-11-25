#pragma once

#include "esp_err.h"

#include "freertos/FreeRTOS.h"

/**
 * Output channel mode. LEFT/RIGHT route the chosen source channel to both
 * speakers; MONO plays the (L+R)/2 downmix on both speakers; STEREO (default)
 * plays the normal left/right mix.
 */
typedef enum {
  AUDIO_CHANNEL_STEREO = 0,
  AUDIO_CHANNEL_LEFT,
  AUDIO_CHANNEL_RIGHT,
  AUDIO_CHANNEL_MONO,
} audio_channel_mode_t;

/** Volume range shared by every input source. 0 dB = unity, -30 dB = silence.
 */
#define AUDIO_VOLUME_MIN_DB     (-30.0f)
#define AUDIO_VOLUME_MAX_DB     (0.0f)
#define AUDIO_VOLUME_DEFAULT_DB (-15.0f)

/**
 * Convert a volume in dB to a Q15 linear gain (0..32768), applying the
 * perceptual (squared) curve. The one place this curve is defined.
 */
int32_t audio_output_volume_q15_from_db(float volume_db);

/**
 * Current output gain as Q15, from the single shared volume setting.
 *
 * Every source that writes to the DAC must scale by this, so that switching
 * between them does not change the level: arply has one physical output, so
 * it has one volume. Both the AirPlay sender's slider and a USB host's UAC
 * volume control write through to that same setting.
 */
int32_t audio_output_volume_q15(void);

/**
 * Initialize the I2S audio output.
 */
esp_err_t audio_output_init(void);

/**
 * Start the audio playback task.
 */
void audio_output_start(void);

/**
 * Flush output buffers (clears stale audio on pause/seek).
 */
void audio_output_flush(void);

/**
 * Stop the AirPlay playback task (for yielding I2S to another source)
 */
void audio_output_stop(void);

/**
 * True while the playback task owns the I2S output.
 */
bool audio_output_is_running(void);

/**
 * Write raw PCM data to the I2S output.
 * Can be used by any audio source (BT A2DP, etc.) when the AirPlay
 * playback task is stopped.
 *
 * @param data   PCM data buffer (interleaved stereo, 16-bit)
 * @param bytes  Number of bytes to write
 * @param wait   Maximum ticks to wait for I2S DMA space
 * @return ESP_OK on success
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/**
 * Change the I2S sample rate (e.g. when BT negotiates 48 kHz)
 *
 * @param rate  Sample rate in Hz (e.g. 44100, 48000)
 */
void audio_output_set_sample_rate(uint32_t rate);

/**
 * Notify the output of the source sample rate (from AirPlay ANNOUNCE).
 * The resampler is re-initialized if the rate changes.
 */
void audio_output_set_source_rate(int rate);

/**
 * Return the I2S DMA pipeline latency in microseconds.
 *
 * This is computed from the DMA descriptor count and frame count
 * (both set at init time) divided by the output sample rate — i.e.
 *   (dma_desc_num × dma_frame_num × 1 000 000) / sample_rate
 *
 * Using this value instead of a hard-coded constant means the latency
 * stays correct if the DMA config or sample rate is ever changed.
 */
uint32_t audio_output_get_hardware_latency_us(void);

/** esp_timer time at which the next sample handed to the output leaves the
 * DAC, from bytes written vs bytes the DMA has sent (exact, not averaged). */
int64_t audio_output_next_play_us(void);

/**
 * Cycle the output channel mode: STEREO -> LEFT -> RIGHT -> MONO -> STEREO.
 * The new mode is persisted to NVS.
 * @return the new mode after cycling.
 */
audio_channel_mode_t audio_output_cycle_channel_mode(void);

/**
 * Set the output channel mode directly and persist it to NVS.
 */
void audio_output_set_channel_mode(audio_channel_mode_t mode);

/**
 * Apply a channel mode for listening tests without persisting it.
 */
void audio_output_preview_channel_mode(audio_channel_mode_t mode);

/**
 * Return to the persisted channel mode (ends a preview).
 */
void audio_output_restore_channel_mode(void);

/**
 * Channel mode, then the output DSP, on interleaved stereo int16 in place.
 * For writers that bypass the playback task (USB DAC input). Returns true
 * if the DSP also applied the volume; otherwise the caller must.
 */
bool audio_output_process(int16_t *buf, size_t frames);

/**
 * Debug probe: record the next @p seconds of what goes into the output
 * processing and what goes to the DAC (PSRAM, replaces an earlier capture).
 */
esp_err_t audio_output_capture_start(uint32_t seconds);

/**
 * The finished capture, interleaved in L, in R, out L, out R at the output
 * rate, or NULL while it is still running or none was started.
 */
const int16_t *audio_output_capture_get(size_t *frames);

/**
 * Get the current output channel mode.
 */
audio_channel_mode_t audio_output_get_channel_mode(void);

/**
 * True when the DAC configuration already fixes the per-output routing, in
 * which case the mode is forced to STEREO and set/cycle are ignored.
 */
bool audio_output_channel_mode_locked(void);
