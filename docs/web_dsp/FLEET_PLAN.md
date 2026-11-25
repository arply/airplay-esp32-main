# Fleet — several arply devices (Phase 8 of PLAN.md)

> Same rules as `PLAN.md`: one task at a time, tick the boxes, commit after
> each task, every phase builds, passes format/lint and is tested on real
> boards before the next one starts. Two boards are available for testing
> (`DUT` + one more); nothing here can be tested with a subwoofer yet.

**Goal:** arply devices on one network find each other. One of them also
answers as `arply.local` and shows an overview of all devices, grouped by
room. Later: actions across devices (via the hub), a shared certificate
authority, a subwoofer role, and calibrating a whole room at once.

**Order (decided 2026-11-24):** F1 discovery first, then F2 the overview
page. The overview follows a mock Till provides (iPhone + desktop); F2 does
not start before the mock is in `docs/web_dsp/`.

---

## Concept (from PLAN.md, with the decisions made here)

- **Discovery:** each device announces `_arply._tcp` (port 80) via mDNS
  with TXT `id`, `name`, `room`, `role`, `fw`, `hub`. Each device browses
  for the others every 20 s and keeps a peer table. Last known IPs are
  cached in NVS as a fallback for networks where multicast is unreliable.
- **Names:** display names stay free (`Living Room`). The `.local` host
  name is derived from it with the DHCP rule (lower case, anything but
  letters and digits → `-`), so a display name with spaces still gives a
  usable URL (`living-room.local`). Today mDNS announces the raw name; that
  changes in F1.
  - `arply` is reserved for the hub. A device whose name maps to `arply`
    (the factory default is `ARPLY`!) uses `arply-<last 4 MAC hex>` as its
    host name instead, and renaming to `arply` is refused.
  - Renaming to a host name another device already uses is refused (409).
- **Rooms:** `room` is free text (≤ 32 chars). It groups devices; later
  phases act per room.
- **Hub:** exactly one device also answers `arply.local`
  (`mdns_delegate_hostname_add`). The user picks it. The pick has to win
  everywhere without writing to the other devices (that needs the fleet
  key from F3), so `hub` is a sequence number, not a flag: picking a
  device sets its `hub` to one more than the highest value it has seen.
  Every device computes the same winner from the same table: the highest
  `hub` among the devices currently seen, ties and "nobody picked" going to
  the lowest `id`. If the picked hub disappears, the next one takes over
  until it returns.
- **Overview (F2):** the hub serves the overview when the request's `Host`
  is `arply.local`, its own settings page otherwise. Read-only data comes
  from the hub's own peer table, so F2 needs no proxy and no cross-origin
  requests. The device certificates already carry `arply.local` in their
  SAN (task 4.2).
- **Later phases** (each gets its own detailed section before it starts):
  - **F3 Pairing + proxy:** a fleet key, exchanged when a device joins
    (confirmed on the device's own page), authenticates hub → device calls.
    The hub forwards multi-device actions over HTTP inside the LAN.
  - **F4 Room actions:** room volume, mute, rename room, copy calibration.
  - **F5 Role `bass`:** mono sum → Linkwitz-Riley 4th order low-pass
    (60–150 Hz, per room) + own EQ, level, delay, polarity; the main
    speakers of that room get the matching high-pass; bass enhancement off
    in that room. Needs a board on a subwoofer to test.
  - **F6 Joint calibration:** one recording, sweeps device by device →
    per-device response, arrival time and level → delay and gain alignment.
    **Open research question first:** arrival times need a common time
    base between devices (NTP is too coarse; PTP only runs during AirPlay
    sessions). Spike before planning.
  - **F7 Fleet CA:** one CA for all devices so the phone trusts all of them
    with one profile. Open question: where the CA key lives and how it
    survives the loss of that device.

---

## F1 — Discovery, names, rooms, hub election

New module `main/network/fleet.c/.h`, started from `start_airplay_services()`
right after `mdns_airplay_init()` (mDNS must be up).

### Task F1.1 — Host names

- [ ] One shared helper for the DNS name of a device (`wifi.c` has
      `sanitize_hostname()`, `tls_cert.c` a copy): lower case, non-alnum →
      `-`, trimmed; `arply` → `arply-<mac4>`.
- [ ] `mdns_hostname_set()` uses it; the AirPlay instance names keep the
      display name. The TLS certificate uses the same helper (a changed
      host name regenerates the certificate, as today).
- [ ] `POST /api/device/name`: refuse names mapping to `arply` (400) and
      names whose host name a seen peer uses (409).

### Task F1.2 — Announce `_arply._tcp`

- [ ] Settings: `room` (string ≤ 32, NVS `room`), `role` (`main` | `bass`,
      NVS `role`), `hub` sequence (u32, NVS `hub_seq`).
- [ ] Register `_arply._tcp` on port 80 with TXT `v=1`, `id` (MAC, 12 hex),
      `name`, `room`, `role`, `fw`, `hub`. Update TXT items in place on
      change (`mdns_service_txt_item_set`).

### Task F1.3 — Peer table and election

- [ ] Task (prio 2, 4 KB): every 20 s `mdns_query_ptr("_arply", "_tcp",
      2000 ms, 16)`; parse TXT + IPv4 into a table of ≤ 16 peers, own
      entry excluded by `id`. A peer not seen for 3 rounds is dropped.
- [ ] Election as above after every round and on every local change.
      Winner = self → `mdns_delegate_hostname_add("arply", own IPv4)`;
      otherwise remove the delegation. Log every change of hub.
- [ ] Re-run election immediately on `POST /api/fleet/self`.

### Task F1.4 — Last-known cache

- [ ] NVS blob `fleet_peers` (`id`, `ip`, `name`), written only when the
      set of peers changes. A cached peer missing from mDNS is probed with
      `GET http://<ip>/api/fleet/self` (800 ms timeout, ≤ 4 per round) and
      kept if it answers with the same `id`.

### Task F1.5 — API and settings page

- [ ] `GET /api/fleet` → `{self:{id,name,host,room,role,fw,hub}, hub:<id>,
      is_hub, peers:[{id,name,host,ip,room,role,fw,hub,age_s,via}]}`
      (`via`: `mdns` | `cache`).
- [ ] `GET /api/fleet/self` → `self` only (used by the cache probe).
- [ ] `POST /api/fleet/self` `{room?, role?, hub?: true}`.
- [ ] Settings page: room field, role select (bass marked “coming later”),
      “Use this device for arply.local” button, list of the other devices
      found (name, room, link).

**Hardware test (2 boards):** each lists the other within ~20 s;
`arply.local` resolves to the elected hub and moves when another device is
picked; unplugging the hub hands `arply.local` to the other board within
~60 s and back when it returns; renaming to `arply` or to the other board's
name is refused; a name with spaces gives a working `name-with-dashes.local`;
room/role changes show up on the other board within one round.

---

## F2 — Overview at arply.local (waits for the mock)

Outline: `data/www/overview.html` in the arply.de design (mock), served by
the hub for `Host: arply.local`; rooms as cards, device rows linking to
`https://<host>.local/`, data from `GET /api/fleet` of the hub, refreshed
every 10 s. To be detailed when the mock is in the repo.
