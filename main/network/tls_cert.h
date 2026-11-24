#pragma once

#include "esp_err.h"

/**
 * This device's TLS certificate and key (PEM, NUL-terminated), generated on
 * first use and kept in NVS. A changed device name yields a new certificate.
 * The pointers stay valid for the lifetime of the firmware.
 */
esp_err_t tls_cert_get(const char *device_name, const char **cert_pem,
                       const char **key_pem);
