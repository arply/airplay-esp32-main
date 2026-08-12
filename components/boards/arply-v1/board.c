/**
 * @file board.c
 * @brief ARPLY v1 board implementation (Seeed XIAO ESP32-S3 + PCM5102A)
 *
 * The PCM5102A has no control bus, so the only board-specific hardware is its
 * XSMT pin: low mutes the DAC, high passes audio.  Unlike the generic boards —
 * which release the mute pin once at boot — this board keeps XSMT low until
 * playback actually starts and re-asserts it on pause/disconnect.
 *
 * iot_board_init() runs long before audio_output_init() creates the I2S
 * channel, so booting muted also avoids a pop while BCK/LRCK/DIN still float.
 */

#include "iot_board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "rtsp_events.h"

static const char TAG[] = "ARPLY-v1";

static bool s_board_initialized = false;

static void set_mute(bool muted) {
#if BOARD_MUTE_GPIO >= 0
  gpio_set_level(BOARD_MUTE_GPIO,
                 muted ? BOARD_MUTE_GPIO_LEVEL : !BOARD_MUTE_GPIO_LEVEL);
#else
  (void)muted;
#endif
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;

  switch (event) {
  case RTSP_EVENT_PLAYING:
    set_mute(false);
    break;
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PAUSED:
  case RTSP_EVENT_DISCONNECTED:
    set_mute(true);
    break;
  case RTSP_EVENT_METADATA:
    break;
  }
}

static esp_err_t init_mute_gpio(void) {
#if BOARD_MUTE_GPIO >= 0
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BOARD_MUTE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure mute GPIO: %s", esp_err_to_name(err));
    return err;
  }

  // Start muted: I2S is not running yet, and playback drives the pin from here
  // on via the RTSP events.
  set_mute(true);

  ESP_LOGI(TAG, "XSMT on GPIO %d initialized muted (active %s)",
           BOARD_MUTE_GPIO, BOARD_MUTE_GPIO_LEVEL ? "high" : "low");
#endif
  return ESP_OK;
}

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  (void)id;
  return NULL;
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

  esp_err_t err = init_mute_gpio();
  if (err != ESP_OK) {
    return err;
  }

  // Follow playback state so the DAC is only unmuted while audio is playing.
  rtsp_events_register(on_rtsp_event, NULL);

  s_board_initialized = true;
  ESP_LOGI(TAG, "%s initialized", BOARD_DESCRIPTION);
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  if (!s_board_initialized) {
    return ESP_OK;
  }

  rtsp_events_unregister(on_rtsp_event);
  set_mute(true);

  s_board_initialized = false;
  return ESP_OK;
}
