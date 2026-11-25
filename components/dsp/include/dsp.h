#pragma once

/**
 * @file dsp.h
 * @brief Output DSP: parametric EQ, preamp, bass enhancement, soft limiter.
 *
 * Pure C (no ESP-IDF), so the same sources build in the host test
 * (scripts/dsp_host_test.sh). Filters belong to the *outputs* (speakers):
 * dsp_process() runs after the channel mode has been applied.
 *
 * Threading: dsp_set_config() may be called from any task; it never blocks
 * the audio path. dsp_process() must only ever run in one task at a time
 * (the I2S writer), because it owns the filter state and work buffers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_MAX_FILTERS_PER_CH 10

#define DSP_F_MIN    20.0f
#define DSP_F_MAX    20000.0f
#define DSP_GAIN_MIN (-18.0f)
#define DSP_GAIN_MAX 6.0f
#define DSP_Q_MIN    0.3f
#define DSP_Q_MAX    12.0f

#define DSP_PREAMP_MIN (-24.0f)
#define DSP_PREAMP_MAX 6.0f

#define DSP_BASS_F_MIN     40.0f // speaker's lowest usable frequency
#define DSP_BASS_F_MAX     200.0f
#define DSP_BASS_BOOST_MAX 12.0f // dB of extension at boost 1.0

typedef enum {
  DSP_CH_L = 0,
  DSP_CH_R = 1,
  DSP_NUM_CH = 2,
} dsp_channel_t;

typedef enum {
  DSP_FILTER_PEAK = 0,
  DSP_FILTER_LOW_SHELF,
  DSP_FILTER_HIGH_SHELF,
  DSP_FILTER_HIGH_PASS,
  DSP_FILTER_LOW_PASS,
  DSP_FILTER_TYPE_COUNT,
} dsp_filter_type_t;

typedef struct {
  dsp_filter_type_t type;
  float f;       // Hz
  float gain_db; // ignored for high/low pass
  float q;       // ignored for shelves (fixed at 1/sqrt(2))
  bool enabled;
} dsp_filter_t;

/**
 * Bass for small speakers, one block of four parts:
 *  - extension: real bass, a boost of up to boost * DSP_BASS_BOOST_MAX dB
 *    below f_low, where the speaker starts to roll off;
 *  - dynamics: the boost backs off when the bass at the DAC (after the
 *    volume, dsp_set_output_gain) would get too loud for the driver;
 *  - loudness: at low volume the boost may go up to 6 dB further;
 *  - harmonics: what lies below f_low / 2 (cut there to spare the driver)
 *    is replaced by its 2nd-4th harmonics, more of them while the dynamics
 *    hold the boost back.
 */
typedef struct {
  bool on;
  float f_low;     // Hz, DSP_BASS_F_MIN..DSP_BASS_F_MAX
  float boost;     // 0..1
  float harmonics; // 0..1
} dsp_bass_t;

typedef struct {
  float preamp_db;
  bool bypass;
  dsp_filter_t filters[DSP_NUM_CH][DSP_MAX_FILTERS_PER_CH];
  uint8_t count[DSP_NUM_CH];
  dsp_bass_t bass;
} dsp_config_t;

/** Initialise for the given output sample rate. Starts flat. */
void dsp_init(uint32_t sample_rate);

/** Clamp every field of @p cfg into the supported ranges, in place. */
void dsp_clamp_config(dsp_config_t *cfg);

/**
 * Apply a new configuration (clamped first). Coefficients are computed here,
 * in the caller's task; the audio task picks the new state up at the start
 * of its next dsp_process() call.
 */
void dsp_set_config(const dsp_config_t *cfg_in);

/** Copy of the configuration last passed to dsp_set_config(). */
void dsp_get_config(dsp_config_t *cfg);

/** Bypass processing without changing the configuration (A/B). */
void dsp_set_bypass(bool bypass);

/**
 * The volume (linear, 0..1). While the DSP is active it applies the volume
 * itself, at its input, so EQ, bass and limiter work at the level that
 * reaches the DAC (and the loudness follows it). Any task.
 */
void dsp_set_output_gain(float gain);

/**
 * Process interleaved stereo int16 in place. Any length. Returns false at
 * once, leaving the samples bit-identical and the volume to the caller,
 * when the configuration is flat or bypassed; true when it processed them,
 * volume included (dsp_set_output_gain).
 */
bool dsp_process(int16_t *buf, size_t frames);

/** True when dsp_process() currently changes the signal. */
bool dsp_is_active(void);

#ifdef __cplusplus
}
#endif
