#pragma once

/**
 * @file audio_dsp.h
 * @brief The user's DSP profile: stored in NVS, resolved onto the outputs.
 *
 * A profile holds three filter sets as the calibration page knows them:
 * L and R (stereo, left only, right only) and M (mono). Which set runs on
 * which output follows the current channel mode:
 *
 *   STEREO: L -> left output, R -> right output
 *   MONO:   M -> both     LEFT: L -> both     RIGHT: R -> both
 *
 * All functions are safe to call from any task except the audio task.
 */

#include <stdbool.h>

#include "audio_output.h"
#include "dsp.h"
#include "esp_err.h"

typedef enum {
  AUDIO_DSP_SET_L = 0,
  AUDIO_DSP_SET_R,
  AUDIO_DSP_SET_M,
  AUDIO_DSP_SET_COUNT,
} audio_dsp_set_t;

/* Used while this device is a subwoofer (fleet role "bass"). */
typedef struct {
  float xo;       // crossover, Hz; the room's speakers get the high-pass
  float gain_db;  // level against the speakers
  float delay_ms; // a subwoofer nearer than the speakers waits for them
  bool invert;
} audio_dsp_sub_t;

typedef struct {
  float preamp_db;
  dsp_filter_t sets[AUDIO_DSP_SET_COUNT][DSP_MAX_FILTERS_PER_CH];
  uint8_t count[AUDIO_DSP_SET_COUNT];
  dsp_bass_t bass;
  audio_dsp_sub_t sub;
} audio_dsp_profile_t;

/** dsp_init() at the output rate, then load and apply the saved profile. */
void audio_dsp_init(uint32_t sample_rate);

/** A flat profile (no filters, 0 dB, bass enhancement off). */
void audio_dsp_profile_flat(audio_dsp_profile_t *p);

/** Clamp every value into range, in place (same rules as the DSP). */
void audio_dsp_profile_clamp(audio_dsp_profile_t *p);

/** The saved profile (not a running preview). */
void audio_dsp_get_saved(audio_dsp_profile_t *p);

/**
 * Apply and persist, and switch to @p mode (persisted too). @p saved_at is
 * the Unix time from the client (the device has no clock), 0 if unknown.
 */
esp_err_t audio_dsp_save(const audio_dsp_profile_t *p,
                         audio_channel_mode_t mode, uint32_t saved_at);

/**
 * When the saved profile was made: Unix time, 1 if it is not flat but the
 * time is unknown, 0 if it is flat (nothing calibrated).
 */
uint32_t audio_dsp_calibrated_at(void);

/** The bass block is on in the saved profile. */
bool audio_dsp_saved_bass(void);

/** Apply without persisting; @p mode is applied but not persisted either. */
void audio_dsp_preview(const audio_dsp_profile_t *p, audio_channel_mode_t mode);

/** Drop a running preview: back to the saved profile and channel mode. */
void audio_dsp_revert(void);

/** True while a preview runs instead of the saved profile. */
bool audio_dsp_previewing(void);

/**
 * The device's place in its room (from fleet): @p is_sub plays the mono
 * low-pass with the profile's sub settings and set M; otherwise @p room_hp
 * > 0 is the crossover of a subwoofer playing in the same room, giving the
 * matching high-pass and the bass block off. Re-resolves on a change.
 */
void audio_dsp_set_room(bool is_sub, float room_hp);

/** What audio_dsp_set_room() last set. */
void audio_dsp_get_room(bool *is_sub, float *room_hp);

/** Crossover of the saved profile's subwoofer settings, Hz. */
float audio_dsp_saved_xo(void);

/** Re-resolve the running profile for a new channel mode. */
void audio_dsp_on_channel_mode(audio_channel_mode_t mode);
