/**
 * @file dsp.c
 * @brief Output DSP chain: preamp → EQ → bass enhancement → limiter → dither.
 *
 * Configuration hand-over without a lock in the audio path: three runtime
 * slots, and one atomic word holding which slot is active and which (if any)
 * is pending. A writer fills a slot that is neither, then publishes it as
 * pending; the audio task promotes pending to active at the start of its
 * next call. With two slots a writer could overwrite the slot the audio
 * task had just switched to; with three, the slot being written is never in
 * use.
 */

#include "dsp.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "dsp_biquad.h"

#define NUM_SLOTS  3
#define SLOT_NONE  0xFFu
#define CHUNK      128
#define PBE_STAGES 6
#define PBE_GAIN   2.5f

// Soft limiter knee at -1 dBFS.
#define LIMIT_T 0.89125094f

enum { PBE_HP_MAIN, PBE_LP1, PBE_LP2, PBE_HP2, PBE_HP3, PBE_LP3 };

typedef struct {
  dsp_coeffs_t k[DSP_NUM_CH][DSP_MAX_FILTERS_PER_CH];
  dsp_state_t s[DSP_NUM_CH][DSP_MAX_FILTERS_PER_CH];
  uint8_t n[DSP_NUM_CH];
  dsp_coeffs_t pbe_k[PBE_STAGES];
  dsp_state_t pbe_s[DSP_NUM_CH][PBE_STAGES];
  float pbe_gain;
  bool pbe_on;
  float preamp;  // linear, includes the int16 -> float scale
  bool identity; // flat: leave samples untouched
  uint32_t structure;
} runtime_t;

static runtime_t s_slots[NUM_SLOTS];
static _Atomic uint32_t s_word; // bits 0-7 active, bits 8-15 pending
static atomic_flag s_writer = ATOMIC_FLAG_INIT;
static atomic_bool s_bypass;
static dsp_config_t s_cfg; // guarded by s_writer
static double s_fs = 44100.0;

// Audio-task-only state.
static float s_work[DSP_NUM_CH][CHUNK];
static float s_pbe_tmp[DSP_NUM_CH][CHUNK];
static bool s_was_idle = true;
static uint32_t s_rng = 0x12345678u;

static inline uint32_t word_make(uint32_t active, uint32_t pending) {
  return active | (pending << 8);
}

static float clampf(float v, float lo, float hi) {
  // fmaxf/fminf return the non-NaN argument, so NaN lands on a bound.
  return fminf(fmaxf(v, lo), hi);
}

void dsp_clamp_config(dsp_config_t *cfg) {
  const float f_max = fminf(DSP_F_MAX, (float)(0.45 * s_fs));
  cfg->preamp_db = clampf(cfg->preamp_db, DSP_PREAMP_MIN, DSP_PREAMP_MAX);
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    if (cfg->count[ch] > DSP_MAX_FILTERS_PER_CH) {
      cfg->count[ch] = DSP_MAX_FILTERS_PER_CH;
    }
    for (int i = 0; i < cfg->count[ch]; i++) {
      dsp_filter_t *f = &cfg->filters[ch][i];
      if ((unsigned)f->type >= DSP_FILTER_TYPE_COUNT) {
        f->type = DSP_FILTER_PEAK;
        f->enabled = false;
      }
      f->f = clampf(f->f, DSP_F_MIN, f_max);
      f->gain_db = clampf(f->gain_db, DSP_GAIN_MIN, DSP_GAIN_MAX);
      f->q = clampf(f->q, DSP_Q_MIN, DSP_Q_MAX);
    }
  }
  cfg->pbe.fc = clampf(cfg->pbe.fc, DSP_PBE_FC_MIN, DSP_PBE_FC_MAX);
  cfg->pbe.amount = clampf(cfg->pbe.amount, 0.0f, 1.0f);
}

/* FNV-1a over what decides the filter graph: which stages exist, in which
 * order. Parameter-only changes keep the hash, so the history survives. */
static uint32_t hash_byte(uint32_t h, uint8_t b) {
  return (h ^ b) * 16777619u;
}

static void build(runtime_t *rt, const dsp_config_t *cfg) {
  uint32_t h = 2166136261u;
  memset(rt, 0, sizeof(*rt));
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    uint8_t n = 0;
    for (int i = 0; i < cfg->count[ch]; i++) {
      const dsp_filter_t *f = &cfg->filters[ch][i];
      if (!f->enabled) {
        continue;
      }
      dsp_biquad_design(f->type, f->f, f->gain_db, f->q, s_fs, &rt->k[ch][n]);
      h = hash_byte(h, (uint8_t)f->type);
      n++;
    }
    rt->n[ch] = n;
    h = hash_byte(h, n);
  }

  rt->pbe_on = cfg->pbe.on;
  if (rt->pbe_on) {
    const double fc = cfg->pbe.fc;
    const double bw = M_SQRT1_2; // Butterworth sections
    dsp_biquad_design(DSP_FILTER_HIGH_PASS, fc, 0, bw, s_fs,
                      &rt->pbe_k[PBE_HP_MAIN]);
    dsp_biquad_design(DSP_FILTER_LOW_PASS, fc, 0, bw, s_fs,
                      &rt->pbe_k[PBE_LP1]);
    rt->pbe_k[PBE_LP2] = rt->pbe_k[PBE_LP1];
    dsp_biquad_design(DSP_FILTER_HIGH_PASS, fc, 0, bw, s_fs,
                      &rt->pbe_k[PBE_HP2]);
    rt->pbe_k[PBE_HP3] = rt->pbe_k[PBE_HP2];
    dsp_biquad_design(DSP_FILTER_LOW_PASS, 4 * fc, 0, bw, s_fs,
                      &rt->pbe_k[PBE_LP3]);
    rt->pbe_gain = cfg->pbe.amount * PBE_GAIN;
  }
  h = hash_byte(h, rt->pbe_on);
  rt->structure = h;

  rt->preamp = powf(10.0f, cfg->preamp_db / 20.0f) / 32768.0f;
  rt->identity = rt->n[DSP_CH_L] == 0 && rt->n[DSP_CH_R] == 0 && !rt->pbe_on &&
                 cfg->preamp_db == 0.0f;
}

void dsp_set_config(const dsp_config_t *cfg_in) {
  dsp_config_t cfg = *cfg_in;
  dsp_clamp_config(&cfg);

  while (atomic_flag_test_and_set_explicit(&s_writer, memory_order_acquire)) {
  }
  s_cfg = cfg;

  uint32_t w = atomic_load(&s_word);
  const uint32_t active = w & 0xFFu;
  const uint32_t pending = (w >> 8) & 0xFFu;
  uint32_t slot = 0;
  while (slot == active || slot == pending) {
    slot++;
  }
  build(&s_slots[slot], &cfg);

  // If the audio task promotes the old pending meanwhile, the CAS fails and
  // is retried against the new active slot, which is still not ours.
  while (
      !atomic_compare_exchange_weak(&s_word, &w, word_make(w & 0xFFu, slot))) {
  }

  atomic_flag_clear_explicit(&s_writer, memory_order_release);
}

void dsp_get_config(dsp_config_t *cfg) {
  while (atomic_flag_test_and_set_explicit(&s_writer, memory_order_acquire)) {
  }
  *cfg = s_cfg;
  atomic_flag_clear_explicit(&s_writer, memory_order_release);
}

void dsp_set_bypass(bool bypass) {
  atomic_store(&s_bypass, bypass);
}

void dsp_init(uint32_t sample_rate) {
  s_fs = (double)sample_rate;
  dsp_config_t flat;
  memset(&flat, 0, sizeof(flat));
  flat.pbe.fc = 100.0f;
  flat.pbe.amount = 0.5f;
  s_cfg = flat;
  build(&s_slots[0], &flat);
  atomic_store(&s_word, word_make(0, SLOT_NONE));
  atomic_store(&s_bypass, false);
  s_was_idle = true;
}

static void reset_state(runtime_t *rt) {
  memset(rt->s, 0, sizeof(rt->s));
  memset(rt->pbe_s, 0, sizeof(rt->pbe_s));
}

/* Promote a pending slot, carrying the filter history over when only
 * parameters changed (no click), and return the slot to run. */
static runtime_t *acquire(void) {
  uint32_t w = atomic_load(&s_word);
  for (;;) {
    const uint32_t active = w & 0xFFu;
    const uint32_t pending = (w >> 8) & 0xFFu;
    if (pending == SLOT_NONE) {
      return &s_slots[active];
    }
    runtime_t *from = &s_slots[active];
    runtime_t *to = &s_slots[pending];
    if (from->structure == to->structure && !s_was_idle) {
      memcpy(to->s, from->s, sizeof(to->s));
      memcpy(to->pbe_s, from->pbe_s, sizeof(to->pbe_s));
    } else {
      reset_state(to);
    }
    if (atomic_compare_exchange_weak(&s_word, &w,
                                     word_make(pending, SLOT_NONE))) {
      return to;
    }
  }
}

static inline float limit(float x) {
  const float a = fabsf(x);
  if (a <= LIMIT_T) {
    return x;
  }
  const float y =
      LIMIT_T + (1.0f - LIMIT_T) * tanhf((a - LIMIT_T) / (1.0f - LIMIT_T));
  return x < 0 ? -y : y;
}

static inline uint32_t xorshift(uint32_t x) {
  x ^= x << 13u;
  x ^= x >> 17u;
  x ^= x << 5u;
  return x;
}

/* TPDF dither of +-1 LSB from one 32-bit draw: the sum of its two 16-bit
 * halves is triangular over [0, 2), shifted to [-1, 1). */
static inline int16_t to_int16(float x, uint32_t r) {
  const float d = (float)((r & 0xFFFFu) + (r >> 16)) * (1.0f / 65536.0f) - 1.0f;
  // Round to nearest without a branch: shift into positive range so the
  // truncating conversion acts as floor(v + 0.5).
  const float v = limit(x) * 32768.0f + d + 32768.5f;
  int32_t i = (int32_t)v - 32768;
  i = i > 32767 ? 32767 : i;
  i = i < -32768 ? -32768 : i;
  return (int16_t)i;
}

/* Both outputs at once, so every stage runs as an interleaved pair. */
static void run_pbe(runtime_t *rt, size_t n) {
  dsp_state_t *sl = rt->pbe_s[DSP_CH_L];
  dsp_state_t *sr = rt->pbe_s[DSP_CH_R];
  float *hl = s_pbe_tmp[DSP_CH_L];
  float *hr = s_pbe_tmp[DSP_CH_R];
  memcpy(hl, s_work[DSP_CH_L], n * sizeof(float));
  memcpy(hr, s_work[DSP_CH_R], n * sizeof(float));
  static const int harmonic_path[2] = {PBE_LP1, PBE_LP2};
  for (int i = 0; i < 2; i++) {
    const int st = harmonic_path[i];
    dsp_biquad_run2(&rt->pbe_k[st], &sl[st], hl, &rt->pbe_k[st], &sr[st], hr,
                    n);
  }
  for (size_t i = 0; i < n; i++) {
    hl[i] = fabsf(hl[i]); // full-wave rectifier: harmonics
    hr[i] = fabsf(hr[i]);
  }
  static const int band_path[3] = {PBE_HP2, PBE_HP3, PBE_LP3};
  for (int i = 0; i < 3; i++) {
    const int st = band_path[i];
    dsp_biquad_run2(&rt->pbe_k[st], &sl[st], hl, &rt->pbe_k[st], &sr[st], hr,
                    n);
  }
  dsp_biquad_run2(&rt->pbe_k[PBE_HP_MAIN], &sl[PBE_HP_MAIN], s_work[DSP_CH_L],
                  &rt->pbe_k[PBE_HP_MAIN], &sr[PBE_HP_MAIN], s_work[DSP_CH_R],
                  n);
  const float g = rt->pbe_gain;
  for (size_t i = 0; i < n; i++) {
    s_work[DSP_CH_L][i] += g * hl[i];
    s_work[DSP_CH_R][i] += g * hr[i];
  }
}

static void process_chunk(runtime_t *rt, int16_t *buf, size_t n) {
  const float pre = rt->preamp;
  for (size_t i = 0; i < n; i++) {
    s_work[DSP_CH_L][i] = (float)buf[2 * i] * pre;
    s_work[DSP_CH_R][i] = (float)buf[2 * i + 1] * pre;
  }
  // Filters k of both outputs run as a pair; the longer chain finishes alone.
  const int nl = rt->n[DSP_CH_L];
  const int nr = rt->n[DSP_CH_R];
  const int pairs = nl < nr ? nl : nr;
  for (int k = 0; k < pairs; k++) {
    dsp_biquad_run2(&rt->k[DSP_CH_L][k], &rt->s[DSP_CH_L][k], s_work[DSP_CH_L],
                    &rt->k[DSP_CH_R][k], &rt->s[DSP_CH_R][k], s_work[DSP_CH_R],
                    n);
  }
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    for (int k = pairs; k < rt->n[ch]; k++) {
      dsp_biquad_run(&rt->k[ch][k], &rt->s[ch][k], s_work[ch], n);
    }
  }
  if (rt->pbe_on) {
    run_pbe(rt, n);
  }
  uint32_t r = s_rng; // kept in a register for the whole chunk
  for (size_t i = 0; i < n; i++) {
    r = xorshift(r);
    buf[2 * i] = to_int16(s_work[DSP_CH_L][i], r);
    r = xorshift(r);
    buf[2 * i + 1] = to_int16(s_work[DSP_CH_R][i], r);
  }
  s_rng = r;
}

void dsp_process(int16_t *buf, size_t frames) {
  runtime_t *rt = acquire();
  if (atomic_load_explicit(&s_bypass, memory_order_relaxed) || rt->identity) {
    s_was_idle = true;
    return;
  }
  if (s_was_idle) {
    reset_state(rt); // history from before the bypass would click
    s_was_idle = false;
  }
  while (frames > 0) {
    const size_t n = frames < CHUNK ? frames : CHUNK;
    process_chunk(rt, buf, n);
    buf += 2 * n;
    frames -= n;
  }
}

bool dsp_is_active(void) {
  const uint32_t w = atomic_load(&s_word);
  return !atomic_load(&s_bypass) && !s_slots[w & 0xFFu].identity;
}
