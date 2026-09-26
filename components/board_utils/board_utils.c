#include "board_utils.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char TAG[] = "board_utils";

/* ========== Common GPIO ISR service ========== */

static bool s_isr_installed = false;

esp_err_t board_gpio_isr_init(void) {
  if (s_isr_installed) {
    return ESP_OK;
  }
  esp_err_t err = gpio_install_isr_service(0);
  if (err == ESP_ERR_INVALID_STATE) {
    s_isr_installed = true;
    return ESP_OK; // already installed
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s",
             esp_err_to_name(err));
    return err;
  }
  s_isr_installed = true;
  return ESP_OK;
}
