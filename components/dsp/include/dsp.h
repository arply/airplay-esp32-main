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

#define DSP_PBE_FC_MIN 60.0f
#define DSP_PBE_FC_MAX 150.0f

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

/** Psychoacoustic bass enhancement (missing fundamental). */
typedef struct {
  bool on;
  float fc;     // Hz, DSP_PBE_FC_MIN..DSP_PBE_FC_MAX
  float amount; // 0..1
} dsp_pbe_t;

typedef struct {
  float preamp_db;
  bool bypass;
  dsp_filter_t filters[DSP_NUM_CH][DSP_MAX_FILTERS_PER_CH];
  uint8_t count[DSP_NUM_CH];
  dsp_pbe_t pbe;
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
 * Process interleaved stereo int16 in place. Any length. Returns at once,
 * leaving the samples bit-identical, when the configuration is flat or
 * bypassed.
 */
void dsp_process(int16_t *buf, size_t frames);

/** True when dsp_process() currently changes the signal. */
bool dsp_is_active(void);

#ifdef __cplusplus
}
#endif
