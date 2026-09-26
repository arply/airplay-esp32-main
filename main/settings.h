#pragma once

#include "esp_err.h"
#include "sdkconfig.h"
#include <stdbool.h>

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

// ---- Sub level offset ----

/**
 * Get saved sub level offset in dB (relative to master volume).
 * @param offset_db Output: offset in dB
 * @return ESP_OK if found, error otherwise
 */
esp_err_t settings_get_sub_offset(float *offset_db);

/**
 * Save sub level offset (dB) to persistent storage.
 */
esp_err_t settings_set_sub_offset(float offset_db);

/**
 * Get the saved sub low-pass crossover frequency in Hz (0 = full range).
 */
esp_err_t settings_get_sub_crossover(float *hz);

/**
 * Save the sub low-pass crossover frequency (Hz) to persistent storage.
 */
esp_err_t settings_set_sub_crossover(float hz);

// ---- Dual DAC (second amplifier) role ----

/**
 * Get the saved second-amplifier role (tas58xx_dual_mode_t value).
 * @param mode Output: 0 = PBTL mono sub, 1 = bi-amp left/right
 * @return ESP_OK if found, error otherwise
 */
esp_err_t settings_get_dual_mode(uint8_t *mode);

/**
 * Save the second-amplifier role to persistent storage.
 */
esp_err_t settings_set_dual_mode(uint8_t mode);

// ---- Crossover per-way EQ (2.1 and bi-amp) ----

/** EQ bands per way on either side of a crossover. */
#define SETTINGS_WAY_BANDS 12

/**
 * Get the saved 2.1 per-way EQ gains, indexed [way][band] where way 0 is the
 * sub and way 1 the satellites.
 */
esp_err_t settings_get_sub_eq(float gains_db[2][SETTINGS_WAY_BANDS]);

/**
 * Save the 2.1 per-way EQ gains.
 */
esp_err_t settings_set_sub_eq(const float gains_db[2][SETTINGS_WAY_BANDS]);

// ---- Bi-amp (two-way active crossover) ----

/**
 * Get the saved woofer/tweeter crossover frequency in Hz.
 */
esp_err_t settings_get_biamp_crossover(float *hz);

/**
 * Save the woofer/tweeter crossover frequency (Hz).
 */
esp_err_t settings_set_biamp_crossover(float hz);

/**
 * Get which amplifier output of each chip drives the woofer.
 * @param swap Output: false = first output, true = second
 */
esp_err_t settings_get_biamp_swap(bool *swap);

/**
 * Save which amplifier output of each chip drives the woofer.
 */
esp_err_t settings_set_biamp_swap(bool swap);

/**
 * Get the saved bi-amp EQ gains, indexed [speaker][way][band] where
 * speaker 0 = left, 1 = right and way 0 = woofer, 1 = tweeter.
 */
esp_err_t settings_get_biamp_eq(float gains_db[2][2][SETTINGS_WAY_BANDS]);

/**
 * Save the bi-amp EQ gains.
 */
esp_err_t settings_set_biamp_eq(const float gains_db[2][2][SETTINGS_WAY_BANDS]);
