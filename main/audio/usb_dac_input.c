/**
 * USB Audio Class (UAC) input — ESP32 as a USB audio DAC
 *
 * The ESP32 enumerates as a USB speaker on the connected host (PC, phone
 * with a USB-C/Lightning-to-USB adapter, etc). Whatever the host plays
 * arrives via the UAC output callback and is written straight to the
 * same I2S/DAC output AirPlay normally drives (audio_output_write()),
 * automatically pausing/resuming the AirPlay playback task around it.
 *
 * "Automatic detection" of which source is active falls out of TinyUSB
 * itself: the host OS opens the speaker's isochronous streaming
 * interface the instant something starts playing to this device, and
 * closes it the instant playback stops — see tud_audio_set_itf_cb() /
 * tud_audio_set_itf_close_EP_cb() in the vendored usb_device_uac
 * component. We piggyback on that by tracking how recently output_cb
 * last delivered data: fresh data means "USB is the active source",
 * silence for USB_IDLE_TIMEOUT_MS means the host stopped (or the cable
 * came out), and AirPlay resumes.
 *
 * Uses the espressif/usb_device_uac managed component in
 * CONFIG_USB_DEVICE_UAC_AS_PART mode — see main/usb/tusb_config.h and
 * main/usb/usb_descriptors.c for the composite UAC + CDC descriptor
 * this requires.
 */

#include "usb_dac_input.h"

#include "audio_output.h"
#include "playback_control.h"
#include "led.h"
#include "rtsp_events.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_device_uac.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "usb_dac"

// Must match CONFIG_UAC_SAMPLE_RATE (enforced below) and the I2S output
// rate, so no runtime resampling is needed on this path.
#define OUTPUT_RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ

#if CONFIG_OUTPUT_SAMPLE_RATE_HZ != CONFIG_UAC_SAMPLE_RATE
#error \
    "USB DAC input requires CONFIG_OUTPUT_SAMPLE_RATE_HZ to match CONFIG_UAC_SAMPLE_RATE"
#endif

// How long without fresh USB audio before falling back to AirPlay.
// Generous enough to survive normal track-to-track gaps and brief host
// stalls without audibly ping-ponging between sources.
#define USB_IDLE_TIMEOUT_MS 500
#define ARBITER_POLL_MS     20

// ~400 ms of stereo 16-bit audio at the configured rate — slack against
// scheduling jitter between the USB receive side (usb_spk_task, driven by
// the vendored UAC component on a CONFIG_UAC_SPK_INTERVAL_MS cadence) and
// the arbiter/I2S consumer side.
#define USB_RINGBUF_BYTES ((size_t)(OUTPUT_RATE) / 5 * 8)

// How often to log a dropped-audio warning, at most — drops likely come in
// bursts (one scheduling hiccup drops several chunks in a row), so this
// keeps logging itself from adding to the load instead of just reporting it.
#define DROP_LOG_INTERVAL_US (1000 * 1000)

#if CONFIG_FREERTOS_UNICORE
#define ARBITER_CORE 0
#else
#define ARBITER_CORE 1
#endif

// Declared in usb_descriptors.c — the UAC streaming interface number in
// this project's composite descriptor layout.
extern uint8_t const usb_dac_spk_itf_num;

static RingbufHandle_t s_ringbuf;
static volatile int64_t s_last_rx_us = 0;
static volatile bool s_usb_active = false;
static volatile int32_t s_volume_percent = 100;
static volatile bool s_muted = false;
static uint32_t s_drop_count = 0;
static int64_t s_last_drop_log_us = 0;
static int64_t s_last_underrun_log_us = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_rx_calls = 0;
static int64_t s_last_rate_log_us = 0;

/* ── UAC output callback (push model) ─────────────────────────────────
 * Called by usb_device_uac's own usb_spk_task, on its
 * CONFIG_UAC_SPK_INTERVAL_MS (10 ms) cadence — not a hardware ISR, but
 * also not somewhere that can afford to block for long: usb_spk_task
 * blocking here for longer than that cadence falls behind the actual USB
 * isochronous OUT traffic, which risks TinyUSB missing its own 1 ms
 * service deadlines and glitching audio well beyond just this chunk. So
 * this send is non-blocking — if the ring buffer's momentarily full
 * (arbiter/I2S consumer briefly behind), drop this chunk and keep the USB
 * receive side moving rather than stall it waiting for room.           */
static esp_err_t usb_output_cb(uint8_t *buf, size_t len, void *cb_ctx) {
  (void)cb_ctx;
  s_last_rx_us = esp_timer_get_time();
  s_rx_bytes += (uint32_t)len;
  s_rx_calls++;
  if (s_last_rx_us - s_last_rate_log_us >= DROP_LOG_INTERVAL_US) {
    // Expected steady-state: OUTPUT_RATE * 4 bytes/sec (stereo 16-bit) and
    // 1000/CONFIG_UAC_SPK_INTERVAL_MS calls/sec. Big shortfalls on either
    // number point at the USB host side, not our own consumption logic.
    ESP_LOGI(TAG,
             "USB rx: %" PRIu32 " bytes, %" PRIu32 " calls in the last "
             "~%" PRId64 " ms",
             s_rx_bytes, s_rx_calls,
             (s_last_rx_us - s_last_rate_log_us) / 1000);
    s_rx_bytes = 0;
    s_rx_calls = 0;
    s_last_rate_log_us = s_last_rx_us;
  }
  if (s_ringbuf && xRingbufferSend(s_ringbuf, buf, len, 0) != pdTRUE) {
    s_drop_count++;
    int64_t now = esp_timer_get_time();
    if (now - s_last_drop_log_us >= DROP_LOG_INTERVAL_US) {
      ESP_LOGW(TAG, "Ring buffer full — dropped %" PRIu32 " chunk(s)",
               s_drop_count);
      s_drop_count = 0;
      s_last_drop_log_us = now;
    }
  }
  return ESP_OK;
}

static void usb_set_volume_cb(uint32_t volume, void *cb_ctx) {
  (void)cb_ctx;
  // Component already maps the feature unit's dB range to 0..100.
  s_volume_percent = (int32_t)volume;
  ESP_LOGI(TAG, "Host volume: %d%%", (int)s_volume_percent);
}

static void usb_set_mute_cb(uint32_t mute, void *cb_ctx) {
  (void)cb_ctx;
  s_muted = mute != 0;
  ESP_LOGI(TAG, "Host mute: %s", s_muted ? "on" : "off");
}

static void apply_gain(int16_t *buf, size_t n) {
  if (s_muted) {
    memset(buf, 0, n * sizeof(int16_t));
    return;
  }
  int32_t vol = s_volume_percent;
  if (vol >= 100) {
    return;
  }
  if (vol < 0) {
    vol = 0;
  }
  int32_t q15 = (vol * 32767) / 100;
  for (size_t i = 0; i < n; i++) {
    buf[i] = (int16_t)(((int32_t)buf[i] * q15) >> 15);
  }
}

/* ── Source arbiter task ───────────────────────────────────────────────
 * Polls "is fresh USB audio arriving" and switches the I2S output
 * between the AirPlay playback task and USB-forwarded PCM accordingly. */
static void arbiter_task(void *arg) {
  (void)arg;

  while (true) {
    bool have_data = s_ringbuf != NULL;
    int64_t idle_ms =
        s_last_rx_us == 0 ? -1 : (esp_timer_get_time() - s_last_rx_us) / 1000;
    bool usb_fresh = have_data && idle_ms >= 0 && idle_ms < USB_IDLE_TIMEOUT_MS;

    if (usb_fresh && !s_usb_active) {
      ESP_LOGI(TAG, "USB audio started — pausing AirPlay output");
      audio_output_stop();
      playback_control_set_source(PLAYBACK_SOURCE_USB);
      // The board's XSMT mute pin only follows RTSP events (see
      // components/boards/arply-v1/board.c) and boots muted, since it's
      // normally driven by actual AirPlay playback. USB input never goes
      // through RTSP, so without this, a device that's never had an
      // AirPlay session play would stay muted forever even with USB audio
      // flowing correctly — playback_control.c's own local mute toggle
      // uses this same re-emit trick for the same reason.
      rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
      s_usb_active = true;
    } else if (!usb_fresh && s_usb_active) {
      ESP_LOGI(TAG, "USB audio stopped — resuming AirPlay output");
      s_usb_active = false;
      playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
      audio_output_start();
    }

    if (s_usb_active) {
      // Drain whatever has arrived and forward it to the DAC. A short
      // receive timeout keeps this responsive to the idle-timeout check
      // above even when the host briefly stops sending.
      size_t item_size = 0;
      void *data = xRingbufferReceiveUpTo(s_ringbuf, &item_size,
                                          pdMS_TO_TICKS(20), 4096);
      if (data && item_size > 0) {
        apply_gain((int16_t *)data, item_size / sizeof(int16_t));
        led_audio_feed((int16_t *)data, item_size / sizeof(int16_t) / 2);
        audio_output_write(data, item_size, portMAX_DELAY);
        vRingbufferReturnItem(s_ringbuf, data);
      } else {
        // Ring buffer ran dry while USB is supposedly active — the I2S
        // side is about to (or already did) underrun. Distinct from the
        // drop counter in usb_output_cb: that one fires when the producer
        // outruns us, this one fires when the producer falls behind (or
        // audio_output_write() above is itself the bottleneck, blocking
        // on I2S DMA backpressure for longer than this task's 20 ms
        // receive window).
        int64_t now = esp_timer_get_time();
        if (now - s_last_underrun_log_us >= DROP_LOG_INTERVAL_US) {
          ESP_LOGW(TAG, "Ring buffer underrun — no USB audio to forward");
          s_last_underrun_log_us = now;
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(ARBITER_POLL_MS));
    }
  }
}

// Required TinyUSB device lifecycle callbacks — the usb_device_uac
// component only provides these when CONFIG_USB_DEVICE_UAC_AS_PART is
// unset, so this project's own composite descriptor build must supply
// them itself.
void tud_mount_cb(void) {
  ESP_LOGI(TAG, "USB mounted");
}
void tud_umount_cb(void) {
  ESP_LOGI(TAG, "USB unmounted");
}
void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  ESP_LOGI(TAG, "USB suspended");
}
void tud_resume_cb(void) {
  ESP_LOGI(TAG, "USB resumed");
}

esp_err_t usb_dac_input_init(void) {
  ESP_LOGI(TAG, "Initialising USB DAC input (rate=%d)", OUTPUT_RATE);

  s_ringbuf = xRingbufferCreate(USB_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
  ESP_RETURN_ON_FALSE(s_ringbuf != NULL, ESP_ERR_NO_MEM, TAG,
                      "Failed to create ring buffer");

  uac_device_config_t uac_cfg = {
      .skip_tinyusb_init = false,
      .output_cb = usb_output_cb, // host-to-device: USB audio in
      .input_cb = NULL,           // no mic direction in this build
      .set_mute_cb = usb_set_mute_cb,
      .set_volume_cb = usb_set_volume_cb,
      .cb_ctx = NULL,
      .spk_itf_num = usb_dac_spk_itf_num,
      .mic_itf_num = 0,
  };

  esp_err_t err = uac_device_init(&uac_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UAC device init failed: %s", esp_err_to_name(err));
    vRingbufferDelete(s_ringbuf);
    s_ringbuf = NULL;
    return err;
  }

  xTaskCreatePinnedToCore(arbiter_task, "usb_dac_arb", 4096, NULL, 7, NULL,
                          ARBITER_CORE);

  ESP_LOGI(TAG, "USB DAC input ready");
  return ESP_OK;
}
