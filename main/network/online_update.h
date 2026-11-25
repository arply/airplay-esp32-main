#pragma once

/**
 * @file online_update.h
 * @brief Firmware and web pages from arply.de: GET/POST /api/update.
 */

#include "esp_http_server.h"

/** Register GET and POST /api/update. Returns the handler count. */
int online_update_register(httpd_handle_t server);

#define ONLINE_UPDATE_URI_COUNT 2
