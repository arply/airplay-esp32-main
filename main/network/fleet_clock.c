/**
 * @file fleet_clock.c
 * @brief Clock comparison between arply devices (FLEET_PLAN F6 spike).
 *
 * Joint measurements need the devices to start sweeps at one moment. Each
 * device answers UDP FLEET_CLOCK_PORT with its esp_timer time; the device
 * that coordinates asks a peer a few dozen times and keeps the exchange
 * with the shortest round trip, which saw the least queueing (NTP's rule):
 * offset = peer time - (sent + received) / 2.
 */

#include "fleet_clock.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "fleet_clock";

#define MAGIC     0x4B435941u // "AYCK"
#define EXCHANGES 32

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t seq;
  int64_t t_dev; // the answering device's esp_timer time, us
} pkt_t;

static void server_task(void *arg) {
  (void)arg;
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_port = htons(FLEET_CLOCK_PORT),
                          .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
    ESP_LOGE(TAG, "Cannot listen on UDP %d", FLEET_CLOCK_PORT);
    vTaskDelete(NULL);
    return;
  }
  for (;;) {
    pkt_t p;
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    const int n = recvfrom(s, &p, sizeof(p), 0, (struct sockaddr *)&from, &len);
    if (n == (int)sizeof(p) && p.magic == MAGIC) {
      p.t_dev = esp_timer_get_time();
      sendto(s, &p, sizeof(p), 0, (struct sockaddr *)&from, len);
    }
  }
}

void fleet_clock_start(void) {
  // Above the fleet task, so a reply is not held up by a browse.
  xTaskCreate(server_task, "fleet_clock", 3072, NULL, 5, NULL);
}

esp_err_t fleet_clock_measure(uint32_t ip, int64_t *offset_us,
                              int32_t *rtt_us) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    return ESP_FAIL;
  }
  const struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in to = {.sin_family = AF_INET,
                           .sin_port = htons(FLEET_CLOCK_PORT),
                           .sin_addr.s_addr = ip};
  int64_t best_rtt = INT64_MAX;
  int64_t best_off = 0;
  for (uint32_t seq = 1; seq <= EXCHANGES; seq++) {
    pkt_t p = {.magic = MAGIC, .seq = seq};
    const int64_t t0 = esp_timer_get_time();
    sendto(s, &p, sizeof(p), 0, (struct sockaddr *)&to, sizeof(to));
    pkt_t r;
    while (recv(s, &r, sizeof(r), 0) == (int)sizeof(r)) {
      if (r.magic != MAGIC || r.seq != seq) {
        continue; // a late answer to an earlier exchange
      }
      const int64_t t1 = esp_timer_get_time();
      if (t1 - t0 < best_rtt) {
        best_rtt = t1 - t0;
        best_off = r.t_dev - (t0 + t1) / 2;
      }
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  close(s);
  if (best_rtt == INT64_MAX) {
    return ESP_ERR_TIMEOUT;
  }
  *offset_us = best_off;
  *rtt_us = (int32_t)best_rtt;
  return ESP_OK;
}
