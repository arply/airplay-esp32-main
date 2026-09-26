#pragma once

#include "esp_err.h"

/* ========== Common GPIO ISR service ========== */

/**
 * @brief Install the shared GPIO ISR service
 *
 * Safe to call multiple times — returns ESP_OK if already installed.
 * Board-specific init and the button driver both rely on this.
 *
 * @return ESP_OK on success
 */
esp_err_t board_gpio_isr_init(void);
