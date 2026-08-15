#pragma once

#include "esp_err.h"

/**
 * USB-DAC input: exposes arply as a USB Audio Class 2.0 speaker in
 * addition to its normal AirPlay receiver. When a connected computer
 * plays audio to arply over USB, that audio automatically takes over
 * the I2S/DAC output; AirPlay resumes automatically once the USB
 * stream stops (or the cable is unplugged).
 *
 * Also brings up a CDC-ACM port as part of the composite USB descriptor
 * (the custom TinyUSB stack this requires takes over the same USB PHY
 * the fixed-function USB Serial/JTAG console normally uses, so that
 * console is unavailable regardless). Console logs are not routed to
 * it — the web UI's own log streaming (main/network/log_stream.c)
 * already covers that and, unlike a CDC redirect, doesn't fight it for
 * the single global esp_log_set_vprintf() hook.
 *
 * Only meaningful when CONFIG_AUDIO_INPUT_USB_DAC is enabled — the I2S
 * output backend (audio_output.c) must be the active audio_output_*
 * implementation, since USB-sourced PCM is written through its
 * audio_output_write() API exactly like the AirPlay path.
 */

/**
 * Bring up the composite USB device (UAC speaker + CDC console) and
 * start the source-arbitration task. Call once, after audio_output_init().
 */
esp_err_t usb_dac_input_init(void);
