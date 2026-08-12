<div align="center">

# arply

**AirPlay 2 receiver for the Seeed XIAO ESP32-S3 + PCM5102A**

[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-green?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s3)

</div>

---

## What is this?

Turns a Seeed XIAO ESP32-S3 with a PCM5102A DAC into a wireless AirPlay 2 speaker. Plug it into any amplifier or powered speakers and it shows up on your iPhone, iPad or Mac like a HomePod.

**No cloud. No app. Just tap and play.**

This is a hardware-specific fork of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32), stripped down to a single board target. Support for SqueezeAMP, Esparagus Audio Brick, TAS57xx/TAS58xx DACs, Bluetooth A2DP and W5500 Ethernet has been removed — see the upstream project if you need any of those.

---

## Hardware

| Component | Notes |
|---|---|
| **Seeed XIAO ESP32-S3** | 8 MB flash, 8 MB octal PSRAM |
| **PCM5102A DAC board** | I2S DAC, no control bus needed |

### Wiring

| XIAO pin | GPIO | PCM5102A | Function |
|---|---|---|---|
| D8 | 7 | BCK | Bit clock |
| D9 | 8 | LRCK | Word select (left/right) |
| D10 | 9 | DIN | Serial audio data |
| D0 | 1 | XSMT | Mute — low mutes, high passes audio |
| 5V / 3V3 | — | VIN | Power |
| GND | — | GND | Ground |

**MCLK is not connected.** The PCM5102A generates it internally.

GPIO 21 drives the XIAO's built-in orange user LED (not on the castellated header).

---

## Build & Flash

There is a single environment: **`arply_v1`**.

### PlatformIO

```bash
# 1. Install PlatformIO CLI
pip install platformio

# 2. Build and flash the firmware over USB-C
pio run -e arply_v1 -t upload

# 3. Flash the SPIFFS image (web UI) — a separate step
pio run -e arply_v1 -t uploadfs

# 4. (Optional) Watch serial output
pio run -e arply_v1 -t monitor
```

> **Note:** `-t upload` does **not** flash the filesystem. Without step 3 the web server runs but serves nothing, and every page returns "file not found".

> **Note:** The XIAO has no USB-UART bridge — console and flashing run over the built-in USB Serial/JTAG on the USB-C port. If the port doesn't appear, hold **BOOT** and tap **RESET** to force download mode.

### ESP-IDF

```bash
source /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.arply_v1" build
idf.py -p /dev/ttyUSB0 flash monitor
```

> If you change any sdkconfig defaults, delete the cached `sdkconfig.arply_v1` before rebuilding so the new values are picked up.

### Configuration

```bash
pio run -e arply_v1 -t menuconfig
```

Board pins live under *Board Configuration → Pin Configuration*, LED behaviour and device naming under *Airplay ESP Configuration*.

---

## Setup (First Boot)

1. **Power up** via USB-C
2. Connect to the WiFi network **`arply setup`**
3. A captive portal opens (IP: `192.168.4.1`)
4. Set a name for the speaker and select your home WiFi
5. The device restarts and joins your network
6. Open any music app, tap the AirPlay icon, select the speaker

Settings are stored in NVS and survive reboots **and firmware flashes**. If the WiFi connection keeps failing, the device falls back to setup mode automatically.

The default device name is `ARPLY`. If you run more than one, give each a distinct name in the web UI — otherwise they collide on mDNS.

---

## Status LED

The XIAO's orange user LED (GPIO 21) shows what the device is doing:

| State | LED |
|---|---|
| Power-on | One full 4 s breath |
| Looking for a network (joining WiFi, or setup mode) | Breathing |
| Connected, nothing playing | Off |
| Audio playing | On, steady |

A breath is never cut off part-way — if the network comes up mid-cycle, the change is applied at the end of that breath. Brightness is adjustable in the web UI.

---

## Web Interface

Reachable at the device's IP, or `http://ARPLY.local` from an Apple device.

| Page | Purpose |
|---|---|
| `/` | WiFi, device name, LED brightness, output channel, firmware update, system info |
| `/logs` | Live ESP log viewer over WebSocket |
| `/speedtest` | Latency and throughput between browser and device |
| `/eq` | Equalizer — only shown on boards with a DSP-capable DAC, so hidden here |

---

## Updating the Firmware (OTA)

Once on your WiFi, update wirelessly without unplugging:

1. Build the new firmware — `pio run -e arply_v1`
2. Open the web interface
3. Upload `.pio/build/arply_v1/firmware.bin` on the firmware update card

---

## SPIFFS Filesystem

A `storage` partition holds the web pages, mounted at `/spiffs` on boot. Updating the UI does not require recompiling the firmware.

### Partition Layout

[`components/boards/partitions.csv`](components/boards/partitions.csv) fills the XIAO's 8 MB exactly:

| Partition | Size | Address |
|---|---|---|
| ota_0 | 3 MB | 0x20000 |
| ota_1 | 3 MB | 0x320000 |
| storage (SPIFFS) | 1.875 MB | 0x620000 |

### Data Directory

```
data/
└── www/               # Web interface pages
    ├── index.html     # Setup / control panel
    ├── logs.html      # Live log viewer
    ├── speedtest.html # Network speedtest
    └── eq.html        # Equalizer (unused on this board)
```

### File Management API

Three HTTP endpoints manage SPIFFS files over WiFi without reflashing:

```bash
curl -X POST "http://<ip>/api/fs/upload?path=/spiffs/www/index.html" --data-binary @index.html
curl -X POST "http://<ip>/api/fs/delete?path=/spiffs/www/old.html"
curl "http://<ip>/api/fs/list?dir=/spiffs/www"
```

Paths are restricted to `/spiffs/`, directory traversal is rejected, and uploads are capped at 64 KB.

---

## Features

- **AirPlay 2 protocol** — appears natively in Control Center and all AirPlay apps
- **ALAC & AAC decoding** — live streaming (Siri, calls) and music playback
- **Multi-room** — PTP-based timing for synchronized playback
- **Hardware mute** — XSMT is held low unless audio is playing, so the DAC is silent when idle
- **Web configuration** — WiFi and device name from any browser
- **OTA updates** — no USB needed after the first flash
- **Status LED** — breathing / off / steady, see above
- **48 kHz output** — optional 44.1 → 48 kHz conversion via sinc resampler
- **Tunable AirPlay timing** — separate early/late thresholds for buffered and realtime streams
- **Optional extras** — OLED or ST7789 display, hardware buttons, cover art (all off by default, see `menuconfig`)

### Limitations

- Audio only — no AirPlay video or photos
- One speaker per board
- No Bluetooth (the ESP32-S3 has no Bluetooth Classic, so A2DP is not possible)
- Needs a decent WiFi signal for stable streaming

---

## Technical Details

### Signal Flow

```
┌─────────────────┐     WiFi      ┌─────────────┐
│  iPhone / Mac   │ ────────────► │  ESP32-S3   │
│    (AirPlay)    │               │             │
└─────────────────┘               └──────┬──────┘
                                         │ I2S
                                  ┌──────▼──────┐
                                  │  PCM5102A   │
                                  └──────┬──────┘
                                         │ Analog
                                  ┌──────▼──────┐
                                  │  Amplifier  │
                                  │  + Speakers │
                                  └─────────────┘
```

### I2S Signals

| Signal | Function |
|---|---|
| BCK | Bit clock — 44100 × 16 × 2 = 1.41 MHz |
| LRCK | Word select — toggles at 44.1 kHz |
| DIN | Serial audio data (16-bit stereo) |

### Audio Formats

| Format | Use Case |
|---|---|
| ALAC (realtime) | Live streaming, Siri |
| AAC (buffered) | Music playback |

### Key Components

| Module | Location | Purpose |
|---|---|---|
| **RTSP Server** | `main/rtsp/` | AirPlay control messages |
| **HAP Pairing** | `main/hap/` | Cryptographic device pairing |
| **Audio Pipeline** | `main/audio/` | Decoding, buffering, timing |
| **PTP Clock** | `main/network/` | Synchronization with source |
| **WiFi** | `main/network/` | AP+STA management, captive portal |
| **Web Server** | `main/network/` | Configuration interface |
| **Board Support** | `components/boards/arply-v1/` | GPIO init, XSMT mute control |
| **SPIFFS Storage** | `components/spiffs_storage/` | Filesystem mount |
| **LED** | `main/led.c` | Status LED animation task |

### Project Structure

```
main/
├── audio/          # Decoders, buffers, timing sync, I2S output
├── rtsp/           # RTSP server and handlers
├── hap/            # HomeKit pairing (SRP, Ed25519)
├── plist/          # Binary plist parsing
├── network/        # WiFi, mDNS, PTP, web server, OTA
├── led.c           # Status LED
├── main.c          # Entry point
└── settings.c      # NVS persistence
components/
├── boards/arply-v1/  # Board HAL — pins, XSMT mute from playback events
├── dac/              # Abstract DAC API (no-op: PCM5102A has no control bus)
├── display/          # Optional display driver
├── audio-resampler/  # 44.1 → 48 kHz sinc resampler
└── spiffs_storage/   # SPIFFS mount
data/
└── www/            # Web interface (flashed to SPIFFS)
```

---

## Acknowledgements

- **[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)** — the upstream project this is based on
- **[Shairport Sync](https://github.com/mikebrady/shairport-sync)** — the reference AirPlay implementation
- **[openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver)** — Python AirPlay 2 implementation
- **[Espressif](https://github.com/espressif)** — ESP-IDF framework and codec libraries

---

## Legal

**Non-commercial use only.** Commercial use requires explicit permission. See [LICENSE](LICENSE).

This is an independent project based on protocol analysis. Not affiliated with Apple Inc. Not guaranteed to work with future iOS/macOS versions. Provided as-is without warranty.
