/**
 * @file fleet.c
 * @brief Discovery of other arply devices and the arply.local hub election.
 *
 * Every ROUND_MS the task browses _arply._tcp, refreshes the peer table,
 * probes cached addresses of peers mDNS did not answer for, and elects the
 * hub. The pick is a sequence number, not a flag, so a choice made on one
 * device wins everywhere without writing to the others: the highest number
 * wins, ties and "never picked" go to the lowest id.
 *
 * Taking arply.local is debounced (CLAIM_ROUNDS scheduled wins in a row),
 * giving it up is immediate. After a new pick the old hub has released the
 * name before the new one claims it, so two devices never announce it at
 * once.
 */

#include "fleet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "audio_dsp.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "settings.h"
#include "web_server.h"

static const char *TAG = "fleet";

#define SERVICE       "_arply"
#define PROTO         "_tcp"
#define ROUND_MS      10000
#define QUERY_MS      2000
#define EXPIRE_ROUNDS 3 // a peer missing this many rounds counts as offline
#define CLAIM_ROUNDS  3 // scheduled wins in a row before taking arply.local
#define PROBE_MS      800
#define PROBES_ROUND  4

#define NVS_NS        "airplay"
#define NVS_KEY_ROOM  "room"
#define NVS_KEY_ROLE  "role"
#define NVS_KEY_HUB   "hub_seq"
#define NVS_KEY_CACHE "fleet_peers"

/* What survives a reboot about a peer, so the overview can show devices
 * that are offline right now. A blob of another size (older firmware) is
 * ignored. */
typedef struct {
  char id[13];
  uint32_t ip;
  char name[65];
  char host[33];
  char room[FLEET_ROOM_MAX + 1];
  uint8_t role;
  uint8_t channel;
} cached_peer_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static fleet_peer_t s_peers[FLEET_MAX_PEERS];
static int s_peer_count;
static cached_peer_t s_cache[FLEET_MAX_PEERS];
static int s_cache_count;

static char s_id[13];
static char s_room[FLEET_ROOM_MAX + 1];
static uint8_t s_role;
static uint32_t s_hub_seq;

static char s_hub_id[13];
static bool s_is_hub;
static int s_win_rounds;
static uint32_t s_delegated_ip;
static char s_stand_in_id[13];
static fleet_peer_t s_announced; // what the TXT record currently says

/* ---- helpers --------------------------------------------------------- */

static const char *role_name(uint8_t role) {
  return role == FLEET_ROLE_BASS ? "bass" : "main";
}

static const char *const CHANNEL_NAMES[] = {"stereo", "left", "right", "mono"};
static const char *const WIFI_NAMES[] = {"unknown", "poor", "fair", "good"};

static uint8_t channel_from(const char *v) {
  for (uint8_t i = 0; i < 4; i++) {
    if (strcmp(v, CHANNEL_NAMES[i]) == 0) {
      return i;
    }
  }
  return 0;
}

static uint8_t wifi_from(const char *v) {
  for (uint8_t i = 0; i < 4; i++) {
    if (strcmp(v, WIFI_NAMES[i]) == 0) {
      return i;
    }
  }
  return FLEET_WIFI_UNKNOWN;
}

static uint8_t wifi_quality(void) {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    return FLEET_WIFI_UNKNOWN;
  }
  return ap.rssi >= -60   ? FLEET_WIFI_GOOD
         : ap.rssi >= -72 ? FLEET_WIFI_FAIR
                          : FLEET_WIFI_POOR;
}

static uint32_t own_ip(void) {
  esp_netif_ip_info_t info;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK) {
    return 0;
  }
  return info.ip.addr;
}

static void build_self(fleet_peer_t *me) {
  memset(me, 0, sizeof(*me));
  strlcpy(me->id, s_id, sizeof(me->id));
  settings_get_device_name(me->name, sizeof(me->name));
  settings_get_host_name(me->host, sizeof(me->host));
  me->ip = own_ip();
  strlcpy(me->room, s_room, sizeof(me->room));
  me->role = s_role;
  strlcpy(me->fw, esp_app_get_description()->version, sizeof(me->fw));
  me->hub = s_hub_seq;
  me->channel = (uint8_t)audio_output_get_channel_mode();
  me->calibrated = audio_dsp_calibrated_at();
  me->pbe = audio_dsp_saved_pbe();
  me->playing = audio_receiver_is_playing() && audio_receiver_has_data();
  me->wifi = wifi_quality();
  me->online = true;
  me->last_seen_us = esp_timer_get_time();
}

static void load_settings(void) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
    return;
  }
  size_t len = sizeof(s_room);
  if (nvs_get_str(nvs, NVS_KEY_ROOM, s_room, &len) != ESP_OK) {
    s_room[0] = '\0';
  }
  nvs_get_u8(nvs, NVS_KEY_ROLE, &s_role);
  nvs_get_u32(nvs, NVS_KEY_HUB, &s_hub_seq);
  len = sizeof(s_cache);
  if (nvs_get_blob(nvs, NVS_KEY_CACHE, s_cache, &len) == ESP_OK &&
      len % sizeof(cached_peer_t) == 0) {
    s_cache_count = (int)(len / sizeof(cached_peer_t));
  }
  nvs_close(nvs);
}

static void save_settings(void) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
    return;
  }
  nvs_set_str(nvs, NVS_KEY_ROOM, s_room);
  nvs_set_u8(nvs, NVS_KEY_ROLE, s_role);
  nvs_set_u32(nvs, NVS_KEY_HUB, s_hub_seq);
  nvs_commit(nvs);
  nvs_close(nvs);
}

/* Update the TXT items whose value changed since the last announcement;
 * called every round, so playing/Wi-Fi state is at most a round old. */
static void announce(void) {
  fleet_peer_t me;
  build_self(&me);
  fleet_peer_t *a = &s_announced;
  char num[12];
  if (strcmp(me.room, a->room) != 0) {
    mdns_service_txt_item_set(SERVICE, PROTO, "room", me.room);
  }
  if (me.role != a->role) {
    mdns_service_txt_item_set(SERVICE, PROTO, "role", role_name(me.role));
  }
  if (me.hub != a->hub) {
    snprintf(num, sizeof(num), "%u", (unsigned)me.hub);
    mdns_service_txt_item_set(SERVICE, PROTO, "hub", num);
  }
  if (me.channel != a->channel) {
    mdns_service_txt_item_set(SERVICE, PROTO, "ch", CHANNEL_NAMES[me.channel]);
  }
  if (me.calibrated != a->calibrated) {
    snprintf(num, sizeof(num), "%u", (unsigned)me.calibrated);
    mdns_service_txt_item_set(SERVICE, PROTO, "cal", num);
  }
  if (me.pbe != a->pbe) {
    mdns_service_txt_item_set(SERVICE, PROTO, "pbe", me.pbe ? "1" : "0");
  }
  if (me.playing != a->playing) {
    mdns_service_txt_item_set(SERVICE, PROTO, "play", me.playing ? "1" : "0");
  }
  if (me.wifi != a->wifi) {
    mdns_service_txt_item_set(SERVICE, PROTO, "wifi", WIFI_NAMES[me.wifi]);
  }
  *a = me;
}

/* ---- peer table (caller holds s_lock) --------------------------------- */

static fleet_peer_t *find_peer(const char *id) {
  for (int i = 0; i < s_peer_count; i++) {
    if (strcmp(s_peers[i].id, id) == 0) {
      return &s_peers[i];
    }
  }
  return NULL;
}

/* A full table makes room by dropping the offline peer seen longest ago. */
static fleet_peer_t *upsert_peer(const char *id) {
  fleet_peer_t *p = find_peer(id);
  if (!p && s_peer_count < FLEET_MAX_PEERS) {
    p = &s_peers[s_peer_count++];
  } else if (!p) {
    for (int i = 0; i < s_peer_count; i++) {
      if (!s_peers[i].online &&
          (!p || s_peers[i].last_seen_us < p->last_seen_us)) {
        p = &s_peers[i];
      }
    }
  }
  if (p && strcmp(p->id, id) != 0) {
    memset(p, 0, sizeof(*p));
    strlcpy(p->id, id, sizeof(p->id));
  }
  return p;
}

/* Peers that stopped answering stay in the table as offline, so the
 * overview can still show them. */
static void expire_peers(void) {
  const int64_t limit = (int64_t)EXPIRE_ROUNDS * ROUND_MS * 1000;
  const int64_t now = esp_timer_get_time();
  for (int i = 0; i < s_peer_count; i++) {
    fleet_peer_t *p = &s_peers[i];
    if (p->online && now - p->last_seen_us > limit) {
      p->online = false;
      ESP_LOGI(TAG, "Lost %s (%s)", p->name, p->id);
    }
  }
}

/* ---- discovery -------------------------------------------------------- */

static const char *txt_get(const mdns_result_t *r, const char *key) {
  for (size_t i = 0; i < r->txt_count; i++) {
    if (strcmp(r->txt[i].key, key) == 0) {
      return r->txt[i].value ? r->txt[i].value : "";
    }
  }
  return NULL;
}

static void browse(void) {
  mdns_result_t *results = NULL;
  if (mdns_query_ptr(SERVICE, PROTO, QUERY_MS, FLEET_MAX_PEERS, &results) !=
      ESP_OK) {
    return;
  }
  const int64_t now = esp_timer_get_time();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (const mdns_result_t *r = results; r; r = r->next) {
    const char *id = txt_get(r, "id");
    if (!id || strlen(id) != 12 || strcmp(id, s_id) == 0) {
      continue;
    }
    uint32_t ip = 0;
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
      if (a->addr.type == ESP_IPADDR_TYPE_V4) {
        ip = a->addr.u_addr.ip4.addr;
      }
    }
    fleet_peer_t *p = upsert_peer(id);
    if (!p) {
      continue;
    }
    if (!p->online) {
      ESP_LOGI(TAG, "Found %s", txt_get(r, "name") ? txt_get(r, "name") : id);
    }
    const char *v;
    if ((v = txt_get(r, "name")) != NULL) {
      strlcpy(p->name, v, sizeof(p->name));
    }
    if (r->hostname) {
      strlcpy(p->host, r->hostname, sizeof(p->host));
    }
    if (ip) {
      p->ip = ip;
    }
    if ((v = txt_get(r, "room")) != NULL) {
      strlcpy(p->room, v, sizeof(p->room));
    }
    if ((v = txt_get(r, "role")) != NULL) {
      p->role = strcmp(v, "bass") == 0 ? FLEET_ROLE_BASS : FLEET_ROLE_MAIN;
    }
    if ((v = txt_get(r, "fw")) != NULL) {
      strlcpy(p->fw, v, sizeof(p->fw));
    }
    if ((v = txt_get(r, "hub")) != NULL) {
      p->hub = (uint32_t)strtoul(v, NULL, 10);
    }
    if ((v = txt_get(r, "ch")) != NULL) {
      p->channel = channel_from(v);
    }
    if ((v = txt_get(r, "cal")) != NULL) {
      p->calibrated = (uint32_t)strtoul(v, NULL, 10);
    }
    if ((v = txt_get(r, "pbe")) != NULL) {
      p->pbe = strcmp(v, "1") == 0;
    }
    if ((v = txt_get(r, "play")) != NULL) {
      p->playing = strcmp(v, "1") == 0;
    }
    if ((v = txt_get(r, "wifi")) != NULL) {
      p->wifi = wifi_from(v);
    }
    p->last_seen_us = now;
    p->online = true;
    p->via_cache = false;
  }
  xSemaphoreGive(s_lock);
  mdns_query_results_free(results);
}

/* Ask a cached address directly when mDNS stayed silent (multicast is
 * unreliable on some mesh/Wi-Fi setups). */
static bool probe(const cached_peer_t *c, fleet_peer_t *out) {
  char url[48];
  snprintf(url, sizeof(url), "http://" IPSTR "/api/fleet/self",
           IP2STR((const esp_ip4_addr_t *)&c->ip));
  esp_http_client_config_t cfg = {.url = url, .timeout_ms = PROBE_MS};
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) {
    return false;
  }
  bool ok = false;
  char body[512];
  if (esp_http_client_open(h, 0) == ESP_OK &&
      esp_http_client_fetch_headers(h) >= 0 &&
      esp_http_client_get_status_code(h) == 200) {
    int n = esp_http_client_read_response(h, body, sizeof(body) - 1);
    if (n > 0) {
      body[n] = '\0';
      cJSON *j = cJSON_Parse(body);
      const cJSON *id = cJSON_GetObjectItem(j, "id");
      if (cJSON_IsString(id) && strcmp(id->valuestring, c->id) == 0) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->id, c->id, sizeof(out->id));
        const cJSON *v;
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "name"))) {
          strlcpy(out->name, v->valuestring, sizeof(out->name));
        }
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "host"))) {
          strlcpy(out->host, v->valuestring, sizeof(out->host));
        }
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "room"))) {
          strlcpy(out->room, v->valuestring, sizeof(out->room));
        }
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "role"))) {
          out->role = strcmp(v->valuestring, "bass") == 0 ? FLEET_ROLE_BASS
                                                          : FLEET_ROLE_MAIN;
        }
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "fw"))) {
          strlcpy(out->fw, v->valuestring, sizeof(out->fw));
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "hub"))) {
          out->hub = (uint32_t)v->valuedouble;
        }
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "ch"))) {
          out->channel = channel_from(v->valuestring);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "calibrated"))) {
          out->calibrated = (uint32_t)v->valuedouble;
        }
        out->pbe = cJSON_IsTrue(cJSON_GetObjectItem(j, "pbe"));
        out->playing = cJSON_IsTrue(cJSON_GetObjectItem(j, "playing"));
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "wifi"))) {
          out->wifi = wifi_from(v->valuestring);
        }
        out->ip = c->ip;
        ok = true;
      }
      cJSON_Delete(j);
    }
  }
  esp_http_client_cleanup(h);
  return ok;
}

static void probe_cached(void) {
  const int64_t fresh = (int64_t)ROUND_MS * 1000;
  int probes = 0;
  for (int i = 0; i < s_cache_count && probes < PROBES_ROUND; i++) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const fleet_peer_t *p = find_peer(s_cache[i].id);
    bool seen = p && esp_timer_get_time() - p->last_seen_us < fresh;
    xSemaphoreGive(s_lock);
    if (seen || s_cache[i].ip == 0) {
      continue;
    }
    probes++;
    fleet_peer_t found;
    if (!probe(&s_cache[i], &found)) {
      continue;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fleet_peer_t *q = upsert_peer(found.id);
    if (q) {
      found.last_seen_us = esp_timer_get_time();
      found.online = true;
      found.via_cache = true;
      *q = found;
    }
    xSemaphoreGive(s_lock);
  }
}

/* Remember the addresses of the peers seen; write NVS only on a change. */
static void update_cache(void) {
  // ~2.5 KB: static, not on the task stack (it overflowed there). Only the
  // fleet task calls this.
  static cached_peer_t next[FLEET_MAX_PEERS];
  int n = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < s_peer_count; i++) {
    if (s_peers[i].ip == 0) {
      continue;
    }
    const fleet_peer_t *p = &s_peers[i];
    memset(&next[n], 0, sizeof(next[n]));
    strlcpy(next[n].id, p->id, sizeof(next[n].id));
    next[n].ip = p->ip;
    strlcpy(next[n].name, p->name, sizeof(next[n].name));
    strlcpy(next[n].host, p->host, sizeof(next[n].host));
    strlcpy(next[n].room, p->room, sizeof(next[n].room));
    next[n].role = p->role;
    next[n].channel = p->channel;
    n++;
  }
  xSemaphoreGive(s_lock);
  // Keep cached peers that are merely offline right now.
  for (int i = 0; i < s_cache_count && n < FLEET_MAX_PEERS; i++) {
    bool present = false;
    for (int k = 0; k < n; k++) {
      present |= strcmp(next[k].id, s_cache[i].id) == 0;
    }
    if (!present) {
      next[n++] = s_cache[i];
    }
  }
  if (n == s_cache_count &&
      memcmp(next, s_cache, (size_t)n * sizeof(next[0])) == 0) {
    return;
  }
  memcpy(s_cache, next, (size_t)n * sizeof(next[0]));
  s_cache_count = n;
  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_blob(nvs, NVS_KEY_CACHE, s_cache,
                 (size_t)n * sizeof(cached_peer_t));
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

/* ---- hub election ----------------------------------------------------- */

static void set_hub(bool on) {
  const uint32_t ip = own_ip();
  if (on && ip && (!s_is_hub || ip != s_delegated_ip)) {
    mdns_ip_addr_t addr = {.next = NULL};
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = ip;
    esp_err_t err =
        s_is_hub
            ? mdns_delegate_hostname_set_address(SETTINGS_HUB_HOST_NAME, &addr)
            : mdns_delegate_hostname_add(SETTINGS_HUB_HOST_NAME, &addr);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "This device is now arply.local");
      s_is_hub = true;
      s_delegated_ip = ip;
    }
  } else if (!on && s_is_hub) {
    mdns_delegate_hostname_remove(SETTINGS_HUB_HOST_NAME);
    ESP_LOGI(TAG, "Released arply.local");
    s_is_hub = false;
    s_delegated_ip = 0;
  }
}

/* @p scheduled: a regular round (counts towards taking the hub role). */
/* Election order: higher pick number first, then lower id. */
static bool ranks_before(uint32_t hub_a, const char *id_a, uint32_t hub_b,
                         const char *id_b) {
  return hub_a > hub_b || (hub_a == hub_b && strcmp(id_a, id_b) < 0);
}

static void elect(bool scheduled) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const char *best_id = s_id;
  uint32_t best_hub = s_hub_seq;
  const char *second_id = NULL;
  uint32_t second_hub = 0;
  for (int i = 0; i < s_peer_count; i++) {
    const fleet_peer_t *p = &s_peers[i];
    if (!p->online) {
      continue;
    }
    if (ranks_before(p->hub, p->id, best_hub, best_id)) {
      second_id = best_id;
      second_hub = best_hub;
      best_id = p->id;
      best_hub = p->hub;
    } else if (!second_id ||
               ranks_before(p->hub, p->id, second_hub, second_id)) {
      second_id = p->id;
      second_hub = p->hub;
    }
  }
  // The stand-in takes over when the hub disappears.
  strlcpy(s_stand_in_id, second_id ? second_id : "", sizeof(s_stand_in_id));
  const bool me = best_id == s_id;
  if (strcmp(s_hub_id, best_id) != 0) {
    ESP_LOGI(TAG, "Hub is %s%s", best_id, me ? " (this device)" : "");
    strlcpy(s_hub_id, best_id, sizeof(s_hub_id));
  }
  xSemaphoreGive(s_lock);

  if (!me) {
    s_win_rounds = 0;
    set_hub(false);
    return;
  }
  if (scheduled && s_win_rounds < CLAIM_ROUNDS) {
    s_win_rounds++;
  }
  if (s_win_rounds >= CLAIM_ROUNDS) {
    set_hub(true);
  }
}

static void fleet_task(void *arg) {
  (void)arg;
  for (;;) {
    // A local change (room, role, pick) wakes the task early; such a round
    // releases the hub at once but does not count towards claiming it.
    bool woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ROUND_MS)) > 0;
    if (woken) {
      s_win_rounds = 0;
    }
    browse();
    probe_cached();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    expire_peers();
    xSemaphoreGive(s_lock);
    update_cache();
    elect(!woken);
    announce();
  }
}

void fleet_start(void) {
  if (s_task) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_id, sizeof(s_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  load_settings();

  // Known from earlier boots, offline until they answer.
  for (int i = 0; i < s_cache_count && i < FLEET_MAX_PEERS; i++) {
    fleet_peer_t *p = &s_peers[s_peer_count++];
    memset(p, 0, sizeof(*p));
    strlcpy(p->id, s_cache[i].id, sizeof(p->id));
    strlcpy(p->name, s_cache[i].name, sizeof(p->name));
    strlcpy(p->host, s_cache[i].host, sizeof(p->host));
    strlcpy(p->room, s_cache[i].room, sizeof(p->room));
    p->ip = s_cache[i].ip;
    p->role = s_cache[i].role;
    p->channel = s_cache[i].channel;
  }

  fleet_peer_t me;
  build_self(&me);
  s_announced = me;
  char hub[12];
  char cal[12];
  snprintf(hub, sizeof(hub), "%u", (unsigned)s_hub_seq);
  snprintf(cal, sizeof(cal), "%u", (unsigned)me.calibrated);
  mdns_txt_item_t txt[] = {
      {"v", "1"},
      {"id", s_id},
      {"name", me.name},
      {"room", s_room},
      {"role", role_name(s_role)},
      {"fw", me.fw},
      {"hub", hub},
      {"ch", CHANNEL_NAMES[me.channel]},
      {"cal", cal},
      {"pbe", me.pbe ? "1" : "0"},
      {"play", me.playing ? "1" : "0"},
      {"wifi", WIFI_NAMES[me.wifi]},
  };
  esp_err_t err = mdns_service_add(me.name, SERVICE, PROTO, 80, txt,
                                   sizeof(txt) / sizeof(txt[0]));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to announce %s.%s: %s", SERVICE, PROTO,
             esp_err_to_name(err));
  }
  strlcpy(s_hub_id, s_id, sizeof(s_hub_id));
  // mDNS query, HTTP probe with JSON parsing and NVS writes share this stack.
  xTaskCreate(fleet_task, "fleet", 8192, NULL, 2, &s_task);
  ESP_LOGI(TAG, "Fleet discovery started (id %s, room '%s', %s)", s_id, s_room,
           role_name(s_role));
}

bool fleet_host_in_use(const char *host) {
  if (!s_lock) {
    return false;
  }
  bool used = false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < s_peer_count && !used; i++) {
    used = strcasecmp(s_peers[i].host, host) == 0;
  }
  xSemaphoreGive(s_lock);
  return used;
}

/* ---- HTTP API --------------------------------------------------------- */

static cJSON *peer_json(const fleet_peer_t *p, bool with_age) {
  cJSON *o = cJSON_CreateObject();
  char ip[16];
  snprintf(ip, sizeof(ip), IPSTR, IP2STR((const esp_ip4_addr_t *)&p->ip));
  cJSON_AddStringToObject(o, "id", p->id);
  cJSON_AddStringToObject(o, "name", p->name);
  cJSON_AddStringToObject(o, "host", p->host);
  cJSON_AddStringToObject(o, "ip", ip);
  cJSON_AddStringToObject(o, "room", p->room);
  cJSON_AddStringToObject(o, "role", role_name(p->role));
  cJSON_AddStringToObject(o, "fw", p->fw);
  cJSON_AddNumberToObject(o, "hub", p->hub);
  cJSON_AddStringToObject(o, "ch", CHANNEL_NAMES[p->channel & 3]);
  cJSON_AddNumberToObject(o, "calibrated", p->calibrated);
  cJSON_AddBoolToObject(o, "pbe", p->pbe);
  cJSON_AddBoolToObject(o, "playing", p->playing);
  cJSON_AddStringToObject(o, "wifi", WIFI_NAMES[p->wifi & 3]);
  if (with_age) {
    cJSON_AddBoolToObject(o, "online", p->online);
    if (p->last_seen_us) {
      cJSON_AddNumberToObject(
          o, "age_s",
          (double)((esp_timer_get_time() - p->last_seen_us) / 1000000));
    } else {
      cJSON_AddNullToObject(o, "age_s"); // not seen since this boot
    }
    cJSON_AddStringToObject(o, "via", p->via_cache ? "cache" : "mdns");
  }
  return o;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!str) {
    return httpd_resp_send_500(req);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, str);
  free(str);
  return err;
}

static esp_err_t self_get_handler(httpd_req_t *req) {
  fleet_peer_t me;
  build_self(&me);
  return send_json(req, peer_json(&me, false));
}

static esp_err_t fleet_get_handler(httpd_req_t *req) {
  fleet_peer_t me;
  build_self(&me);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddItemToObject(json, "self", peer_json(&me, false));
  cJSON *peers = cJSON_AddArrayToObject(json, "peers");
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < s_peer_count; i++) {
    cJSON_AddItemToArray(peers, peer_json(&s_peers[i], true));
  }
  cJSON_AddStringToObject(json, "hub", s_hub_id);
  cJSON_AddStringToObject(json, "stand_in", s_stand_in_id);
  xSemaphoreGive(s_lock);
  cJSON_AddBoolToObject(json, "is_hub", s_is_hub);
  return send_json(req, json);
}

/* {room?, role?: "main"|"bass", hub?: true} */
static esp_err_t self_post_handler(httpd_req_t *req) {
  char *body = web_server_recv_body(req, 256);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  if (!json) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
  }
  const cJSON *room = cJSON_GetObjectItem(json, "room");
  const cJSON *role = cJSON_GetObjectItem(json, "role");
  const cJSON *hub = cJSON_GetObjectItem(json, "hub");
  if ((room &&
       (!cJSON_IsString(room) || strlen(room->valuestring) > FLEET_ROOM_MAX)) ||
      (role &&
       (!cJSON_IsString(role) || (strcmp(role->valuestring, "main") != 0 &&
                                  strcmp(role->valuestring, "bass") != 0)))) {
    cJSON_Delete(json);
    return httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        "Expected {room: string (max 32), role: main|bass, hub: true}");
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (room) {
    strlcpy(s_room, room->valuestring, sizeof(s_room));
  }
  if (role) {
    s_role = strcmp(role->valuestring, "bass") == 0 ? FLEET_ROLE_BASS
                                                    : FLEET_ROLE_MAIN;
  }
  if (cJSON_IsTrue(hub)) {
    // One more than any pick seen: this device wins on every device.
    uint32_t max = s_hub_seq;
    for (int i = 0; i < s_peer_count; i++) {
      max = s_peers[i].hub > max ? s_peers[i].hub : max;
    }
    s_hub_seq = max + 1;
    ESP_LOGI(TAG, "Picked as hub (sequence %u)", (unsigned)s_hub_seq);
  }
  xSemaphoreGive(s_lock);
  cJSON_Delete(json);
  save_settings();
  if (s_task) {
    xTaskNotifyGive(s_task);
  }
  return self_get_handler(req);
}

int fleet_register_handlers(httpd_handle_t server) {
  const httpd_uri_t uris[] = {
      {.uri = "/api/fleet", .method = HTTP_GET, .handler = fleet_get_handler},
      {.uri = "/api/fleet/self",
       .method = HTTP_GET,
       .handler = self_get_handler},
      {.uri = "/api/fleet/self",
       .method = HTTP_POST,
       .handler = self_post_handler},
  };
  int n = 0;
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    n += httpd_register_uri_handler(server, &uris[i]) == ESP_OK;
  }
  return n;
}
