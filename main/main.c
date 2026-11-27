#include "audio_output.h"
#include "audio_receiver.h"
#include "buttons.h"
#include "dns_server.h"
#include "led.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "playback_control.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"
#include "web_server.h"
#include "log_stream.h"
#include "wifi.h"
#include "spiffs_storage.h"
#include "https_server.h"
#include "chime.h"
#include "fleet.h"

#ifdef CONFIG_AUDIO_INPUT_USB_DAC
#include "usb_dac_input.h"
#endif

#include "iot_board.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// AP mode IP address (192.168.4.1 in network byte order)
#define AP_IP_ADDR 0x0104A8C0

static bool s_airplay_started = false;
static bool s_airplay_infrastructure_ready = false;
static bool s_audio_output_ready = false;

// audio_output_init() has two callers with no fixed ordering between them:
// the USB DAC path brings it up before the network stack (see app_main), while
// start_airplay_services() needs it whenever a network appears first. Whichever
// runs first wins; the other is a no-op.
static esp_err_t ensure_audio_output_init(void) {
  if (s_audio_output_ready) {
    return ESP_OK;
  }
  esp_err_t err = audio_output_init();
  if (err == ESP_OK) {
    s_audio_output_ready = true;
  }
  return err;
}

static void start_airplay_services(void) {
  if (s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Starting AirPlay services...");

  // One-time infrastructure init (PTP, HAP, audio receiver/output)
  if (!s_airplay_infrastructure_ready) {
    esp_err_t err = ptp_clock_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to init PTP clock: %s", esp_err_to_name(err));
      s_airplay_started = false;
      return;
    }

    ESP_ERROR_CHECK(hap_init());
    ESP_ERROR_CHECK(audio_receiver_init());
    ESP_ERROR_CHECK(ensure_audio_output_init());
    mdns_airplay_init();
    fleet_start();
    s_airplay_infrastructure_ready = true;
  }

  audio_output_start();
  chime_boot_play();

  ESP_ERROR_CHECK(rtsp_server_start());

  s_airplay_started = true;
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
  ESP_LOGI(TAG, "AirPlay ready");
}

/* The captive-portal DNS and the breathing LED follow the setup hotspot,
 * which can be on next to a working network (power-cycle setup). */
static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;
  bool had_network = wifi_is_connected();
  bool dns_running = false;

  while (1) {
    bool has_network = wifi_is_connected();
    wifi_mode_t mode;
    bool ap_on = esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA;

    if (ap_on != dns_running) {
      if (ap_on) {
        dns_server_start(AP_IP_ADDR);
      } else {
        dns_server_stop();
      }
      dns_running = ap_on;
    }
    led_set_network(has_network && !ap_on);

    if (has_network && !had_network) {
      ESP_LOGI(TAG, "Network up");
      start_airplay_services();
    }
    had_network = has_network;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(settings_init());
  chime_boot_arm();
  spiffs_storage_init();
  log_stream_init();
  ESP_ERROR_CHECK(playback_control_init());
  led_init();

  // Initialize board-specific hardware (includes I2C/SPI bus for DAC)
  ESP_LOGI(TAG, "Board: %s", iot_board_get_info());
  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

#ifdef CONFIG_AUDIO_INPUT_USB_DAC
  // USB DAC input has no network dependency — bring it up here, before
  // WiFi, so arply enumerates as a USB Audio device regardless of
  // network state. This used to live in start_airplay_services(), which only
  // runs once a network is up, so a board with no stored credentials sat in
  // captive-portal setup mode and never enumerated as a DAC at all — the host
  // just saw the plain USB Serial/JTAG device.
  ESP_ERROR_CHECK(ensure_audio_output_init());
  ESP_ERROR_CHECK(usb_dac_input_init());
#endif

  wifi_init_apsta(NULL, NULL);

  // Wait for initial WiFi connection if credentials exist
  if (settings_has_wifi_credentials()) {
    if (!wifi_wait_connected(30000)) {
      ESP_LOGI(TAG, "Connect to '%s' -> http://192.168.4.1",
               CONFIG_DEFAULT_AP_SSID);
    }
  } else {
    ESP_LOGI(TAG, "Connect to '%s' -> http://192.168.4.1",
             CONFIG_DEFAULT_AP_SSID);
  }

  web_server_start(80);
  https_server_start();
  xTaskCreate(network_monitor_task, "net_mon", 4096, NULL, 5, NULL);

  if (wifi_is_connected()) {
    start_airplay_services();
  }

  // Boot baseline: free internal DRAM once WiFi (and BT, where enabled) are
  // resident but before any stream is active.  Compare against the
  // "Buffered start" log to see the headroom available for WiFi/TCP buffers.
  ESP_LOGI(TAG,
           "Boot baseline: free heap %lu internal (largest block %lu), "
           "%lu SPIRAM",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  buttons_init();

  // An image installed over the air boots "pending verify": if it crashes
  // before this point, the bootloader goes back to the previous firmware.
  // A minute of uptime counts as healthy.
  bool confirmed = false;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (!confirmed && esp_timer_get_time() > 60LL * 1000 * 1000) {
      esp_ota_mark_app_valid_cancel_rollback();
      confirmed = true;
    }
  }
}
