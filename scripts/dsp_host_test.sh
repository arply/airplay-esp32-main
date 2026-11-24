#!/usr/bin/env bash
# Build and run the DSP host test (components/dsp, no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -Icomponents/dsp/include \
  components/dsp/dsp.c components/dsp/dsp_biquad.c components/dsp/dsp_signal.c \
  components/dsp/test/host_test.c \
  -lm -o "$out/dsp_host_test"
"$out/dsp_host_test"
