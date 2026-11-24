#include "chime.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "settings.h"
#include "test_signal.h"

static const char *TAG = "chime";

// The playback task starts once a network is up; a chime half a minute
// after power-on would be confusing, so give up after this.
#define CHIME_BOOT_WINDOW_US (15LL * 1000 * 1000)

#if CONFIG_OUTPUT_SAMPLE_RATE_HZ == 48000
extern const uint8_t
    chime_start[] asm("_binary_arply_chime_48000_pcm_start"); // NOLINT
extern const uint8_t
    chime_end[] asm("_binary_arply_chime_48000_pcm_end"); // NOLINT
#else
extern const uint8_t
    chime_start[] asm("_binary_arply_chime_44100_pcm_start"); // NOLINT
extern const uint8_t
    chime_end[] asm("_binary_arply_chime_44100_pcm_end"); // NOLINT
#endif

static bool s_armed;

esp_err_t chime_play(float gain) {
  return test_signal_pcm((const int16_t *)chime_start,
                         (size_t)(chime_end - chime_start) / sizeof(int16_t),
                         gain);
}

void chime_boot_arm(void) {
  bool on = true;
  settings_get_chime_on(&on);
  s_armed = on && esp_reset_reason() == ESP_RST_POWERON;
}

void chime_boot_play(void) {
  if (!s_armed) {
    return;
  }
  s_armed = false;
  if (esp_timer_get_time() > CHIME_BOOT_WINDOW_US) {
    ESP_LOGI(TAG, "Playback started too late after power-on, no chime");
    return;
  }
  if (chime_play(1.0f) != ESP_OK) {
    ESP_LOGW(TAG, "Output busy, no chime");
  }
}
