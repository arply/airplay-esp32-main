# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ARPLY — AirPlay 2 receiver firmware for the Seeed XIAO ESP32-S3 with a PCM5102A I2S DAC. Supports ALAC and AAC decoding, hardware buttons, and OTA updates. Single board target: `arply_v1`.

Stripped-down fork of rbouteiller/airplay-esp32; SqueezeAMP, Esparagus, TAS57xx/TAS58xx, Bluetooth A2DP and W5500 Ethernet support has been removed.

## Build & Flash

**PlatformIO** (recommended):
```bash
pio run -e arply_v1                 # Build firmware ('-t build' is not a target)
pio run -e arply_v1 -t upload       # Build + flash via USB
pio run -e arply_v1 -t monitor      # Serial monitor (USB Serial/JTAG)
pio run -e arply_v1 -t uploadfs     # Flash SPIFFS from data/ (separate step!)
pio run -e arply_v1 -t menuconfig   # Kconfig configuration
```

**ESP-IDF** (native):
```bash
source /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.arply_v1" build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

## Build Environments

| Environment | Board | Notes |
|---|---|---|
| `arply_v1` | Seeed XIAO ESP32-S3 + PCM5102A | The only target. 8MB flash, 8MB octal PSRAM, XSMT mute follows playback |

Sdkconfig defaults are layered via `cmake_extra_args` (left-to-right override). Custom board config: create `sdkconfig.user.<name>` + `user_platformio.ini` to extend any environment without modifying the main config.

## Architecture

```
main/
├── main.c                  # Entry point — initializes NVS, WiFi, starts AirPlay services
├── settings.c              # NVS persistence for device name, WiFi credentials, volume
├── audio/                  # Audio pipeline
│   ├── audio_receiver.c    # RTSP session manager — orchestrates streams (buffered/unbuffered)
│   ├── audio_stream.c      # Base stream abstraction
│   ├── audio_stream_buffered.c   # AirPlay 2 AAC (deep jitter buffer)
│   ├── audio_stream_realtime.c     # AirPlay 1 ALAC (low latency UDP)
│   ├── audio_decoder.c     # ALAC and AAC decoders
│   ├── audio_buffer.c      # Frame buffering between receiver and output
│   ├── audio_timing.c      # PTP-based timing — early/late frame handling
│   ├── audio_resample.c    # Sample rate conversion (44.1→48kHz)
│   ├── audio_output.c      # I2S output
│   ├── audio_output_spdif.c # S/PDIF output
│   ├── audio_output_usb.c  # USB audio output
│   ├── audio_crypto.c      # AirPlay encryption
│   └── eq_events.c         # EQ parameter changes (TAS58xx)
├── rtsp/                   # RTSP protocol server
│   ├── rtsp_server.c       # RTSP connection handler
│   ├── rtsp_conn.c         # Connection management
│   ├── rtsp_handlers.c     # RTSP method handlers (OPTIONS, SETUP, PLAY, etc.)
│   ├── rtsp_events.c       # RTSP event handling (including BT passthrough)
│   ├── rtsp_crypto.c       # RTSP-level encryption
│   ├── rtsp_fairplay.c     # Apple FairPlay integration
│   └── rtsp_rsa.c          # RSA crypto
├── hap/                    # HomeKit Accessory Protocol
│   ├── hap.c               # Core HAP
│   ├── hap_pair_setup.c    # Pairing setup (SRP handshake)
│   ├── hap_pair_verify.c   # Pair verify (Ed25519)
│   ├── hap_crypto.c        # HAP encryption
│   └── srp.c               # SRP-6a key exchange
├── plist/                  # Apple Property List parsing
├── network/                # Network stack
│   ├── wifi.c              # WiFi AP+STA, captive portal, auto-reconnect
│   ├── mdns_airplay.c      # mDNS AirPlay service advertisement
│   ├── ptp_clock.c         # Precision Time Protocol clock
│   ├── ntp_clock.c         # NTP time sync fallback
│   ├── web_server.c        # HTTP config/control server
│   ├── ota.c               # OTA firmware updates
│   ├── dns_server.c        # Captive portal DNS
│   └── log_stream.c        # Remote log streaming
├── dacp_client.c           # DACP (Digital Audio Control Protocol) — button/remote commands
├── playback_control.c      # Unified playback control abstraction
├── buttons.c               # Hardware button input with debounce + auto-repeat
└── led.c                   # LED status indicator

components/
├── dac/                    # Abstract DAC API (Kconfig-selected implementation)
│   └── dac.c               # Dispatch layer → TAS57xx or TAS58xx driver
├── boards/                 # Board support (HAL)
│   ├── board_common.c      # Shared board utilities
│   └── arply-v1/          # ARPLY v1 board init (pins, XSMT mute from RTSP events)
├── spiffs_storage/         # SPIFFS filesystem mount (stores web pages + DSP configs)
├── audio-resampler/        # sinc-based audio resampler (44.1→48kHz)
└── board_utils/            # Board-level utilities
```

## Key Conventions

- **CMake/Kconfig**: The board is selected via `CONFIG_BOARD_ARPLY_V1`; `CONFIG_BOARD_TARGET_PATH` names the directory under `components/boards/` that gets compiled. No `CONFIG_DAC_*` driver is set — the PCM5102A has no control bus, so `components/dac/` degrades to no-ops and volume is applied in software. Buttons remain Kconfig-gated and are off.
- **Component structure**: Each component has its own `CMakeLists.txt` with `idf_component_register()`.
- **SPIFFS**: `data/` contents are flashed to SPIFFS by `-t uploadfs`, which `-t upload` does **not** do. `data/www/` holds the web UI (index, logs, speedtest, eq).
- **Audio pipeline**: AudioReceiver (rtsp) → decoder → AudioBuffer → AudioOutput (I2S/SPDIF/USB). Buffered streams (AAC) use deep jitter buffer; realtime streams (ALAC) use low-latency UDP with early/late timing thresholds.
- **Status LED**: One full breath at boot, breathing while searching for a network, off when idle, steady on when playing. Driven by a dedicated task in `main/led.c` — not a FreeRTOS timer, whose small stack overflows on the end-of-cycle handover.

## Code Quality

**Requirements**: ESP-IDF >= 5.5 (tested against v5.5.2). Older versions may need workarounds.

**Formatting**: LLVM-style, 2-space indent, 80-char column limit. See `.clang-format`.

**Linting**: clang-tidy with bugprone, performance, portability, and readability checks. See `.clang-tidy`.

**Pre-commit hook**: auto-formats staged C/H files with clang-format, runs clang-tidy (requires `build/compile_commands.json`). Install via `git config core.hooksPath .githooks`.

**CI** (`.github/workflows/ci-release.yml`): On push/PR to `main`:
- `format-check`: clang-format dry-run on all C/H files
- `lint-check`: clang-tidy on build output (requires ESP-IDF v5.5 toolchain)
- `build`: compiles the `arply_v1` target
- `release`: auto-creates GitHub release with merged firmware bins (only on push)

**Local tooling** (in `scripts/`):
```bash
scripts/format.sh          # Format all C/H files
scripts/lint.sh            # Run clang-tidy on all C/H files
scripts/lint.sh --fix      # Attempt to auto-fix clang-tidy issues
```

**No unit tests**: This is embedded firmware — no test framework is in place. Manual testing on hardware is required.
