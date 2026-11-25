#pragma once

/**
 * @file fleet.h
 * @brief arply devices finding each other (docs/web_dsp/FLEET_PLAN.md, F1).
 *
 * Each device announces _arply._tcp via mDNS and browses for the others.
 * One of them, the hub, also answers as arply.local: the device the user
 * picked last (highest "hub" sequence number among those seen), otherwise
 * the one with the lowest id.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define FLEET_MAX_PEERS 16
#define FLEET_ROOM_MAX  32

typedef enum {
  FLEET_ROLE_MAIN = 0,
  FLEET_ROLE_BASS = 1,
} fleet_role_t;

typedef struct {
  char id[13];   // MAC, 12 hex digits
  char name[65]; // display name
  char host[33]; // .local host name without the domain
  uint32_t ip;   // IPv4, network byte order (0 = unknown)
  char room[FLEET_ROOM_MAX + 1];
  uint8_t role; // fleet_role_t
  char fw[32];
  uint32_t hub;         // pick sequence number, 0 = never picked
  uint8_t channel;      // audio_channel_mode_t of the device
  uint32_t calibrated;  // Unix time of the saved EQ, 1 = date unknown, 0 = flat
  bool bass;            // bass block on in the saved profile
  bool playing;         // a stream is playing right now
  uint8_t wifi;         // fleet_wifi_t
  bool online;          // answered within the last EXPIRE_ROUNDS rounds
  int64_t last_seen_us; // esp_timer time of the last answer, 0 = not this boot
  bool via_cache;       // last seen through the cached-IP probe
} fleet_peer_t;

typedef enum {
  FLEET_WIFI_UNKNOWN = 0,
  FLEET_WIFI_POOR,
  FLEET_WIFI_FAIR,
  FLEET_WIFI_GOOD,
} fleet_wifi_t;

/** Announce this device and start browsing. Call once mDNS is up. */
void fleet_start(void);

/** True if a device other than this one uses @p host (case-insensitive). */
bool fleet_host_in_use(const char *host);

/** Register /api/fleet and /api/fleet/self. Returns the handler count. */
int fleet_register_handlers(httpd_handle_t server);

#define FLEET_URI_COUNT 3
