#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

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
