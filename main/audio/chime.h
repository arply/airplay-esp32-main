#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @file chime.h
 * @brief The arply power-on chime ("F5 sparkle", 2.0 s, -18 dBFS peak).
 *
 * Rendered by scripts/gen_chime.py and embedded at the output sample rate.
 * Plays through the test-signal source, so it shares the DAC unmute and
 * never collides with the playback task.
 */

/** Play the chime now at @p gain (0..1, 1 = as designed). */
esp_err_t chime_play(float gain);

/**
 * Call once early in app_main(): remembers whether this boot is a power-on
 * with the chime enabled. Soft resets, OTA and crashes stay silent.
 */
void chime_boot_arm(void);

/**
 * Call right after the playback task starts: plays the armed chime, unless
 * the playback task came up too late after power-on to still make sense.
 */
void chime_boot_play(void);
