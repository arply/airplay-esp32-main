#pragma once

#include "esp_err.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Persistent settings storage (NVS)
 */

// Default device name (used if none configured)
#ifdef CONFIG_DEFAULT_DEVICE_NAME
#define SETTINGS_DEFAULT_DEVICE_NAME CONFIG_DEFAULT_DEVICE_NAME
#else
#define SETTINGS_DEFAULT_DEVICE_NAME "ESP32 AirPlay"
#endif

/**
 * Initialize settings module (call once at startup)
 */
esp_err_t settings_init(void);

/**
 * Get saved volume in dB
 * @param volume_db Output: volume in dB (0 = max, -30 = mute)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_volume(float *volume_db);

/**
 * Apply volume (updates cached value and DAC, does NOT write to NVS).
 * @param volume_db Volume in dB (0 = max, -30 = mute)
 */
esp_err_t settings_set_volume(float volume_db);

/**
 * Persist the current cached volume to NVS.
 * Call once at session disconnect rather than on every change.
 */
esp_err_t settings_persist_volume(void);

// Known WiFi networks are kept as a most-recently-used (MRU) list so a
// device that has ever connected to a network stays reconnectable to it,
// not just to whichever network was configured last.
#define SETTINGS_MAX_WIFI_NETWORKS 8

typedef struct {
  char ssid[33];
  char password[65];
} settings_wifi_network_t;

/**
 * Get the most-recently-used saved WiFi SSID
 * @param ssid Output buffer for SSID
 * @param len Size of SSID buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_wifi_ssid(char *ssid, size_t len);

/**
 * Get the most-recently-used saved WiFi password
 * @param password Output buffer for password
 * @param len Size of password buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_wifi_password(char *password, size_t len);

/**
 * Get all saved WiFi networks, most-recently-used first.
 * @param networks Output array of SETTINGS_MAX_WIFI_NETWORKS entries
 * @param count Output: number of valid entries filled in
 * @return ESP_OK if at least the list was read (count may be 0),
 *         ESP_ERR_NOT_FOUND if nothing has ever been saved
 */
esp_err_t settings_get_wifi_networks(
    settings_wifi_network_t networks[SETTINGS_MAX_WIFI_NETWORKS], int *count);

/**
 * Save WiFi credentials to persistent storage. Moves this network to the
 * front of the known-networks list (creating it if new), evicting the
 * oldest entry once SETTINGS_MAX_WIFI_NETWORKS is exceeded. Previously
 * saved networks are kept, not overwritten.
 * @param ssid WiFi SSID
 * @param password WiFi password
 */
esp_err_t settings_set_wifi_credentials(const char *ssid, const char *password);

/**
 * Remove a previously saved WiFi network.
 * @param ssid WiFi SSID to forget
 * @return ESP_OK if removed, ESP_ERR_NOT_FOUND if it wasn't saved
 */
esp_err_t settings_forget_wifi_network(const char *ssid);

/**
 * Check if any WiFi credentials are stored
 * @return true if at least one saved network exists, false otherwise
 */
bool settings_has_wifi_credentials(void);

/**
 * Get device name (returns default if none saved)
 * @param name Output buffer for device name
 * @param len Size of name buffer
 * @return ESP_OK (always returns a valid name)
 */
esp_err_t settings_get_device_name(char *name, size_t len);

/**
 * Save device name to persistent storage
 * @param name Device name
 */
esp_err_t settings_set_device_name(const char *name);

/** The hub's host name: arply.local always reaches the fleet overview. */
#define SETTINGS_HUB_HOST_NAME "arply"

/**
 * The .local host name for a display name: lower case, anything but letters
 * and digits becomes '-'. A name mapping to "arply" (the factory default
 * ARPLY) becomes "arply-<last 4 MAC hex>", because arply.local is the hub.
 * Used for mDNS, DHCP and the TLS certificate.
 */
void settings_host_name_from(const char *name, char *out, size_t out_len);

/** settings_host_name_from() for this device's name. */
void settings_get_host_name(char *out, size_t out_len);

// ---- LED settings ----

/**
 * Get saved LED brightness (0–255). Returns compile-time default if not set.
 */
esp_err_t settings_get_led_brightness(uint8_t *brightness);

/**
 * Save LED brightness (0–255) to persistent storage.
 */
esp_err_t settings_set_led_brightness(uint8_t brightness);

// ---- EQ settings ----

/** Number of EQ bands stored in NVS */
#define SETTINGS_EQ_BANDS 15

/**
 * Get saved EQ gains.
 * @param gains_db Output array of SETTINGS_EQ_BANDS floats
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved EQ
 */
esp_err_t settings_get_eq_gains(float gains_db[SETTINGS_EQ_BANDS]);

/**
 * Save EQ gains to persistent storage.
 * @param gains_db Array of SETTINGS_EQ_BANDS floats (dB)
 */
esp_err_t settings_set_eq_gains(const float gains_db[SETTINGS_EQ_BANDS]);

/**
 * Clear saved EQ (revert to flat on next boot).
 */
esp_err_t settings_clear_eq(void);

/**
 * Check if EQ gains are saved.
 */
bool settings_has_eq(void);

// ---- Output channel mode ----

/**
 * Get saved output channel mode (audio_channel_mode_t value).
 * @param mode Output: channel mode enum value
 * @return ESP_OK if found, error otherwise
 */
esp_err_t settings_get_channel_mode(uint8_t *mode);

/**
 * Save output channel mode to persistent storage.
 * @param mode audio_channel_mode_t value
 */
esp_err_t settings_set_channel_mode(uint8_t mode);

// ---- DSP profile (main/audio/audio_dsp.c owns the format) ----

/**
 * Read the stored DSP profile blob.
 * @param buf Output buffer
 * @param len In: buffer size. Out: stored size.
 * @return ESP_OK, or ESP_ERR_NVS_NOT_FOUND if nothing is stored
 */
esp_err_t settings_get_dsp_blob(void *buf, size_t *len);

/** Store the DSP profile blob. Writes flash: never call from the audio task. */
esp_err_t settings_set_dsp_blob(const void *buf, size_t len);

// ---- Boot chime ----

/** Whether the power-on chime plays (default true when never set). */
esp_err_t settings_get_chime_on(bool *on);

/** Enable or disable the power-on chime. */
esp_err_t settings_set_chime_on(bool on);

// ---- Setup hotspot ----

/** Quick power cycles counted towards forcing the setup hotspot. */
esp_err_t settings_get_power_cycles(uint8_t *count);
esp_err_t settings_set_power_cycles(uint8_t count);

// ---- Sub level offset ----
