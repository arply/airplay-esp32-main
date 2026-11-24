#pragma once

#include "esp_err.h"

/**
 * Start the HTTPS server on port 443 next to the plain HTTP server.
 *
 * Browsers only grant microphone access (getUserMedia) in a secure context,
 * so pages that record audio on a phone must be served from here.
 */
esp_err_t https_server_start(void);
