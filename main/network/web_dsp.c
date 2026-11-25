/**
 * @file web_dsp.c
 * @brief /api/dsp: read, preview and save the output DSP profile.
 *
 * Registered on both the HTTP and the HTTPS server, so the calibration page
 * works from either origin with relative URLs. JSON format: v2 as exported
 * by docs/web_dsp/calibration-prototype.html; per-filter "b"/"a"
 * coefficient arrays are accepted and ignored (the device computes its own
 * for its actual output rate).
 */

#include "web_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dsp.h"
#include "audio_output.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "test_signal.h"
#include "web_server.h"

static const char *TAG = "web_dsp";

#define DSP_BODY_MAX       8192
#define PREVIEW_MIN_GAP_US 50000
#define NOISE_MAX_MS       30000
#define GAP_MS_MIN         500
#define GAP_MS_MAX         5000
#define GAP_MS_DEFAULT     2000
// A listening sample: ~17 s of 44.1 kHz stereo s16, held in PSRAM.
#define LISTEN_MAX_BYTES (3 * 1024 * 1024)

static const char *const SET_KEYS[AUDIO_DSP_SET_COUNT] = {"L", "R", "M"};
static const char *const TYPE_NAMES[DSP_FILTER_TYPE_COUNT] = {
    [DSP_FILTER_PEAK] = "peak",     [DSP_FILTER_LOW_SHELF] = "ls",
    [DSP_FILTER_HIGH_SHELF] = "hs", [DSP_FILTER_HIGH_PASS] = "hp",
    [DSP_FILTER_LOW_PASS] = "lp",
};
static const char *const MODE_NAMES[] = {
    [AUDIO_CHANNEL_STEREO] = "stereo",
    [AUDIO_CHANNEL_LEFT] = "left",
    [AUDIO_CHANNEL_RIGHT] = "right",
    [AUDIO_CHANNEL_MONO] = "mono",
};

static bool s_bypass;

// The listening sample being played. Two server tasks may upload at once.
static int16_t *s_listen_buf;
static SemaphoreHandle_t s_listen_lock;

/* ---- JSON helpers ---------------------------------------------------- */

static double round_to(double v, double step) {
  return round(v / step) * step;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!str) {
    return httpd_resp_send_500(req);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, str);
  free(str);
  return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "error", msg);
  httpd_resp_set_status(req, status);
  return send_json(req, json);
}

static cJSON *profile_to_json(const audio_dsp_profile_t *p,
                              audio_channel_mode_t mode) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "version", 2);
  cJSON_AddStringToObject(json, "mode", MODE_NAMES[mode]);
  cJSON_AddNumberToObject(json, "preamp_db", round_to(p->preamp_db, 0.01));
  cJSON *channels = cJSON_AddObjectToObject(json, "channels");
  for (int s = 0; s < AUDIO_DSP_SET_COUNT; s++) {
    cJSON *arr = cJSON_AddArrayToObject(channels, SET_KEYS[s]);
    for (int i = 0; i < p->count[s]; i++) {
      const dsp_filter_t *f = &p->sets[s][i];
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", TYPE_NAMES[f->type]);
      cJSON_AddNumberToObject(o, "f", round_to(f->f, 0.1));
      cJSON_AddNumberToObject(o, "gain_db", round_to(f->gain_db, 0.01));
      cJSON_AddNumberToObject(o, "q", round_to(f->q, 0.001));
      cJSON_AddBoolToObject(o, "enabled", f->enabled);
      cJSON_AddItemToArray(arr, o);
    }
  }
  cJSON *bass = cJSON_AddObjectToObject(json, "bass");
  cJSON_AddBoolToObject(bass, "on", p->bass.on);
  cJSON_AddNumberToObject(bass, "f_low", round_to(p->bass.f_low, 0.1));
  cJSON_AddNumberToObject(bass, "boost", round_to(p->bass.boost, 0.001));
  cJSON_AddNumberToObject(bass, "harmonics",
                          round_to(p->bass.harmonics, 0.001));
  cJSON_AddNumberToObject(json, "fs", CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  cJSON_AddNumberToObject(json, "max_filters", DSP_MAX_FILTERS_PER_CH);
  cJSON_AddBoolToObject(json, "bypass", s_bypass);
  cJSON_AddBoolToObject(json, "previewing", audio_dsp_previewing());
  return json;
}

static bool read_number(const cJSON *obj, const char *key, float *out,
                        bool required) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!v) {
    return !required;
  }
  if (!cJSON_IsNumber(v)) {
    return false;
  }
  *out = (float)v->valuedouble;
  return true;
}

static bool parse_filter(const cJSON *o, dsp_filter_t *f, const char **err) {
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "type");
  if (!cJSON_IsString(type)) {
    *err = "filter.type missing";
    return false;
  }
  int t = -1;
  for (int i = 0; i < DSP_FILTER_TYPE_COUNT; i++) {
    if (strcmp(type->valuestring, TYPE_NAMES[i]) == 0) {
      t = i;
    }
  }
  if (t < 0) {
    *err = "filter.type must be peak, ls, hs, hp or lp";
    return false;
  }
  f->type = (dsp_filter_type_t)t;
  f->gain_db = 0;
  f->q = (float)M_SQRT1_2;
  if (!read_number(o, "f", &f->f, true) ||
      !read_number(o, "gain_db", &f->gain_db, false) ||
      !read_number(o, "q", &f->q, false)) {
    *err = "filter.f, gain_db and q must be numbers";
    return false;
  }
  const cJSON *en = cJSON_GetObjectItemCaseSensitive(o, "enabled");
  f->enabled = en ? cJSON_IsTrue(en) : true;
  return true;
}

/* Parse a request into @p p. Keys that are absent keep the value already in
 * @p p, so a page that only sends the sets of its current mode does not wipe
 * the others. Returns NULL on success, else a message for the client. */
static const char *parse_profile(const cJSON *json, audio_dsp_profile_t *p,
                                 audio_channel_mode_t *mode) {
  const char *err = NULL;
  const cJSON *m = cJSON_GetObjectItemCaseSensitive(json, "mode");
  if (m) {
    int found = -1;
    for (int i = 0; i <= AUDIO_CHANNEL_MONO; i++) {
      if (cJSON_IsString(m) && strcmp(m->valuestring, MODE_NAMES[i]) == 0) {
        found = i;
      }
    }
    if (found < 0) {
      return "mode must be stereo, mono, left or right";
    }
    *mode = (audio_channel_mode_t)found;
  }
  if (!read_number(json, "preamp_db", &p->preamp_db, false)) {
    return "preamp_db must be a number";
  }
  const cJSON *channels = cJSON_GetObjectItemCaseSensitive(json, "channels");
  if (channels && !cJSON_IsObject(channels)) {
    return "channels must be an object";
  }
  for (int s = 0; channels && s < AUDIO_DSP_SET_COUNT; s++) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(channels, SET_KEYS[s]);
    if (!arr) {
      continue;
    }
    if (!cJSON_IsArray(arr)) {
      return "channels.L/R/M must be arrays";
    }
    if (cJSON_GetArraySize(arr) > DSP_MAX_FILTERS_PER_CH) {
      return "too many filters (max 10 per set)";
    }
    uint8_t n = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
      if (!cJSON_IsObject(o) || !parse_filter(o, &p->sets[s][n], &err)) {
        return err ? err : "filters must be objects";
      }
      n++;
    }
    p->count[s] = n;
  }
  const cJSON *bass = cJSON_GetObjectItemCaseSensitive(json, "bass");
  if (bass) {
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(bass, "on");
    if (on) {
      p->bass.on = cJSON_IsTrue(on);
    }
    if (!read_number(bass, "f_low", &p->bass.f_low, false) ||
        !read_number(bass, "boost", &p->bass.boost, false) ||
        !read_number(bass, "harmonics", &p->bass.harmonics, false)) {
      return "bass.f_low, bass.boost and bass.harmonics must be numbers";
    }
  }
  // Older exports (prototype, firmware before 2026-11-25): only "on" is
  // taken over.
  const cJSON *pbe = cJSON_GetObjectItemCaseSensitive(json, "pbe");
  if (pbe && !bass) {
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(pbe, "on");
    if (on) {
      p->bass.on = cJSON_IsTrue(on);
    }
  }
  return NULL;
}

/* Read and parse a request body on top of the saved profile. */
static esp_err_t read_request(httpd_req_t *req, audio_dsp_profile_t *p,
                              audio_channel_mode_t *mode, uint32_t *saved_at) {
  char *body = web_server_recv_body(req, DSP_BODY_MAX);
  if (!body) {
    send_error(req, "400 Bad Request", "body missing or larger than 8 KB");
    return ESP_FAIL;
  }
  cJSON *json = cJSON_Parse(body);
  free(body);
  if (!json) {
    send_error(req, "400 Bad Request", "invalid JSON");
    return ESP_FAIL;
  }
  audio_dsp_get_saved(p);
  *mode = audio_output_get_channel_mode();
  const char *err = parse_profile(json, p, mode);
  const cJSON *at = cJSON_GetObjectItemCaseSensitive(json, "saved_at");
  if (saved_at && cJSON_IsNumber(at) && at->valuedouble > 0) {
    *saved_at = (uint32_t)at->valuedouble;
  }
  cJSON_Delete(json);
  if (err) {
    send_error(req, "400 Bad Request", err);
    return ESP_FAIL;
  }
  return ESP_OK;
}

/* ---- Handlers -------------------------------------------------------- */

static esp_err_t dsp_page_handler(httpd_req_t *req) {
  if (web_server_redirect_to_https(req)) {
    return ESP_OK; // the microphone only works on HTTPS
  }
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return web_server_send_file(req, "/spiffs/www/dsp.html", "text/html");
}

static esp_err_t dsp_get_handler(httpd_req_t *req) {
  audio_dsp_profile_t p; // ~600 B; httpd stacks are 8 KB+
  audio_dsp_get_saved(&p);
  return send_json(req, profile_to_json(&p, audio_output_get_channel_mode()));
}

static esp_err_t dsp_post_handler(httpd_req_t *req) {
  audio_dsp_profile_t p;
  audio_channel_mode_t mode;
  uint32_t saved_at = 0;
  if (read_request(req, &p, &mode, &saved_at) != ESP_OK) {
    return ESP_OK; // error already sent
  }
  esp_err_t err = audio_dsp_save(&p, mode, saved_at);
  if (err != ESP_OK) {
    return send_error(req, "500 Internal Server Error", "saving failed");
  }
  ESP_LOGI(TAG, "Saved: %u/%u/%u filters, preamp %.1f dB, bass %d", p.count[0],
           p.count[1], p.count[2], (double)p.preamp_db, p.bass.on);
  audio_dsp_get_saved(&p);
  return send_json(req, profile_to_json(&p, audio_output_get_channel_mode()));
}

static esp_err_t dsp_preview_handler(httpd_req_t *req) {
  static int64_t s_last_us;
  audio_dsp_profile_t p;
  int64_t now = esp_timer_get_time();
  audio_channel_mode_t mode;
  if (read_request(req, &p, &mode, NULL) != ESP_OK) {
    return ESP_OK;
  }
  cJSON *json = cJSON_CreateObject();
  // Dragging a slider fires faster than anyone can hear; drop the excess.
  bool apply = now - s_last_us >= PREVIEW_MIN_GAP_US;
  if (apply) {
    s_last_us = now;
    audio_dsp_preview(&p, mode);
  }
  cJSON_AddBoolToObject(json, "applied", apply);
  return send_json(req, json);
}

static esp_err_t dsp_revert_handler(httpd_req_t *req) {
  audio_dsp_profile_t p;
  audio_dsp_revert();
  audio_dsp_get_saved(&p);
  return send_json(req, profile_to_json(&p, audio_output_get_channel_mode()));
}

static esp_err_t dsp_bypass_handler(httpd_req_t *req) {
  char *body = web_server_recv_body(req, 64);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  const cJSON *on = json ? cJSON_GetObjectItemCaseSensitive(json, "on") : NULL;
  if (!cJSON_IsBool(on)) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "expected {\"on\": bool}");
  }
  s_bypass = cJSON_IsTrue(on);
  cJSON_Delete(json);
  dsp_set_bypass(s_bypass);
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "bypass", s_bypass);
  return send_json(req, resp);
}

/* ---- Test signals ---------------------------------------------------- */

/* "L", "R" or "LR" -> output mask; 0 if unknown. */
static uint8_t outs_from_name(const char *s) {
  if (strcmp(s, "L") == 0) {
    return TEST_SIGNAL_OUT_L;
  }
  if (strcmp(s, "R") == 0) {
    return TEST_SIGNAL_OUT_R;
  }
  if (strcmp(s, "LR") == 0) {
    return TEST_SIGNAL_OUT_BOTH;
  }
  return 0;
}

/* Parse {"channels": [...]} into one output mask per entry. */
static int parse_channels(const cJSON *json, uint8_t *outs, int max) {
  const cJSON *arr = cJSON_GetObjectItemCaseSensitive(json, "channels");
  if (!arr) {
    outs[0] = TEST_SIGNAL_OUT_BOTH;
    return 1;
  }
  if (!cJSON_IsArray(arr)) {
    return -1;
  }
  int n = 0;
  const cJSON *v;
  cJSON_ArrayForEach(v, arr) {
    if (n >= max || !cJSON_IsString(v)) {
      return -1;
    }
    outs[n] = outs_from_name(v->valuestring);
    if (!outs[n]) {
      return -1;
    }
    n++;
  }
  return n;
}

static cJSON *read_json(httpd_req_t *req) {
  if (req->content_len == 0) {
    return cJSON_CreateObject();
  }
  char *body = web_server_recv_body(req, 512);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  return json;
}

static esp_err_t busy_error(httpd_req_t *req) {
  return send_error(req, "409 Conflict",
                    "output busy: a stream is playing or a signal is running");
}

static esp_err_t sweep_handler(httpd_req_t *req) {
  cJSON *json = read_json(req);
  if (!json) {
    return send_error(req, "400 Bad Request", "invalid JSON");
  }
  uint8_t outs[TEST_SIGNAL_MAX_SWEEPS];
  int n = parse_channels(json, outs, TEST_SIGNAL_MAX_SWEEPS);
  float gap = GAP_MS_DEFAULT;
  bool gap_ok = read_number(json, "gap_ms", &gap, false);
  cJSON_Delete(json);
  if (n < 1 || !gap_ok) {
    return send_error(req, "400 Bad Request",
                      "expected {\"channels\": [\"L\"|\"R\"|\"LR\", ...] "
                      "(1-4), \"gap_ms\": number}");
  }
  const uint32_t gap_ms = (uint32_t)fminf(fmaxf(gap, GAP_MS_MIN), GAP_MS_MAX);
  if (test_signal_sweep(outs, n, gap_ms) != ESP_OK) {
    return busy_error(req);
  }
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddNumberToObject(resp, "lead_ms", TEST_SIGNAL_LEAD_MS);
  cJSON_AddNumberToObject(resp, "sweep_ms", TEST_SIGNAL_SWEEP_MS);
  cJSON_AddNumberToObject(resp, "gap_ms", gap_ms);
  cJSON_AddNumberToObject(resp, "count", n);
  cJSON_AddNumberToObject(resp, "fs", CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  return send_json(req, resp);
}

static esp_err_t noise_handler(httpd_req_t *req) {
  cJSON *json = read_json(req);
  if (!json) {
    return send_error(req, "400 Bad Request", "invalid JSON");
  }
  const cJSON *on = cJSON_GetObjectItemCaseSensitive(json, "on");
  uint8_t list[4];
  int n = parse_channels(json, list, 4);
  cJSON_Delete(json);
  if (n < 1) {
    return send_error(req, "400 Bad Request",
                      "channels must be a list of \"L\", \"R\", \"LR\"");
  }
  cJSON *resp = cJSON_CreateObject();
  if (on && !cJSON_IsTrue(on)) {
    test_signal_stop();
    cJSON_AddBoolToObject(resp, "on", false);
    return send_json(req, resp);
  }
  uint8_t outs = 0;
  for (int i = 0; i < n; i++) {
    outs |= list[i];
  }
  if (test_signal_noise(outs, NOISE_MAX_MS) != ESP_OK) {
    cJSON_Delete(resp);
    return busy_error(req);
  }
  cJSON_AddBoolToObject(resp, "on", true);
  cJSON_AddNumberToObject(resp, "max_ms", NOISE_MAX_MS);
  return send_json(req, resp);
}

/* Free the listening sample once nothing plays it any more. */
static void release_listen_buffer(void) {
  if (test_signal_wait_idle(500)) {
    heap_caps_free(s_listen_buf);
    s_listen_buf = NULL;
  }
}

/* POST raw s16le PCM at the output rate (?channels=1|2, default 2): arply
 * loops it through its own DSP and volume until /api/dsp/stop. */
static esp_err_t listen_handler(httpd_req_t *req) {
  int channels = 2;
  char query[32];
  char val[4];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "channels", val, sizeof(val)) == ESP_OK) {
    channels = atoi(val);
  }
  const size_t frame_bytes = (size_t)channels * sizeof(int16_t);
  const size_t len = req->content_len;
  if ((channels != 1 && channels != 2) || len == 0 || len > LISTEN_MAX_BYTES ||
      len % frame_bytes != 0) {
    return send_error(req, "400 Bad Request",
                      "expected s16le PCM, whole frames, at most 3 MB");
  }

  xSemaphoreTake(s_listen_lock, portMAX_DELAY);
  test_signal_stop();
  release_listen_buffer();
  if (s_listen_buf) {
    xSemaphoreGive(s_listen_lock);
    return busy_error(req);
  }
  s_listen_buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!s_listen_buf) {
    xSemaphoreGive(s_listen_lock);
    return send_error(req, "500 Internal Server Error", "out of memory");
  }
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, (char *)s_listen_buf + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (r <= 0) {
      heap_caps_free(s_listen_buf);
      s_listen_buf = NULL;
      xSemaphoreGive(s_listen_lock);
      return ESP_FAIL;
    }
    got += (size_t)r;
  }
  esp_err_t err = test_signal_listen(s_listen_buf, len / frame_bytes, channels);
  xSemaphoreGive(s_listen_lock);
  if (err != ESP_OK) {
    return busy_error(req);
  }
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "playing", true);
  cJSON_AddNumberToObject(resp, "seconds",
                          (double)(len / frame_bytes) /
                              CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  return send_json(req, resp);
}

static esp_err_t stop_handler(httpd_req_t *req) {
  test_signal_stop();
  xSemaphoreTake(s_listen_lock, portMAX_DELAY);
  release_listen_buffer();
  xSemaphoreGive(s_listen_lock);
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "stopped", true);
  return send_json(req, resp);
}

/* ---- Debug probe ------------------------------------------------------ */

/* POST /api/debug/capture?seconds=N (1-10): record the output path. */
static esp_err_t capture_start_handler(httpd_req_t *req) {
  int seconds = 10;
  char query[32];
  char val[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "seconds", val, sizeof(val)) == ESP_OK) {
    seconds = atoi(val);
  }
  if (seconds < 1 || seconds > 10) {
    return send_error(req, "400 Bad Request", "seconds must be 1-10");
  }
  esp_err_t err = audio_output_capture_start((uint32_t)seconds);
  if (err != ESP_OK) {
    return send_error(req, "409 Conflict", esp_err_to_name(err));
  }
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddNumberToObject(resp, "seconds", seconds);
  cJSON_AddNumberToObject(resp, "fs", CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  return send_json(req, resp);
}

/* GET /api/debug/capture: the finished capture as a 4-channel WAV
 * (in L, in R, out L, out R). */
static esp_err_t capture_get_handler(httpd_req_t *req) {
  size_t frames = 0;
  const int16_t *pcm = audio_output_capture_get(&frames);
  if (!pcm) {
    return send_error(req, "409 Conflict", "no finished capture");
  }
  const uint32_t rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ;
  const uint32_t data = (uint32_t)(frames * 8);
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  const uint32_t riff = 36 + data;
  memcpy(h + 4, &riff, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  const uint32_t fmt_len = 16;
  const uint16_t fmt = 1;
  const uint16_t channels = 4;
  const uint32_t byte_rate = rate * 8;
  const uint16_t align = 8;
  const uint16_t bits = 16;
  memcpy(h + 16, &fmt_len, 4);
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &rate, 4);
  memcpy(h + 28, &byte_rate, 4);
  memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &data, 4);
  httpd_resp_set_type(req, "audio/wav");
  if (httpd_resp_send_chunk(req, (const char *)h, sizeof(h)) != ESP_OK) {
    return ESP_FAIL;
  }
  const char *p = (const char *)pcm;
  for (size_t off = 0; off < data; off += 4096) {
    const size_t n = data - off < 4096 ? data - off : 4096;
    if (httpd_resp_send_chunk(req, p + off, (ssize_t)n) != ESP_OK) {
      return ESP_FAIL;
    }
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

int web_dsp_register(httpd_handle_t server) {
  if (!s_listen_lock) {
    s_listen_lock = xSemaphoreCreateMutex();
  }
  const httpd_uri_t uris[] = {
      {.uri = "/dsp", .method = HTTP_GET, .handler = dsp_page_handler},
      {.uri = "/api/dsp", .method = HTTP_GET, .handler = dsp_get_handler},
      {.uri = "/api/dsp", .method = HTTP_POST, .handler = dsp_post_handler},
      {.uri = "/api/dsp/preview",
       .method = HTTP_POST,
       .handler = dsp_preview_handler},
      {.uri = "/api/dsp/revert",
       .method = HTTP_POST,
       .handler = dsp_revert_handler},
      {.uri = "/api/dsp/bypass",
       .method = HTTP_POST,
       .handler = dsp_bypass_handler},
      {.uri = "/api/dsp/sweep", .method = HTTP_POST, .handler = sweep_handler},
      {.uri = "/api/dsp/noise", .method = HTTP_POST, .handler = noise_handler},
      {.uri = "/api/dsp/stop", .method = HTTP_POST, .handler = stop_handler},
      {.uri = "/api/dsp/listen",
       .method = HTTP_POST,
       .handler = listen_handler},
      {.uri = "/api/debug/capture",
       .method = HTTP_POST,
       .handler = capture_start_handler},
      {.uri = "/api/debug/capture",
       .method = HTTP_GET,
       .handler = capture_get_handler},
  };
  int n = 0;
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    if (httpd_register_uri_handler(server, &uris[i]) == ESP_OK) {
      n++;
    } else {
      ESP_LOGE(TAG, "Failed to register %s", uris[i].uri);
    }
  }
  return n;
}
