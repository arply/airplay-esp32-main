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
  for the others every 10 s and keeps a peer table. Last known IPs are
  cached in NVS as a fallback for networks where multicast is unreliable.
- **Names:** display names stay free (`Living Room`). The `.local` host
  name is derived from it with the DHCP rule (lower case, anything but
  letters and digits → `-`), so a display name with spaces still gives a
  usable URL (`living-room.local`). Today mDNS announces the raw name; that
  changes in F1.
  - `arply` is reserved for the hub. A device whose name maps to `arply`
    (the factory default is `ARPLY`!) uses `arply-<last 4 MAC hex>` as its
    host name instead, so no name needs to be refused for it.
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
  - ~~**F7 Fleet CA:**~~ **dropped (2026-11-25, Till).** One CA for all
    devices would spare one certificate profile per device, but a CA the
    phone trusts is trusted for every website, and its key would sit on
    devices that take firmware updates over Wi-Fi without any check, so
    anything on the network could read it out. Name constraints
    (`.local` only) would limit that, but iOS enforcing them for
    user-installed roots is not documented. Too much risk for little
    benefit; per-device certificates stay. Revisit only after signed
    OTA updates.

---

## F1 — Discovery, names, rooms, hub election

New module `main/network/fleet.c/.h`, started from `start_airplay_services()`
right after `mdns_airplay_init()` (mDNS must be up).

### Task F1.1 — Host names

- [x] One shared helper for the DNS name of a device (`wifi.c` has
      `sanitize_hostname()`, `tls_cert.c` a copy): lower case, non-alnum →
      `-`, trimmed; `arply` → `arply-<mac4>`.
- [x] `mdns_hostname_set()` uses it; the AirPlay instance names keep the
      display name. The TLS certificate uses the same helper (a changed
      host name regenerates the certificate, as today).
- [x] `POST /api/device/name`: refuse names whose host name a seen peer
      uses (`success: false` with a message, like the other errors there).

### Task F1.2 — Announce `_arply._tcp`

- [x] Settings: `room` (string ≤ 32, NVS `room`), `role` (`main` | `bass`,
      NVS `role`), `hub` sequence (u32, NVS `hub_seq`).
- [x] Register `_arply._tcp` on port 80 with TXT `v=1`, `id` (MAC, 12 hex),
      `name`, `room`, `role`, `fw`, `hub`. Update TXT items in place on
      change (`mdns_service_txt_item_set`).

### Task F1.3 — Peer table and election

- [x] Task (prio 2, 4 KB): every 10 s `mdns_query_ptr("_arply", "_tcp",
      2000 ms, 16)`; parse TXT + IPv4 into a table of ≤ 16 peers, own
      entry excluded by `id`. A peer not seen for 3 rounds is dropped.
- [x] Election as above after every round and on every local change.
      Winner = self → `mdns_delegate_hostname_add("arply", own IPv4)`;
      otherwise remove the delegation. Log every change of hub. Taking the
      name needs 3 scheduled wins in a row, releasing it is immediate, so
      after a new pick the old hub has let go (≤ 12 s) before the new one
      claims (≥ 20 s): never two devices announcing `arply.local`.
- [x] Re-run election immediately on `POST /api/fleet/self`.

### Task F1.4 — Last-known cache

- [x] NVS blob `fleet_peers` (`id`, `ip`, `name`), written only when the
      set of peers changes. A cached peer missing from mDNS is probed with
      `GET http://<ip>/api/fleet/self` (800 ms timeout, ≤ 4 per round) and
      kept if it answers with the same `id`.

### Task F1.5 — API and settings page

- [x] `GET /api/fleet` → `{self:{id,name,host,room,role,fw,hub}, hub:<id>,
      is_hub, peers:[{id,name,host,ip,room,role,fw,hub,age_s,via}]}`
      (`via`: `mdns` | `cache`).
- [x] `GET /api/fleet/self` → `self` only (used by the cache probe).
- [x] `POST /api/fleet/self` `{room?, role?, hub?: true}`.
- [x] Settings page: room field, role select (bass marked “coming later”),
      “Use this device for arply.local” button, list of the other devices
      found (name, room, link).

**Results (2026-11-25, `DUT` 192.168.86.29 on `arply_v1`, `Arply`
192.168.86.31 on `arply_v1_usbdac`, both updated over the air):**
- Each lists the other within one round (age 1–7 s), via mDNS.
- `Arply` maps to `arply-68b4.local` (reserved name rule); `dut.local`,
  `arply-68b4.local` and `arply.local` (→ the hub) resolve.
- Hub: DUT by lowest id at first. Picking Arply: DUT released after ~10 s,
  Arply claimed at ~40 s, never both. Picking DUT again: claimed at ~30 s.
- Renaming Arply to `dut` is refused; a room set on DUT shows on Arply
  within 6 s; invalid role → 400. Arply still works as a USB speaker.
- **Stale caches:** the releasing hub sends no goodbye for `arply.local`,
  so a Mac kept resolving it to the old hub until the 120 s TTL ran out.
  F2 handles this: a device that is not the hub redirects
  `Host: arply.local` requests to the current hub.
- **Not tested yet:** failover when the hub is unplugged (needs ≥ 60 s
  off power), the cached-address probe (needs a network where multicast
  fails), a display name with spaces end to end.

**Hardware test (2 boards):** each lists the other within ~10 s;
`arply.local` resolves to the elected hub and moves when another device is
picked; unplugging the hub hands `arply.local` to the other board within
~60 s and back when it returns; renaming to `arply` or to the other board's
name is refused; a name with spaces gives a working `name-with-dashes.local`;
room/role changes show up on the other board within one round.

---

## F2 — Overview at arply.local

Design: `overview-mock.html` (cherry-picked from the remote branch).
Built 2026-11-25 as `data/www/overview.html`.

- [x] Every device serves the overview for `Host: arply.local` (not only
      the hub): each one knows the whole fleet, and a phone whose mDNS cache
      still points at a former hub gets the same page. This replaces the
      planned redirect.
- [x] More state in the TXT record, updated each round only when it
      changes: `ch` (channel mode), `cal` (time of the saved EQ; the page
      sends `saved_at` on save, the device has no clock), `pbe`, `play`,
      `wifi` (good ≥ −60 dBm, fair ≥ −72, else poor).
- [x] Peers that stop answering stay in the table as offline (the page
      shows "offline since" from `age_s`); the NVS cache keeps host, room,
      role and channel, so devices offline at boot still appear.
- [x] `stand_in`: the device that would take over the hub.
- [x] Page per the mock: rooms as cards (summary, Playing / Calibrated /
      offline chips, device rows with Left/Right/Mono/Stereo/Bass and Hub
      tags, link to each device), "New on the network" for devices without a
      room, Network panel (hub, stand-in, discovery, certificate download),
      firmware panel only when versions differ. Real logo, system fonts,
      dark mode. "Calibrate" opens a single device's calibration page.
- **Left out until the writes exist (F3/F4):** room volume, Mute, Edit, New
  room, Add device, Assign room, Change hub, Update all, language switch.
  The mock's buttons for those are not shown rather than shown inert.

**Incident (2026-11-25):** the first F2 build put the larger peer cache on
the 4 KB stack of the fleet task (`update_cache`, ~2.5 KB plus an NVS
write). Both boards crashed ~12 s after every boot, too early for OTA, and
had to be recovered over USB (BOOT+RESET). Fixed: the array is static, the
task has 8 KB. **App rollback is now enabled**
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): an OTA image is confirmed after
60 s of uptime, an image that crashes earlier is rolled back by the
bootloader. Verified: after an OTA DUT boots from `ota_1`, and after a later
restart again from `ota_1` (confirmed, no rollback).

**Tested on both boards:** each serves the overview for `arply.local`;
`/api/fleet` carries the new fields; a calibration saved on DUT shows on
Arply with its date within one round; a restart with cached peers works.
**Pending:** "offline since" with a board unplugged for a minute; playing
state while streaming; the page on the iPhone.

---

## F3/F4 — Proxy and room actions (first part done 2026-11-25)

**No pairing:** with F7 dropped, nothing on the devices needs protecting
from the rest of the LAN that is not already open there (every API, OTA
included, answers anyone on the network). A fleet key would only matter
together with locking those APIs, which needs a PIN for the user too.

- [x] **Proxy** `GET/POST /api/fleet/proxy?id=<peer>&to=self|volume` on
      every device (the overview uses the hub's): forwards a JSON body of
      < 256 B over plain HTTP to the peer's `/api/fleet/self` or
      `/api/audio/volume`, 3 s timeout, returns the peer's status and body.
      Only those two targets; 404 for a device that is not online. The page
      calls its own device directly (its httpd would wait on itself). A
      proxied change of `self` wakes the hub's fleet task, so the overview
      shows it within ~2 s instead of a round.
- [x] **Overview:** room volume (all online devices of the room, the same
      dB; persisted on release), "Rename room" (every online device of the
      room; offline ones keep the old name and are reported), "Assign
      room" for devices without one (field with the existing rooms), "Make
      hub" in the Network panel. No redraw while a slider moves or a field
      has focus; refresh 1.5 s and 6 s after a change.
- Tested: jsdom against a fake hub (each action sends the right requests,
  own device direct, peer via proxy); on the boards via DUT as proxy: GET
  over HTTP and HTTPS, 400/404, Arply's room changed and back, visible on
  DUT after 2 s.
- [x] **Update all from arply.de** (Till chose the website as source):
      `assets/firmware/ota/manifest.json` names the version, the pages and
      the app image per build; the files live in `ota/<version>/`.
      `GET /api/update` reports current/latest (background check at most
      every 10 min), `POST /api/update {"install": true}` downloads the app
      into the free slot, then the pages as `.new`, swaps them only when
      all arrived and reboots (failure: old firmware and pages stay;
      crash of the new image: rollback). Refuses while playing. The
      overview offers "Update all": one device at a time through the
      proxy, followed until it is back with the new version, hub last;
      devices without the endpoint are reported for the web flasher.
      Release: bump `version.txt`, `scripts/publish_ota.sh`, commit and
      push the website's `ota` folder. The web flasher's merged image is
      separate (stays 0.2.0 until it is updated on purpose).
      Tested: DUT updated itself from arply.de to 0.2.1 in ~45 s (app
      26 s, pages 13 s, restart) and stayed on it past the rollback window;
      jsdom: order (hub last), progress, an old device reported.
- [ ] Mute, copy calibration: not built (iOS mutes AirPlay groups itself;
      copying only helps for identical speakers in similar places).

---

## F5 — Subwoofer role (done 2026-11-25, before F3/F4)

Built without pairing or a proxy: everything a speaker needs to know about
its subwoofer is in the subwoofer's mDNS TXT record.

- [x] **DSP** (`dsp_xover_t` in `dsp_config_t`): Linkwitz-Riley 4th order
      (two Butterworth sections) appended to the EQ chain, low-pass or
      high-pass, 40–200 Hz. A low-pass also sums L+R to mono. Level
      (±12 dB) and polarity fold into the preamp factor; a delay line on
      the input (0–20 ms, 961 stereo int16 frames, 3.8 KB) delays a
      subwoofer that stands nearer than the speakers.
- [x] **Profile v3:** `sub {xo, gain_db, delay_ms, invert}` (default 80 Hz),
      v2 profiles load with these defaults. `/api/dsp` reads and writes
      `sub` and reports `role` and `room_hp`.
- [x] **Fleet:** a device with role `bass` announces its saved crossover
      as TXT `xo`. After every round each device calls
      `audio_dsp_set_room()`: role `bass` → mono, set M, low-pass with the
      sub settings, bass block off; otherwise, if an online peer with role
      `bass` in the same (non-empty, case-insensitive) room is **playing**
      → high-pass at its `xo`, bass block off. Only while it plays, so a
      speaker AirPlayed alone keeps its full range. "Playing" now includes
      arply's own signals (listening sample, sweeps). A start or stop of
      playback triggers a round at once (polled every second), announced
      before browsing.
- [x] **Pages:** the role is selectable on the settings page. On a
      subwoofer the calibration page shows a Subwoofer panel (crossover,
      level, delay with cm, polarity) instead of the bass block, locks the
      output to mono, levels measurements on 30 Hz…crossover instead of
      200 Hz–2 kHz, fits the EQ up to 2× the crossover and draws the
      low-pass into the chart. A speaker next to a playing subwoofer shows
      the high-pass and a note in the bass panel.
- Host tests: −6.02 dB at the crossover, −48.2 dB two octaves away, low- +
  high-pass sum flat within 0.005 dB (40–160 Hz), mono sum, polarity,
  +6 dB level, 10 ms delay to the sample.
- **Bench test** (`DUT` as subwoofer in Arply's room, captured with the
  debug probe, crossover 80 Hz): DUT out L = R, 60–90 Hz −4.2, 90–120
  −10.7, 180–300 −33.7 dB; Arply 20–45 −23.8, 60–90 −7.7, 120–180 −0.8,
  mids 0.0 dB. Arply switched 1 s after DUT started playing, back 2 s
  after it stopped.
- Bypass (A/B on the page) drops EQ and bass block but keeps the crossover,
  its level, polarity and delay (Till); checked on DUT: same low-pass with
  bypass on.
- **Open:** a real subwoofer and AirPlay multi-room listening; with USB audio on a speaker
  the subwoofer gets nothing, so a playing subwoofer would still take the
  speaker's bass (only if it plays something else at the same time).

---

## F6 — Time base spike (done 2026-11-25)

Question: can sweeps on several devices start at one moment precisely
enough (~1 ms, the crossover to a subwoofer) for a joint calibration, and
does what such a measurement finds hold for AirPlay playback?

- **Clock comparison** (`fleet_clock.c`, UDP 7123): 32 exchanges, the one
  with the shortest round trip; RTT 4–6 ms over Wi-Fi, offsets repeat
  within ~0.6 ms. `GET /api/fleet/clock?id=` on any device.
- **Scheduled sweeps** (`POST /api/dsp/sweep {"at_us"}`): first with the
  AirPlay model (average DMA occupancy + 5 ms): ±3 ms, the descriptor
  phase. Then `audio_output_next_play_us()` (bytes written vs bytes the
  DMA has sent, on_sent ISR; base = one ring after enable, ≥ one
  descriptor after an idle writer — the driver's queue of DESC_NUM-1
  freed descriptors): two sweeps on one device land within **0.01 ms**.
  The real DAC delay varies 41.3–45.7 ms (DUT, 44.1 kHz) and 37.9–40.6 ms
  (Arply, 48 kHz).
- **Joint, Mac microphone** (`scratchpad/f6/joint.py`): DUT, then Arply
  6 s later, 8 runs: Arply **+6.89 ms** after plan, **std 0.21 ms**
  (acoustic path and speaker delay to the microphone, plus any bias of
  the clock comparison).
- **AirPlay check** (Music app on the Mac to DUT + Arply, test file with
  sweeps on L at 20/40 s and R at 26/46 s, DUT previewed to "left",
  Arply to "right", both flat): Arply **+5.9…+6.0 ms** in two sessions —
  within ~1 ms of the scheduled measurement. A calibration from
  scheduled sweeps holds for AirPlay playback. In the first seconds after
  a start the offset still moves (PTP settling: ptp_gap 38–80 ms on
  Arply); stop/play in quick succession made DUT miss its first seconds.
- **Start servo:** a late start now shrinks 3 samples per frame in the
  first 30 s (DUT: -6 ms back inside 1.5 ms after ~1 s, 261 trims). Early
  starts still stretch 1 sample per 4 frames (Arply +14 ms took 16 s): the
  output buffers hold only one spare sample.

## F6 — "Align with the speakers" (first version, 2026-11-25)

On the subwoofer's calibration page (Subwoofer panel):
- The subwoofer compares clocks with every online speaker of its room,
  reads their volumes (proxy `to=volume`), schedules one sweep per speaker
  (proxy `to=sweep`, new) and then itself, 5 s apart from 2.5 s on; the
  phone records once.
- One deconvolution of the whole recording; anchor = the first speaker's
  peak; every slot is cut out at its planned place (20 ms before, 300 ms
  long), so relative arrival survives; speakers are summed, each divided
  by its volume gain ((dB+30)/30)², the subwoofer too: equal volumes.
- `DSP.alignSub()`: LR4 high-pass on the speakers, LR4 low-pass on the
  subwoofer (raw sweeps, the devices' EQs are not included); level equal
  at the crossover (±1/6 octave), then delay 0–20 ms × polarity for the
  most energy from xo/2 to 2 xo; of the local maxima within 0.2 dB of
  the best the earliest (not a period later). Reports how far the sum is
  from a perfect in-phase sum and, if a negative delay would be > 1 dB
  better, that the subwoofer stands too far away.
- The result goes into the Subwoofer panel as a live preview; "Save to
  arply" keeps it.
- **One run for both (Till, same day):** on a subwoofer, step 1's
  "Measure" is this joint run; there is no separate align button. Each
  measurement stores the subwoofer's response (levelled on 30 Hz…xo) and
  the alignment spectra (20–400 Hz, `alignSpectra`), so: level, delay
  and polarity come from all positions together (`alignRuns` sums the
  energy over them), the room-mode cuts from their average, range 20 Hz
  to 2× the crossover, cuts only, and a new crossover re-aligns and
  re-fits without measuring again. The Advanced section (target, range,
  manual filters) and the filter editor are hidden; step 2 is "Room
  modes", a list of the cuts. jsdom with the three real Mac recordings:
  three measurements accumulate to +6.5 dB, 1.4 ms, inverted (the
  devices' volumes were -5.5 and -20.9 dB and are corrected), 6 cuts;
  crossover 200 → 120 Hz re-aligns to 0.9 ms and re-fits 20–240 Hz.
- Tests: synthetic responses (flat, sub 2 ms earlier, inverted, -6 dB →
  2.0 ms, inverted, +6.2 dB, sum perfect; speaker roll-offs → sum within
  0.1 dB). On the boards with the Mac microphone (Arply as subwoofer at
  200 Hz, DUT as speaker), 3 runs: +6.6…+6.8 dB, 1.25–1.40 ms,
  inverted, sum 0.6 dB short of ideal.
- Open: the iPhone in the real listening spot; EQs of speakers and
  subwoofer in the calculation; more than one subwoofer.
