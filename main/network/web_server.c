#include "web_dsp.h"
#include "web_server.h"

#include "esp_netif.h"
#include "lwip/sockets.h"
#include "fleet.h"
#include "tls_cert.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_wifi.h"
#include "nvs_flash.h"

#include "settings.h"
#include "led.h"
#include "wifi.h"
#include "ota.h"
#include "log_stream.h"
#include "rtsp_server.h"
#include "audio_output.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define SPIFFS_CHUNK_SIZE 1024

esp_err_t web_server_send_file(httpd_req_t *req, const char *path,
                               const char *content_type) {
  FILE *f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s", path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, content_type);
  char buf[SPIFFS_CHUNK_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// API handlers
/* True if @p req reached us through the setup hotspot (captive portal):
 * those clients stay on plain HTTP. */
static bool via_setup_ap(httpd_req_t *req) {
  struct sockaddr_in local;
  socklen_t len = sizeof(local);
  esp_netif_ip_info_t ap;
  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) !=
          0 ||
      local.sin_family != AF_INET || !ap_netif ||
      esp_netif_get_ip_info(ap_netif, &ap) != ESP_OK) {
    return false;
  }
  return local.sin_addr.s_addr == ap.ip.addr;
}

typedef struct {
  esp_err_t (*handler)(httpd_req_t *req);
  void *user_ctx;
} lan_only_t;

static esp_err_t lan_only_handler(httpd_req_t *req) {
  const lan_only_t *h = req->user_ctx;
  if (via_setup_ap(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Wi-Fi setup only");
    return ESP_FAIL;
  }
  req->user_ctx = h->user_ctx;
  return h->handler(req);
}

esp_err_t web_server_register_lan_only(httpd_handle_t server,
                                       const httpd_uri_t *uri) {
  lan_only_t *h = malloc(sizeof(*h)); // lives as long as the server
  if (!h) {
    return ESP_ERR_NO_MEM;
  }
  h->handler = uri->handler;
  h->user_ctx = uri->user_ctx;
  httpd_uri_t wrapped = *uri;
  wrapped.handler = lan_only_handler;
  wrapped.user_ctx = h;
  esp_err_t err = httpd_register_uri_handler(server, &wrapped);
  if (err != ESP_OK) {
    free(h);
  }
  return err;
}

bool web_server_redirect_to_https(httpd_req_t *req) {
  if (req->handle != s_server || via_setup_ap(req)) {
    return false; // already HTTPS, or a captive-portal client
  }
  char host[64];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return false;
  }
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
  }
  char location[sizeof(host) + 8 + CONFIG_HTTPD_MAX_URI_LEN];
  snprintf(location, sizeof(location), "https://%s%s", host, req->uri);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_send(req, NULL, 0);
  return true;
}

/* arply.local is the fleet overview. Every device knows the whole fleet, so
 * every device serves it: a phone whose mDNS cache still points at a former
 * hub gets the same page. */
static bool asked_for_overview(httpd_req_t *req) {
  char host[64];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return false;
  }
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
  }
  return strcasecmp(host, SETTINGS_HUB_HOST_NAME ".local") == 0;
}

static esp_err_t root_handler(httpd_req_t *req) {
  // The captive portal shows only the Wi-Fi setup (see index.html).
  if (via_setup_ap(req) && !strstr(req->uri, "?setup")) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/?setup");
    return httpd_resp_send(req, NULL, 0);
  }
  if (web_server_redirect_to_https(req)) {
    return ESP_OK;
  }
  if (asked_for_overview(req)) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return web_server_send_file(req, "/spiffs/www/overview.html", "text/html");
  }
  return web_server_send_file(req, "/spiffs/www/index.html", "text/html");
}

/* The device certificate, for installing it as trusted on a phone (iOS
 * offers a profile install for this content type): no more warnings. */
static esp_err_t cert_handler(httpd_req_t *req) {
  char name[65];
  settings_get_device_name(name, sizeof(name));
  const char *cert;
  const char *key;
  if (tls_cert_get(name, &cert, &key) != ESP_OK) {
    return httpd_resp_send_500(req);
  }
  httpd_resp_set_type(req, "application/x-x509-ca-cert");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=\"arply.crt\"");
  return httpd_resp_sendstr(req, cert);
}

static esp_err_t volume_get_handler(httpd_req_t *req) {
  float db = AUDIO_VOLUME_DEFAULT_DB;
  settings_get_volume(&db);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "db", db);
  cJSON_AddNumberToObject(json, "min", AUDIO_VOLUME_MIN_DB);
  cJSON_AddNumberToObject(json, "max", AUDIO_VOLUME_MAX_DB);
  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, str);
  free(str);
  return ESP_OK;
}

/* The one shared volume (AirPlay, USB and arply's own signals). {"db": x}
 * applies at once; add "persist": true on release, not on every move. */
static esp_err_t volume_post_handler(httpd_req_t *req) {
  char *body = web_server_recv_body(req, 128);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  const cJSON *db = json ? cJSON_GetObjectItem(json, "db") : NULL;
  if (!cJSON_IsNumber(db)) {
    cJSON_Delete(json);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Expected {\"db\": number}");
  }
  float v = (float)db->valuedouble;
  v = v < AUDIO_VOLUME_MIN_DB ? AUDIO_VOLUME_MIN_DB : v;
  v = v > AUDIO_VOLUME_MAX_DB ? AUDIO_VOLUME_MAX_DB : v;
  settings_set_volume(v);
  if (cJSON_IsTrue(cJSON_GetObjectItem(json, "persist"))) {
    settings_persist_volume();
  }
  cJSON_Delete(json);
  return volume_get_handler(req);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t logs_page_handler(httpd_req_t *req) {
  return web_server_send_file(req, "/spiffs/www/logs.html", "text/html");
}

static esp_err_t speedtest_page_handler(httpd_req_t *req) {
  return web_server_send_file(req, "/spiffs/www/speedtest.html", "text/html");
}

// Tiny endpoint used by JS for RTT timing. Returns minimal body.
static esp_err_t speedtest_ping_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, "ok", 2);
  return ESP_OK;
}

// Streams `bytes` octets of filler data so the browser can measure DL speed.
// Capped to avoid pathological requests starving audio.
#define SPEEDTEST_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define SPEEDTEST_CHUNK     2048

static esp_err_t speedtest_download_handler(httpd_req_t *req) {
  size_t bytes = (size_t)1024 * 1024;
  char qbuf[64];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(qbuf, "bytes", val, sizeof(val)) == ESP_OK) {
      long v = strtol(val, NULL, 10);
      if (v > 0) {
        bytes = (size_t)v;
      }
    }
  }
  if (bytes > SPEEDTEST_MAX_BYTES) {
    bytes = SPEEDTEST_MAX_BYTES;
  }

  // Reuse a single buffer of filler bytes. Static so we don't repeatedly
  // hammer the heap; content is irrelevant but non-zero to thwart any
  // compression along the way.
  static uint8_t filler[SPEEDTEST_CHUNK];
  static bool filler_init = false;
  if (!filler_init) {
    for (size_t i = 0; i < sizeof(filler); i++) {
      filler[i] = (uint8_t)(i * 37);
    }
    filler_init = true;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  size_t remaining = bytes;
  while (remaining > 0) {
    ssize_t n =
        remaining < SPEEDTEST_CHUNK ? (ssize_t)remaining : SPEEDTEST_CHUNK;
    if (httpd_resp_send_chunk(req, (const char *)filler, n) != ESP_OK) {
      return ESP_FAIL;
    }
    remaining -= (size_t)n;
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// Consumes a POST body and reports how many bytes were received.
static esp_err_t speedtest_upload_handler(httpd_req_t *req) {
  size_t total = req->content_len;
  size_t got = 0;
  uint8_t buf[SPEEDTEST_CHUNK];
  while (got < total) {
    size_t want = total - got;
    if (want > sizeof(buf)) {
      want = sizeof(buf);
    }
    int r = httpd_req_recv(req, (char *)buf, want);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    got += (size_t)r;
  }
  char reply[64];
  int n = snprintf(reply, sizeof(reply), "received=%u", (unsigned)got);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, reply, n);
  return ESP_OK;
}

// Captive portal detection handlers
// These endpoints are requested by various OS to detect captive portals
static esp_err_t captive_portal_redirect(httpd_req_t *req) {
  // Redirect to the configuration page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/?setup");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// Apple devices (iOS/macOS) check these
static esp_err_t captive_apple_handler(httpd_req_t *req) {
  // Apple expects specific response, redirect instead
  return captive_portal_redirect(req);
}

// Android checks this
static esp_err_t captive_android_handler(httpd_req_t *req) {
  // Android expects 204 for no captive portal, anything else triggers portal
  return captive_portal_redirect(req);
}

// Windows checks this
static esp_err_t captive_windows_handler(httpd_req_t *req) {
  return captive_portal_redirect(req);
}

// Catch-all for any URI not matched by a more specific handler above. Windows
// NCSI, once it decides a captive portal is present via /connecttest.txt,
// opens its sign-in browser against http://www.msftconnecttest.com/redirect
// — a path we'd otherwise 404 on, which is why that browser was landing on
// an error page instead of the setup UI. Registered last so it only catches
// requests nothing else claimed; only redirects while the AP is actually up,
// so a stray 404 in normal (STA-only) operation still behaves like a 404.
static esp_err_t captive_catchall_handler(httpd_req_t *req) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK &&
      (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
    return captive_portal_redirect(req);
  }
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
  return ESP_OK;
}

/* Index of @p ssid among the saved networks, or -1. */
static int saved_index(const settings_wifi_network_t *nets, int n,
                       const char *ssid) {
  for (int i = 0; i < n; i++) {
    if (strcmp(nets[i].ssid, ssid) == 0) {
      return i;
    }
  }
  return -1;
}

/* Saved networks come with "known": true (never their password). */
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;
  settings_wifi_network_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  int n_saved = 0;
  settings_get_wifi_networks(saved, &n_saved);

  cJSON *json = cJSON_CreateObject();
  esp_err_t err = wifi_scan(&ap_list, &ap_count);

  if (err == ESP_OK && ap_list) {
    cJSON *networks = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_count; i++) {
      cJSON *net = cJSON_CreateObject();
      cJSON_AddStringToObject(net, "ssid", (char *)ap_list[i].ssid);
      cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
      cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
      cJSON_AddBoolToObject(
          net, "known",
          saved_index(saved, n_saved, (char *)ap_list[i].ssid) >= 0);
      cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddItemToObject(json, "networks", networks);
    cJSON_AddBoolToObject(json, "success", true);
    free(ap_list);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
  }

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  return ESP_OK;
}

/* Tries the network and answers at once; the page follows the outcome on
 * /api/wifi/status. Saved only once it works, so no restart. Without a
 * "password", a saved network is joined with its stored one. */
static esp_err_t wifi_config_handler(httpd_req_t *req) {
  char *body = web_server_recv_body(req, 512);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(json, "ssid"));
  const char *pass =
      cJSON_GetStringValue(cJSON_GetObjectItem(json, "password"));
  settings_wifi_network_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  int n_saved = 0;
  if (ssid && !pass) {
    settings_get_wifi_networks(saved, &n_saved);
    int i = saved_index(saved, n_saved, ssid);
    pass = i >= 0 ? saved[i].password : NULL;
  }
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || !pass || strlen(pass) > 64) {
    cJSON_Delete(json);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               pass ? "Invalid SSID" : "Password needed");
  }
  // Answer before the try: a client on the current network loses it.
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  esp_err_t err = wifi_try_credentials(ssid, pass);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Wi-Fi try not started: %s", esp_err_to_name(err));
  }
  cJSON_Delete(json);
  return ESP_OK;
}

/* The saved networks, most recently used first (names only). */
static esp_err_t wifi_saved_handler(httpd_req_t *req) {
  settings_wifi_network_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  int n = 0;
  settings_get_wifi_networks(saved, &n);
  cJSON *json = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(json, "networks");
  for (int i = 0; i < n; i++) {
    cJSON_AddItemToArray(list, cJSON_CreateString(saved[i].ssid));
  }
  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, str);
  free(str);
  return ESP_OK;
}

/* {"ssid": "..."}: forget a saved network and its password. The current
 * connection stays until the next restart. */
static esp_err_t wifi_forget_handler(httpd_req_t *req) {
  char *body = web_server_recv_body(req, 128);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(json, "ssid"));
  esp_err_t err =
      ssid ? settings_forget_wifi_network(ssid) : ESP_ERR_INVALID_ARG;
  cJSON_Delete(json);
  if (err != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               err == ESP_ERR_NOT_FOUND ? "Not saved"
                                                        : esp_err_to_name(err));
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"success\":true}");
}

static esp_err_t wifi_status_handler(httpd_req_t *req) {
  static const char *const STATES[] = {"idle", "trying", "ok", "failed"};
  char ip[16] = "";
  char host[33];
  char name[65];
  wifi_get_ip_str(ip, sizeof(ip));
  settings_get_host_name(host, sizeof(host));
  settings_get_device_name(name, sizeof(name));
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "state", STATES[wifi_try_state()]);
  cJSON_AddStringToObject(json, "ip", ip);
  cJSON_AddStringToObject(json, "host", host);
  cJSON_AddStringToObject(json, "name", name);
  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, str);
  free(str);
  return ESP_OK;
}

static esp_err_t device_name_handler(httpd_req_t *req) {
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *name_json = cJSON_GetObjectItem(json, "name");
  cJSON *response = cJSON_CreateObject();

  char host[33] = "";
  if (cJSON_IsString(name_json)) {
    settings_host_name_from(cJSON_GetStringValue(name_json), host,
                            sizeof(host));
  }
  if (host[0] && fleet_host_in_use(host)) {
    // Two devices on one host name: one of them would be unreachable.
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Another arply on this network uses that name");
  } else if (name_json && cJSON_IsString(name_json)) {
    const char *name = cJSON_GetStringValue(name_json);
    esp_err_t err = settings_set_device_name(name);
    if (err == ESP_OK) {
      wifi_set_hostname(name);
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid name");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

static esp_err_t led_brightness_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "brightness", led_get_brightness());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t led_brightness_post_handler(httpd_req_t *req) {
  char content[64];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "brightness");
  if (val && cJSON_IsNumber(val)) {
    int b = (int)val->valuedouble;
    if (b < 0) {
      b = 0;
    }
    if (b > 255) {
      b = 255;
    }
    esp_err_t err = led_set_brightness((uint8_t)b);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected {\"brightness\": 0-255}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* Read a whole request body into a NUL-terminated heap buffer. httpd_req_recv()
 * can return a short read, so keep going until the declared length arrives. */
char *web_server_recv_body(httpd_req_t *req, size_t max_len) {
  int len = req->content_len;
  if (len <= 0 || (size_t)len > max_len) {
    return NULL;
  }
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    return NULL;
  }
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, buf + got, (size_t)(len - got));
    if (r <= 0) {
      free(buf);
      return NULL;
    }
    got += r;
  }
  buf[len] = '\0';
  return buf;
}

/* True for a JSON number that is a whole value inside [lo, hi]. */
static bool json_int_in_range(const cJSON *v, int lo, int hi) {
  return cJSON_IsNumber(v) && v->valuedouble == (double)v->valueint &&
         v->valueint >= lo && v->valueint <= hi;
}

static esp_err_t chime_get_handler(httpd_req_t *req) {
  bool on = true;
  settings_get_chime_on(&on);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "on", on);
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_PrintUnformatted(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json_str);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t chime_post_handler(httpd_req_t *req) {
  char *content = web_server_recv_body(req, 64);
  cJSON *json = content ? cJSON_Parse(content) : NULL;
  free(content);
  const cJSON *on = json ? cJSON_GetObjectItem(json, "on") : NULL;
  cJSON *response = cJSON_CreateObject();
  if (cJSON_IsBool(on) && settings_set_chime_on(cJSON_IsTrue(on)) == ESP_OK) {
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddBoolToObject(response, "on", cJSON_IsTrue(on));
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"on\": bool}");
  }
  cJSON_Delete(json);
  char *json_str = cJSON_PrintUnformatted(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json_str);
  free(json_str);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t channel_mode_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "mode", audio_output_get_channel_mode());
  cJSON_AddBoolToObject(json, "locked", audio_output_channel_mode_locked());
  cJSON_AddBoolToObject(json, "success", true);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t channel_mode_post_handler(httpd_req_t *req) {
  char *content = web_server_recv_body(req, 128);
  if (!content) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  free(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *val = cJSON_GetObjectItem(json, "mode");
  if (json_int_in_range(val, AUDIO_CHANNEL_STEREO, AUDIO_CHANNEL_MONO)) {
    audio_output_set_channel_mode((audio_channel_mode_t)val->valueint);
    cJSON_AddBoolToObject(response, "success", true);
    /* Boards with two amplifiers keep stereo whatever was asked for. */
    cJSON_AddNumberToObject(response, "mode", audio_output_get_channel_mode());
    cJSON_AddBoolToObject(response, "locked",
                          audio_output_channel_mode_locked());
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Expected {\"mode\": 0-3}");
  }

  char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
  if (req->content_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware uploaded");
    return ESP_FAIL;
  }

  // Stop AirPlay to free resources during OTA
  ESP_LOGI(TAG, "Stopping AirPlay for OTA update");
  rtsp_server_stop();

  esp_err_t err = ota_start_from_http(req);

  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return ESP_FAIL;
  }

  // Send response before restarting
  httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

static esp_err_t system_info_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *info = cJSON_CreateObject();

  char ip_str[16] = {0};
  char mac_str[18] = {0};
  char device_name[65] = {0};
  bool wifi_connected = wifi_is_connected();

  wifi_get_ip_str(ip_str, sizeof(ip_str));
  wifi_get_mac_str(mac_str, sizeof(mac_str));
  settings_get_device_name(device_name, sizeof(device_name));

  cJSON_AddStringToObject(info, "ip", ip_str);
  cJSON_AddStringToObject(info, "mac", mac_str);
  cJSON_AddStringToObject(info, "device_name", device_name);
  cJSON_AddBoolToObject(info, "wifi_connected", wifi_connected);
  cJSON_AddNumberToObject(info, "free_heap", esp_get_free_heap_size());

  // WiFi link diagnostics (only meaningful when associated as STA)
  if (wifi_connected) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      char ssid_buf[33];
      size_t slen = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
      if (slen > sizeof(ssid_buf) - 1) {
        slen = sizeof(ssid_buf) - 1;
      }
      memcpy(ssid_buf, ap.ssid, slen);
      ssid_buf[slen] = '\0';
      char bssid_buf[18];
      snprintf(bssid_buf, sizeof(bssid_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
               ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
               ap.bssid[5]);
      const char *phy = "?";
      if (ap.phy_11n) {
        phy = "11n";
      } else if (ap.phy_11g) {
        phy = "11g";
      } else if (ap.phy_11b) {
        phy = "11b";
      } else if (ap.phy_lr) {
        phy = "LR";
      }
      cJSON_AddStringToObject(info, "wifi_ssid", ssid_buf);
      cJSON_AddStringToObject(info, "wifi_bssid", bssid_buf);
      cJSON_AddNumberToObject(info, "wifi_rssi", ap.rssi);
      cJSON_AddNumberToObject(info, "wifi_channel", ap.primary);
      cJSON_AddStringToObject(info, "wifi_phy", phy);
    }
  }
  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddStringToObject(info, "firmware_version", app_desc->version);
  cJSON_AddBoolToObject(info, "eq_supported", false);
  cJSON_AddBoolToObject(info, "sub_supported", false);

  cJSON_AddItemToObject(json, "info", info);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  return ESP_OK;
}

static esp_err_t system_restart_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  ESP_LOGI(TAG, "Restart requested via web interface");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

/* Erases every setting (saved Wi-Fi networks, name, room, DSP, pairings,
 * certificate) and restarts as a new device with the setup hotspot. */
static esp_err_t system_factory_reset_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  ESP_LOGW(TAG, "Factory reset requested via web interface");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
  }
  esp_restart();
  return ESP_OK;
}

/* ================================================================== */
/*  SPIFFS File Management API                                         */
/* ================================================================== */

// Allowed path prefixes for file upload (prevent writes outside SPIFFS)
static const char *ALLOWED_PREFIXES[] = {"/spiffs/"};

static bool is_path_allowed(const char *path) {
  for (int i = 0; i < sizeof(ALLOWED_PREFIXES) / sizeof(ALLOWED_PREFIXES[0]);
       i++) {
    if (strncmp(path, ALLOWED_PREFIXES[i], strlen(ALLOWED_PREFIXES[i])) == 0) {
      // Reject path traversal
      if (strstr(path, "..") != NULL) {
        return false;
      }
      return true;
    }
  }
  return false;
}

static esp_err_t fs_upload_handler(httpd_req_t *req) {
  // Get target path from query string
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  // Streams to SPIFFS in small chunks; the cap only guards the partition.
  // The calibration page alone is ~68 KB.
  if (req->content_len == 0 || req->content_len > (size_t)(256 * 1024)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Body required (max 256KB)");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create %s", path);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create file");
    return ESP_FAIL;
  }

  char buf[SPIFFS_CHUNK_SIZE];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }
    fwrite(buf, 1, (size_t)received, f);
    remaining -= (size_t)received;
  }
  fclose(f);

  ESP_LOGI(TAG, "Uploaded %u bytes to %s", (unsigned)req->content_len, path);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "size", (double)req->content_len);
  cJSON_AddStringToObject(json, "path", path);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_delete_handler(httpd_req_t *req) {
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_CreateObject();
  if (remove(path) == 0) {
    ESP_LOGI(TAG, "Deleted %s", path);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "File not found");
  }
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_list_handler(httpd_req_t *req) {
  char query[128] = {0};
  char dir_path[64] = "/spiffs";

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dir_path, sizeof(dir_path));
  }

  if (!is_path_allowed(dir_path) && strcmp(dir_path, "/spiffs") != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  DIR *d = opendir(dir_path);
  cJSON *json = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();

  if (d) {
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entry->d_name);

      char full_path[320];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
      struct stat st;
      if (stat(full_path, &st) == 0) {
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
      }
      cJSON_AddItemToArray(files, item);
    }
    closedir(d);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "Cannot open directory");
  }

  cJSON_AddItemToObject(json, "files", files);
  char *json_str = cJSON_Print(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* ================================================================== */
/*  EQ Page + API  (only when TAS58xx DAC is configured)               */
/* ================================================================== */

/* Served on both HTTP and HTTPS, also to setup-hotspot clients. */
static const httpd_uri_t SETUP_URIS[] = {
    {.uri = "/", .method = HTTP_GET, .handler = root_handler},
    {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler},
    {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler},
    {.uri = "/api/wifi/config",
     .method = HTTP_POST,
     .handler = wifi_config_handler},
    {.uri = "/api/wifi/status",
     .method = HTTP_GET,
     .handler = wifi_status_handler},
};

/* Pages and APIs served on both HTTP and HTTPS, on the LAN only. */
static const httpd_uri_t SHARED_URIS[] = {
    {.uri = "/arply.crt", .method = HTTP_GET, .handler = cert_handler},
    {.uri = "/api/wifi/saved",
     .method = HTTP_GET,
     .handler = wifi_saved_handler},
    {.uri = "/api/wifi/forget",
     .method = HTTP_POST,
     .handler = wifi_forget_handler},
    {.uri = "/api/device/name",
     .method = HTTP_POST,
     .handler = device_name_handler},
    {.uri = "/api/led/brightness",
     .method = HTTP_GET,
     .handler = led_brightness_get_handler},
    {.uri = "/api/led/brightness",
     .method = HTTP_POST,
     .handler = led_brightness_post_handler},
    {.uri = "/api/audio/channel",
     .method = HTTP_GET,
     .handler = channel_mode_get_handler},
    {.uri = "/api/audio/channel",
     .method = HTTP_POST,
     .handler = channel_mode_post_handler},
    {.uri = "/api/audio/chime",
     .method = HTTP_GET,
     .handler = chime_get_handler},
    {.uri = "/api/audio/chime",
     .method = HTTP_POST,
     .handler = chime_post_handler},
    {.uri = "/api/audio/volume",
     .method = HTTP_GET,
     .handler = volume_get_handler},
    {.uri = "/api/audio/volume",
     .method = HTTP_POST,
     .handler = volume_post_handler},
    {.uri = "/api/ota/update",
     .method = HTTP_POST,
     .handler = ota_update_handler},
    {.uri = "/api/system/info",
     .method = HTTP_GET,
     .handler = system_info_handler},
    {.uri = "/api/system/restart",
     .method = HTTP_POST,
     .handler = system_restart_handler},
    {.uri = "/api/system/factory-reset",
     .method = HTTP_POST,
     .handler = system_factory_reset_handler},
    {.uri = "/api/fs/upload",
     .method = HTTP_POST,
     .handler = fs_upload_handler},
    {.uri = "/api/fs/delete",
     .method = HTTP_POST,
     .handler = fs_delete_handler},
    {.uri = "/api/fs/list", .method = HTTP_GET, .handler = fs_list_handler},
};

/* HTTP only: the log viewer's websocket supports one server, the speedtest
 * should measure Wi-Fi rather than TLS, and captive-portal probes are plain
 * HTTP. */
static const httpd_uri_t HTTP_ONLY_URIS[] = {
    {.uri = "/logs", .method = HTTP_GET, .handler = logs_page_handler},
    {.uri = "/speedtest",
     .method = HTTP_GET,
     .handler = speedtest_page_handler},
    {.uri = "/api/speedtest/ping",
     .method = HTTP_GET,
     .handler = speedtest_ping_handler},
    {.uri = "/api/speedtest/download",
     .method = HTTP_GET,
     .handler = speedtest_download_handler},
    {.uri = "/api/speedtest/upload",
     .method = HTTP_POST,
     .handler = speedtest_upload_handler},
};

/* Captive-portal probes, for setup-hotspot clients. */
static const httpd_uri_t CAPTIVE_URIS[] = {
    {.uri = "/hotspot-detect.html",
     .method = HTTP_GET,
     .handler = captive_apple_handler},
    {.uri = "/library/test/success.html",
     .method = HTTP_GET,
     .handler = captive_apple_handler},
    {.uri = "/generate_204",
     .method = HTTP_GET,
     .handler = captive_android_handler},
    {.uri = "/connecttest.txt",
     .method = HTTP_GET,
     .handler = captive_windows_handler},
};

/* On HTTPS, send the HTTP-only pages back to plain HTTP. */
static esp_err_t to_http_handler(httpd_req_t *req) {
  char host[64];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return httpd_resp_send_404(req);
  }
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
  }
  char location[sizeof(host) + 7 + CONFIG_HTTPD_MAX_URI_LEN];
  snprintf(location, sizeof(location), "http://%s%s", host, req->uri);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  return httpd_resp_send(req, NULL, 0);
}

int web_server_register_shared(httpd_handle_t server) {
  int n = 0;
  if (server != s_server) {
    static const httpd_uri_t to_http[] = {
        {.uri = "/logs", .method = HTTP_GET, .handler = to_http_handler},
        {.uri = "/speedtest", .method = HTTP_GET, .handler = to_http_handler},
    };
    for (size_t i = 0; i < 2; i++) {
      web_server_register_lan_only(server, &to_http[i]);
    }
  }
  for (size_t i = 0; i < sizeof(SETUP_URIS) / sizeof(SETUP_URIS[0]); i++) {
    n += httpd_register_uri_handler(server, &SETUP_URIS[i]) == ESP_OK;
  }
  for (size_t i = 0; i < sizeof(SHARED_URIS) / sizeof(SHARED_URIS[0]); i++) {
    n += web_server_register_lan_only(server, &SHARED_URIS[i]) == ESP_OK;
  }
  return n + web_dsp_register(server) + fleet_register_handlers(server) +
         online_update_register(server);
}

esp_err_t web_server_start(uint16_t port) {
  if (s_server) {
    ESP_LOGW(TAG, "Web server already running");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_open_sockets = 3;    // Limit to save lwIP socket slots for AirPlay
  config.lru_purge_enable = true; // Reclaim stale sockets when all are in use
  config.max_uri_handlers = WEB_SERVER_SHARED_URI_COUNT + 11; // + HTTP-only
  config.max_resp_headers = 8;
  config.stack_size = 8192;
  config.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
    return err;
  }

  web_server_register_shared(s_server);
  for (size_t i = 0; i < sizeof(HTTP_ONLY_URIS) / sizeof(HTTP_ONLY_URIS[0]);
       i++) {
    web_server_register_lan_only(s_server, &HTTP_ONLY_URIS[i]);
  }
  for (size_t i = 0; i < sizeof(CAPTIVE_URIS) / sizeof(CAPTIVE_URIS[0]); i++) {
    httpd_register_uri_handler(s_server, &CAPTIVE_URIS[i]);
  }
  log_stream_register(s_server);

  // Must be registered last: with wildcard matching enabled, this only
  // catches requests no more specific handler above already claimed.
  httpd_uri_t captive_catchall = {
      .uri = "/*", .method = HTTP_GET, .handler = captive_catchall_handler};
  httpd_register_uri_handler(s_server, &captive_catchall);

  ESP_LOGI(TAG, "Web server started on port %d with captive portal support",
           port);
  return ESP_OK;
}

void web_server_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
  }
}
