#pragma once

/**
 * @file fleet_clock.h
 * @brief Clock comparison between arply devices over UDP.
 */

#include <stdint.h>

#include "esp_err.h"

#define FLEET_CLOCK_PORT 7123

/** Answer clock requests from other devices. Call once the network is up. */
void fleet_clock_start(void);

/**
 * Compare this device's esp_timer clock with the device at @p ip (network
 * byte order): peer time = own time + @p offset_us, from the exchange with
 * the shortest round trip @p rtt_us. Blocks ~0.4 s.
 */
esp_err_t fleet_clock_measure(uint32_t ip, int64_t *offset_us, int32_t *rtt_us);
