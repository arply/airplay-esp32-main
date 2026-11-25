#include "audio_dsp.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

static const char *TAG = "audio_dsp";

// Bump when audio_dsp_profile_t changes; a stored blob with another
// version (or size) is ignored and the device starts flat.
#define PROFILE_VERSION 2

typedef struct {
  uint16_t version;
  uint16_t size;
  audio_dsp_profile_t profile;
} stored_profile_t;

/* Version 1 (until 2026-11-25): bass enhancement was harmonics only,
 * {on, fc, amount}. Loaded and converted so saved EQs survive the update. */
typedef struct {
  float preamp_db;
  dsp_filter_t sets[AUDIO_DSP_SET_COUNT][DSP_MAX_FILTERS_PER_CH];
  uint8_t count[AUDIO_DSP_SET_COUNT];
  struct {
    bool on;
    float fc;
    float amount;
  } pbe;
} profile_v1_t;

typedef struct {
  uint16_t version;
  uint16_t size;
  profile_v1_t profile;
} stored_v1_t;

static SemaphoreHandle_t s_lock;
static audio_dsp_profile_t s_saved;
static audio_dsp_profile_t s_running; // == s_saved unless previewing
static bool s_previewing;
static uint32_t s_saved_at;

#define NVS_KEY_SAVED_AT "dsp_saved_at"

static bool is_flat(const audio_dsp_profile_t *p) {
  return p->count[AUDIO_DSP_SET_L] == 0 && p->count[AUDIO_DSP_SET_R] == 0 &&
         p->count[AUDIO_DSP_SET_M] == 0 && !p->bass.on;
}

uint32_t audio_dsp_calibrated_at(void) {
  if (!s_lock || is_flat(&s_saved)) {
    return 0;
  }
  return s_saved_at ? s_saved_at : 1;
}

bool audio_dsp_saved_bass(void) {
  return s_saved.bass.on;
}

void audio_dsp_profile_flat(audio_dsp_profile_t *p) {
  memset(p, 0, sizeof(*p));
  // Defaults for small drivers: the limit is set per speaker on the page.
  p->bass.f_low = 120.0f;
  p->bass.boost = 0.5f;
  p->bass.harmonics = 0.3f;
}

void audio_dsp_profile_clamp(audio_dsp_profile_t *p) {
  // Reuse the DSP's own rules: pack the sets into dsp_config_t pairs.
  dsp_config_t c;
  memset(&c, 0, sizeof(c));
  c.preamp_db = p->preamp_db;
  c.bass = p->bass;
  for (int s = 0; s < AUDIO_DSP_SET_COUNT; s++) {
    const int ch = s % DSP_NUM_CH;
    memcpy(c.filters[ch], p->sets[s], sizeof(c.filters[ch]));
    c.count[ch] = p->count[s];
    dsp_clamp_config(&c);
    memcpy(p->sets[s], c.filters[ch], sizeof(p->sets[s]));
    p->count[s] = c.count[ch];
  }
  p->preamp_db = c.preamp_db;
  p->bass = c.bass;
}

/* Build the per-output configuration for the current channel mode. */
static void resolve(const audio_dsp_profile_t *p, audio_channel_mode_t mode) {
  int set_l;
  int set_r;
  switch (mode) {
  case AUDIO_CHANNEL_MONO:
    set_l = set_r = AUDIO_DSP_SET_M;
    break;
  case AUDIO_CHANNEL_LEFT:
    set_l = set_r = AUDIO_DSP_SET_L;
    break;
  case AUDIO_CHANNEL_RIGHT:
    set_l = set_r = AUDIO_DSP_SET_R;
    break;
  default:
    set_l = AUDIO_DSP_SET_L;
    set_r = AUDIO_DSP_SET_R;
    break;
  }
  static dsp_config_t c; // ~500 B, kept off the caller's stack
  memset(&c, 0, sizeof(c));
  c.preamp_db = p->preamp_db;
  c.bass = p->bass;
  memcpy(c.filters[DSP_CH_L], p->sets[set_l], sizeof(c.filters[DSP_CH_L]));
  c.count[DSP_CH_L] = p->count[set_l];
  memcpy(c.filters[DSP_CH_R], p->sets[set_r], sizeof(c.filters[DSP_CH_R]));
  c.count[DSP_CH_R] = p->count[set_r];
  dsp_set_config(&c);
}

void audio_dsp_init(uint32_t sample_rate) {
  dsp_init(sample_rate);
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
  }
  audio_dsp_profile_flat(&s_saved);

  static union {
    stored_profile_t v2;
    stored_v1_t v1;
  } stored;
  size_t len = sizeof(stored);
  bool loaded = false;
  if (settings_get_dsp_blob(&stored, &len) == ESP_OK) {
    if (len == sizeof(stored.v2) && stored.v2.version == PROFILE_VERSION &&
        stored.v2.size == sizeof(stored.v2.profile)) {
      s_saved = stored.v2.profile;
      loaded = true;
    } else if (len == sizeof(stored.v1) && stored.v1.version == 1 &&
               stored.v1.size == sizeof(stored.v1.profile)) {
      const profile_v1_t *o = &stored.v1.profile;
      s_saved.preamp_db = o->preamp_db;
      memcpy(s_saved.sets, o->sets, sizeof(s_saved.sets));
      memcpy(s_saved.count, o->count, sizeof(s_saved.count));
      s_saved.bass.on = o->pbe.on;
      s_saved.bass.f_low = 2 * o->pbe.fc; // the old cutoff was the cut point
      s_saved.bass.harmonics = o->pbe.amount;
      ESP_LOGI(TAG, "Converted a version 1 DSP profile");
      loaded = true;
    }
  }
  if (loaded) {
    audio_dsp_profile_clamp(&s_saved);
    ESP_LOGI(TAG, "Loaded DSP profile: %u/%u/%u filters, preamp %.1f dB",
             s_saved.count[0], s_saved.count[1], s_saved.count[2],
             (double)s_saved.preamp_db);
  } else {
    ESP_LOGI(TAG, "No stored DSP profile, starting flat");
  }
  nvs_handle_t nvs;
  if (nvs_open("airplay", NVS_READONLY, &nvs) == ESP_OK) {
    nvs_get_u32(nvs, NVS_KEY_SAVED_AT, &s_saved_at);
    nvs_close(nvs);
  }
  s_running = s_saved;
  s_previewing = false;
  resolve(&s_running, audio_output_get_channel_mode());
}

void audio_dsp_get_saved(audio_dsp_profile_t *p) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *p = s_saved;
  xSemaphoreGive(s_lock);
}

bool audio_dsp_previewing(void) {
  return s_previewing;
}

esp_err_t audio_dsp_save(const audio_dsp_profile_t *p,
                         audio_channel_mode_t mode, uint32_t saved_at) {
  static stored_profile_t stored;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_saved = *p;
  audio_dsp_profile_clamp(&s_saved);
  s_running = s_saved;
  s_previewing = false;
  stored.version = PROFILE_VERSION;
  stored.size = sizeof(stored.profile);
  stored.profile = s_saved;
  esp_err_t err = settings_set_dsp_blob(&stored, sizeof(stored));
  s_saved_at = saved_at;
  nvs_handle_t nvs;
  if (nvs_open("airplay", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u32(nvs, NVS_KEY_SAVED_AT, s_saved_at);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  xSemaphoreGive(s_lock);

  // Outside the lock: this calls back into audio_dsp_on_channel_mode(),
  // which re-resolves even when the mode did not change.
  audio_output_set_channel_mode(mode);
  return err;
}

void audio_dsp_preview(const audio_dsp_profile_t *p,
                       audio_channel_mode_t mode) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_running = *p;
  audio_dsp_profile_clamp(&s_running);
  s_previewing = true;
  xSemaphoreGive(s_lock);
  audio_output_preview_channel_mode(mode);
}

void audio_dsp_revert(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_running = s_saved;
  s_previewing = false;
  xSemaphoreGive(s_lock);
  audio_output_restore_channel_mode();
}

void audio_dsp_on_channel_mode(audio_channel_mode_t mode) {
  if (!s_lock) {
    return; // audio_dsp_init() not run yet; it resolves on its own
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  resolve(&s_running, mode);
  xSemaphoreGive(s_lock);
}
