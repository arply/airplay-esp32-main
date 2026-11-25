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

#define NUM_SLOTS 3
#define SLOT_NONE 0xFFu
#define CHUNK     128

/* Harmonics ("missing fundamental"), part of the bass block: the bass the
 * speaker cannot play is replaced by its 2nd, 3rd and 4th harmonics, from
 * which the ear reconstructs the fundamental. Consecutive harmonics matter:
 * their spacing is the pitch the ear hears (2f, 4f alone, as a full-wave
 * rectifier makes them, sounds an octave too high).
 *
 * Per band: normalise to the envelope, then Chebyshev polynomials, since
 * T_n(cos t) = cos(n t) gives each harmonic at a fixed level whatever the
 * signal level. Any nonlinearity also makes sum tones of notes that sound
 * together (bass + kick); splitting the bass an octave below the cutoff
 * keeps most such pairs in different bands. */
#define PBE_W2      1.0f
#define PBE_W3      0.7f
#define PBE_W4      0.45f
#define PBE_GAIN    0.8f  // harmonics gain at amount 1.0
#define PBE_RELEASE 0.1   // envelope release, seconds
#define PBE_FLOOR   1e-5f // below ~-100 dBFS: no harmonics
// Everything after the bass low-pass lives below 5 fc (<= 750 Hz), so the
// rest of the bass low-pass, the band split, the harmonics and their filters
// run at fs / PBE_DECIM and are interpolated back up. The first low-pass
// section at full rate is the anti-alias filter (12 dB/oct from <= 150 Hz:
// > 60 dB down at the new Nyquist, where music has little energy anyway).
#define PBE_DECIM 4

/* Bass extension: an RBJ low shelf at f_low whose gain g the dynamics move
 * per chunk (with S = 1 the coefficients depend on g only through two square
 * roots, so recomputing them per chunk is cheap). A parallel form,
 * x + (g - 1) LP(x), would dip ~1.5 dB above f_low where the low-pass phase
 * turns against the signal. The level is detected on LP(f_low)(x). The
 * protection keeps the boosted bass band at the DAC (after the volume) under
 * BASS_PROTECT; loudness adds up to BASS_LOUD_MAX_DB at low volume
 * (BASS_LOUD_SLOPE dB per dB of volume cut). */
// Headroom guard only (-6 dBFS peak of the bass band at the DAC): how loud
// the driver may go is the boost setting. -18 dBFS held the boost back at
// normal listening levels (probe, 2026-11-25).
#define BASS_PROTECT      0.5f
#define BASS_LOUD_MAX_DB  6.0f
#define BASS_LOUD_SLOPE   0.3f
#define BASS_RELEASE      0.3  // seconds for the boost to come back
#define BASS_HARM_REST    0.5f // harmonics share while the boost is free ...
#define BASS_HARM_LIMITED 2.0f // ... and while it is fully held back

// Soft limiter knee at -1 dBFS.
#define LIMIT_T 0.89125094f

#define PBE_BANDS 2
enum {
  PBE_HP_MAIN,   // main path: subsonic high-pass at f_low / 3 (LR4:
  PBE_HP_MAIN2,  //   a 2nd-order cut let the shelf boost 20-45 Hz by 5 dB)
  PBE_LP1,       // bass band (LR4 low-pass at fc; 1st section at
  PBE_LP2,       //   full rate, 2nd at the low rate) ...
  PBE_SPLIT_LP1, // ... split into fc/4..fc/2 ...
  PBE_SPLIT_LP2, //
  PBE_SPLIT_HP1, // ... and fc/2..fc
  PBE_SPLIT_HP2, //
  PBE_HP2,       // harmonics band: high-pass at fc ...
  PBE_HP3,       //
  PBE_LP3,       // ... low-pass at 3 fc
  PBE_STAGES
};

typedef struct {
  float env; // peak envelope
  float inv; // 1 / env, kept without a division per sample
} pbe_env_t;

typedef struct {
  dsp_coeffs_t k[DSP_NUM_CH][DSP_MAX_FILTERS_PER_CH];
  dsp_state_t s[DSP_NUM_CH][DSP_MAX_FILTERS_PER_CH];
  uint8_t n[DSP_NUM_CH];
  dsp_coeffs_t pbe_k[PBE_STAGES]; // split and harmonic stages at fs/DECIM
  dsp_state_t pbe_s[DSP_NUM_CH][PBE_STAGES];
  pbe_env_t pbe_env[DSP_NUM_CH][PBE_BANDS];
  float pbe_y[DSP_NUM_CH][2]; // last two low-rate outputs, for interpolation
  uint8_t pbe_phase;          // position within the decimation period
  float pbe_release;          // per-low-rate-sample envelope decay factor
  float pbe_gain;             // harmonics gain before the dynamics factor
  bool pbe_on;
  dsp_coeffs_t ext_k; // detector band: low-pass at f_low
  dsp_state_t ext_s[DSP_NUM_CH];
  float shelf_cos;   // cos(w0) of the shelf
  float shelf_alpha; // sin(w0) / sqrt(2) (S = 1)
  dsp_coeffs_t shelf_k[DSP_NUM_CH];
  dsp_state_t shelf_s[DSP_NUM_CH];
  float ext_env[DSP_NUM_CH]; // peak of the extension band
  float ext_g[DSP_NUM_CH];   // current extension gain (linear, >= 1)
  float ext_max_db;          // boost setting, before loudness
  float ext_g_max;           // linear gain for ext_g_max_db (powf is slow)
  float ext_g_max_db;        // the dB value ext_g_max was computed for
  float ext_release;         // per-chunk approach factor of the gain
  float preamp;              // linear, includes the int16 -> float scale
  bool identity;             // flat: leave samples untouched
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
static float s_ext[DSP_NUM_CH][CHUNK];
static _Atomic float s_out_gain = 1.0f; // volume after the DSP, linear
static _Atomic float s_loud_db;         // loudness extra for that volume
static bool s_was_idle = true;
static float s_vol; // volume gain currently applied (ramped)
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
  cfg->bass.f_low = clampf(cfg->bass.f_low, DSP_BASS_F_MIN, DSP_BASS_F_MAX);
  cfg->bass.boost = clampf(cfg->bass.boost, 0.0f, 1.0f);
  cfg->bass.harmonics = clampf(cfg->bass.harmonics, 0.0f, 1.0f);
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

  rt->pbe_on = cfg->bass.on;
  if (rt->pbe_on) {
    // Harmonics are made from the bass below the limit and added above it;
    // the real bass stays (removing it sounded thin: probe, 2026-11-25).
    // Only below a third of the limit is cut, to spare the driver.
    const double fc = cfg->bass.f_low;
    const double bw = M_SQRT1_2; // Butterworth sections; two make an LR4
    dsp_coeffs_t *k = rt->pbe_k;
    dsp_biquad_design(DSP_FILTER_HIGH_PASS, cfg->bass.f_low / 3, 0, bw, s_fs,
                      &k[PBE_HP_MAIN]);
    k[PBE_HP_MAIN2] = k[PBE_HP_MAIN];
    dsp_biquad_design(DSP_FILTER_LOW_PASS, fc, 0, bw, s_fs, &k[PBE_LP1]);
    const double low_fs = s_fs / PBE_DECIM;
    dsp_biquad_design(DSP_FILTER_LOW_PASS, fc, 0, bw, low_fs, &k[PBE_LP2]);
    dsp_biquad_design(DSP_FILTER_LOW_PASS, fc / 2, 0, bw, low_fs,
                      &k[PBE_SPLIT_LP1]);
    k[PBE_SPLIT_LP2] = k[PBE_SPLIT_LP1];
    dsp_biquad_design(DSP_FILTER_HIGH_PASS, fc / 2, 0, bw, low_fs,
                      &k[PBE_SPLIT_HP1]);
    k[PBE_SPLIT_HP2] = k[PBE_SPLIT_HP1];
    dsp_biquad_design(DSP_FILTER_HIGH_PASS, fc, 0, bw, low_fs, &k[PBE_HP2]);
    k[PBE_HP3] = k[PBE_HP2];
    // Harmonics stay below 3 fc: higher up they buzz in the midrange.
    dsp_biquad_design(DSP_FILTER_LOW_PASS, 3 * fc, 0, bw, low_fs, &k[PBE_LP3]);
    rt->pbe_release = (float)exp(-1.0 / (PBE_RELEASE * low_fs));
    rt->pbe_gain = cfg->bass.harmonics * PBE_GAIN;
    dsp_biquad_design(DSP_FILTER_LOW_PASS, cfg->bass.f_low, 0, bw, s_fs,
                      &rt->ext_k);
    rt->ext_max_db = cfg->bass.boost * DSP_BASS_BOOST_MAX;
    rt->ext_g_max_db = -1000.0f; // forces the first computation
    const double w0 = 2.0 * M_PI * cfg->bass.f_low / s_fs;
    rt->shelf_cos = (float)cos(w0);
    rt->shelf_alpha = (float)(sin(w0) * M_SQRT1_2);
    rt->ext_release =
        (float)(1.0 - exp(-(double)CHUNK / (BASS_RELEASE * s_fs)));
    for (int ch = 0; ch < DSP_NUM_CH; ch++) {
      rt->ext_g[ch] = 1.0f;
    }
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

void dsp_set_output_gain(float gain) {
  gain = clampf(gain, 0.0f, 1.0f);
  if (gain == atomic_load_explicit(&s_out_gain, memory_order_relaxed)) {
    return;
  }
  atomic_store_explicit(&s_out_gain, gain, memory_order_relaxed);
  const float cut_db = gain > 1e-3f ? -20.0f * log10f(gain) : 60.0f;
  atomic_store_explicit(&s_loud_db,
                        fminf(BASS_LOUD_MAX_DB, BASS_LOUD_SLOPE * cut_db),
                        memory_order_relaxed);
}

void dsp_set_bypass(bool bypass) {
  atomic_store(&s_bypass, bypass);
}

void dsp_init(uint32_t sample_rate) {
  s_fs = (double)sample_rate;
  dsp_config_t flat;
  memset(&flat, 0, sizeof(flat));
  flat.bass.f_low = 120.0f;
  flat.bass.boost = 0.5f;
  flat.bass.harmonics = 0.0f;
  s_cfg = flat;
  build(&s_slots[0], &flat);
  atomic_store(&s_word, word_make(0, SLOT_NONE));
  atomic_store(&s_bypass, false);
  s_was_idle = true;
}

static void reset_state(runtime_t *rt) {
  memset(rt->s, 0, sizeof(rt->s));
  memset(rt->pbe_s, 0, sizeof(rt->pbe_s));
  memset(rt->pbe_env, 0, sizeof(rt->pbe_env));
  memset(rt->pbe_y, 0, sizeof(rt->pbe_y));
  rt->pbe_phase = 0;
  memset(rt->ext_s, 0, sizeof(rt->ext_s));
  memset(rt->ext_env, 0, sizeof(rt->ext_env));
  memset(rt->shelf_s, 0, sizeof(rt->shelf_s));
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    rt->ext_g[ch] = 1.0f;
  }
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
      memcpy(to->pbe_env, from->pbe_env, sizeof(to->pbe_env));
      memcpy(to->pbe_y, from->pbe_y, sizeof(to->pbe_y));
      to->pbe_phase = from->pbe_phase;
      memcpy(to->ext_s, from->ext_s, sizeof(to->ext_s));
      memcpy(to->ext_env, from->ext_env, sizeof(to->ext_env));
      memcpy(to->shelf_s, from->shelf_s, sizeof(to->shelf_s));
      memcpy(to->ext_g, from->ext_g, sizeof(to->ext_g));
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

/* Envelope-normalised Chebyshev harmonics of one band sample. Instant
 * attack, exponential release; 1/env follows the release by multiplication,
 * so the only division is on attack (a slow library call on this FPU). */
static inline float harmonics(float b, pbe_env_t *st, float release,
                              float inv_release) {
  const float a = fabsf(b);
  float e = st->env * release;
  float inv = st->inv * inv_release;
  if (a >= e) {
    e = a;
    inv = a > PBE_FLOOR ? 1.0f / a : 0.0f;
  }
  st->env = e;
  st->inv = inv;
  if (e < PBE_FLOOR) {
    return 0.0f;
  }
  const float u = b * inv; // |u| <= 1
  const float u2 = u * u;
  const float t2 = 2.0f * u2 - 1.0f;
  const float t3 = u * (4.0f * u2 - 3.0f);
  const float t4 = 8.0f * u2 * (u2 - 1.0f) + 1.0f;
  return e * (PBE_W2 * t2 + PBE_W3 * t3 + PBE_W4 * t4);
}

static void run_pair(runtime_t *rt, int stage, float *l, float *r, size_t n) {
  dsp_biquad_run2(&rt->pbe_k[stage], &rt->pbe_s[DSP_CH_L][stage], l,
                  &rt->pbe_k[stage], &rt->pbe_s[DSP_CH_R][stage], r, n);
}

/* One sample through one low-rate stage (TDF-II, as dsp_biquad_run). */
static inline float step(const dsp_coeffs_t *k, dsp_state_t *st, float x) {
  const float y = k->b0 * x + st->z1;
  st->z1 = k->b1 * x - k->a1 * y + st->z2;
  st->z2 = k->b2 * x - k->a2 * y;
  return y;
}

/* One low-rate sample of the harmonic path for channel @p ch. */
static float harmonic_sample(runtime_t *rt, int ch, float bass, float rel,
                             float inv_rel) {
  const dsp_coeffs_t *k = rt->pbe_k;
  dsp_state_t *st = rt->pbe_s[ch];
  bass = step(&k[PBE_LP2], &st[PBE_LP2], bass);
  float lo = step(&k[PBE_SPLIT_LP1], &st[PBE_SPLIT_LP1], bass);
  lo = step(&k[PBE_SPLIT_LP2], &st[PBE_SPLIT_LP2], lo);
  float hi = step(&k[PBE_SPLIT_HP1], &st[PBE_SPLIT_HP1], bass);
  hi = step(&k[PBE_SPLIT_HP2], &st[PBE_SPLIT_HP2], hi);
  float y = harmonics(lo, &rt->pbe_env[ch][0], rel, inv_rel) +
            harmonics(hi, &rt->pbe_env[ch][1], rel, inv_rel);
  y = step(&k[PBE_HP2], &st[PBE_HP2], y);
  y = step(&k[PBE_HP3], &st[PBE_HP3], y);
  return step(&k[PBE_LP3], &st[PBE_LP3], y);
}

/* Extension gain for this chunk: the full boost (plus loudness), unless the
 * boosted bass at the DAC would exceed BASS_PROTECT. Falls at once, comes
 * back with BASS_RELEASE. Returns how far it is held back, 0..1. */
static float extension_gain(runtime_t *rt, int ch, const float *band, size_t n,
                            float *g_from, float *g_to) {
  // fmaxf() is a library call on this toolchain, not an instruction: a
  // comparison keeps the per-sample loop cheap.
  float peak = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const float a = fabsf(band[i]);
    peak = a > peak ? a : peak;
  }
  const float rel = rt->ext_release;
  float env = rt->ext_env[ch] * (1.0f - rel);
  env = fmaxf(env, peak);
  rt->ext_env[ch] = env;

  const float max_db =
      rt->ext_max_db + atomic_load_explicit(&s_loud_db, memory_order_relaxed);
  if (max_db != rt->ext_g_max_db) { // only when the volume or setting moves
    rt->ext_g_max_db = max_db;
    rt->ext_g_max = powf(10.0f, max_db / 20.0f);
  }
  const float g_max = rt->ext_g_max;
  const float at_dac = env; // the chain already runs after the volume
  float target = g_max;
  if (at_dac * g_max > BASS_PROTECT) {
    target = fmaxf(1.0f, BASS_PROTECT / at_dac);
  }
  const float g0 = rt->ext_g[ch];
  const float g1 = target < g0 ? target : g0 + (target - g0) * rel;
  rt->ext_g[ch] = g1;
  *g_from = g0;
  *g_to = g1;
  return g_max > 1.0001f ? (g_max - g1) / (g_max - 1.0f) : 0.0f;
}

/* RBJ low shelf (S = 1) with linear amplitude gain @p g at the runtime's
 * f_low: A = sqrt(g), the rest is arithmetic. */
static void shelf_coeffs(const runtime_t *rt, float g, dsp_coeffs_t *k) {
  const float a = sqrtf(g);
  const float c = rt->shelf_cos;
  const float r = 2.0f * sqrtf(a) * rt->shelf_alpha;
  const float ap = a + 1.0f;
  const float am = a - 1.0f;
  const float inv_a0 = 1.0f / (ap + am * c + r);
  k->b0 = a * (ap - am * c + r) * inv_a0;
  k->b1 = 2.0f * a * (am - ap * c) * inv_a0;
  k->b2 = a * (ap - am * c - r) * inv_a0;
  k->a1 = -2.0f * (am + ap * c) * inv_a0;
  k->a2 = (ap + am * c - r) * inv_a0;
}

/* Bass block, both outputs at once (see dsp_bass_t): extension with
 * dynamics, harmonics of what lies below f_low / 2 at fs / PBE_DECIM, and
 * the subsonic high-pass. */
static void run_bass(runtime_t *rt, size_t n) {
  float *bass[DSP_NUM_CH] = {s_pbe_tmp[DSP_CH_L], s_pbe_tmp[DSP_CH_R]};
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    memcpy(bass[ch], s_work[ch], n * sizeof(float));
    memcpy(s_ext[ch], s_work[ch], n * sizeof(float));
  }
  run_pair(rt, PBE_LP1, bass[0], bass[1], n);
  dsp_biquad_run2(&rt->ext_k, &rt->ext_s[DSP_CH_L], s_ext[DSP_CH_L], &rt->ext_k,
                  &rt->ext_s[DSP_CH_R], s_ext[DSP_CH_R], n);

  // Extension: the shelf takes this chunk's gain (the steps are small; the
  // gain falls at most once per chunk and rises with BASS_RELEASE).
  float held[DSP_NUM_CH];
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    float g0;
    float g1;
    held[ch] = extension_gain(rt, ch, s_ext[ch], n, &g0, &g1);
    shelf_coeffs(rt, g1, &rt->shelf_k[ch]);
  }
  dsp_biquad_run2(&rt->shelf_k[DSP_CH_L], &rt->shelf_s[DSP_CH_L],
                  s_work[DSP_CH_L], &rt->shelf_k[DSP_CH_R],
                  &rt->shelf_s[DSP_CH_R], s_work[DSP_CH_R], n);

  const float rel = rt->pbe_release;
  const float inv_rel = 1.0f / rel;
  const float frac = 1.0f / PBE_DECIM;
  uint8_t phase = rt->pbe_phase;
  for (size_t i = 0; i < n; i++) {
    if (++phase == PBE_DECIM) {
      phase = 0;
      for (int ch = 0; ch < DSP_NUM_CH; ch++) {
        rt->pbe_y[ch][0] = rt->pbe_y[ch][1];
        rt->pbe_y[ch][1] = harmonic_sample(rt, ch, bass[ch][i], rel, inv_rel);
      }
    }
    // The interpolated harmonics overwrite the bass buffer in place.
    const float t = (float)(phase + 1) * frac;
    for (int ch = 0; ch < DSP_NUM_CH; ch++) {
      const float *y = rt->pbe_y[ch];
      bass[ch][i] = y[0] + (y[1] - y[0]) * t;
    }
  }
  rt->pbe_phase = phase;
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    for (int s = PBE_LP2; s < PBE_STAGES; s++) {
      dsp_state_t *z = &rt->pbe_s[ch][s];
      z->z1 = fabsf(z->z1) < 1e-15f ? 0.0f : z->z1; // denormal flush
      z->z2 = fabsf(z->z2) < 1e-15f ? 0.0f : z->z2;
    }
  }

  run_pair(rt, PBE_HP_MAIN, s_work[DSP_CH_L], s_work[DSP_CH_R], n);
  run_pair(rt, PBE_HP_MAIN2, s_work[DSP_CH_L], s_work[DSP_CH_R], n);
  for (int ch = 0; ch < DSP_NUM_CH; ch++) {
    // More harmonics while the dynamics hold real bass back.
    const float g =
        rt->pbe_gain *
        (BASS_HARM_REST + (BASS_HARM_LIMITED - BASS_HARM_REST) * held[ch]);
    float *x = s_work[ch];
    const float *h = bass[ch];
    for (size_t i = 0; i < n; i++) {
      x[i] += g * h[i];
    }
  }
}

static void process_chunk(runtime_t *rt, int16_t *buf, size_t n) {
  // The volume is applied here, at the input, so EQ, bass and limiter work
  // at the level that really reaches the DAC: at low volume a bass boost
  // has the headroom it needs instead of hitting the limiter. It moves
  // halfway to its target per chunk, ramped across the chunk (no zipper).
  const float target = atomic_load_explicit(&s_out_gain, memory_order_relaxed);
  const float v0 = s_vol;
  const float v1 = v0 + (target - v0) * 0.5f;
  s_vol = fabsf(v1 - target) < 1e-5f ? target : v1;
  const float pre = rt->preamp;
  float g = v0 * pre;
  const float dg = (s_vol - v0) * pre / (float)n;
  for (size_t i = 0; i < n; i++) {
    g += dg;
    s_work[DSP_CH_L][i] = (float)buf[2 * i] * g;
    s_work[DSP_CH_R][i] = (float)buf[2 * i + 1] * g;
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
    run_bass(rt, n);
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

bool dsp_process(int16_t *buf, size_t frames) {
  runtime_t *rt = acquire();
  if (atomic_load_explicit(&s_bypass, memory_order_relaxed) || rt->identity) {
    s_was_idle = true;
    return false;
  }
  if (s_was_idle) {
    reset_state(rt); // history from before the bypass would click
    s_vol = atomic_load_explicit(&s_out_gain, memory_order_relaxed);
    s_was_idle = false;
  }
  while (frames > 0) {
    const size_t n = frames < CHUNK ? frames : CHUNK;
    process_chunk(rt, buf, n);
    buf += 2 * n;
    frames -= n;
  }
  return true;
}

bool dsp_is_active(void) {
  const uint32_t w = atomic_load(&s_word);
  return !atomic_load(&s_bypass) && !s_slots[w & 0xFFu].identity;
}
