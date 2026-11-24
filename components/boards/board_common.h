#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "board_utils.h"

/**
 * @brief Initialize board-specific hardware
 *
 * This function is called early during startup to initialize any
 * board-specific peripherals such as DACs, GPIOs, power management, etc.
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t iot_board_init(void);

/**
 * @brief Get board information string
 *
 * @return Board name string (never NULL)
 */
const char *iot_board_get_info(void);

/**
 * @brief Unmute the DAC for a signal arply plays itself (test sweep, noise,
 *        boot chime), independent of the stream state.
 *
 * The DAC passes audio while a stream plays OR such a signal is active.
 * Safe from any task.
 */
void iot_board_set_signal_unmute(bool on);

/**
 * @brief Power the board off.
 *
 * arply-v1 has no power latch, so this enters deep sleep.
 */
void board_power_off(void);
