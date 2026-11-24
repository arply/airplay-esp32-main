#pragma once

#include "esp_err.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Play the ARPLY power-on chime (2.0 s, mono, 44.1 kHz, already at -18 dBFS peak).
 *
 * Expects an enabled I2S TX channel configured for 44.1 kHz, 16-bit, stereo.
 * Blocks for about 2.1 s. Call once after power-up, before AirPlay starts.
 *
 * @param tx               enabled I2S TX channel
 * @param mute_gpio        DAC mute pin, or -1 if not used
 * @param mute_active_lvl  level that MUTES the DAC (0 or 1)
 * @param gain             extra gain, 0.0 ... 1.0 (1.0 = as designed)
 */
esp_err_t chime_play(i2s_chan_handle_t tx, int mute_gpio, int mute_active_lvl, float gain);

#ifdef __cplusplus
}
#endif
