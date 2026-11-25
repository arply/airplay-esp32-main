#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "web_dsp.h"

/**
 * Web server for control panel
 * Provides:
 * - WiFi configuration
 * - Device name configuration
 * - OTA update
 */

/**
 * Initialize and start the web server
 * @param port HTTP server port (default: 80)
 */
esp_err_t web_server_start(uint16_t port);

/**
 * Stop the web server
 */
void web_server_stop(void);

/**
 * Read a whole request body into a NUL-terminated heap buffer (free() it).
 * Returns NULL if the body is empty, longer than @p max_len, or the read
 * fails.
 */
char *web_server_recv_body(httpd_req_t *req, size_t max_len);

/**
 * Send a file (e.g. "/spiffs/www/index.html") as a chunked response; 404 if
 * it does not exist.
 */
esp_err_t web_server_send_file(httpd_req_t *req, const char *path,
                               const char *content_type);

/** Handlers web_server_register_shared() adds, for max_uri_handlers. */
#define WEB_SERVER_SHARED_URI_COUNT (25 + WEB_DSP_URI_COUNT)

/**
 * Register the pages and APIs that both the HTTP and the HTTPS server
 * serve (settings page, calibration page, their APIs). Returns the count.
 */
int web_server_register_shared(httpd_handle_t server);

/**
 * For a page request on plain HTTP: redirect to the same URL on HTTPS and
 * return true. Returns false (nothing sent) on HTTPS, and for clients on the
 * setup hotspot, which stay on HTTP for the captive portal.
 */
bool web_server_redirect_to_https(httpd_req_t *req);
