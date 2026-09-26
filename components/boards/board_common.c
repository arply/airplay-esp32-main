#include "board_common.h"

#include "esp_sleep.h"

// arply-v1 has no software power latch, so "off" is deep sleep.
void board_power_off(void) {
  esp_deep_sleep_start();
}
