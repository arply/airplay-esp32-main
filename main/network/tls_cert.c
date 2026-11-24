/**
 * @file tls_cert.c
 * @brief Per-device self-signed TLS certificate for the HTTPS server.
 *
 * Generated once (EC P-256) and kept in NVS. A device that is renamed gets a
 * new certificate at the next boot, because the name is in the SAN. The
 * certificate is its own CA (basicConstraints CA:TRUE) so iOS can list it
 * under Certificate Trust Settings when it is installed as a profile.
 */

#include "tls_cert.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/asn1.h"
#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

static const char *TAG = "tls_cert";

#define NVS_NS       "airplay"
#define NVS_KEY_HOST "tls_host"
#define NVS_KEY_CERT "tls_crt"
#define NVS_KEY_KEY  "tls_key"
#define CERT_PEM_MAX 2048
#define KEY_PEM_MAX  512
#define HOST_MAX     64
// iOS rejects user-trusted server certificates valid for more than 825 days.
// The device has no clock at first boot, so the window is fixed.
#define NOT_BEFORE "20260101000000"
#define NOT_AFTER  "20280405000000"

static char *s_cert;
static char *s_key;

static int rng(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  esp_fill_random(buf, len);
  return 0;
}

/* Same rule as the DHCP hostname in wifi.c, lower-cased: the name users
 * type in the address bar. */
static void dns_name(const char *device_name, char *out, size_t out_len) {
  size_t j = 0;
  for (size_t i = 0; device_name[i] && j < out_len - 1; i++) {
    unsigned char c = (unsigned char)device_name[i];
    if (isalnum(c)) {
      out[j++] = (char)tolower(c);
    } else if (j > 0 && out[j - 1] != '-') {
      out[j++] = '-';
    }
  }
  while (j > 0 && out[j - 1] == '-') {
    j--;
  }
  if (j == 0) {
    strlcpy(out, "arply", out_len);
    return;
  }
  out[j] = '\0';
}

static esp_err_t load(const char *host) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }
  char stored[HOST_MAX];
  size_t len = sizeof(stored);
  esp_err_t err = nvs_get_str(nvs, NVS_KEY_HOST, stored, &len);
  if (err == ESP_OK && strcmp(stored, host) != 0) {
    err = ESP_ERR_INVALID_STATE; // renamed
  }
  if (err == ESP_OK) {
    s_cert = malloc(CERT_PEM_MAX);
    s_key = malloc(KEY_PEM_MAX);
    size_t cert_len = CERT_PEM_MAX;
    size_t key_len = KEY_PEM_MAX;
    if (!s_cert || !s_key ||
        nvs_get_str(nvs, NVS_KEY_CERT, s_cert, &cert_len) != ESP_OK ||
        nvs_get_str(nvs, NVS_KEY_KEY, s_key, &key_len) != ESP_OK) {
      free(s_cert);
      free(s_key);
      s_cert = s_key = NULL;
      err = ESP_ERR_NOT_FOUND;
    }
  }
  nvs_close(nvs);
  return err;
}

static esp_err_t store(const char *host) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_str(nvs, NVS_KEY_CERT, s_cert);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_KEY, s_key);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_HOST, host);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  return err;
}

static esp_err_t generate(const char *host) {
  char subject[HOST_MAX + 32];
  char fqdn[HOST_MAX + 8];
  snprintf(fqdn, sizeof(fqdn), "%s.local", host);
  snprintf(subject, sizeof(subject), "CN=%s,O=arply", fqdn);

  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  s_cert = calloc(1, CERT_PEM_MAX);
  s_key = calloc(1, KEY_PEM_MAX);
  int ret = (s_cert && s_key) ? 0 : MBEDTLS_ERR_PK_ALLOC_FAILED;

  if (ret == 0) {
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  }
  if (ret == 0) {
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng,
                              NULL);
  }

  unsigned char serial[16];
  esp_fill_random(serial, sizeof(serial));
  serial[0] &= 0x7F; // positive INTEGER
  serial[0] |= 0x01; // and no leading zero byte

  mbedtls_x509_san_list san_arply = {
      .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
               .san.unstructured_name = {.p = (unsigned char *)"arply.local",
                                         .len = 11}},
      .next = NULL};
  mbedtls_x509_san_list san_host = {
      .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
               .san.unstructured_name = {.p = (unsigned char *)fqdn,
                                         .len = strlen(fqdn)}},
      .next = &san_arply};
  mbedtls_asn1_sequence eku = {
      .buf = {.tag = MBEDTLS_ASN1_OID,
              .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH),
              .p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH},
      .next = NULL};

  if (ret == 0) {
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_validity(&crt, NOT_BEFORE, NOT_AFTER);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_key_usage(
        &crt,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_host);
  }
  if (ret == 0) {
    ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)s_cert, CERT_PEM_MAX,
                                    rng, NULL);
  }
  if (ret == 0) {
    ret = mbedtls_pk_write_key_pem(&key, (unsigned char *)s_key, KEY_PEM_MAX);
  }

  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  if (ret != 0) {
    ESP_LOGE(TAG, "Certificate generation failed: -0x%04x", (unsigned)-ret);
    free(s_cert);
    free(s_key);
    s_cert = s_key = NULL;
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t tls_cert_get(const char *device_name, const char **cert_pem,
                       const char **key_pem) {
  if (!s_cert) {
    char host[HOST_MAX];
    dns_name(device_name, host, sizeof(host));
    if (load(host) == ESP_OK) {
      ESP_LOGI(TAG, "Using stored certificate for %s.local", host);
    } else {
      ESP_LOGI(TAG, "Generating certificate for %s.local ...", host);
      if (generate(host) != ESP_OK) {
        return ESP_FAIL;
      }
      if (store(host) != ESP_OK) {
        ESP_LOGW(TAG, "Could not store the certificate; regenerating next "
                      "boot");
      }
      ESP_LOGI(TAG, "Certificate ready");
    }
  }
  *cert_pem = s_cert;
  *key_pem = s_key;
  return ESP_OK;
}
