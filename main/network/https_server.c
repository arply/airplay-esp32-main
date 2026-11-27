/**
 * @file https_server.c
 * @brief HTTPS server for pages that need a secure context (microphone).
 *
 * Runs as a second httpd instance on port 443 with its own control port, so
 * the plain HTTP server on port 80 is unaffected. Two sockets at most: TLS
 * sessions cost ~20 KB each and lwIP sockets are shared with AirPlay.
 */

#include "https_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "tls_cert.h"
#include "web_dsp.h"
#include "web_server.h"

static const char *TAG = "https";

#define HTTPS_CTRL_PORT     32769 // HTTP server uses the default 32768
#define MICTEST_LOG_MAX_LEN 1024
#define HTTPS_INIT_STACK    8192

extern const char
    mictest_html_start[] asm("_binary_mictest_html_start");           // NOLINT
extern const char mictest_html_end[] asm("_binary_mictest_html_end"); // NOLINT

static httpd_handle_t s_server = NULL;

static void log_heap(const char *when) {
  ESP_LOGI(TAG, "%s: internal free %u (largest %u), psram free %u", when,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void https_event_handler(void *arg, esp_event_base_t base, int32_t id,
                                void *data) {
  (void)arg;
  (void)base;
  if (id == HTTPS_SERVER_EVENT_ON_CONNECTED) {
    log_heap("TLS session up");
  } else if (id == HTTPS_SERVER_EVENT_DISCONNECTED) {
    log_heap("TLS session closed");
  } else if (id == HTTPS_SERVER_EVENT_ERROR) {
    // Browsers drop idle connections (reset) and refuse the self-signed
    // certificate until it is trusted: both normal, not errors.
    const esp_https_server_last_error_t *e = data;
    const int code = e ? abs(e->esp_tls_error_code) : 0;
    if (code == -MBEDTLS_ERR_NET_CONN_RESET) {
      ESP_LOGD(TAG, "A browser dropped its connection");
    } else if (code == -MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
      ESP_LOGI(TAG, "A browser refused the certificate (until it is trusted)");
    } else {
      ESP_LOGW(TAG, "TLS error: %s (mbedtls -0x%04x)",
               e ? esp_err_to_name(e->last_error) : "?", code);
    }
  }
}

static esp_err_t mictest_page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, mictest_html_start,
                         mictest_html_end - mictest_html_start - 1);
}

/* The test page reports what the browser allowed, so the result ends up in
 * the serial log instead of having to be read off the phone. */
static esp_err_t mictest_log_handler(httpd_req_t *req) {
  char *buf = web_server_recv_body(req, MICTEST_LOG_MAX_LEN);
  if (!buf) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
  }
  ESP_LOGI(TAG, "mictest: %s", buf);
  free(buf);
  return httpd_resp_sendstr(req, "ok");
}

static esp_err_t start(void) {
  log_heap("before HTTPS start");

  char name[65];
  settings_get_device_name(name, sizeof(name));
  const char *cert_pem;
  const char *key_pem;
  esp_err_t err = tls_cert_get(name, &cert_pem, &key_pem);
  if (err != ESP_OK) {
    return err;
  }

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.httpd.ctrl_port = HTTPS_CTRL_PORT;
  conf.httpd.max_open_sockets = 3; // a page load opens a few at once
  conf.httpd.lru_purge_enable = true;
  conf.httpd.max_uri_handlers = 2 + WEB_SERVER_SHARED_URI_COUNT;
  conf.httpd.stack_size = 10240; // TLS handshake needs more than plain HTTP
  // mbedTLS wants the PEM length including the terminating NUL.
  conf.servercert = (const uint8_t *)cert_pem;
  conf.servercert_len = strlen(cert_pem) + 1;
  conf.prvtkey_pem = (const uint8_t *)key_pem;
  conf.prvtkey_len = strlen(key_pem) + 1;

  esp_event_handler_register(ESP_HTTPS_SERVER_EVENT, ESP_EVENT_ANY_ID,
                             https_event_handler, NULL);

  err = httpd_ssl_start(&s_server, &conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(err));
    s_server = NULL;
    return err;
  }

  // The stack logs each of those as an error (and every handshake as info);
  // the event handler above tells them apart instead. esp-tls-mbedtls is
  // loud again while the online update talks to arply.de.
  esp_log_level_set("esp_https_server", ESP_LOG_NONE);
  esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
  esp_log_level_set("httpd", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_ws", ESP_LOG_ERROR);

  const httpd_uri_t mictest_uri = {
      .uri = "/mictest", .method = HTTP_GET, .handler = mictest_page_handler};
  web_server_register_lan_only(s_server, &mictest_uri);

  const httpd_uri_t mictest_log_uri = {.uri = "/api/mictest/log",
                                       .method = HTTP_POST,
                                       .handler = mictest_log_handler};
  web_server_register_lan_only(s_server, &mictest_log_uri);

  web_server_register_shared(s_server);

  log_heap("after HTTPS start");
  ESP_LOGI(TAG, "HTTPS server started on port 443");
  return ESP_OK;
}

static void start_task(void *arg) {
  (void)arg;
  start();
  vTaskDelete(NULL);
}

esp_err_t https_server_start(void) {
  static bool s_started;
  if (s_started) {
    return ESP_OK;
  }
  s_started = true;
  // EC key generation on the first boot needs far more stack than the main
  // task has, and takes ~0.4 s, so it runs in a short-lived task of its own.
  return xTaskCreate(start_task, "https_init", HTTPS_INIT_STACK, NULL, 4,
                     NULL) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
