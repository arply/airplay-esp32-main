#pragma once

/**
 * @file test_signal.h
 * @brief Signals arply plays itself: measurement sweeps, level-check noise,
 *        and embedded PCM (boot chime).
 *
 * The playback task plays these instead of the receiver's audio, without
 * DSP and volume (raw speaker at a fixed level). Starting one fails with
 * ESP_ERR_INVALID_STATE while a stream owns the output or another signal
 * runs; a stream that starts playing stops the signal.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Output selection, a bit mask. */
#define TEST_SIGNAL_OUT_L    0x1
#define TEST_SIGNAL_OUT_R    0x2
#define TEST_SIGNAL_OUT_BOTH (TEST_SIGNAL_OUT_L | TEST_SIGNAL_OUT_R)

#define TEST_SIGNAL_MAX_SWEEPS 4
#define TEST_SIGNAL_SWEEP_MS   4000
#define TEST_SIGNAL_LEAD_MS    500

void test_signal_init(uint32_t sample_rate);

/**
 * Play @p count sweeps back to back, sweep i on outputs @p outs[i].
 * Timeline: TEST_SIGNAL_LEAD_MS silence, then per sweep TEST_SIGNAL_SWEEP_MS
 * sweep + @p gap_ms silence.
 */
esp_err_t test_signal_sweep(const uint8_t *outs, int count, uint32_t gap_ms);

/** Pink noise at -15 dBFS RMS on @p outs, stopping by itself after
 * @p max_ms. */
esp_err_t test_signal_noise(uint8_t outs, uint32_t max_ms);

/** Mono s16 PCM on both outputs at @p gain (0..1), e.g. the boot chime. */
esp_err_t test_signal_pcm(const int16_t *pcm, size_t samples, float gain);

/**
 * Loop @p frames of s16 PCM (@p channels 1 or 2, interleaved, at the output
 * rate) until stopped, through the channel mode, DSP and volume like
 * normal playback: for listening to the correction on arply's speakers.
 * The caller keeps @p pcm valid until test_signal_wait_idle() after a stop.
 */
esp_err_t test_signal_listen(const int16_t *pcm, size_t frames, int channels);

/** Wait until no signal is active; false on timeout. */
bool test_signal_wait_idle(uint32_t timeout_ms);

/** Stop whatever plays (5 ms fade). */
void test_signal_stop(void);

/** True while a signal is requested or playing. */
bool test_signal_active(void);

/* What the playback task must still do with a filled frame. */
#define TEST_SIGNAL_FOLLOW_VOLUME 0x1 // sweeps, noise, listening samples
#define TEST_SIGNAL_APPLY_DSP     0x2 // listening samples only

/**
 * Audio task only: fill @p frames interleaved stereo frames. Always fills
 * the whole buffer (silence after the end). Returns TEST_SIGNAL_* flags.
 * Measurement signals follow arply's volume (so they are as loud as the
 * user's music) but skip the DSP; the chime has a fixed level.
 */
uint8_t test_signal_fill(int16_t *buf, size_t frames);
