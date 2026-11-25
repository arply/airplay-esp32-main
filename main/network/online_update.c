/**
 * @file online_update.c
 * @brief Update from arply.de (docs/web_dsp/FLEET_PLAN.md, F4 "Update all").
 *
 * The website publishes ota/manifest.json:
 *   {"version": "0.2.1", "www": ["index.html", ...],
 *    "builds": {"arply_v1": "arply_v1.bin", "arply_v1_usbdac": "..."}}
 * and the files under ota/<version>/ (app images, www/<page>). An install
 * writes the app to the free OTA slot, then the pages as .new files, and
 * only when everything arrived swaps the pages and reboots; a failure
 * leaves the running firmware and pages as they were. A crash of the new
 * image in its first 60 s rolls back (main.c).
 */

#include "online_update.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "online_update";

#define OTA_DIR    "https://arply.de/assets/firmware/ota/"
#define WWW_DIR    "/spiffs/www/"
#define RECHECK_US (10LL * 60 * 1000000) // a GET re-checks at most this often
#define MAX_PAGES  8

#if CONFIG_AUDIO_INPUT_USB_DAC
#define BUILD "arply_v1_usbdac"
#else
#define BUILD "arply_v1"
#endif

typedef enum {
  ST_IDLE,
  ST_CHECKING,
  ST_FIRMWARE,
  ST_PAGES,
  ST_REBOOTING,
  ST_ERROR,
} state_t;

static const char *const STATE_NAMES[] = {"idle",  "checking",  "firmware",
                                          "pages", "rebooting", "error"};

static atomic_bool s_busy;
static volatile state_t s_state;
static volatile int s_progress; // percent of the current step
static char s_latest[16];       // from the last check, "" = unknown
static char s_error[80];
static int64_t s_checked_us;

static char s_manifest[2048];
static char s_buf[2048];

/* ---- helpers ---------------------------------------------------------- */

static bool newer(const char *a, const char *b) {
  int x[3] = {0};
  int y[3] = {0};
  sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
  sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; i++) {
    if (x[i] != y[i]) {
      return x[i] > y[i];
    }
  }
  return false;
}

/* Page names from the website land in SPIFFS: plain names only. */
static bool safe_page(const char *n) {
  const size_t len = strlen(n);
  if (len < 6 || len > 20 || strcmp(n + len - 5, ".html") != 0) {
    return false;
  }
  for (const char *c = n; *c; c++) {
    if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
          *c == '_' || *c == '.')) {
      return false;
    }
  }
  return strstr(n, "..") == NULL;
}

static void fail(const char *what) {
  ESP_LOGW(TAG, "%s", what);
  strlcpy(s_error, what, sizeof(s_error));
  s_state = ST_ERROR;
}

/* GET @p url into @p f, or into @p buf (NUL-terminated) when f is NULL. */
static bool fetch(const char *url, FILE *f, char *buf, size_t cap) {
  esp_http_client_config_t cfg = {.url = url,
                                  .timeout_ms = 15000,
                                  .crt_bundle_attach = esp_crt_bundle_attach};
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) {
    return false;
  }
  bool ok = esp_http_client_open(h, 0) == ESP_OK &&
            esp_http_client_fetch_headers(h) >= 0 &&
            esp_http_client_get_status_code(h) == 200;
  size_t got = 0;
  while (ok) {
    char *dst = f ? s_buf : buf + got;
    const size_t room = f ? sizeof(s_buf) : cap - 1 - got;
    if (room == 0) {
      ok = false; // larger than expected
      break;
    }
    const int n = esp_http_client_read(h, dst, (int)room);
    if (n < 0) {
      ok = false;
    } else if (n == 0) {
      break;
    } else if (f && fwrite(s_buf, 1, (size_t)n, f) != (size_t)n) {
      ok = false;
    } else {
      got += (size_t)n;
    }
  }
  if (ok && !f) {
    buf[got] = '\0';
  }
  ok = ok && esp_http_client_is_complete_data_received(h);
  esp_http_client_cleanup(h);
  return ok;
}

/* ---- the update ------------------------------------------------------- */

static bool install_firmware(const char *url) {
  s_state = ST_FIRMWARE;
  s_progress = 0;
  esp_http_client_config_t http = {.url = url,
                                   .timeout_ms = 15000,
                                   .keep_alive_enable = true,
                                   .crt_bundle_attach = esp_crt_bundle_attach};
  esp_https_ota_config_t cfg = {.http_config = &http};
  esp_https_ota_handle_t h = NULL;
  if (esp_https_ota_begin(&cfg, &h) != ESP_OK) {
    fail("Could not reach the firmware on arply.de");
    return false;
  }
  esp_err_t err;
  while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    const int size = esp_https_ota_get_image_size(h);
    if (size > 0) {
      s_progress = (int)(100LL * esp_https_ota_get_image_len_read(h) / size);
    }
  }
  if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
    esp_https_ota_abort(h);
    fail("Firmware download broke off");
    return false;
  }
  if (esp_https_ota_finish(h) != ESP_OK) { // also selects it for the boot
    fail("The downloaded firmware is not valid");
    return false;
  }
  return true;
}

static void drop_new_pages(const cJSON *www) {
  const cJSON *p;
  cJSON_ArrayForEach(p, www) {
    char tmp[48];
    snprintf(tmp, sizeof(tmp), WWW_DIR "%s.new", p->valuestring);
    unlink(tmp);
  }
}

static bool download_pages(const char *base, const cJSON *www) {
  s_state = ST_PAGES;
  s_progress = 0;
  const int n = cJSON_GetArraySize(www);
  int i = 0;
  const cJSON *p;
  cJSON_ArrayForEach(p, www) {
    char url[160];
    char tmp[48];
    snprintf(url, sizeof(url), "%swww/%s", base, p->valuestring);
    snprintf(tmp, sizeof(tmp), WWW_DIR "%s.new", p->valuestring);
    FILE *f = fopen(tmp, "w");
    const bool ok = f && fetch(url, f, NULL, 0);
    if (f) {
      fclose(f);
    }
    if (!ok) {
      fail("A web page did not download");
      return false;
    }
    s_progress = 100 * ++i / n;
  }
  return true;
}

static void swap_pages(const cJSON *www) {
  const cJSON *p;
  cJSON_ArrayForEach(p, www) {
    char tmp[48];
    char dst[48];
    snprintf(tmp, sizeof(tmp), WWW_DIR "%s.new", p->valuestring);
    snprintf(dst, sizeof(dst), WWW_DIR "%s", p->valuestring);
    unlink(dst); // SPIFFS does not rename over an existing file
    rename(tmp, dst);
  }
}

static void update_task(void *arg) {
  const bool install = arg != NULL;
  s_state = ST_CHECKING;
  s_error[0] = '\0';
  cJSON *m = NULL;
  if (!fetch(OTA_DIR "manifest.json", NULL, s_manifest, sizeof(s_manifest)) ||
      !(m = cJSON_Parse(s_manifest))) {
    fail("arply.de did not answer");
    goto done;
  }
  const cJSON *ver = cJSON_GetObjectItem(m, "version");
  const cJSON *www = cJSON_GetObjectItem(m, "www");
  const cJSON *app =
      cJSON_GetObjectItem(cJSON_GetObjectItem(m, "builds"), BUILD);
  bool pages_ok = cJSON_IsArray(www) && cJSON_GetArraySize(www) <= MAX_PAGES;
  const cJSON *p;
  cJSON_ArrayForEach(p, www) {
    pages_ok = pages_ok && cJSON_IsString(p) && safe_page(p->valuestring);
  }
  if (!cJSON_IsString(ver) || strlen(ver->valuestring) >= sizeof(s_latest) ||
      !cJSON_IsString(app) || strchr(app->valuestring, '/') || !pages_ok) {
    fail("The update information on arply.de is not readable");
    goto done;
  }
  strlcpy(s_latest, ver->valuestring, sizeof(s_latest));
  s_checked_us = esp_timer_get_time();
  const char *current = esp_app_get_description()->version;
  ESP_LOGI(TAG, "arply.de has %s, this is %s (%s)", s_latest, current, BUILD);
  if (!install) {
    s_state = ST_IDLE;
    goto done;
  }
  if (!newer(s_latest, current)) {
    fail("Already up to date");
    goto done;
  }
  char base[96];
  char url[160];
  snprintf(base, sizeof(base), OTA_DIR "%s/", s_latest);
  snprintf(url, sizeof(url), "%s%s", base, app->valuestring);
  ESP_LOGI(TAG, "Installing %s", url);
  if (!install_firmware(url)) {
    goto done;
  }
  if (!download_pages(base, www)) {
    drop_new_pages(www);
    // keep booting what runs now: the new app without its pages is worse
    esp_ota_set_boot_partition(esp_ota_get_running_partition());
    goto done;
  }
  swap_pages(www);
  s_state = ST_REBOOTING;
  ESP_LOGI(TAG, "Update to %s complete, rebooting", s_latest);
  vTaskDelay(pdMS_TO_TICKS(1000)); // let a status poll see it
  esp_restart();
done:
  cJSON_Delete(m);
  atomic_store(&s_busy, false);
  vTaskDelete(NULL);
}

static bool start(bool install) {
  bool expected = false;
  if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
    return false;
  }
  // TLS and the image download need more than the HTTP server's stack.
  if (xTaskCreate(update_task, "online_upd", 8192, install ? (void *)1 : NULL,
                  3, NULL) != pdPASS) {
    atomic_store(&s_busy, false);
    return false;
  }
  return true;
}

/* ---- API ---------------------------------------------------------------- */

static esp_err_t send_state(httpd_req_t *req) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "current", esp_app_get_description()->version);
  cJSON_AddStringToObject(j, "build", BUILD);
  if (s_latest[0]) {
    cJSON_AddStringToObject(j, "latest", s_latest);
  } else {
    cJSON_AddNullToObject(j, "latest");
  }
  cJSON_AddStringToObject(j, "state", STATE_NAMES[s_state]);
  cJSON_AddNumberToObject(j, "progress", s_progress);
  if (s_error[0]) {
    cJSON_AddStringToObject(j, "error", s_error);
  }
  char *s = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  httpd_resp_set_type(req, "application/json");
  esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
  free(s);
  return err;
}

/* GET /api/update: state, and the latest version from the last check. A
 * check older than 10 minutes starts a new one in the background. */
static esp_err_t get_handler(httpd_req_t *req) {
  if (!atomic_load(&s_busy) &&
      (s_checked_us == 0 || esp_timer_get_time() - s_checked_us > RECHECK_US)) {
    s_checked_us = esp_timer_get_time(); // also rate-limits failed checks
    start(false);
  }
  return send_state(req);
}

/* POST /api/update {"install": true}: download and install, then reboot. */
static esp_err_t post_handler(httpd_req_t *req) {
  char body[64] = {0};
  const int n = httpd_req_recv(req, body, sizeof(body) - 1);
  cJSON *j = n > 0 ? cJSON_Parse(body) : NULL;
  const bool install = cJSON_IsTrue(cJSON_GetObjectItem(j, "install"));
  cJSON_Delete(j);
  if (!install) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Expected {\"install\": true}");
  }
  if (audio_receiver_is_playing() && audio_receiver_has_data()) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"playing\"}");
  }
  if (!start(true)) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"busy\"}");
  }
  s_state = ST_CHECKING;
  return send_state(req);
}

int online_update_register(httpd_handle_t server) {
  const httpd_uri_t uris[] = {
      {.uri = "/api/update", .method = HTTP_GET, .handler = get_handler},
      {.uri = "/api/update", .method = HTTP_POST, .handler = post_handler},
  };
  int n = 0;
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    n += httpd_register_uri_handler(server, &uris[i]) == ESP_OK;
  }
  return n;
}
