#include "test_signal.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_timing.h"
#include "board_common.h"
#include "dsp_signal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtsp_events.h"

static const char *TAG = "test_signal";

#define MAX_SEGS     (2 * TEST_SIGNAL_MAX_SWEEPS + 4)
#define CHUNK        128
#define STOP_FADE_MS 5
#define NOISE_DBFS   (-15.0f)
#define SWEEP_AMP    0.25f
// After the last sample leaves the task it still has to cross the DMA ring
// (~46 ms) before the DAC may be muted again.
#define REMUTE_DELAY_US 150000

// SEG_LOOP repeats the plan's PCM until stopped (listening samples).
typedef enum {
  SEG_SILENCE,
  SEG_SWEEP,
  SEG_NOISE,
  SEG_PCM,
  SEG_LOOP
} seg_kind_t;

typedef struct {
  seg_kind_t kind;
  uint8_t outs;
  uint32_t frames;
} seg_t;

typedef struct {
  seg_t seg[MAX_SEGS];
  int count;
  const int16_t *pcm;
  float pcm_gain;
  size_t loop_frames;
  int loop_channels;
  uint8_t flags; // TEST_SIGNAL_FOLLOW_VOLUME | TEST_SIGNAL_APPLY_DSP
  int64_t at_us; // > 0: the first non-silent sample leaves the DAC then
} plan_t;

// IDLE -> CLAIMED (a requester is writing s_req) -> REQUESTED -> RUNNING.
enum { STATE_IDLE, STATE_CLAIMED, STATE_REQUESTED, STATE_RUNNING };

static uint32_t s_fs = 44100;
static _Atomic int s_state = STATE_IDLE;
static atomic_bool s_stop;
static plan_t s_req; // written only by the requester holding CLAIMED
static esp_timer_handle_t s_remute_timer;

// Audio task state.
static plan_t s_run;
static int s_seg;
static uint32_t s_pos;
static float s_fade;
static dsp_sweep_t s_sweep;
static dsp_pink_t s_pink;
static float s_tmp[CHUNK];

static uint32_t ms_to_frames(uint32_t ms) {
  return (uint32_t)((uint64_t)ms * s_fs / 1000);
}

static void remute_cb(void *arg) {
  (void)arg;
  // A new signal may have started since this timer was armed.
  if (atomic_load(&s_state) == STATE_IDLE) {
    iot_board_set_signal_unmute(false);
  }
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  if (event == RTSP_EVENT_PLAYING && test_signal_active()) {
    ESP_LOGI(TAG, "Stream started, stopping test signal");
    test_signal_stop();
  }
}

void test_signal_init(uint32_t sample_rate) {
  s_fs = sample_rate;
  if (!s_remute_timer) {
    const esp_timer_create_args_t args = {.callback = remute_cb,
                                          .name = "sig_remute"};
    esp_timer_create(&args, &s_remute_timer);
    rtsp_events_register(on_rtsp_event, NULL);
  }
}

static bool output_busy(void) {
  return !audio_output_is_running() ||
         (audio_receiver_is_playing() && audio_receiver_has_data());
}

/* Hand a plan to the audio task. Fails if anything owns the output or
 * another signal is pending or playing. */
static esp_err_t submit(const plan_t *p) {
  int expected = STATE_IDLE;
  if (output_busy() ||
      !atomic_compare_exchange_strong(&s_state, &expected, STATE_CLAIMED)) {
    return ESP_ERR_INVALID_STATE;
  }
  s_req = *p;
  atomic_store(&s_stop, false);
  esp_timer_stop(s_remute_timer);
  iot_board_set_signal_unmute(true);
  atomic_store(&s_state, STATE_REQUESTED);
  return ESP_OK;
}

static seg_t silence(uint32_t ms) {
  return (seg_t){.kind = SEG_SILENCE, .outs = 0, .frames = ms_to_frames(ms)};
}

esp_err_t test_signal_sweep(const uint8_t *outs, int count, uint32_t gap_ms,
                            int64_t at_us) {
  if (count < 1 || count > TEST_SIGNAL_MAX_SWEEPS) {
    return ESP_ERR_INVALID_ARG;
  }
  plan_t p = {0};
  p.flags = TEST_SIGNAL_FOLLOW_VOLUME;
  p.at_us = at_us;
  p.seg[p.count++] = silence(TEST_SIGNAL_LEAD_MS);
  p.seg[p.count++] = (seg_t){.kind = SEG_NOISE,
                             .outs = TEST_SIGNAL_OUT_BOTH,
                             .frames = ms_to_frames(TEST_SIGNAL_WAKE_MS)};
  p.seg[p.count++] = silence(TEST_SIGNAL_WAKE_REST_MS);
  dsp_sweep_t probe;
  dsp_sweep_init(&probe, s_fs, TEST_SIGNAL_SWEEP_MS / 1000.0, 20, 20000,
                 SWEEP_AMP);
  for (int i = 0; i < count; i++) {
    p.seg[p.count++] = (seg_t){
        .kind = SEG_SWEEP, .outs = outs[i], .frames = (uint32_t)probe.len};
    p.seg[p.count++] = silence(gap_ms);
  }
  esp_err_t err = submit(&p);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Sweep x%d, gap %u ms", count, (unsigned)gap_ms);
  }
  return err;
}

esp_err_t test_signal_noise(uint8_t outs, uint32_t max_ms) {
  plan_t p = {0};
  p.flags = TEST_SIGNAL_FOLLOW_VOLUME;
  p.seg[p.count++] = silence(100); // XSMT settles before the noise starts
  p.seg[p.count++] =
      (seg_t){.kind = SEG_NOISE, .outs = outs, .frames = ms_to_frames(max_ms)};
  esp_err_t err = submit(&p);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Noise on outputs 0x%x for up to %u ms", outs,
             (unsigned)max_ms);
  }
  return err;
}

esp_err_t test_signal_pcm(const int16_t *pcm, size_t samples, float gain) {
  plan_t p = {0};
  p.seg[p.count++] = silence(100);
  p.seg[p.count++] = (seg_t){.kind = SEG_PCM,
                             .outs = TEST_SIGNAL_OUT_BOTH,
                             .frames = (uint32_t)samples};
  p.seg[p.count++] = silence(100);
  p.pcm = pcm;
  p.pcm_gain = fminf(fmaxf(gain, 0.0f), 1.0f) / 32768.0f;
  return submit(&p);
}

esp_err_t test_signal_listen(const int16_t *pcm, size_t frames, int channels) {
  if (!pcm || frames == 0 || channels < 1 || channels > 2) {
    return ESP_ERR_INVALID_ARG;
  }
  plan_t p = {0};
  p.flags = TEST_SIGNAL_FOLLOW_VOLUME | TEST_SIGNAL_APPLY_DSP;
  p.seg[p.count++] = silence(100);
  p.seg[p.count++] = (seg_t){
      .kind = SEG_LOOP, .outs = TEST_SIGNAL_OUT_BOTH, .frames = UINT32_MAX};
  p.pcm = pcm;
  p.loop_frames = frames;
  p.loop_channels = channels;
  esp_err_t err = submit(&p);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Listening sample: %u frames, %d ch, looping",
             (unsigned)frames, channels);
  }
  return err;
}

bool test_signal_wait_idle(uint32_t timeout_ms) {
  for (uint32_t t = 0; test_signal_active() && t < timeout_ms; t += 10) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return !test_signal_active();
}

void test_signal_stop(void) {
  if (atomic_load(&s_state) == STATE_IDLE) {
    return;
  }
  atomic_store(&s_stop, true);
  if (!audio_output_is_running()) {
    // Nobody will consume the request (USB took the output): drop it here.
    atomic_store(&s_state, STATE_IDLE);
    iot_board_set_signal_unmute(false);
  }
}

bool test_signal_active(void) {
  return atomic_load(&s_state) != STATE_IDLE;
}

static void start_segment(void) {
  s_pos = 0;
  if (s_seg >= s_run.count) {
    return;
  }
  const seg_t *seg = &s_run.seg[s_seg];
  if (seg->kind == SEG_SWEEP) {
    dsp_sweep_init(&s_sweep, s_fs, TEST_SIGNAL_SWEEP_MS / 1000.0, 20, 20000,
                   SWEEP_AMP);
  } else if (seg->kind == SEG_NOISE) {
    dsp_pink_init(&s_pink, NOISE_DBFS, (uint32_t)esp_timer_get_time() | 1u);
  }
}

static void finish(void) {
  atomic_store(&s_state, STATE_IDLE);
  esp_timer_start_once(s_remute_timer, REMUTE_DELAY_US);
  ESP_LOGI(TAG, "Signal finished");
}

/* Loop the listening sample into @p out (interleaved stereo). Returns true
 * once a stop fade has reached silence. */
static bool fill_loop(int16_t *out, size_t n, bool stopping, float fade_step) {
  const int16_t *pcm = s_run.pcm;
  const int ch = s_run.loop_channels;
  for (size_t i = 0; i < n; i++) {
    const size_t at = s_pos * (size_t)ch;
    int32_t l = pcm[at];
    int32_t r = ch == 2 ? pcm[at + 1] : l;
    if (stopping) {
      s_fade = s_fade > fade_step ? s_fade - fade_step : 0.0f;
      l = (int32_t)((float)l * s_fade);
      r = (int32_t)((float)r * s_fade);
    }
    out[2 * i] = (int16_t)l;
    out[2 * i + 1] = (int16_t)r;
    if (++s_pos >= s_run.loop_frames) {
      s_pos = 0;
    }
  }
  return stopping && s_fade <= 0.0f;
}

static inline int16_t to_s16(float v) {
  float s = v * 32767.0f;
  s = s > 32767.0f ? 32767.0f : (s < -32768.0f ? -32768.0f : s);
  return (int16_t)lrintf(s);
}

uint8_t test_signal_fill(int16_t *buf, size_t frames) {
  if (atomic_load(&s_state) == STATE_REQUESTED) {
    s_run = s_req;
    if (s_run.at_us > 0 && s_run.seg[0].kind == SEG_SILENCE) {
      // The first sample of this buffer leaves the DAC at
      // audio_output_next_play_us(); the lead fills the time up to at_us.
      const int64_t now = esp_timer_get_time();
      const int64_t next = audio_output_next_play_us();
      int64_t before_us = 0; // wake noise and rest between lead and sweep
      for (int i = 1; i < s_run.count && s_run.seg[i].kind != SEG_SWEEP; i++) {
        before_us += (int64_t)s_run.seg[i].frames * 1000000 / s_fs;
      }
      const int64_t lead_us = s_run.at_us - next - before_us;
      ESP_LOGI(TAG, "Scheduled: DAC delay %lld us (model %u us)",
               (long long)(next - now),
               (unsigned)audio_timing_get_playout_delay_us());
      s_run.seg[0].frames =
          lead_us > 0 ? (uint32_t)(lead_us * (int64_t)s_fs / 1000000) + 1 : 1;
      if (lead_us < 0) {
        ESP_LOGW(TAG, "Scheduled start %lld us too late", (long long)-lead_us);
      }
    }
    s_seg = 0;
    s_fade = 1.0f;
    start_segment();
    atomic_store(&s_state, STATE_RUNNING);
  }
  memset(buf, 0, frames * 2 * sizeof(int16_t));
  if (atomic_load(&s_state) != STATE_RUNNING) {
    return 0;
  }
  const uint8_t flags = s_run.flags;

  const float fade_step = 1000.0f / (STOP_FADE_MS * (float)s_fs);
  size_t done = 0;
  while (done < frames && s_seg < s_run.count) {
    const seg_t *seg = &s_run.seg[s_seg];
    size_t n = frames - done;
    if (n > CHUNK) {
      n = CHUNK;
    }
    if (seg->kind == SEG_LOOP) {
      if (fill_loop(&buf[2 * done], n, atomic_load(&s_stop),
                    1000.0f / (STOP_FADE_MS * (float)s_fs))) {
        finish();
        return flags;
      }
      done += n;
      continue;
    }
    if (n > seg->frames - s_pos) {
      n = seg->frames - s_pos;
    }
    switch (seg->kind) {
    case SEG_SWEEP:
      n = dsp_sweep_fill(&s_sweep, s_tmp, n);
      break;
    case SEG_NOISE:
      dsp_pink_fill(&s_pink, s_tmp, n);
      break;
    case SEG_PCM:
      for (size_t i = 0; i < n; i++) {
        s_tmp[i] = (float)s_run.pcm[s_pos + i] * s_run.pcm_gain;
      }
      break;
    default:
      memset(s_tmp, 0, n * sizeof(float));
      break;
    }
    const bool stopping = atomic_load(&s_stop);
    for (size_t i = 0; i < n; i++) {
      float v = s_tmp[i];
      if (stopping) {
        s_fade = s_fade > fade_step ? s_fade - fade_step : 0.0f;
        v *= s_fade;
      }
      const int16_t s = to_s16(v);
      int16_t *out = &buf[2 * (done + i)];
      out[0] = (seg->outs & TEST_SIGNAL_OUT_L) ? s : 0;
      out[1] = (seg->outs & TEST_SIGNAL_OUT_R) ? s : 0;
    }
    done += n;
    s_pos += (uint32_t)n;
    if (stopping && s_fade <= 0.0f) {
      finish();
      return flags;
    }
    if (s_pos >= seg->frames || n == 0) {
      s_seg++;
      start_segment();
    }
  }
  if (s_seg >= s_run.count) {
    finish();
  }
  return flags;
}
