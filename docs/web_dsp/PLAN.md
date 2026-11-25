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
| `overview-mock.html` | Static mock of the fleet overview at `arply.local` (sample data, buttons inert, responsive phone/desktop). Target design for Phase 8. |

The real logo was `docs/arply-logo.jpg` (geometric wordmark built from circles
and straight stems). Commit `bd1e2c6` (“Drop 20 MB of unreferenced binaries”)
removed it from the tree; get it back with
`git show 934c239:docs/arply-logo.jpg > /tmp/arply-logo.jpg`.

---

## What the code looks like today (read before starting)

> **Rebased 2026-11-24** onto `chore/delete-dead-code` (DAC driver layer,
> TAS57xx/58xx, SPDIF/USB output backends, BT, Ethernet and the board
> abstraction are gone). The bullets below describe that tree.

- **Output path** — `main/audio/audio_output.c`, `playback_task()` (core 1,
  prio 7, **4 KB stack**):
  `audio_receiver_read()` → optional resample → `apply_volume()` (int16, Q15
  ramp, target from `audio_output_volume_q15()`) → `apply_channel_mode()` →
  `led_audio_feed()` → `i2s_channel_write()`.
  Frames of up to `FRAME_SAMPLES + 1` = 353 stereo samples (more after
  44.1→48 kHz resampling, `MAX_RESAMPLE_FRAMES`).
- **Second source: USB DAC** (`arply_v1_usbdac` env, 48 kHz output).
  `main/audio/usb_dac_input.c` `arbiter_task()` **stops** the playback task
  while USB audio flows and writes to I2S itself via `audio_output_write()`
  after its own `apply_gain()`. It does **not** apply the channel mode. Items
  can be up to `ARBITER_MAX_DRAIN_BYTES` (16 KB = 4096 frames). Any DSP must
  be hooked in here too, and `dsp_process()` must not assume ≤ 353 frames.
  Only one of the two writers runs at a time.
- **I2S** is 16-bit stereo Philips at `CONFIG_OUTPUT_SAMPLE_RATE_HZ`
  (44100 for `arply_v1`, 48000 for `arply_v1_usbdac`). The PCM5102A accepts
  32-bit slots.
- **Channel modes** (`audio_channel_mode_t`): STEREO, LEFT, RIGHT, MONO.
  ⚠️ LEFT/RIGHT route the chosen *source* channel to **both** outputs. The
  prototype assumed “left only = only the left jack plays”. The firmware
  semantics win; the page must be adapted (see Phase 2).
- **Mute** — `components/boards/board.c` keeps XSMT low except while RTSP
  reports PLAYING. `usb_dac_input.c` and `playback_control.c` unmute by
  re-emitting `RTSP_EVENT_PLAYING`. Anything that plays outside AirPlay
  (sweep, noise, chime) must unmute through a new board hook.
- **Web server** — `main/network/web_server.c`: plain HTTP on port 80,
  `max_open_sockets = 3`, `max_uri_handlers = 30` (26 used incl.
  `log_stream`), 8 KB stack, wildcard URI matching with a `/*` captive-portal
  catch-all registered **last**, pages served from SPIFFS (`data/www/*`,
  flashed with `-t uploadfs`). JSON via cJSON. A `recv_body(req, max_len)`
  helper (full-length read into a heap buffer) already exists; some older
  handlers still use a single `httpd_req_recv` into small stack buffers.
- `/api/eq`, `eq.html` and the TAS58xx EQ are gone after the rebase. The new
  API is still called **`/api/dsp`**.
- **Flashing** — the `arply_v1_usbdac` image has no console and no auto-reset:
  flashing it needs BOOT+RESET by hand and a physical replug after the USB
  descriptor set changes. Develop on `arply_v1` (USB Serial/JTAG console,
  esptool auto-reset), then verify the USB path once at the end.
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

- [x] Add `esp_https_server` (ESP-IDF component `esp_https_server`) next to the
      existing HTTP server, port 443, `max_open_sockets = 2`. Keep port 80
      unchanged.
- [x] For the spike, embed a self-signed cert/key (EC P-256, CN = device
      hostname, SAN = `<hostname>.local` and `arply.local`) via
      `EMBED_TXTFILES`. Generate with `openssl` and document the command.
- [x] Serve a minimal page at `https://<host>.local/mictest` that calls
      `getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,
      autoGainControl:false}})` and shows a live RMS level.
- [x] On an iPhone: open the page, accept the certificate warning, check
      whether the mic prompt appears and levels move. Also test once with the
      certificate installed as a trusted profile (Settings → General → About
      → Certificate Trust Settings). **Done in use (2026-11-24/25):** Till
      measured on the iPhone over HTTPS with the accepted warning many times;
      the trusted-profile variant was not needed.
- [ ] Log free internal RAM / PSRAM before and after a TLS handshake while
      AirPlay is playing (`heap_caps_get_free_size`). Check for audio dropouts
      during the handshake.
- [x] Write the result into this file under “Spike results”. **Decision
      (from use):** works with the accepted warning, so per-device
      self-signed certificates (Phase 4.2) stay; the fleet CA was dropped
      (FLEET_PLAN F7). **Decision:**
      - works with accepted warning → Phase 4 uses a per-device self-signed
        cert generated on first boot;
      - works only with trusted profile → Phase 4 needs the profile download
        (and Phase 8's fleet CA moves earlier);
      - doesn't work / too much RAM → Phase 4 uses the import route only
        (record with Voice Memos, import file) and live measuring stays off.

### Task 0.2 — CPU budget for float biquads

- [x] Temporarily add 20 float biquads (10 per channel, DF2T) in
      `playback_task()` after resampling, operating on a float copy of the
      frame. Measure with `esp_timer_get_time()` around the block, log
      average and max µs per frame every 5 s.
- [ ] Do it at 44.1 kHz and with 48 kHz resampling enabled, while streaming
      AAC (AirPlay 2) and ALAC.
- [x] Budget: one frame of 352 samples lasts ~8 ms. Target < 0.8 ms (10 %)
      for EQ + bass enhancement. If plain C is too slow, use
      `espressif/esp-dsp` (`dsps_biquad_f32`, S3-optimised) on de-interleaved
      buffers.
- [x] Record results under “Spike results”. Remove the spike code.

#### Detail (Phase 0)

- **0.1 files:** `main/network/https_server.c/.h` (`https_server_start()`,
  called next to `web_server_start(80)`), `main/network/mictest.html`
  embedded with `EMBED_TXTFILES`, cert/key in `main/certs/`. Needs
  `CONFIG_ESP_HTTPS_SERVER_ENABLE=y` in `sdkconfig.defaults.arply_v1`.
  The second httpd instance needs its own `ctrl_port` (`32769`); HTTP keeps
  `32768`.
- **0.1 cert:** iOS only accepts a user-trusted server cert with
  **validity ≤ 825 days**, a SAN and `extendedKeyUsage = serverAuth`.
  Spike command (the spike certificate is gone again, replaced by the
  on-device one from task 4.2):
  `openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes
  -days 825 -keyout key.pem -out cert.pem -subj "/CN=<host>.local"
  -addext "subjectAltName=DNS:<host>.local,DNS:arply.local"
  -addext "extendedKeyUsage=serverAuth"`.
- **0.1 result reporting:** the mictest page POSTs what it saw
  (`getUserMedia` ok/error name, `AudioContext.sampleRate`, track settings,
  peak level) to `/api/mictest/log`, which prints it to the serial log. The
  result is then on record without anyone copying it by hand.
- **0.1 RAM:** log `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`,
  `…_largest_free_block(MALLOC_CAP_INTERNAL)` and SPIRAM free from the
  `ESP_HTTPS_SERVER_EVENT` handler (`ON_CONNECTED`, `ON_DISCONNECTED`).
  lwIP has `CONFIG_LWIP_MAX_SOCKETS=16`; count the sockets in use with
  AirPlay playing before and after (listen + 2 clients).
- **0.2:** spike code is compiled only with `-DDSP_BENCH` (no Kconfig) and is
  removed afterwards. The benchmark runs on **every** frame, the silence
  frames included, so it gives an idle number without a stream. Streaming
  numbers need AirPlay playing (the user starts it). 48 kHz is measured by
  building once with `CONFIG_OUTPUT_SAMPLE_RATE_48000=y` and reverting.
  Expectation: 20 biquads × 353 samples ≈ 7 k biquad steps ≈ 70 k cycles ≈
  0.3 ms at 240 MHz in plain C.

### Spike results

Measured 2026-11-24 on the `DUT` board (XIAO ESP32-S3, `arply_v1`, 44.1 kHz).

**0.1 HTTPS** — works on the device: a second httpd instance on 443
(`ctrl_port` 32769, 2 sockets, 10 KB stack), EC P-256.
- TLS handshake from a Mac (curl): 1.46 s for the first, 0.28 s after that.
- RAM per TLS session: ~2.7 KB internal + ~26 KB PSRAM. Starting the
  server costs ~15 KB internal (task stack). Internal free stays > 175 KB.
- `/dsp` (69 KB) loads in 0.31 s over HTTP and 0.91 s over HTTPS (incl.
  handshake).
- **iPhone result (2026-11-24, Till):** Safari grants the microphone on
  `https://dut.local/dsp` after the certificate warning is accepted (the
  level check reached its mic branch). **Decision: per-device self-signed
  certificate (4.2), HTTPS as the default** — done. The warning looked
  confusing at first, so the settings page now also offers the certificate
  as a profile download (`/arply.crt`) with the steps to trust it.
- The level meter read "far too quiet" with very loud noise: the analyser
  was not connected to the destination, and Safari only pulls audio through
  nodes that reach the output. Fixed: a fresh AudioContext after
  `getUserMedia`, analysis chain ends in a muted gain at the destination,
  and a "microphone delivers silence" message below −100 dBFS.
  **Pending:** re-test on the iPhone.
- **Pending:** TLS handshakes while AirPlay plays, listening for dropouts.

**0.2 CPU** — measured on the device with the real `dsp_process()`
(not the synthetic loop), per 352-sample frame (8 ms):

| Config | avg | max | share |
|---|---|---|---|
| flat / bypass (fast path) | 1 µs | 30 µs | 0 % |
| preamp only (int16↔float, limiter, dither) | 171 µs | 235 µs | 2.1 % |
| 10 + 10 filters | 547 µs | 582 µs | 6.8 % |
| 6 + 6 filters + bass enhancement (typical auto-fit) | 659 µs | 730 µs | 8.2 % |
| 10 + 10 filters + bass enhancement (worst case) | 809 µs | 891 µs | 10.1 % |

- The firmware already builds with `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
  The synthetic 20-biquad loop took 519 µs. Running the L and R filters
  as interleaved pairs (`dsp_biquad_run2`) cut 18 %: the DF2T dependency
  chain, not the instruction count, was the limit (~12.8 → ~8 cycles per
  biquad-sample). One 32-bit random draw per sample for TPDF dither and
  branch-free rounding took the fixed part from 204 to 171 µs.
- **Decision:** plain C, no `esp-dsp`. Worst case sits at the 10 % budget,
  typical configs well below it.
- 48 kHz (USB DAC env, or 44.1→48 resampling): up to 384 samples per frame,
  so ×1.09 → worst case ≈ 0.88 ms (11 %). Computed, not measured.
- Measured on silence frames (per-sample cost does not depend on the data).
  **Pending:** a run while streaming AAC/ALAC, to see the max under decoder
  load.
- Playback task stack: 1.7 KB of 4 KB still free at the high-water mark
  (incl. the benchmark's own logging). Stays at 4 KB.

**1.4 decision (24-bit output):** not now. The CPU budget is used up at the
worst case, and with TPDF dither a few dB of preamp cut on a 16-bit
PCM5102A path is inaudible. Revisit only if someone hears it.

---

## Phase 1 — DSP core component

New component `components/dsp/` (`dsp.h`, `dsp.c`, `dsp_biquad.c`,
`CMakeLists.txt`). Pure C, no ESP-IDF dependencies in the maths so it can be
unit-tested on the host.

### Task 1.1 — Biquad design and processing

- [x] Port `coeffs()` from the prototype (RBJ cookbook): types `peak`, `ls`,
      `hs` (shelves fixed at Q = 1/√2, S = 1), `hp`, `lp`. Compute
      coefficients **on the device** for the actual output rate; ignore any
      coefficients the client sends.
- [x] Process with transposed direct form II in `float`, state per channel
      per filter. Denormal guard (add 1e-20 or flush).
- [x] Limits: `DSP_MAX_FILTERS_PER_CH = 10`, f 20–20000 Hz (clamp below
      0.45·fs), gain −18…+6 dB, Q 0.3–12.

### Task 1.2 — Config, double buffering, processing entry point

- [x] `dsp_config_t`: version, `preamp_db`, `bypass`, per output channel
      (`DSP_CH_L`, `DSP_CH_R`) an array of `dsp_filter_t {type, f, gain_db, q,
      enabled}` + count, bass-enhancement params (Phase 5).
- [x] Two prepared runtime states (coefficients + zeroed history). The web task
      prepares the inactive one and swaps a pointer atomically; the audio task
      picks it up at the next frame start. No mutex in the audio path.
- [x] On swap, reset filter history only if the filter *structure* changed;
      for parameter-only changes keep history to avoid clicks. Crossfade old →
      new over one frame if clicks are audible.
- [x] Entry point:
      `void dsp_process(int16_t *buf, size_t frames);` (interleaved stereo,
      in place). Internally: int16 → float, preamp, filters per channel,
      soft limiter (tanh-style knee above −1 dBFS), float → int16 with TPDF
      dither. Keep the work buffer static (no allocation per frame).
- [x] `dsp_init(uint32_t sample_rate)`, `dsp_set_config(const dsp_config_t*)`,
      `dsp_get_config(dsp_config_t*)`, `dsp_set_bypass(bool)`.

### Task 1.3 — Hook into the output path

- [x] In `playback_task()` the new order is:
      resample → `apply_channel_mode()` → **`dsp_process()`** →
      `apply_volume()` → `led_audio_feed()` → `i2s_channel_write()`.
      Channel mode first, because the filters belong to the *outputs*
      (speakers), not to the source channels. Volume last, so the user's
      volume never changes the EQ's headroom.
- [x] Call `dsp_init(OUTPUT_RATE)` from `audio_output_init()`.
- [x] Check the 4 KB task stack still fits (`uxTaskGetStackHighWaterMark`);
      raise to 6 KB if needed.
- [x] Same hook for `audio_output_spdif.c` / `audio_output_usb.c` only if
      trivial; otherwise leave a TODO (arply_v1 uses I2S).

### Task 1.4 — Optional: 24-bit output

- [x] Evaluate switching I2S to `I2S_DATA_BIT_WIDTH_32BIT` and having
      `dsp_process()` write int32. Benefit: preamp/cuts don't cost resolution.
      Cost: double DMA bandwidth, `led_audio_feed()` and the silence buffer
      need int16 views. Only do it if Phase 0 left CPU headroom. Record the
      decision here.

### Task 1.5 — Host test

- [x] `components/dsp/test/host_test.c` (gcc, no IDF): run a 1 kHz sine and
      a log sweep through known filters, compare measured gain with the
      analytic magnitude (`magDb()` from the prototype) at ~20 frequencies,
      tolerance 0.1 dB. Add `scripts/dsp_host_test.sh`. Not wired into CI yet.

#### Detail (Phase 1)

- **Layout:** `components/dsp/{CMakeLists.txt, include/dsp.h, dsp.c,
  dsp_biquad.c, dsp_biquad.h, test/host_test.c}`. `CMakeLists.txt` is
  `idf_component_register(SRCS "dsp.c" "dsp_biquad.c" INCLUDE_DIRS
  "include")`. No REQUIRES: C11 `<stdatomic.h>` and `<math.h>` only, so the
  host test builds the same files with `cc`.
- **Coefficients** are computed in `double` (the ESP32-S3 has no double FPU,
  but this runs only on config changes), stored as `float`.
- **Swap without a mutex in the audio path — three slots.** One `_Atomic
  uint32_t` word holds `active` and `pending` slot indices. Writer
  (`dsp_set_config`, any task, serialised by an `atomic_flag` spinlock):
  pick a slot that is neither active nor pending, fill it, CAS
  `pending = slot`, retrying if the audio side consumed the previous
  pending meanwhile. Reader (`dsp_process`, once per call): if `pending` is
  set, CAS `active = pending, pending = none`. With only two slots a writer
  could overwrite the slot the audio task has just switched to; with three
  the free slot is never in use. A newer pending config simply replaces an
  unconsumed older one.
- **History handover:** each slot carries a `structure_hash` (per output:
  filter count + type of every filter). On swap the audio task copies the
  `z1/z2` state from the old slot if the hash matches, otherwise it starts
  from zero.
- **Identity fast path:** a config with no enabled filter, `preamp_db == 0`
  and PBE off, or `bypass`, makes `dsp_process()` return immediately. That is
  what makes “flat = bit-identical” true, and it costs nothing for users who
  never touch the EQ.
- **Block processing:** convert in chunks of 128 frames into a static
  `float work[2][128]` (1 KB). Run each biquad over the whole chunk (keeps
  coefficients in registers), then the limiter, then dither back to int16.
  Static buffers are fine because only one writer task runs at a time (see
  USB DAC above). The input can be any length (USB items are up to 4096
  frames).
- **Denormals:** after each chunk, flush `|z| < 1e-15f` to 0 (cheap,
  per filter per chunk, not per sample).
- **Limiter:** memoryless soft knee, `T = 10^(−1/20)`:
  `|x| ≤ T → x`, else `sign(x)·(T + (1−T)·tanh((|x|−T)/(1−T)))`. Continuous
  with slope 1 at `T`, never exceeds full scale.
- **Dither:** TPDF, ±1 LSB, two draws from a 32-bit xorshift per sample.
- **Mode → outputs:** the component knows only outputs `DSP_CH_L/R`.
  Mapping the JSON `L/R/M` sets onto outputs for the current channel mode
  lives in `main/audio/audio_dsp.c` (Phase 2), so a mode change from the old
  index page (`/api/audio/channel`) also re-resolves the filters.
- **USB DAC hook (1.3):** in `arbiter_task()` the order becomes
  `dsp_process()` → `apply_gain()` (volume last, as in the AirPlay path).
- **Decision for 1.4 (24-bit):** decided after the Phase 0 CPU numbers;
  recorded under “Spike results”.

**Hardware test:** flat config = bit-identical to before (bypass), a −12 dB
peak at 1 kHz Q 1 audibly removes a 1 kHz test tone, no clicks when toggling.
Bit-identical flat/bypass, the −12 dB level and a smooth parameter change
are verified by the host test. **Pending (needs a listener):** the audible
check on the speakers, and that slider moves on the page do not click.

---

## Phase 2 — Settings, `/api/dsp`, live preview

### Task 2.1 — Persistence

- [x] `settings_get_dsp_config()` / `settings_set_dsp_config()` storing a
      versioned packed struct as an NVS blob, key `"dsp_cfg"`. Unknown
      version → ignore and start flat.
- [x] Load in `audio_output_init()` before playback starts.

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

- [x] `GET /api/dsp` → current saved config + `{"fs": <output rate>,
      "max_filters": 10, "bypass": false}`.
- [x] `POST /api/dsp` → validate, clamp, apply, **save to NVS**, set channel
      mode via `audio_output_set_channel_mode()`. Reply with the stored
      config.
- [x] `POST /api/dsp/preview` → same validation, apply **without saving**
      (for live A/B while dragging sliders). Rate-limit: ignore if the last
      preview was < 50 ms ago.
- [x] `POST /api/dsp/bypass` `{"on": true}` → bypass without changing the
      config (for A/B).
- [x] Body handling: read the full body in a loop up to 4 KB into a heap
      buffer (current handlers use a single `httpd_req_recv` into 512 bytes).
- [x] Bump `config.max_uri_handlers` accordingly.
- [x] Test with `curl` (document the commands here).

  ```sh
  H=http://dut.local; J='Content-Type: application/json'
  curl $H/api/dsp                                            # saved profile
  curl -X POST -H "$J" -d '{"mode":"stereo","preamp_db":-1.5,
    "channels":{"L":[{"type":"peak","f":96.8,"gain_db":-1.9,"q":11.85}],
                "R":[{"type":"peak","f":234.6,"gain_db":-2.8,"q":5.4}]}}' \
    $H/api/dsp                                               # apply + save
  curl -X POST -H "$J" -d '{"mode":"mono","channels":{"M":[]}}' \
    $H/api/dsp/preview                                       # RAM only
  curl -X POST $H/api/dsp/revert                             # drop preview
  curl -X POST -H "$J" -d '{"on":true}' $H/api/dsp/bypass    # A/B
  curl -X POST -H "$J" -d '{"channels":["L","R"],"gap_ms":2000}' \
    $H/api/dsp/sweep                                         # 409 if busy
  curl -X POST -H "$J" -d '{"on":true,"channels":["LR"]}' $H/api/dsp/noise
  curl -X POST $H/api/dsp/stop
  ```

  Verified on the device: 400 for invalid JSON, unknown type, > 10 filters
  and unknown mode; out-of-range values come back clamped (30 kHz → 19845
  Hz, +20 dB → +6, Q 50 → 12); `b`/`a` arrays ignored; a preview that only
  sends `M` keeps `L`/`R`; revert restores the saved profile and channel
  mode; the profile survives a restart.

#### Detail (Phase 2)

- **Stored profile ≠ runtime config.** NVS holds the *profile*: `mode`,
  `preamp_db`, all three filter sets `L`, `R`, `M` (≤ 10 each) and `pbe`.
  Each filter is `{u8 type, u8 enabled, float f, gain_db, q}`, so 3 × 10 ×
  16 B + header ≈ 500 B. `main/audio/audio_dsp.c` resolves the profile for
  the current channel mode into a `dsp_config_t` (table above) and
  re-resolves on every `audio_output_set_channel_mode()`. Keeping all three
  sets means switching mode on the page does not lose the other mode's
  filters.
- **Handlers** live in `main/network/web_dsp.c` with
  `web_dsp_register(httpd_handle_t)`. HTTP and HTTPS both call it, so the
  page works on either origin with relative URLs. All handlers must be
  registered **before** the `/*` catch-all.
- **Validation:** unknown `type` or a non-number field → 400 with a message.
  Out-of-range values are **clamped**, not rejected, and the reply carries
  the clamped values so the page can show what the device actually runs.
  More than 10 filters per set → 400.
- **Preview** runs from RAM only. The device keeps playing it until the next
  save, a `POST /api/dsp/revert` (reload the saved profile) or a reboot.
  `GET /api/dsp` returns the saved profile plus `"previewing": true` while
  one is active, so the page can warn about unsaved changes.
- `max_uri_handlers` 30 → 41 on HTTP (32 + 9 DSP), 11 on HTTPS (page, GET/POST dsp, preview, revert, bypass, sweep, noise, stop, mictest page + log, chime GET/POST, with headroom).

---

## Phase 3 — Test signals from the device

The phone records; arply plays. This avoids AirPlay latency (~2 s), the risk
that iOS moves audio off AirPlay while the mic is open, and the user's volume
setting.

### Task 3.1 — Signal source in the output task

- [x] New `main/audio/test_signal.c/.h`: generators that fill an int16 stereo
      frame **on the fly** (no 4-second buffers):
      - exponential sine sweep 20 Hz–20 kHz, 4 s, amplitude 0.25, 10 ms
        fade-in / 5 ms fade-out — identical to `sweep()` in the prototype
        (phase from the closed form, in `double`);
      - pink noise (Paul Kellet), −15 dBFS RMS, looping;
      - routed to `L`, `R` or both outputs.
- [x] In `playback_task()`: while a test signal is active, take samples from
      the generator instead of `audio_receiver_read()`, **skip DSP and
      volume** (raw speaker measurement at fixed level), still feed the LED.
- [x] Refuse (HTTP 409) while AirPlay is playing. If a client starts playing
      during a test signal, stop the test signal.

### Task 3.2 — Unmute hook

- [x] Add `iot_board_set_signal_unmute(bool)` (or a small mute arbiter in
      `board.c`): XSMT is released if RTSP is PLAYING **or** a test signal /
      chime is active. Re-mute 100 ms after the signal ends.

### Task 3.3 — API

- [x] `POST /api/dsp/sweep` `{"channels": ["L","R"], "gap_ms": 2000}` →
      plays one sweep per listed output back to back, starting 500 ms after
      the request. Responds immediately with `{"lead_ms":500, "sweep_ms":4000,
      "gap_ms":2000, "fs":44100}` so the page knows the timeline.
- [x] `POST /api/dsp/noise` `{"on": true, "channels": ["L","R"]}` → level
      check noise; auto-off after 30 s.
- [x] `POST /api/dsp/stop` → stop any test signal.

Verified on the device: the L,R sweep runs exactly 12.5 s (0.5 + 2 × (4 + 2));
a second request while it runs gets 409; noise starts and `stop` ends it
(5 ms fade). **Pending (needs a listener):** the sweep and noise are
audible on the right jack at the right time, XSMT unmutes and re-mutes
without a pop, an AirPlay stream started during a sweep stops it.

#### Detail (Phase 3)

- **Sweep phase without per-sample `exp()` in double.** The S3 has no
  double FPU, and one soft-float `exp`+`sin` per sample costs a few ms per
  frame. Compute the closed form `φ = K·(exp(n/(fs·L)) − 1)` exactly in
  `double` once per chunk, step inside the chunk with the constant ratio
  `r = exp(1/(fs·L))` (one double multiply per sample), wrap to ±π in
  double, then `sinf()` in float. The host test checks it against the pure
  closed form (max error < 1e-4 over the full 4 s), because the page
  deconvolves with its own `sweep()` and needs a sample-exact match.
- **Pink noise level:** the Kellet filter's output RMS for uniform white
  input is a constant. The host test measures it once, the value is
  hard-coded and the test asserts −15 dBFS ± 0.5 dB. No per-start
  calibration pass.
- **Timeline** (sweep request): `lead_ms` silence (500) → per channel: sweep
  (4 s) → `gap_ms` silence. Unmute is requested at the start of the lead,
  so XSMT has settled long before the first sample. Re-mute is an
  `esp_timer` one-shot 100 ms after the last signal sample left the task
  (+ DMA latency).
- **Who may play:** refuse with 409 if `audio_receiver_is_playing() &&
  audio_receiver_has_data()` (the same test `usb_dac_input.c` uses) or if
  the playback task is not running (USB DAC owns the output). An
  `RTSP_EVENT_PLAYING` while a signal runs stops it. That covers AirPlay
  and the USB/playback_control re-emits.
- **Mute arbiter in `board.c`:** `rtsp_playing || signal_active` under a
  `portMUX` critical section, new `iot_board_set_signal_unmute(bool)` in
  `board_common.h`.

---

## Phase 4 — Calibration page served by arply

### Task 4.1 — Page

- [x] Copy `calibration-prototype.html` to `data/www/dsp.html`; serve it at
      `/dsp` (HTTP and, if Phase 0 succeeded, HTTPS).
- [x] Remove the Google Fonts `<link>`: the device often has no internet (AP
      mode). Use `system-ui` / `ui-monospace` fallbacks. Replace the logo
      placeholder with an inline SVG traced from `docs/arply-logo.jpg`
      (circles + rectangles; keep it < 2 KB).
- [x] On load: `GET /api/dsp` and fill the filter lists, mode and bass
      enhancement from the device.
- [x] Measuring: replace the WebAudio sweep playback with
      `POST /api/dsp/sweep`; start recording first, then trigger. Analyse with
      the existing “import” path (it finds the first sweep peak itself and
      walks forward by sweep + gap). Level check: `POST /api/dsp/noise`.
- [x] Output modes: “Left only/Right only” now mean the firmware semantics
      (that source channel on both jacks). Update hint texts. The sweep for
      `left`/`right`/`mono` plays on both outputs; stereo measures L then R.
- [x] “Send to arply” → `POST /api/dsp`. While dragging a filter slider send
      `POST /api/dsp/preview` (throttled), so changes are audible on the
      speakers. The “Listen” panel's built-in samples stay browser-side.
- [x] Keep the demo measurement (useful without a microphone).
- [x] Link the page from `index.html` (“Room calibration”).
- [x] Size check: the page must stay well under the SPIFFS budget and load in
      < 1 s on the device.

### Task 4.2 — Certificate (only if Phase 0 said yes)

- [x] On first boot generate an EC P-256 key + self-signed cert with
      mbedTLS (`mbedtls_x509write_crt`), SAN = `<hostname>.local`, store in
      NVS. Regenerate when the device is renamed.
- [x] If Phase 0 showed a trusted profile is required: serve the cert as a
      `.mobileconfig` download on the setup page. **Not required:** the
      accepted warning works; `/arply.crt` is offered for those who want it.

Done ahead of the iPhone result, because every outcome that keeps HTTPS
needs it and a private key in the repo would be shared by every device:
`main/network/tls_cert.c` generates EC P-256 via mbedTLS, SAN `<name>.local`
+ `arply.local`, `serverAuth`, CA:TRUE (so iOS can trust it as a profile),
a fixed 825-day window 2026-01-01 … 2028-04-05 (no clock at first boot),
kept in NVS, regenerated when the name changes. Generation takes 0.2 s and
runs in its own 8 KB task: in the main task it overflowed the stack.
Verified with `openssl s_client` and across a restart (loaded, not
regenerated). The DNS name follows the DHCP hostname rule (lower case,
non-alphanumerics → `-`); mDNS still announces the raw device name, so a
name with spaces gives an unusable `.local` name — pre-existing, Phase 8.

---

## Phase 5 — Bass enhancement on the device

- [x] Port `buildPbe()` from the prototype into `components/dsp/`:
      main path 2nd-order high-pass at `fc`; harmonic path 2× low-pass at
      `fc` → full-wave rectifier `|x|` → 2× high-pass at `fc` → low-pass at
      `4·fc` → gain `amount × 2.5` → summed back. Butterworth sections.
- [x] Runs after the EQ, before the limiter, per output channel.
- [x] Params from `pbe` in the JSON (`fc` 60–150 Hz, `amount` 0–1). Off by
      default.
- [x] Host test: 50 Hz in, `fc` = 100 → 50 Hz ≈ −12 dB, 100 Hz and 200 Hz
      harmonics present (the prototype measured −12 / −21 dB at
      `amount` 0.5).
- [x] Later (Phase 8): off while a subwoofer plays in the room (FLEET_PLAN.md, F5).

Host test result: 50 Hz −12.3 dB, 100 Hz −12.1 dB, 200 Hz −20.8 dB
(prototype: −12 / −21).

**Rework (2026-11-25, after "the bass enhancement is shit"):** the
prototype's full-wave rectifier makes only even harmonics (2f, 4f), whose
spacing the ear hears as the pitch — an octave too high — and it smears two
simultaneous notes into sum/difference tones (two-tone test 45 + 62 Hz: only
15 % of the added energy were harmonics, the 107 Hz sum tone 11 dB above
them). Now: the bass band is split an octave below the cutoff, each band is
normalised to its envelope and fed through Chebyshev T2/T3/T4 (weights
1 / 0.7 / 0.45), giving consecutive harmonics 2f/3f/4f at fixed ratios. Two
notes an octave apart: 33 % harmonics (old 13 %); notes closer than the
split stay the inherent weak spot of any per-sample method. The harmonic
path runs at fs/4 with linear interpolation (images < −76 dB). CPU on the
board: bass enhancement alone 509 µs/frame, 6 + 6 filters + it 737 µs
(9.2 %), worst case 10 + 10 + it 886 µs (11 %, old 809 µs). Host tests:
50 Hz −12.4, 100 Hz −13.7, 150 Hz −13.3, 200 Hz −17.0 dB.

**Bass block (2026-11-25, after "sounds like a tin can, better without"):**
cutting all real bass below the cutoff and replacing it by harmonics is
only right for drivers that play nothing there. Now one block of four parts
(`dsp_bass_t`: `f_low`, `boost`, `harmonics`), defaults for very small
drivers (f_low 120 Hz, +6 dB, harmonics 30 %):
- **Extension:** RBJ low shelf at `f_low`, up to +12 dB, coefficients
  recomputed per chunk from the dynamic gain (S = 1: only two square roots
  depend on it). A parallel x + (g−1)·LP(x) form dipped −1.5 dB above
  `f_low`; the shelf rises +0.7 dB at 200 Hz instead.
- **Dynamics:** the boost falls at once when the boosted bass band at the
  DAC would exceed −18 dBFS peak, comes back over 0.3 s.
- **Loudness:** +0.3 dB per dB of volume cut, at most +6 dB.
- **Subsonic + harmonics:** high-pass at `f_low`/2; what lies below it goes
  through the Chebyshev harmonics (gain ×0.5 while the boost is free, up to
  ×2 while the dynamics hold it back).
- **Volume moved into the DSP** (at the input of the float chain) while it
  is active: EQ, bass and limiter now work at the level that reaches the
  DAC, so a boost at low volume has headroom instead of hitting the limiter.
  `dsp_process()` returns whether it applied the volume; flat/bypass keeps
  the old int16 path, bit-identical. The USB host mute is honoured either
  way.
- Profiles are version 2; version 1 (`pbe`) is converted on load
  (`f_low` = 2 × old cutoff). `/api/dsp` speaks `bass`, still accepts `pbe`.
- Host tests: quiet 80 Hz +3.8 dB, loud held back to ≈ 0 dB, at −26 dB
  volume +8.5 dB, no dip above the limit.
- CPU (board, silence frames): bass alone 639 µs, 6+6 filters + bass
  866 µs (10.8 %), 10+10 + bass 1016 µs (12.7 %) — over the 10 % budget.
  Found on the way: `fmaxf`/`fminf`/`powf` are library calls here; a
  per-sample `fmaxf` cost ~160 µs/frame.
- **Measurement** (same day): the level check now rates the margin over the
  room noise (20 dB is enough) instead of absolute dBFS, which asked for
  painful volume with an AGC-free phone microphone.

**Bass block v3 (2026-11-25, "still sounds terrible"), found with a probe:**
`scripts/dsp_probe.py` plays a synthetic groove through the device's DSP
(preview only), captures DSP input and DAC output (`/api/debug/capture`,
4 channels, PSRAM) and prints gain per band, limiter use and clipping. It
showed what made the tin can: the migrated profile (f_low 200 Hz from an
old cutoff of 100), the −18 dBFS guard keeping the boost near 0 dB, the
subsonic at f_low/2 removing real bass, and harmonics landing in
300–1000 Hz. Now: guard at −6 dBFS, subsonic LR4 at f_low/3, harmonics
taken from below `f_low` and added between `f_low` and 3·`f_low`, default
harmonics 0, v1 profiles keep only `on`. Probe at defaults (120 Hz, 50 %,
volume −10.5 dB): 45–60 +5.3, 60–90 +6.3, 90–120 +5.2, 180–300 +0.9,
300–1k +0.1 dB, no limiter, no clipping; DSP flat is exactly 0.0 dB.
Harmonics 50 % still add +2…+4 dB in 300–1000 Hz, so they stay opt-in.

**Measurement, checked before building anything (simulation of sweep +
deconvolution with room noise):** the exponential sweep's deconvolution is
already the "band-pass that follows the sweep"; with 10 dB margin the
response error stays < 1 dB, so the level check now asks for 10 dB, not
20. A down sweep is worse than a second up sweep (its distortion lands
after the impulse response, not before it). The start time is found
exactly from the deconvolved peak, so acoustic markers add nothing.

**Three sweeps per channel, averaged (same day, "dann sweepe einfach
mehrfach"):** averaging power spectra barely helps (it lowers the
variance, not the noise floor), but averaging the impulse responses
aligned on their own peaks lowers the noise by 10·log10(N) dB and is
immune to the phone/arply clock drift (50 ppm simulated). Bass error
(40–300 Hz, mean of 6 runs): 1 sweep 0.87 dB at 5 dB margin, 3 sweeps
0.52 dB, about what one sweep gives at 10 dB. A long sweep gains the
same per second as more short ones; several sweeps were chosen because
they need no new sweep length on both sides. The pause between sweeps
drops from 2 s to 0.5 s: impulse responses lie 4.5 s apart in the
deconvolved recording, so reverb and the next sweep's distortion stay
clear of the 0.4 s window; the "longer pause" option is gone. Stereo
measurement: 6 sweeps, 28 s (was 13 s). Level check: Good from 5 dB
margin. Checked in jsdom on a simulated recording (6 sweeps, noise,
drift): channels assigned correctly, bass error 0.6–0.9 dB at 5 dB margin,
0.2 dB at 20 dB.

**Distortion, speaker limit, delay and polarity from the same sweeps (same
day):**
- The k-th harmonic's impulse response lies L·ln(k) before the linear one
  (0.40 s for the 2nd … 0.93 s for the 5th). The page windows the 2nd–5th
  (0.1 s each, same window for the fundamental) and stores THD per
  frequency up to 300 Hz with every measurement (`thd`). Noise comes from
  five windows 1.15–1.55 s before the peak; harmonics count only at ≥ 3×
  the noise, otherwise the noise-limited upper bound is kept if it is below
  10 %. Only sweeps with 1.6 s of recording before them are used (the
  first one in a run usually is not).
- Simulated driver (resonance 90 Hz, cubic stiffness, 25 % THD below it,
  model crossing 10 % at ≈ 100 Hz): found at 78–106 Hz at 5 dB margin,
  89–95 Hz at 10–20 dB. Clean driver: no false alarm in 13 runs (one noise
  window and a 2× threshold gave 2 in 6).
- "Suggest from measurement" now takes the higher of the −6 dB roll-off
  (if ≥ 85 Hz, below that it is the phone microphone's own) and 3× the
  frequency below which THD exceeds 10 %, so the bass block's cut (a third
  of the limit) sits where the driver distorts. Distortion grows with
  volume, so the page asks to measure at listening volume when it cannot
  tell.
- A joint L+R sweep would add nothing: the room is linear, so the sum is
  the sum of the separate responses. From the separate sweeps the page now
  reports the right speaker's arrival vs the left (clock drift removed via
  the left sweeps' spacing; shown from 0.3 ms) and a reversed polarity
  (sign of the direct sound, only when it clearly beats the opposite lobe
  on all sweeps of both channels). Delay compensation would need a delay
  line in the firmware; not built.

**Level and microphone (same day, Till: "the mic test should not be its own
block, show it subtly at the right of the spectrum; the level check should
set the volume itself, from the lowest step up; manual volume only in
Step 3"):**
- One microphone chain per page, asked for as soon as the page opens
  (Till) and kept open; a 4 px vertical meter in the chart's right
  margin shows its level (−90…−10 dBFS) with a mark at the background
  noise, red near clipping. iOS ends the track in the background; the next
  action opens it again.
- "1. Set level": 1 s of background, then arply's noise from volume −30 dB
  upwards in 0.5 dB steps every 250 ms (2 dB steps were audible jumps)
  until 15 dB above the background
  (Till's choice; 10 dB measured the response well but left the distortion
  check "too quiet to tell" in a quiet room), the microphone nears
  clipping, or full volume.
  The volume is saved. Noise and sweep have the same RMS (−15 dBFS), so the
  margin holds for the sweeps. The Step 1 volume slider is gone.
- Turning the noise off now waits until arply is idle, so a sweep requested
  right after it is not refused as busy.
- jsdom with a simulated room (background −70 dBFS): stops at −14 dB with
  11 dB margin, saves it, turns the noise off; the measurement afterwards
  records through the same microphone.

**Detail:** the prototype's WebAudio `Q` of `20·log10(1/√2)` dB *is* linear
Q = 1/√2 (WebAudio's LP/HP Q is in dB), so all sections are plain
Butterworth biquads. The prototype's `oversample='2x'` on `|x|` is not
needed on the device: the rectifier input is already low-passed twice at
≤ 150 Hz and the result is band-limited to `4·fc` ≤ 600 Hz, far from
Nyquist. Cost: 6 extra biquads per output. The host test uses Goertzel at
50/100/200 Hz.

---

## Phase 6 — Boot chime

- [x] Add `chime.c/.h` from this folder to `main/audio/`, embed the PCM with
      `EMBED_FILES`. Generate it with `gen_chime.py`; if
      `OUTPUT_SAMPLE_RATE_HZ` is 48000, extend the script to also render
      48 kHz and pick the matching file at build time.
- [x] Rework `chime_play()` to use the Phase 3 signal source (play from the
      embedded PCM) instead of writing to I2S directly, so it shares the
      unmute hook and never collides with the playback task.
- [x] Play once after `audio_output_init()` **only on power-on**
      (`esp_reset_reason() == ESP_RST_POWERON`), not after OTA/soft reset.
      Fixed level, independent of AirPlay volume.
- [x] NVS flag `"chime_on"` (default on) + toggle in the settings page.

**Detail:** the playback task only starts in `start_airplay_services()`, so
once a network is up. The chime is queued at boot as a pending signal and
plays as soon as the playback task runs, but only if that happens within
15 s of power-on; otherwise it is dropped (a chime half a minute later
would be confusing). `gen_chime.py` takes the sample rate as an argument;
both `arply_chime_44100.pcm` and `arply_chime_48000.pcm` are committed, and
CMake embeds the one matching `CONFIG_OUTPUT_SAMPLE_RATE_HZ`. API:
`GET/POST /api/audio/chime {"on": bool}`, toggle in `index.html`.
Verified: a soft restart plays no chime; a real power-on plays it (Till,
2026-11-24).

---

## Phase 7 — Setup hotspot hardening

Today the hotspot (`CONFIG_DEFAULT_AP_SSID`, `wifi.c`) is open and re-enables
itself after repeated STA failures.

- [x] AP auto-off after 10 min without a connected client (keep it on while a
      client is connected). Log it.
- [x] Optional WPA2 password for the setup AP (Kconfig + printed label);
      default stays open to not break existing setups — decide and record.
- [x] Re-enter setup on demand without buttons: power-cycle 3× within 5 s
      (counter in RTC memory / NVS, cleared after 5 s uptime) → AP on.
- [x] Never return stored Wi-Fi passwords from any API.

**Detail:** the AP is enabled in `wifi.c` `enable_ap_mode()` (APSTA) and
dropped back to STA on connect. Auto-off: an `esp_timer` checks
`esp_wifi_ap_get_sta_list()` every 30 s and switches to STA after 10 min
with zero stations. The 3× power-cycle counter lives in NVS (RTC memory
does not survive a power cut): increment at boot, clear after 5 s uptime,
≥ 3 → force AP. Only `ESP_RST_POWERON` counts. WPA2 decision: **stays
open by default**, optional `CONFIG_SETUP_AP_PASSWORD` (empty = open),
because existing setups and printed instructions assume an open AP. The
password option already existed as `CONFIG_DEFAULT_AP_PASSWORD` (empty =
open), so nothing new was needed. No API returns a stored password
(audited: only `wifi.c` reads them). **Pending:** the 3× power-cycle
needs real power cycles; the 10-min auto-off needs a device that has
lost its network.

---

## Phase 8 — Fleet (outline — write a separate plan before starting)

> Detailed plan: [`FLEET_PLAN.md`](FLEET_PLAN.md) (2026-11-24).

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
- UI: see `overview-mock.html` in this folder — rooms as cards,
  device rows linking to `https://<name>.local`, “Calibrate” per room.

---

## Out of scope for this branch

- Restyling the existing `index.html`, `logs.html`, `speedtest.html`.
- Distortion measurement (harmonics from the sweep) — possible later; the
  impulse response already contains them at negative times.
- Per-iPhone-model microphone correction curves.


---

## Feedback round 1 (2026-11-24) — what changed

From Till's first test on the iPhone:

- **HTTPS is the default.** Plain-HTTP requests for `/` and `/dsp` get a
  302 to the same URL on HTTPS, except for clients on the setup hotspot
  (captive portal stays HTTP). Settings page, calibration page and their
  APIs are registered on both servers from one table
  (`web_server_register_shared()`); `/logs` (single-server websocket) and
  `/speedtest` (should measure Wi-Fi, not TLS) stay HTTP-only and redirect
  back from HTTPS. This also fixes "Nothing matches the given URI" on the
  "← Settings" link, which pointed at `/` on HTTPS. APIs on HTTP keep
  working (curl, scripts). HTTPS now allows 3 sockets.
- **The original logo.** `dut.local` had a hand-drawn SVG, `/dsp` a trace
  of an older source image. Both now use `arply-web/assets/arply-logo.png`
  (1.5 KB) as a CSS mask, so it follows light/dark mode.
- **Level check far too loud.** Measurement signals now follow arply's one
  volume (the one AirPlay and USB use); they used to play at full digital
  level regardless. New `GET/POST /api/audio/volume` and a volume slider
  in Step 1 and in Listen. The chime keeps its fixed level.
- **Listen plays on arply, not the phone.** The page renders the built-in
  sample at arply's rate and uploads it (`POST /api/dsp/listen?channels=`,
  raw s16le, ≤ 3 MB, PSRAM); arply loops it through channel mode, DSP and
  volume until `/api/dsp/stop`. Groove (stereo, 1.7 MB) uploads in 4.6 s
  over HTTPS, 3.9 s over HTTP. Own music: play it via AirPlay. The browser
  WebAudio preview chain is gone.
- **Removed** demo measurement, "Play sweep only" and recording import.
- **Target and range:** filters are calculated automatically after every
  measurement; the sliders moved into a collapsed "Advanced" section of the
  filter panel and recalculate on change.
- **OTA works:** `curl -X POST --data-binary @.pio/build/arply_v1/firmware.bin
  http://dut.local/api/ota/update` (39 s), then pages with
  `curl -X POST --data-binary @data/www/dsp.html
  "http://dut.local/api/fs/upload?path=/spiffs/www/dsp.html"` (upload cap
  raised from 64 to 256 KB for the 68 KB page).
- Checked in jsdom against a fake API (loads, previews, listen upload,
  volume, bypass, save) and on the board with curl (redirects, routes on
  both servers, certificate download, listen upload/stop).
