#pragma once

#include "esp_http_server.h"

/** Number of URI handlers web_dsp_register() adds (for max_uri_handlers). */
#define WEB_DSP_URI_COUNT 10

/**
 * Register the /api/dsp handlers on @p server. Call before any wildcard
 * catch-all handler. Returns the number of handlers registered.
 */
int web_dsp_register(httpd_handle_t server);
