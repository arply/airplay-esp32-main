# web_dsp — Room calibration, DSP, boot chime and fleet

> **For agentic workers:** work through the phases in order, one task at a time.
> Steps use `- [ ]` — tick them off in this file as you go and commit after each
> task. Every phase ends in a firmware that builds (`pio run -e arply_v1`),
> passes `scripts/format.sh` / `scripts/lint.sh`, and can be flashed and tested
> on hardware on its own. **Do not start a phase before the previous one has
> been tested on a real board.** Follow `CLAUDE.md` (LLVM style, 2-space indent,
> 80 columns, components with their own `CMakeLists.txt`).

**Goal:** arply measures the room with an iPhone, corrects it with an on-device
parametric EQ (plus optional psychoacoustic bass), and is configured from a web
page in the arply.de design. Later: a boot chime, a safer setup hotspot, and an
overview page for several arply devices.

**Reference material in this folder** (built and tested in the browser, not on
hardware):

| File | What it is |
|---|---|
| `calibration-prototype.html` | Working calibration page: sweep measurement, level check, stereo/mono/left/right, auto-EQ fit, frequency-dependent smoothing, bass enhancement, built-in listening samples, JSON export. **The DSP maths in its `/*DSP-START*/…/*DSP-END*/` block is the reference implementation** — port it, don't reinvent it. |
| `design-language.md` | arply.de design tokens (colours, type, components). German. All device UI text is **English**. |
| `chime.c`, `chime.h`, `gen_chime.py` | Boot chime (“F5 sparkle”, 2.0 s, −18 dBFS) and its generator. |

The real logo is `docs/arply-logo.jpg` (geometric wordmark built from circles
and straight stems).

---

## What the code looks like today (read before starting)

- **Output path** — `main/audio/audio_output.c`, `playback_task()` (core 1,
  prio 7, **4 KB stack**):
  `audio_receiver_read()` → optional resample → `apply_volume()` (int16, Q15
  ramp) → `apply_channel_mode()` → `led_audio_feed()` → `i2s_channel_write()`.
  Frames of up to `FRAME_SAMPLES + 1` = 353 stereo samples.
- **I2S** is 16-bit stereo Philips at `CONFIG_OUTPUT_SAMPLE_RATE_HZ`
  (44100 default, 48000 optional). The PCM5102A accepts 32-bit slots.
- **Channel modes** (`audio_channel_mode_t`): STEREO, LEFT, RIGHT, MONO.
  ⚠️ LEFT/RIGHT route the chosen *source* channel to **both** outputs. The
  prototype assumed “left only = only the left jack plays”. The firmware
  semantics win; the page must be adapted (see Phase 2).
- **Mute** — `components/boards/arply-v1/board.c` keeps XSMT low except while
  RTSP reports PLAYING. Anything that plays outside AirPlay (sweep, noise,
  chime) must unmute through a new board hook.
- **Web server** — `main/network/web_server.c`: plain HTTP on port 80,
  `max_open_sockets = 3`, `max_uri_handlers = 30`, 8 KB stack, wildcard URI
  matching, pages served from SPIFFS (`data/www/*`, flashed with
  `-t uploadfs`). JSON via cJSON. Handlers read bodies into **512-byte**
  buffers — too small for DSP configs.
- ⚠️ `/api/eq` and `data/www/eq.html` already exist for the (removed) TAS58xx
  15-band hardware EQ, behind `CONFIG_DAC_TAS58XX`. **Use `/api/dsp`** to
  avoid confusion.
- **Settings** — `main/settings.c`, NVS namespace `"airplay"`, keys ≤ 15
  chars. NVS writes stall the flash cache; `audio_output.c` survives that via
  `auto_clear`, but **never write NVS from the audio task** and never on every
  slider move.
- **mDNS** — `main/network/mdns_airplay.c` sets the device name as hostname
  (so a device named `bassmonster` is already `bassmonster.local`) and
  registers `_airplay._tcp` / `_raop._tcp`.
- **Existing web UI** (`data/www/index.html`) uses an Apple-like look
  (`#f5f5f7`, `#0071e3`). New pages use the arply.de design; restyling the
  existing pages is a separate, later task.
- AirPlay **2** is implemented, so multi-room sync is handled by iOS.

---

## Phase index

| Phase | Title | Depends on |
|---|---|---|
| **0** | Spikes: HTTPS + iPhone microphone, CPU budget | – |
| **1** | DSP core component (EQ, preamp, limiter) | 0 |
| **2** | DSP settings, `/api/dsp`, live preview | 1 |
| **3** | Test signals from the device (sweep, noise) | 2 |
| **4** | Calibration page served by arply | 2, 3 (0 for live mic) |
| **5** | Bass enhancement on the device | 1 |
| **6** | Boot chime | 3 (signal source + unmute hook) |
| **7** | Setup hotspot hardening | – |
| **8** | Fleet: discovery, overview, rooms, bass role | 2–4 (own plan later) |

---

## Phase 0 — Spikes (answer the two risky questions first)

### Task 0.1 — HTTPS and `getUserMedia` on iPhone Safari

The calibration page needs the microphone, and Safari only allows it in a
secure context. Find out *now* whether a self-signed certificate on the ESP is
enough.

- [ ] Add `esp_https_server` (ESP-IDF component `esp_https_server`) next to the
      existing HTTP server, port 443, `max_open_sockets = 2`. Keep port 80
      unchanged.
- [ ] For the spike, embed a self-signed cert/key (EC P-256, CN = device
      hostname, SAN = `<hostname>.local` and `arply.local`) via
      `EMBED_TXTFILES`. Generate with `openssl` and document the command.
- [ ] Serve a minimal page at `https://<host>.local/mictest` that calls
      `getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,
      autoGainControl:false}})` and shows a live RMS level.
- [ ] On an iPhone: open the page, accept the certificate warning, check
      whether the mic prompt appears and levels move. Also test once with the
      certificate installed as a trusted profile (Settings → General → About
      → Certificate Trust Settings).
- [ ] Log free internal RAM / PSRAM before and after a TLS handshake while
      AirPlay is playing (`heap_caps_get_free_size`). Check for audio dropouts
      during the handshake.
- [ ] Write the result into this file under “Spike results”. **Decision:**
      - works with accepted warning → Phase 4 uses a per-device self-signed
        cert generated on first boot;
      - works only with trusted profile → Phase 4 needs the profile download
        (and Phase 8's fleet CA moves earlier);
      - doesn't work / too much RAM → Phase 4 uses the import route only
        (record with Voice Memos, import file) and live measuring stays off.

### Task 0.2 — CPU budget for float biquads

- [ ] Temporarily add 20 float biquads (10 per channel, DF2T) in
      `playback_task()` after resampling, operating on a float copy of the
      frame. Measure with `esp_timer_get_time()` around the block, log
      average and max µs per frame every 5 s.
- [ ] Do it at 44.1 kHz and with 48 kHz resampling enabled, while streaming
      AAC (AirPlay 2) and ALAC.
- [ ] Budget: one frame of 352 samples lasts ~8 ms. Target < 0.8 ms (10 %)
      for EQ + bass enhancement. If plain C is too slow, use
      `espressif/esp-dsp` (`dsps_biquad_f32`, S3-optimised) on de-interleaved
      buffers.
- [ ] Record results under “Spike results”. Remove the spike code.

### Spike results

_(fill in)_

---

## Phase 1 — DSP core component

New component `components/dsp/` (`dsp.h`, `dsp.c`, `dsp_biquad.c`,
`CMakeLists.txt`). Pure C, no ESP-IDF dependencies in the maths so it can be
unit-tested on the host.

### Task 1.1 — Biquad design and processing

- [ ] Port `coeffs()` from the prototype (RBJ cookbook): types `peak`, `ls`,
      `hs` (shelves fixed at Q = 1/√2, S = 1), `hp`, `lp`. Compute
      coefficients **on the device** for the actual output rate; ignore any
      coefficients the client sends.
- [ ] Process with transposed direct form II in `float`, state per channel
      per filter. Denormal guard (add 1e-20 or flush).
- [ ] Limits: `DSP_MAX_FILTERS_PER_CH = 10`, f 20–20000 Hz (clamp below
      0.45·fs), gain −18…+6 dB, Q 0.3–12.

### Task 1.2 — Config, double buffering, processing entry point

- [ ] `dsp_config_t`: version, `preamp_db`, `bypass`, per output channel
      (`DSP_CH_L`, `DSP_CH_R`) an array of `dsp_filter_t {type, f, gain_db, q,
      enabled}` + count, bass-enhancement params (Phase 5).
- [ ] Two prepared runtime states (coefficients + zeroed history). The web task
      prepares the inactive one and swaps a pointer atomically; the audio task
      picks it up at the next frame start. No mutex in the audio path.
- [ ] On swap, reset filter history only if the filter *structure* changed;
      for parameter-only changes keep history to avoid clicks. Crossfade old →
      new over one frame if clicks are audible.
- [ ] Entry point:
      `void dsp_process(int16_t *buf, size_t frames);` (interleaved stereo,
      in place). Internally: int16 → float, preamp, filters per channel,
      soft limiter (tanh-style knee above −1 dBFS), float → int16 with TPDF
      dither. Keep the work buffer static (no allocation per frame).
- [ ] `dsp_init(uint32_t sample_rate)`, `dsp_set_config(const dsp_config_t*)`,
      `dsp_get_config(dsp_config_t*)`, `dsp_set_bypass(bool)`.

### Task 1.3 — Hook into the output path

- [ ] In `playback_task()` the new order is:
      resample → `apply_channel_mode()` → **`dsp_process()`** →
      `apply_volume()` → `led_audio_feed()` → `i2s_channel_write()`.
      Channel mode first, because the filters belong to the *outputs*
      (speakers), not to the source channels. Volume last, so the user's
      volume never changes the EQ's headroom.
- [ ] Call `dsp_init(OUTPUT_RATE)` from `audio_output_init()`.
- [ ] Check the 4 KB task stack still fits (`uxTaskGetStackHighWaterMark`);
      raise to 6 KB if needed.
- [ ] Same hook for `audio_output_spdif.c` / `audio_output_usb.c` only if
      trivial; otherwise leave a TODO (arply_v1 uses I2S).

### Task 1.4 — Optional: 24-bit output

- [ ] Evaluate switching I2S to `I2S_DATA_BIT_WIDTH_32BIT` and having
      `dsp_process()` write int32. Benefit: preamp/cuts don't cost resolution.
      Cost: double DMA bandwidth, `led_audio_feed()` and the silence buffer
      need int16 views. Only do it if Phase 0 left CPU headroom. Record the
      decision here.

### Task 1.5 — Host test

- [ ] `components/dsp/test/host_test.c` (gcc, no IDF): run a 1 kHz sine and
      a log sweep through known filters, compare measured gain with the
      analytic magnitude (`magDb()` from the prototype) at ~20 frequencies,
      tolerance 0.1 dB. Add `scripts/dsp_host_test.sh`. Not wired into CI yet.

**Hardware test:** flat config = bit-identical to before (bypass), a −12 dB
peak at 1 kHz Q 1 audibly removes a 1 kHz test tone, no clicks when toggling.

---

## Phase 2 — Settings, `/api/dsp`, live preview

### Task 2.1 — Persistence

- [ ] `settings_get_dsp_config()` / `settings_set_dsp_config()` storing a
      versioned packed struct as an NVS blob, key `"dsp_cfg"`. Unknown
      version → ignore and start flat.
- [ ] Load in `audio_output_init()` before playback starts.

### Task 2.2 — JSON API

JSON format (v2, as exported by the prototype — `b`/`a` coefficient arrays
may be present and are ignored):

```json
{
  "version": 2,
  "mode": "stereo",
  "preamp_db": -1.5,
  "channels": {
    "L": [{"type": "peak", "f": 96.8, "gain_db": -1.9, "q": 11.85}],
    "R": [{"type": "peak", "f": 234.6, "gain_db": -2.8, "q": 5.4}]
  },
  "pbe": {"on": false, "fc": 100, "amount": 0.5}
}
```

Mapping to the firmware's channel modes:

| `mode` | firmware mode | filter sets used |
|---|---|---|
| `stereo` | STEREO | `L` → left output, `R` → right output |
| `mono` | MONO | `M` → both outputs |
| `left` | LEFT | `L` → both outputs (left source on both jacks) |
| `right` | RIGHT | `R` → both outputs |

- [ ] `GET /api/dsp` → current saved config + `{"fs": <output rate>,
      "max_filters": 10, "bypass": false}`.
- [ ] `POST /api/dsp` → validate, clamp, apply, **save to NVS**, set channel
      mode via `audio_output_set_channel_mode()`. Reply with the stored
      config.
- [ ] `POST /api/dsp/preview` → same validation, apply **without saving**
      (for live A/B while dragging sliders). Rate-limit: ignore if the last
      preview was < 50 ms ago.
- [ ] `POST /api/dsp/bypass` `{"on": true}` → bypass without changing the
      config (for A/B).
- [ ] Body handling: read the full body in a loop up to 4 KB into a heap
      buffer (current handlers use a single `httpd_req_recv` into 512 bytes).
- [ ] Bump `config.max_uri_handlers` accordingly.
- [ ] Test with `curl` (document the commands here).

---

## Phase 3 — Test signals from the device

The phone records; arply plays. This avoids AirPlay latency (~2 s), the risk
that iOS moves audio off AirPlay while the mic is open, and the user's volume
setting.

### Task 3.1 — Signal source in the output task

- [ ] New `main/audio/test_signal.c/.h`: generators that fill an int16 stereo
      frame **on the fly** (no 4-second buffers):
      - exponential sine sweep 20 Hz–20 kHz, 4 s, amplitude 0.25, 10 ms
        fade-in / 5 ms fade-out — identical to `sweep()` in the prototype
        (phase from the closed form, in `double`);
      - pink noise (Paul Kellet), −15 dBFS RMS, looping;
      - routed to `L`, `R` or both outputs.
- [ ] In `playback_task()`: while a test signal is active, take samples from
      the generator instead of `audio_receiver_read()`, **skip DSP and
      volume** (raw speaker measurement at fixed level), still feed the LED.
- [ ] Refuse (HTTP 409) while AirPlay is playing. If a client starts playing
      during a test signal, stop the test signal.

### Task 3.2 — Unmute hook

- [ ] Add `iot_board_set_signal_unmute(bool)` (or a small mute arbiter in
      `board.c`): XSMT is released if RTSP is PLAYING **or** a test signal /
      chime is active. Re-mute 100 ms after the signal ends.

### Task 3.3 — API

- [ ] `POST /api/dsp/sweep` `{"channels": ["L","R"], "gap_ms": 2000}` →
      plays one sweep per listed output back to back, starting 500 ms after
      the request. Responds immediately with `{"lead_ms":500, "sweep_ms":4000,
      "gap_ms":2000, "fs":44100}` so the page knows the timeline.
- [ ] `POST /api/dsp/noise` `{"on": true, "channels": ["L","R"]}` → level
      check noise; auto-off after 30 s.
- [ ] `POST /api/dsp/stop` → stop any test signal.

---

## Phase 4 — Calibration page served by arply

### Task 4.1 — Page

- [ ] Copy `calibration-prototype.html` to `data/www/dsp.html`; serve it at
      `/dsp` (HTTP and, if Phase 0 succeeded, HTTPS).
- [ ] Remove the Google Fonts `<link>`: the device often has no internet (AP
      mode). Use `system-ui` / `ui-monospace` fallbacks. Replace the logo
      placeholder with an inline SVG traced from `docs/arply-logo.jpg`
      (circles + rectangles; keep it < 2 KB).
- [ ] On load: `GET /api/dsp` and fill the filter lists, mode and bass
      enhancement from the device.
- [ ] Measuring: replace the WebAudio sweep playback with
      `POST /api/dsp/sweep`; start recording first, then trigger. Analyse with
      the existing “import” path (it finds the first sweep peak itself and
      walks forward by sweep + gap). Level check: `POST /api/dsp/noise`.
- [ ] Output modes: “Left only/Right only” now mean the firmware semantics
      (that source channel on both jacks). Update hint texts. The sweep for
      `left`/`right`/`mono` plays on both outputs; stereo measures L then R.
- [ ] “Send to arply” → `POST /api/dsp`. While dragging a filter slider send
      `POST /api/dsp/preview` (throttled), so changes are audible on the
      speakers. The “Listen” panel's built-in samples stay browser-side.
- [ ] Keep the demo measurement (useful without a microphone).
- [ ] Link the page from `index.html` (“Room calibration”).
- [ ] Size check: the page must stay well under the SPIFFS budget and load in
      < 1 s on the device.

### Task 4.2 — Certificate (only if Phase 0 said yes)

- [ ] On first boot generate an EC P-256 key + self-signed cert with
      mbedTLS (`mbedtls_x509write_crt`), SAN = `<hostname>.local`, store in
      NVS. Regenerate when the device is renamed.
- [ ] If Phase 0 showed a trusted profile is required: serve the cert as a
      `.mobileconfig` download on the setup page.

---

## Phase 5 — Bass enhancement on the device

- [ ] Port `buildPbe()` from the prototype into `components/dsp/`:
      main path 2nd-order high-pass at `fc`; harmonic path 2× low-pass at
      `fc` → full-wave rectifier `|x|` → 2× high-pass at `fc` → low-pass at
      `4·fc` → gain `amount × 2.5` → summed back. Butterworth sections.
- [ ] Runs after the EQ, before the limiter, per output channel.
- [ ] Params from `pbe` in the JSON (`fc` 60–150 Hz, `amount` 0–1). Off by
      default.
- [ ] Host test: 50 Hz in, `fc` = 100 → 50 Hz ≈ −12 dB, 100 Hz and 200 Hz
      harmonics present (the prototype measured −12 / −21 dB at
      `amount` 0.5).
- [ ] Later (Phase 8): force off when the room has a device with role `bass`.

---

## Phase 6 — Boot chime

- [ ] Add `chime.c/.h` from this folder to `main/audio/`, embed the PCM with
      `EMBED_FILES`. Generate it with `gen_chime.py`; if
      `OUTPUT_SAMPLE_RATE_HZ` is 48000, extend the script to also render
      48 kHz and pick the matching file at build time.
- [ ] Rework `chime_play()` to use the Phase 3 signal source (play from the
      embedded PCM) instead of writing to I2S directly, so it shares the
      unmute hook and never collides with the playback task.
- [ ] Play once after `audio_output_init()` **only on power-on**
      (`esp_reset_reason() == ESP_RST_POWERON`), not after OTA/soft reset.
      Fixed level, independent of AirPlay volume.
- [ ] NVS flag `"chime_on"` (default on) + toggle in the settings page.

---

## Phase 7 — Setup hotspot hardening

Today the hotspot (`CONFIG_DEFAULT_AP_SSID`, `wifi.c`) is open and re-enables
itself after repeated STA failures.

- [ ] AP auto-off after 10 min without a connected client (keep it on while a
      client is connected). Log it.
- [ ] Optional WPA2 password for the setup AP (Kconfig + printed label);
      default stays open to not break existing setups — decide and record.
- [ ] Re-enter setup on demand without buttons: power-cycle 3× within 5 s
      (counter in RTC memory / NVS, cleared after 5 s uptime) → AP on.
- [ ] Never return stored Wi-Fi passwords from any API.

---

## Phase 8 — Fleet (outline — write a separate plan before starting)

Decided concept (see the project notes):

- **Discovery:** register `_arply._tcp` via ESP-IDF mDNS with TXT `name`,
  `room`, `role`, `fw`, `hub`, `id`. Query peers with `mdns_query_ptr`; cache
  last known IPs in NVS as fallback.
- **Names:** device names stay free (`bassmonster.local`). `arply` is
  reserved; refuse it as a device name. Check uniqueness on rename.
- **Rooms:** `room` is a free-text attribute grouping devices that belong
  together. Calibration, volume, roles and crossover act per room; updates
  act network-wide.
- **Hub:** exactly one device also answers `arply.local`
  (`mdns_delegate_hostname_add`) and serves the overview; it decides by the
  `Host` header whether to show the overview or its own page. User picks the
  hub; if it disappears, the device with the lowest id takes over until it
  returns.
- **Proxy:** the hub forwards multi-device actions to peers over HTTP inside
  the LAN, authenticated with a fleet key exchanged at pairing.
- **Fleet CA:** first device creates a CA; each device gets a cert for its
  own name **and** `arply.local`; the iPhone installs the CA profile once.
- **Role `bass`:** mono sum → Linkwitz-Riley 4th-order low-pass (60–150 Hz,
  per-room setting) + own EQ, level, delay, polarity. Main speakers in the
  room get the matching high-pass. Bass enhancement off in that room.
- **Joint calibration:** sweeps play device by device inside one room, one
  recording → per-device response + arrival time + level → per-device delay
  and gain alignment.
- UI: see the overview mock (arply.local, iPhone + desktop) — rooms as cards,
  device rows linking to `https://<name>.local`, “Calibrate” per room.

---

## Out of scope for this branch

- Restyling the existing `index.html`, `logs.html`, `speedtest.html`.
- Distortion measurement (harmonics from the sweep) — possible later; the
  impulse response already contains them at negative times.
- Per-iPhone-model microphone correction curves.
