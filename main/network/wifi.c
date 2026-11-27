#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "wifi.h"
#include "settings.h"

static const char *TAG = "wifi";

// Event group to signal WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Re-enable AP after this many consecutive failures
#define AP_REENABLE_THRESHOLD 5
// lwIP DHCP hostnames are limited to 31 characters plus the trailing NUL.
#define DHCP_HOSTNAME_MAX_LEN 31

static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static bool s_bssid_set = false;
static esp_timer_handle_t s_retry_timer = NULL;

// Saved AP config from init, used to re-enable AP without duplication
static wifi_config_t s_ap_config;

// The setup hotspot is open by default, so it should not stay up longer
// than needed: it switches off after AP_IDLE_OFF_US without any client.
#define AP_IDLE_OFF_US     (10LL * 60 * 1000 * 1000)
#define AP_CHECK_PERIOD_US (30LL * 1000 * 1000)
// Power-cycling this many times, each within SETUP_CYCLE_WINDOW_US of
// boot, forces the hotspot on even when a known network is in range. The
// window outlasts the boot chime (~5.5 s): people wait for it, then unplug.
#define SETUP_POWER_CYCLES    3
#define SETUP_CYCLE_WINDOW_US (10LL * 1000 * 1000)

static esp_timer_handle_t s_ap_check_timer = NULL;
static int64_t s_ap_active_since = 0; // last time the AP was on with clients
static bool s_ap_forced = false;      // setup requested by power cycling

// Credentials from the setup page are tried first and saved only once they
// work. After a successful try the hotspot stays up until the phone that
// sent them has left, so it can still show the result.
static volatile wifi_try_state_t s_try = WIFI_TRY_IDLE;
static char s_try_ssid[33];
static char s_try_pass[65];
#define TRY_TIMEOUT_MS 30000

static void wifi_select_best_ap(const char *ssid);
static bool wifi_select_best_known_network(void);
static void scan_and_connect_task(void *arg);

void wifi_set_hostname(const char *device_name) {
  if (!s_sta_netif || !device_name) {
    return;
  }
  char hostname[DHCP_HOSTNAME_MAX_LEN + 1];
  settings_host_name_from(device_name, hostname, sizeof(hostname));
  esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set hostname '%s': %s", hostname,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Hostname set to: %s", hostname);
  }
}

static void retry_timer_callback(void *arg) {
  if (!s_sta_connected) {
    ESP_LOGI(TAG, "Retry timer fired, reconnecting (attempt %d)...",
             s_retry_num + 1);
    esp_wifi_connect();
  }
}

static void schedule_retry(void) {
  // Exponential backoff: 5s, 10s, 20s, 30s (max)
  int delay_s = 5;
  if (s_retry_num > AP_REENABLE_THRESHOLD) {
    int backoff_count = s_retry_num - AP_REENABLE_THRESHOLD;
    delay_s = 5 * (1 << (backoff_count > 3 ? 3 : backoff_count));
    if (delay_s > 30) {
      delay_s = 30;
    }
  }
  ESP_LOGI(TAG, "Scheduling retry in %d seconds", delay_s);
  esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
}

static void enable_ap_mode(void) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
    ESP_LOGI(TAG, "Re-enabling AP mode for configuration access");
    if (!s_ap_netif) {
      s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
    s_ap_active_since = esp_timer_get_time();
  }
}

/* Periodic: switch the setup hotspot off once nobody has used it for
 * AP_IDLE_OFF_US. A device without any saved network keeps it — that is
 * its only way in. */
static void ap_check_cb(void *arg) {
  (void)arg;
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_APSTA) {
    return;
  }
  wifi_sta_list_t clients;
  if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
    s_ap_active_since = esp_timer_get_time();
    return;
  }
  if (!settings_has_wifi_credentials() ||
      (s_try != WIFI_TRY_OK &&
       esp_timer_get_time() - s_ap_active_since < AP_IDLE_OFF_US)) {
    return;
  }
  ESP_LOGI(TAG, "Setup hotspot off: no client left on it");
  s_ap_forced = false;
  s_try = WIFI_TRY_IDLE;
  esp_wifi_set_mode(WIFI_MODE_STA);
}

static void clear_power_cycles_cb(void *arg) {
  (void)arg;
  settings_set_power_cycles(0);
}

/* Count quick power cycles (only power-on resets count; a quick replug can
 * come up as a brown-out). Other resets, e.g. a serial monitor opening the
 * USB port, neither count nor clear. The counter is cleared once the device
 * has been up for SETUP_CYCLE_WINDOW_US. */
static void check_setup_request(void) {
  uint8_t cycles = 0;
  settings_get_power_cycles(&cycles);
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
    cycles++;
  }
  ESP_LOGI(TAG, "Reset reason %d: power cycle %u of %d", (int)reason,
           (unsigned)cycles, SETUP_POWER_CYCLES);
  if (cycles >= SETUP_POWER_CYCLES) {
    ESP_LOGW(TAG, "Power-cycled %u times: setup hotspot forced on",
             (unsigned)cycles);
    s_ap_forced = true;
    cycles = 0;
  }
  settings_set_power_cycles(cycles);
  if (cycles > 0) {
    const esp_timer_create_args_t args = {.callback = clear_power_cycles_cb,
                                          .name = "pwr_cycles"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
      esp_timer_start_once(t, SETUP_CYCLE_WINDOW_US);
    }
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // Defer scan+connect to a separate task — the blocking scan uses too
    // much stack to run inside the sys_evt event loop (2–4 KB).
    xTaskCreate(scan_and_connect_task, "wifi_scan", 4096, NULL, 3, NULL);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_sta_connected = false;
    wifi_event_sta_disconnected_t *disconnected =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGI(TAG, "Disconnected from AP, reason: %d", disconnected->reason);

    s_retry_num++;

    if (s_retry_num < AP_REENABLE_THRESHOLD) {
      // Fast retries — reconnect immediately
      ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_num,
               AP_REENABLE_THRESHOLD);
      esp_wifi_connect();
    } else {
      if (s_retry_num == AP_REENABLE_THRESHOLD) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGW(TAG,
                 "WiFi connection failed after %d attempts, switching to "
                 "backoff retries",
                 AP_REENABLE_THRESHOLD);
        enable_ap_mode();
      }
      // Delayed retries with backoff
      schedule_retry();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_sta_connected = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // Disable AP mode when STA connects (unless setup was requested by
    // power cycling, or the setup page still has to show its result: then
    // ap_check_cb turns it off)
    wifi_mode_t mode;
    if (!s_ap_forced && s_try != WIFI_TRY_RUNNING && s_try != WIFI_TRY_OK &&
        esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
      ESP_LOGI(TAG, "STA connected, disabling AP mode");
      esp_wifi_set_mode(WIFI_MODE_STA);
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
    ESP_LOGI(TAG, "AP started");
  }
}

// One-shot task: scan for best AP then connect — runs outside the event loop
// to avoid overflowing the sys_evt stack.
static void scan_and_connect_task(void *arg) {
  // Prefer any known network currently in range over whichever one happens
  // to be configured (which is just the most-recently-used one) — this is
  // what lets the device reconnect to an older saved network if that's the
  // one actually available right now.
  if (!wifi_select_best_known_network()) {
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
        strlen((char *)cfg.sta.ssid) > 0) {
      wifi_select_best_ap((char *)cfg.sta.ssid);
    }
  }
  esp_wifi_connect();
  vTaskDelete(NULL);
}

// Scans all visible APs and, among the ones matching a saved network,
// configures STA for the strongest one. Returns false (leaving STA config
// untouched) if no saved network is currently in range.
static bool wifi_select_best_known_network(void) {
  settings_wifi_network_t networks[SETTINGS_MAX_WIFI_NETWORKS];
  int net_count = 0;
  if (settings_get_wifi_networks(networks, &net_count) != ESP_OK ||
      net_count == 0) {
    return false;
  }

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time = {.active = {.min = 0,
                               .max = 0}}, // 0, 0 needed for BT co-exist
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Known-network scan failed: %s", esp_err_to_name(err));
    return false;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    return false;
  }

  wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
  if (!ap_list) {
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return false;
  }
  esp_wifi_scan_get_ap_records(&ap_count, ap_list);

  int best_ap_idx = -1;
  int best_net_idx = -1;
  for (int i = 0; i < ap_count; i++) {
    for (int n = 0; n < net_count; n++) {
      if (strcmp((char *)ap_list[i].ssid, networks[n].ssid) == 0) {
        if (best_ap_idx < 0 || ap_list[i].rssi > ap_list[best_ap_idx].rssi) {
          best_ap_idx = i;
          best_net_idx = n;
        }
        break;
      }
    }
  }

  bool found = false;
  if (best_ap_idx >= 0) {
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, networks[best_net_idx].ssid,
            sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, networks[best_net_idx].password,
            sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    memcpy(sta_cfg.sta.bssid, ap_list[best_ap_idx].bssid, 6);
    sta_cfg.sta.bssid_set = true;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    s_bssid_set = true;
    ESP_LOGI(TAG, "Selected known network '%s' (rssi=%d)",
             networks[best_net_idx].ssid, ap_list[best_ap_idx].rssi);
    found = true;
  }

  free(ap_list);
  return found;
}

// Scan for the best AP matching our SSID and set its BSSID in the STA config
static void wifi_select_best_ap(const char *ssid) {
  wifi_scan_config_t scan_config = {
      .ssid = (uint8_t *)ssid,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time = {.active = {.min = 0,
                               .max = 0}}, // 0, 0 needed for BT co-exist
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Best-AP scan failed: %s", esp_err_to_name(err));
    return;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    ESP_LOGW(TAG, "Best-AP scan: no APs found for SSID %s", ssid);
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return;
  }

  wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
  if (!ap_list) {
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return;
  }

  esp_wifi_scan_get_ap_records(&ap_count, ap_list);

  // Find AP with strongest signal
  int best_idx = 0;
  for (int i = 1; i < ap_count; i++) {
    if (ap_list[i].rssi > ap_list[best_idx].rssi) {
      best_idx = i;
    }
  }

#if CONFIG_WIFI_PREFER_5GHZ
  // Prefer the 5 GHz band when the same SSID is present on both bands.
  // 5 GHz APs use channels above 14; 2.4 GHz uses channels 1-14. Pick the
  // strongest AP in each band, then favour 5 GHz unless it is too weak and a
  // 2.4 GHz AP is available (in which case 2.4 GHz is the more reliable link).
  int best_5g = -1;
  int best_24 = -1;
  for (int i = 0; i < ap_count; i++) {
    if (ap_list[i].primary > 14) {
      if (best_5g < 0 || ap_list[i].rssi > ap_list[best_5g].rssi) {
        best_5g = i;
      }
    } else {
      if (best_24 < 0 || ap_list[i].rssi > ap_list[best_24].rssi) {
        best_24 = i;
      }
    }
  }
  if (best_5g >= 0 && (best_24 < 0 || ap_list[best_5g].rssi >=
                                          CONFIG_WIFI_PREFER_5GHZ_MIN_RSSI)) {
    best_idx = best_5g;
    ESP_LOGI(TAG, "Preferring 5 GHz AP (rssi=%d, ch=%d)", ap_list[best_5g].rssi,
             ap_list[best_5g].primary);
  } else if (best_5g >= 0) {
    ESP_LOGI(TAG, "5 GHz AP too weak (rssi=%d < %d), falling back to 2.4 GHz",
             ap_list[best_5g].rssi, CONFIG_WIFI_PREFER_5GHZ_MIN_RSSI);
    best_idx = best_24;
  }
#endif

  ESP_LOGI(TAG, "Found %d APs for SSID '%s', best: " MACSTR " (rssi=%d, ch=%d)",
           ap_count, ssid, MAC2STR(ap_list[best_idx].bssid),
           ap_list[best_idx].rssi, ap_list[best_idx].primary);

  for (int i = 0; i < ap_count; i++) {
    if (i != best_idx) {
      ESP_LOGI(TAG, "  Other AP: " MACSTR " (rssi=%d, ch=%d)",
               MAC2STR(ap_list[i].bssid), ap_list[i].rssi, ap_list[i].primary);
    }
  }

  // Set BSSID in the STA config to lock to the best AP
  wifi_config_t sta_cfg;
  esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
  memcpy(sta_cfg.sta.bssid, ap_list[best_idx].bssid, 6);
  sta_cfg.sta.bssid_set = true;
  esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  s_bssid_set = true;

  free(ap_list);
}

static void set_sta(const char *ssid, const char *password) {
  wifi_config_t cfg = {0};
  // Not NUL-terminated at full length: a 32-char SSID or 64-char PSK fits.
  strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
  strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
  cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  s_bssid_set = false;
}

static void set_sta_from_settings(void) {
  char ssid[33] = "";
  char password[65] = "";
  settings_get_wifi_ssid(ssid, sizeof(ssid));
  settings_get_wifi_password(password, sizeof(password));
  set_sta(ssid, password);
}

/* Waits for the try to end; outside the event loop, whose stack is too small
 * for saving the network list. */
static void try_task(void *arg) {
  (void)arg;
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(TRY_TIMEOUT_MS));
  if ((bits & WIFI_CONNECTED_BIT) &&
      settings_set_wifi_credentials(s_try_ssid, s_try_pass) == ESP_OK) {
    ESP_LOGI(TAG, "Joined '%s': saved", s_try_ssid);
    s_try = WIFI_TRY_OK;
  } else {
    // The pending retry reconnects to the saved network, if there is one.
    ESP_LOGW(TAG, "Could not join '%s'", s_try_ssid);
    set_sta_from_settings();
    s_try = WIFI_TRY_FAILED;
  }
  vTaskDelete(NULL);
}

esp_err_t wifi_try_credentials(const char *ssid, const char *password) {
  if (s_try == WIFI_TRY_RUNNING) {
    return ESP_ERR_INVALID_STATE;
  }
  strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
  strlcpy(s_try_pass, password, sizeof(s_try_pass));
  s_try = WIFI_TRY_RUNNING;
  esp_timer_stop(s_retry_timer);
  s_retry_num = 0;
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  esp_wifi_disconnect();
  set_sta(ssid, password);
  esp_wifi_connect();
  if (xTaskCreate(try_task, "wifi_try", 4096, NULL, 3, NULL) != pdPASS) {
    s_try = WIFI_TRY_FAILED;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

wifi_try_state_t wifi_try_state(void) {
  return s_try;
}

static void wifi_init_base(void) {
  if (s_wifi_initialized) {
    return;
  }

  s_wifi_event_group = xEventGroupCreate();

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  // Create event loop if it doesn't exist
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  // Create one-shot retry timer (no background task needed)
  const esp_timer_create_args_t timer_args = {
      .callback = retry_timer_callback,
      .name = "wifi_retry",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

  s_wifi_initialized = true;
}

void wifi_init_apsta(const char *ap_ssid, const char *ap_password) {
  wifi_init_base();
  check_setup_request();

  if (!s_sta_netif) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    char dev_name[65];
    settings_get_device_name(dev_name, sizeof(dev_name));
    wifi_set_hostname(dev_name);
  }
  if (!s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
  }

  // Configure AP and save for later re-enable
  const char *default_ssid = ap_ssid ? ap_ssid : CONFIG_DEFAULT_AP_SSID;
  const char *default_password =
      ap_password ? ap_password : CONFIG_DEFAULT_AP_PASSWORD;

  memset(&s_ap_config, 0, sizeof(s_ap_config));
  strncpy((char *)s_ap_config.ap.ssid, default_ssid,
          sizeof(s_ap_config.ap.ssid) - 1);
  s_ap_config.ap.ssid_len = strlen(default_ssid);
  s_ap_config.ap.channel = 1;
  s_ap_config.ap.max_connection = 4;

  if (strlen(default_password) == 0) {
    s_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    strncpy((char *)s_ap_config.ap.password, default_password,
            sizeof(s_ap_config.ap.password) - 1);
    s_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }

  s_retry_num = 0;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  set_sta_from_settings();
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  s_ap_active_since = esp_timer_get_time();
  if (!s_ap_check_timer) {
    const esp_timer_create_args_t args = {.callback = ap_check_cb,
                                          .name = "ap_check"};
    if (esp_timer_create(&args, &s_ap_check_timer) == ESP_OK) {
      esp_timer_start_periodic(s_ap_check_timer, AP_CHECK_PERIOD_US);
    }
  }

  ESP_LOGI(TAG, "AP+STA mode started: AP SSID=%s", default_ssid);
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (!s_wifi_event_group) {
    return false;
  }

  TickType_t timeout_ticks =
      timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, timeout_ticks);

  if (bits & WIFI_CONNECTED_BIT) {
    return true;
  }
  if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to WiFi");
  }
  return false;
}

void wifi_get_mac_str(char *mac_str, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

bool wifi_is_connected(void) {
  return s_sta_connected;
}

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  if (!s_sta_netif || !ip_str || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  if (err == ESP_OK) {
    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
  }
  return err;
}

esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  if (!ap_list || !ap_count) {
    return ESP_ERR_INVALID_ARG;
  }

  // A connection attempt blocks scanning: stop it (the retry logic resumes
  // it). Connected, scan as is, or the answer to a LAN client is lost.
  if (!s_sta_connected) {
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // Clear BSSID lock so next connect can use a fresh scan result
  if (!s_sta_connected && s_bssid_set) {
    wifi_config_t sta_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
      memset(sta_cfg.sta.bssid, 0, sizeof(sta_cfg.sta.bssid));
      sta_cfg.sta.bssid_set = false;
      esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    }
    s_bssid_set = false;
  }

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false, // no name to pick; type it in instead
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
    return err;
  }

  uint16_t number = 0;
  err = esp_wifi_scan_get_ap_num(&number);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(err));
    return err;
  }

  if (number == 0) {
    *ap_list = NULL;
    *ap_count = 0;
    return ESP_OK;
  }

  wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * number);
  if (!aps) {
    return ESP_ERR_NO_MEM;
  }

  err = esp_wifi_scan_get_ap_records(&number, aps);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
    free(aps);
    return err;
  }

  *ap_list = aps;
  *ap_count = number;
  return ESP_OK;
}

void wifi_stop(void) {
  if (s_wifi_initialized) {
    esp_timer_stop(s_retry_timer);
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_initialized = false;
    s_sta_connected = false;
    s_retry_num = 0;
    if (s_wifi_event_group) {
      xEventGroupClearBits(s_wifi_event_group,
                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
  }
}
