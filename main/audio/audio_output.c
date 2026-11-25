#include "audio_output.h"
#include "rtsp_server.h"

#include "audio_dsp.h"
#include "audio_resample.h"
#include "dsp.h"
#include "test_signal.h"
#include "led.h"
#include "settings.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_receiver.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG           "audio_output"
#define I2S_SCK_PIN   CONFIG_I2S_SCK_IO
#define I2S_BCK_PIN   CONFIG_I2S_BCK_IO
#define I2S_LRCK_PIN  CONFIG_I2S_WS_IO
#define I2S_DOUT_PIN  CONFIG_I2S_DO_IO
#define OUTPUT_RATE   CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_SAMPLES 352

// DMA ring-buffer configuration.  Total DMA latency (in samples) is
//   I2S_DMA_DESC_NUM × I2S_DMA_FRAME_NUM
// which at OUTPUT_RATE gives the hardware pipeline delay in µs.
// Keep these in sync with the i2s_chan_config_t initialisation below.
#define I2S_DMA_DESC_NUM  8
#define I2S_DMA_FRAME_NUM 256

/* Max output frames after resampling one input frame */
#define MAX_RESAMPLE_FRAMES \
  ((size_t)((FRAME_SAMPLES + 2) * ((double)OUTPUT_RATE / 44100) + 16))

#if CONFIG_FREERTOS_UNICORE
#define PLAYBACK_CORE 0
#else
#define PLAYBACK_CORE 1
#endif

static i2s_chan_handle_t tx_handle;
static volatile bool flush_requested = false;
static volatile bool playback_running = false;
static TaskHandle_t playback_task_handle = NULL;
static volatile int source_rate = 44100;
static volatile bool resample_reinit_needed = false;
static volatile audio_channel_mode_t channel_mode = AUDIO_CHANNEL_STEREO;

/* Exact DAC timing: bytes handed to the DMA against bytes it has sent (the
 * on_sent interrupt, one per descriptor). The next written byte leaves the
 * DAC when "sent" reaches "written": exact to a sample, where the average
 * occupancy model (audio_output_get_hardware_latency_us) swings +-2.9 ms
 * with the descriptor phase (measured on scheduled sweeps). */
//
// The driver hands the writer freed descriptors from a queue of DESC_NUM-1,
// dropping the oldest when full. Right after enable the first written byte
// therefore plays one full ring of zeros later (base = ring); after the
// writer was idle (USB build between sources) it lands in the descriptor
// after the one being sent: never less than one descriptor ahead.
#define DMA_DESC_BYTES (I2S_DMA_FRAME_NUM * 2 * (int64_t)sizeof(int16_t))
static portMUX_TYPE s_dma_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_dma_sent;    // bytes the DMA finished sending
static int64_t s_dma_written; // bytes accepted by i2s_channel_write()
static int64_t s_dma_base;    // DMA bytes before the first written one
static int64_t s_dma_sent_us; // when the last descriptor finished

static bool IRAM_ATTR on_dma_sent(i2s_chan_handle_t h, i2s_event_data_t *e,
                                  void *ctx) {
  (void)h;
  (void)ctx;
  portENTER_CRITICAL_ISR(&s_dma_lock);
  s_dma_sent += e->size;
  s_dma_sent_us = esp_timer_get_time();
  portEXIT_CRITICAL_ISR(&s_dma_lock);
  return false;
}

static void dma_count_written(size_t bytes) {
  portENTER_CRITICAL(&s_dma_lock);
  if (s_dma_written + s_dma_base < s_dma_sent + DMA_DESC_BYTES) {
    s_dma_base = s_dma_sent + DMA_DESC_BYTES - s_dma_written; // was idle
  }
  s_dma_written += (int64_t)bytes;
  portEXIT_CRITICAL(&s_dma_lock);
}

/* After (re)enabling the channel the ring starts over. */
static void dma_count_reset(void) {
  portENTER_CRITICAL(&s_dma_lock);
  s_dma_sent = 0;
  s_dma_written = 0;
  s_dma_base = I2S_DMA_DESC_NUM * DMA_DESC_BYTES;
  s_dma_sent_us = esp_timer_get_time();
  portEXIT_CRITICAL(&s_dma_lock);
}

int64_t audio_output_next_play_us(void) {
  portENTER_CRITICAL(&s_dma_lock);
  int64_t ahead = s_dma_written + s_dma_base - s_dma_sent;
  const int64_t at = s_dma_sent_us;
  portEXIT_CRITICAL(&s_dma_lock);
  if (ahead < DMA_DESC_BYTES) {
    ahead = DMA_DESC_BYTES; // writer idle: the next descriptor
  }
  return at + ahead * 1000000LL / (OUTPUT_RATE * 2 * (int64_t)sizeof(int16_t));
}

int32_t audio_output_volume_q15_from_db(float volume_db) {
  if (volume_db <= AUDIO_VOLUME_MIN_DB) {
    return 0;
  }
  if (volume_db >= AUDIO_VOLUME_MAX_DB) {
    return 32768;
  }
  // Map -30..0 dB to 0..1, then square for perceptual control.
  float normalized = (volume_db - AUDIO_VOLUME_MIN_DB) /
                     (AUDIO_VOLUME_MAX_DB - AUDIO_VOLUME_MIN_DB);
  return (int32_t)(normalized * normalized * 32768.0f);
}

int32_t audio_output_volume_q15(void) {
  float volume_db;
  if (settings_get_volume(&volume_db) != ESP_OK) {
    volume_db = AUDIO_VOLUME_DEFAULT_DB;
  }
  return audio_output_volume_q15_from_db(volume_db);
}

static void apply_volume(int16_t *buf, size_t n) {
  // Ramp toward the target gain instead of applying volume changes
  // instantly.  An abrupt gain step mid-waveform is a discontinuity scaled
  // by the signal's current amplitude — the classic volume "zipper" click,
  // audible on every step of the sender's volume slider.  Approach the
  // target exponentially, stepping once per stereo frame (even indices) so
  // both channels always carry the same gain; the /256 divisor gives a
  // ~3 ms time constant and a worst-case per-frame gain step of ~0.4%,
  // with a minimum step of 1 so the ramp always completes.
  static int32_t cur_q15 = -1;
  int32_t target = audio_output_volume_q15();
  if (cur_q15 < 0) {
    cur_q15 = target; // first call: no audio has played yet, jump silently
  }
  for (size_t i = 0; i < n; i++) {
    if ((i & 1) == 0 && cur_q15 != target) {
      int32_t diff = target - cur_q15;
      int32_t step = diff / 256;
      if (step == 0) {
        step = diff > 0 ? 1 : -1;
      }
      cur_q15 += step;
    }
    buf[i] = (int16_t)(((int32_t)buf[i] * cur_q15) >> 15);
  }
}

// Apply the selected channel mode to an interleaved stereo buffer (L,R,...).
// LEFT/RIGHT route the chosen source channel to BOTH outputs so the selected
// track is heard from both speakers; MONO plays the (L+R)/2 downmix on both
// outputs; STEREO leaves the buffer untouched.
static void apply_channel_mode(int16_t *buf, size_t frames) {
  audio_channel_mode_t mode = channel_mode;
  if (mode == AUDIO_CHANNEL_STEREO) {
    return;
  }
  if (mode == AUDIO_CHANNEL_MONO) {
    for (size_t i = 0; i < frames; i++) {
      int16_t m = (int16_t)(((int32_t)buf[i * 2] + buf[i * 2 + 1]) / 2);
      buf[i * 2] = m;
      buf[i * 2 + 1] = m;
    }
    return;
  }
  size_t src = (mode == AUDIO_CHANNEL_RIGHT) ? 1 : 0;
  for (size_t i = 0; i < frames; i++) {
    int16_t s = buf[i * 2 + src];
    buf[i * 2] = s;
    buf[i * 2 + 1] = s;
  }
}

bool audio_output_process(int16_t *buf, size_t frames) {
  dsp_set_output_gain((float)audio_output_volume_q15() / 32768.0f);
  apply_channel_mode(buf, frames);
  return dsp_process(buf, frames);
}

/* ---- Debug probe ------------------------------------------------------
 * Records, inside this task, what goes into the output processing and what
 * goes to the DAC, so the DSP can be checked on the device with real audio.
 * Interleaved frames of 4 samples: in L, in R, out L, out R. */
enum { CAPTURE_IDLE, CAPTURE_RUNNING, CAPTURE_DONE };
static int16_t *s_cap;
static size_t s_cap_frames;
static size_t s_cap_pos;
static _Atomic int s_cap_state = CAPTURE_IDLE;
static int16_t s_cap_in[MAX_RESAMPLE_FRAMES * 2];

esp_err_t audio_output_capture_start(uint32_t seconds) {
  if (atomic_load(&s_cap_state) == CAPTURE_RUNNING) {
    return ESP_ERR_INVALID_STATE;
  }
  heap_caps_free(s_cap);
  s_cap_frames = (size_t)seconds * OUTPUT_RATE;
  s_cap =
      heap_caps_malloc(s_cap_frames * 4 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!s_cap) {
    atomic_store(&s_cap_state, CAPTURE_IDLE);
    return ESP_ERR_NO_MEM;
  }
  s_cap_pos = 0;
  atomic_store(&s_cap_state, CAPTURE_RUNNING);
  return ESP_OK;
}

const int16_t *audio_output_capture_get(size_t *frames) {
  if (atomic_load(&s_cap_state) != CAPTURE_DONE) {
    return NULL;
  }
  *frames = s_cap_frames;
  return s_cap;
}

static inline bool capturing(void) {
  return atomic_load_explicit(&s_cap_state, memory_order_relaxed) ==
         CAPTURE_RUNNING;
}

static void capture(const int16_t *in, const int16_t *out, size_t frames) {
  const size_t n =
      frames < s_cap_frames - s_cap_pos ? frames : s_cap_frames - s_cap_pos;
  int16_t *dst = s_cap + s_cap_pos * 4;
  for (size_t i = 0; i < n; i++) {
    dst[4 * i] = in[2 * i];
    dst[4 * i + 1] = in[2 * i + 1];
    dst[4 * i + 2] = out[2 * i];
    dst[4 * i + 3] = out[2 * i + 1];
  }
  s_cap_pos += n;
  if (s_cap_pos >= s_cap_frames) {
    atomic_store(&s_cap_state, CAPTURE_DONE);
  }
}

static void playback_task(void *arg) {
  int16_t *pcm = malloc((size_t)(FRAME_SAMPLES + 1) * 2 * sizeof(int16_t));
  int16_t *silence = calloc((size_t)FRAME_SAMPLES * 2, sizeof(int16_t));
  int16_t *resample_buf = malloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t));
  if (!pcm || !silence || !resample_buf) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    free(pcm);
    free(silence);
    playback_task_handle = NULL;
    free(resample_buf);
    vTaskDelete(NULL);
    return;
  }

  size_t written;
  while (playback_running) {
    if (resample_reinit_needed) {
      resample_reinit_needed = false;
      audio_resample_init((uint32_t)source_rate, OUTPUT_RATE, 2);
    }
    if (flush_requested) {
      flush_requested = false;
      audio_resample_reset();
      i2s_channel_disable(tx_handle);
      i2s_channel_enable(tx_handle);
      dma_count_reset();
    }
    if (test_signal_active()) {
      // arply's own signal: measurement signals follow the volume (as loud
      // as the user's music) but skip the DSP, listening samples get both,
      // the chime neither.
      uint8_t fx = test_signal_fill(pcm, FRAME_SAMPLES);
      const bool cap = capturing();
      if (cap) {
        memcpy(s_cap_in, pcm, (size_t)FRAME_SAMPLES * 2 * sizeof(int16_t));
      }
      bool volume_done = false;
      if (fx & TEST_SIGNAL_APPLY_DSP) {
        volume_done = audio_output_process(pcm, FRAME_SAMPLES);
      }
      if ((fx & TEST_SIGNAL_FOLLOW_VOLUME) && !volume_done) {
        apply_volume(pcm, (size_t)FRAME_SAMPLES * 2);
      }
      if (cap) {
        capture(s_cap_in, pcm, FRAME_SAMPLES);
      }
      led_audio_feed(pcm, FRAME_SAMPLES);
      i2s_channel_write(tx_handle, pcm,
                        (size_t)FRAME_SAMPLES * 2 * sizeof(int16_t), &written,
                        portMAX_DELAY);
      dma_count_written(written);
      continue;
    }
    size_t samples = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    if (samples > 0) {
      int16_t *play_buf = pcm;
      size_t play_samples = samples;
      if (audio_resample_is_active()) {
        play_samples = audio_resample_process(pcm, samples, resample_buf,
                                              MAX_RESAMPLE_FRAMES);
        play_buf = resample_buf;
      }
      // Channel mode first: the filters belong to the outputs (speakers),
      // not to the source channels. An active DSP applies the volume itself
      // (in float, before its limiter); otherwise it is applied here.
      const bool cap = capturing();
      if (cap) {
        memcpy(s_cap_in, play_buf, play_samples * 2 * sizeof(int16_t));
      }
      if (!audio_output_process(play_buf, play_samples)) {
        apply_volume(play_buf, play_samples * 2);
      }
      if (cap) {
        capture(s_cap_in, play_buf, play_samples);
      }
      led_audio_feed(play_buf, play_samples);
      i2s_channel_write(tx_handle, play_buf, play_samples * 2 * sizeof(int16_t),
                        &written, portMAX_DELAY);
      dma_count_written(written);
      taskYIELD();
    } else {
      // Receiver underflow — output a frame of silence.  Block on the DMA
      // write (portMAX_DELAY) so the write itself paces the loop, instead of a
      // short timeout plus vTaskDelay(1) which produced jittery silence.
      led_audio_feed(silence, FRAME_SAMPLES);
      i2s_channel_write(tx_handle, silence,
                        (size_t)FRAME_SAMPLES * 2 * sizeof(int16_t), &written,
                        portMAX_DELAY);
      dma_count_written(written);
    }
  }

  free(pcm);
  free(silence);
  playback_task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t audio_output_init(void) {
  uint8_t saved_mode;
  if (settings_get_channel_mode(&saved_mode) == ESP_OK &&
      saved_mode <= AUDIO_CHANNEL_MONO) {
    channel_mode = (audio_channel_mode_t)saved_mode;
    ESP_LOGI(TAG, "Loaded channel mode: %d", saved_mode);
  }

  if (channel_mode != AUDIO_CHANNEL_STEREO &&
      audio_output_channel_mode_locked()) {
    ESP_LOGI(TAG, "Dual DAC output: ignoring saved channel mode");
    // Not persisted: the preference is only meaningless while two amps are
    // fitted, so keep it for if the board is ever reconfigured.
    channel_mode = AUDIO_CHANNEL_STEREO;
  }

  audio_dsp_init(OUTPUT_RATE);
  test_signal_init(OUTPUT_RATE);

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
  chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
  // Zero each DMA descriptor after it is sent.  Without this, a writer
  // stall longer than the DMA ring (~46 ms — e.g. an NVS/flash write
  // disabling the cache, or a CPU burst from the web server) makes the
  // hardware REPLAY the stale ring contents in a loop: a loud stutter, then
  // a second discontinuity on recovery.  With auto_clear an underrun
  // degrades to plain silence.
  chan_cfg.auto_clear = true;

  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, NULL), TAG,
                      "channel create failed");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUTPUT_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_SCK_PIN,
              .bclk = I2S_BCK_PIN,
              .ws = I2S_LRCK_PIN,
              .dout = I2S_DOUT_PIN,
              .din = I2S_GPIO_UNUSED,
          },
  };

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG,
                      "std mode init failed");
  const i2s_event_callbacks_t cbs = {.on_sent = on_dma_sent};
  ESP_RETURN_ON_ERROR(
      i2s_channel_register_event_callback(tx_handle, &cbs, NULL), TAG,
      "DMA callback failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG,
                      "channel enable failed");
  dma_count_reset();
  ESP_LOGI(TAG, "I2S initialized: Rate=%u, DMA_Desc=%d, DMA_Frame=%d",
           (unsigned int)OUTPUT_RATE, I2S_DMA_DESC_NUM, I2S_DMA_FRAME_NUM);

  audio_resample_init(44100, OUTPUT_RATE, 2);

  return ESP_OK;
}

void audio_output_start(void) {
  if (playback_task_handle != NULL) {
    return; // already running
  }
  playback_running = true;
  xTaskCreatePinnedToCore(playback_task, "audio_play", 4096, NULL, 7,
                          &playback_task_handle, PLAYBACK_CORE);
}

bool audio_output_is_running(void) {
  return playback_task_handle != NULL;
}

void audio_output_stop(void) {
  if (playback_task_handle == NULL) {
    return;
  }
  playback_running = false;
  // Wait for task to exit cleanly
  int timeout = 40;
  while (playback_task_handle != NULL && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (playback_task_handle != NULL) {
    ESP_LOGW(TAG, "Playback task did not exit within timeout");
  } else {
    ESP_LOGI(TAG, "Playback task stopped");
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  size_t written = 0;
  const esp_err_t err =
      i2s_channel_write(tx_handle, data, bytes, &written, wait);
  dma_count_written(written);
  return err;
}

void audio_output_set_sample_rate(uint32_t rate) {
  // Only safe to call when no writer task is actively using I2S
  // (AirPlay playback task must be stopped, BT calls this before
  // the I2S writer task starts consuming data)
  ESP_LOGI(TAG, "Setting sample rate to %" PRIu32 " Hz", rate);
  i2s_channel_disable(tx_handle);
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
  i2s_channel_enable(tx_handle);
  dma_count_reset();
}

void audio_output_flush(void) {
  flush_requested = true;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != source_rate) {
    source_rate = rate;
    resample_reinit_needed = true;
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  // Delay between i2s_channel_write() accepting a sample and that sample
  // leaving the DAC.  This is the DMA ring occupancy AHEAD of the newly
  // written data, which is NOT the full ring: i2s_channel_write() blocks
  // only until space frees, so the writer refills as soon as a descriptor
  // completes and steady-state occupancy oscillates between
  // (DESC_NUM - 1) and DESC_NUM descriptors.
  //
  // Using the full ring (DESC_NUM) overstates the delay by half a
  // descriptor on average — 2.9 ms at 44.1 kHz with the config below — and
  // that bias lands directly in compute_early_us(), pushing every frame
  // toward the "late" side of the threshold.  Model the midpoint instead:
  //   (DESC_NUM - 0.5) x FRAME_NUM == (2*DESC_NUM - 1) x FRAME_NUM / 2
  // The residual +/-2.9 ms swing is real jitter that the drift servo in
  // audio_timing.c absorbs; only the constant bias is removed here.
  return (uint32_t)((((uint64_t)(2 * I2S_DMA_DESC_NUM - 1) * I2S_DMA_FRAME_NUM *
                      1000000ULL) /
                     2) /
                    OUTPUT_RATE);
}

/* With two amplifiers the DAC configuration already fixes the routing, so a
 * channel selection on top of that would only mute a speaker. */
bool audio_output_channel_mode_locked(void) {
  return false;
}

audio_channel_mode_t audio_output_cycle_channel_mode(void) {
  if (audio_output_channel_mode_locked()) {
    return channel_mode;
  }
  audio_channel_mode_t next;
  switch (channel_mode) {
  case AUDIO_CHANNEL_STEREO:
    next = AUDIO_CHANNEL_LEFT;
    break;
  case AUDIO_CHANNEL_LEFT:
    next = AUDIO_CHANNEL_RIGHT;
    break;
  case AUDIO_CHANNEL_RIGHT:
    next = AUDIO_CHANNEL_MONO;
    break;
  default:
    next = AUDIO_CHANNEL_STEREO;
    break;
  }
  audio_output_set_channel_mode(next);
  return next;
}

static void change_channel_mode(audio_channel_mode_t mode, bool persist) {
  if (audio_output_channel_mode_locked()) {
    return;
  }
  if (mode > AUDIO_CHANNEL_MONO) {
    mode = AUDIO_CHANNEL_STEREO;
  }
  bool changed = mode != channel_mode;
  channel_mode = mode;
  // The EQ filters belong to the outputs, so a new routing needs the filter
  // sets re-resolved (see audio_dsp.h).
  audio_dsp_on_channel_mode(mode);
  if (persist) {
    settings_set_channel_mode((uint8_t)mode);
  }
  if (changed || persist) {
    ESP_LOGI(TAG, "Channel mode: %s%s",
             mode == AUDIO_CHANNEL_LEFT    ? "LEFT only"
             : mode == AUDIO_CHANNEL_RIGHT ? "RIGHT only"
             : mode == AUDIO_CHANNEL_MONO  ? "MONO (L+R)"
                                           : "STEREO",
             persist ? "" : " (preview)");
  }
}

void audio_output_set_channel_mode(audio_channel_mode_t mode) {
  change_channel_mode(mode, true);
}

void audio_output_preview_channel_mode(audio_channel_mode_t mode) {
  change_channel_mode(mode, false);
}

void audio_output_restore_channel_mode(void) {
  uint8_t saved;
  if (settings_get_channel_mode(&saved) != ESP_OK ||
      saved > AUDIO_CHANNEL_MONO) {
    saved = AUDIO_CHANNEL_STEREO;
  }
  change_channel_mode((audio_channel_mode_t)saved, false);
}

audio_channel_mode_t audio_output_get_channel_mode(void) {
  return channel_mode;
}
