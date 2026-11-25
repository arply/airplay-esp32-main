/**
 * Host test for components/dsp (no ESP-IDF). Run: scripts/dsp_host_test.sh
 *
 * Every check feeds real signals through the same code the device runs and
 * compares against the analytic response (magDb() in the prototype).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dsp_biquad.h"
#include "dsp.h"
#include "dsp_signal.h"

#define FS 44100.0

static int s_failures;

static void check(int ok, const char *what, double got, double want,
                  double tol) {
  if (!ok) {
    s_failures++;
  }
  printf("%s %-46s got %8.3f want %8.3f (+-%.2f)\n", ok ? "  ok " : "FAIL ",
         what, got, want, tol);
}

static void check_near(const char *what, double got, double want, double tol) {
  check(fabs(got - want) <= tol, what, got, want, tol);
}

/* Amplitude of the component at f in x[0..n), from the correlation with sin
 * and cos over a whole number of periods (so no leakage from f itself). */
static double tone_amp(const float *x, size_t n, double f, double fs) {
  const size_t period_count = (size_t)floor((double)n * f / fs);
  const size_t len = (size_t)llround((double)period_count * fs / f);
  double i_sum = 0;
  double q_sum = 0;
  for (size_t k = 0; k < len; k++) {
    const double ph = 2 * M_PI * f * (double)k / fs;
    i_sum += x[k] * cos(ph);
    q_sum += x[k] * sin(ph);
  }
  return 2.0 * sqrt(i_sum * i_sum + q_sum * q_sum) / (double)len;
}

static double db(double v) {
  return 20.0 * log10(v);
}

/* Biquad magnitude, measured with a sine after the filter has settled. */
static void test_biquad_magnitudes(void) {
  struct {
    dsp_filter_type_t type;
    double f, gain, q;
    const char *name;
  } cases[] = {
      {DSP_FILTER_PEAK, 1000, -12, 1, "peak 1k -12 Q1"},
      {DSP_FILTER_PEAK, 96.8, -1.9, 11.85, "peak 96.8 -1.9 Q11.85"},
      {DSP_FILTER_LOW_SHELF, 120, 6, 0, "low shelf 120 +6"},
      {DSP_FILTER_HIGH_SHELF, 6000, -3, 0, "high shelf 6k -3"},
      {DSP_FILTER_HIGH_PASS, 80, 0, 0.7071, "high pass 80"},
      {DSP_FILTER_LOW_PASS, 5000, 0, 0.7071, "low pass 5k"},
  };
  const size_t n = (size_t)(3 * FS); // high-Q low peaks ring for a while
  const size_t settle = (size_t)(1.5 * FS);
  float *x = malloc(n * sizeof(float));
  float worst = 0;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    dsp_coeffs_t k;
    dsp_biquad_design(cases[c].type, cases[c].f, cases[c].gain, cases[c].q, FS,
                      &k);
    for (int i = 0; i < 20; i++) {
      const double f = 20.0 * pow(1000.0, i / 19.0); // 20 Hz .. 20 kHz
      for (size_t s = 0; s < n; s++) {
        x[s] = (float)(0.5 * sin(2 * M_PI * f * (double)s / FS));
      }
      dsp_state_t st = {0, 0};
      dsp_biquad_run(&k, &st, x, n);
      const double got = db(tone_amp(x + settle, n - settle, f, FS) / 0.5);
      const double want = dsp_biquad_mag_db(&k, f, FS);
      const double err = fabs(got - want);
      if (err > worst) {
        worst = (float)err;
      }
      if (err > 0.1) {
        char what[96];
        snprintf(what, sizeof(what), "%s @ %.0f Hz", cases[c].name, f);
        check(0, what, got, want, 0.1);
      }
    }
  }
  check(worst <= 0.1, "6 filters x 20 freqs, worst error (dB)", worst, 0, 0.1);
  free(x);
}

static void sine16(int16_t *buf, size_t frames, double f, double amp) {
  for (size_t i = 0; i < frames; i++) {
    const int16_t v =
        (int16_t)lrint(amp * 32767.0 * sin(2 * M_PI * f * (double)i / FS));
    buf[2 * i] = v;
    buf[2 * i + 1] = v;
  }
}

static double chan_amp(const int16_t *buf, size_t from, size_t frames, int ch,
                       double f) {
  float *x = malloc(frames * sizeof(float));
  for (size_t i = 0; i < frames; i++) {
    x[i] = buf[2 * (from + i) + ch] / 32768.0f;
  }
  const double a = tone_amp(x, frames, f, FS);
  free(x);
  return a;
}

static dsp_config_t flat(void) {
  dsp_config_t c;
  memset(&c, 0, sizeof(c));
  c.bass.f_low = 120;
  c.bass.boost = 0.5f;
  c.bass.harmonics = 0.3f;
  return c;
}

static void add_filter(dsp_config_t *c, int ch, dsp_filter_type_t t, float f,
                       float g, float q) {
  c->filters[ch][c->count[ch]++] =
      (dsp_filter_t){.type = t, .f = f, .gain_db = g, .q = q, .enabled = true};
}

/* Run a whole buffer through dsp_process in playback-task sized frames. */
static void process_frames(int16_t *buf, size_t frames) {
  for (size_t at = 0; at < frames; at += 352) {
    const size_t n = frames - at < 352 ? frames - at : 352;
    dsp_process(buf + 2 * at, n);
  }
}

static void test_identity_and_bypass(void) {
  const size_t n = 44100;
  int16_t *a = malloc(n * 4);
  int16_t *b = malloc(n * 4);
  sine16(a, n, 997, 0.9);
  memcpy(b, a, n * 4);

  dsp_init(44100);
  dsp_config_t c = flat();
  dsp_set_config(&c);
  process_frames(b, n);
  check(memcmp(a, b, n * 4) == 0, "flat config is bit-identical", 0, 0, 0);

  add_filter(&c, DSP_CH_L, DSP_FILTER_PEAK, 1000, -12, 1);
  dsp_set_config(&c);
  dsp_set_bypass(true);
  process_frames(b, n);
  check(memcmp(a, b, n * 4) == 0, "bypass is bit-identical", 0, 0, 0);
  dsp_set_bypass(false);

  c.filters[DSP_CH_L][0].enabled = false;
  dsp_set_config(&c);
  process_frames(b, n);
  check(memcmp(a, b, n * 4) == 0, "disabled filter is bit-identical", 0, 0, 0);
  free(a);
  free(b);
}

static void test_chain_level(void) {
  const size_t n = 2 * 44100;
  int16_t *buf = malloc(n * 4);
  dsp_init(44100);
  dsp_config_t c = flat();
  add_filter(&c, DSP_CH_L, DSP_FILTER_PEAK, 1000, -12, 1);
  dsp_set_config(&c);

  sine16(buf, n, 1000, 0.5);
  process_frames(buf, n);
  const double l = db(chan_amp(buf, n / 2, n / 2, 0, 1000) / 0.5);
  const double r = db(chan_amp(buf, n / 2, n / 2, 1, 1000) / 0.5);
  check_near("int16 chain: L 1 kHz with -12 dB peak", l, -12.0, 0.15);
  check_near("int16 chain: R untouched (dither only)", r, 0.0, 0.05);

  // Preamp: +6 dB on a full-scale sine must not wrap, only soft-limit.
  c = flat();
  c.preamp_db = 6;
  dsp_set_config(&c);
  sine16(buf, n, 1000, 1.0);
  process_frames(buf, n);
  int16_t peak = 0;
  int jumps = 0;
  for (size_t i = 1; i < n; i++) {
    const int16_t v = buf[2 * i];
    if (abs(v) > peak) {
      peak = (int16_t)abs(v);
    }
    if (abs(v - buf[2 * (i - 1)]) > 20000) {
      jumps++;
    }
  }
  check(peak <= 32767 && jumps == 0, "+6 dB preamp on 0 dBFS: no wrap", jumps,
        0, 0);
  check(peak > 32000, "  ... and the limiter reaches close to full scale", peak,
        32767, 767);

  // Level just below the knee passes unchanged: -2 dBFS, 0 dB preamp.
  c = flat();
  add_filter(&c, DSP_CH_R, DSP_FILTER_PEAK, 50, -1, 1); // makes it non-flat
  dsp_set_config(&c);
  sine16(buf, n, 1000, pow(10, -2.0 / 20));
  process_frames(buf, n);
  check_near("-2 dBFS below limiter knee: L level (dB)",
             db(chan_amp(buf, n / 2, n / 2, 0, 1000)), -2.0, 0.02);
  free(buf);
}

/* A parameter-only change keeps the filter history: no step in the output. */
static void test_param_change_is_smooth(void) {
  const size_t n = 44100;
  int16_t *buf = malloc(n * 4);
  dsp_init(44100);
  dsp_config_t c = flat();
  add_filter(&c, DSP_CH_L, DSP_FILTER_PEAK, 100, -6, 2);
  add_filter(&c, DSP_CH_R, DSP_FILTER_PEAK, 100, -6, 2);
  dsp_set_config(&c);
  sine16(buf, n, 100, 0.5);
  int max_step = 0;
  for (size_t at = 0; at < n; at += 352) {
    const size_t len = n - at < 352 ? n - at : 352;
    if (at == 352 * 60) {
      c.filters[DSP_CH_L][0].gain_db = -5.5f; // small move, as a slider does
      dsp_set_config(&c);
    }
    dsp_process(buf + 2 * at, len);
  }
  for (size_t i = 352 * 50; i < 352 * 70; i++) {
    const int step = abs(buf[2 * i] - buf[2 * (i - 1)]);
    if (step > max_step) {
      max_step = step;
    }
  }
  // A 100 Hz sine at -6 dBFS moves at most ~2.3% of full scale per sample.
  check(max_step < 800, "slider move mid-stream: max sample step", max_step, 0,
        800);
  free(buf);
}

/* Harmonics: 50 Hz in, limit 100 Hz, no extension. The fundamental stays;
 * consecutive harmonics (2f, 3f, 4f) above the limit carry the pitch. */
static void test_pbe(void) {
  const size_t n = 3 * 44100;
  int16_t *buf = malloc(n * 4);
  dsp_init(44100);
  dsp_config_t c = flat();
  c.bass.on = true;
  c.bass.f_low = 100; // harmonics from below 100 Hz, added above it
  c.bass.boost = 0;
  c.bass.harmonics = 1;
  dsp_set_config(&c);
  sine16(buf, n, 50, 0.5);
  process_frames(buf, n);
  const size_t from = n / 2;
  const double base = 0.5;
  // the real bass stays: only the subsonic cut (LR4 at 33 Hz) touches it
  check_near("harmonics: 50 Hz fundamental kept (dB)",
             db(chan_amp(buf, from, n - from, 0, 50) / base), -1.6, 0.5);
  check_near("harmonics: 100 Hz, 2nd harmonic (dB)",
             db(chan_amp(buf, from, n - from, 0, 100) / base), -13.7, 1.0);
  check_near("harmonics: 150 Hz, 3rd harmonic (dB)",
             db(chan_amp(buf, from, n - from, 0, 150) / base), -13.3, 1.0);
  check_near("harmonics: 200 Hz, 4th harmonic (dB)",
             db(chan_amp(buf, from, n - from, 0, 200) / base), -17.7, 1.0);
  free(buf);
}

/* Two notes an octave apart (bass + kick): most of what the enhancement
 * adds should be harmonics rather than sum/difference tones: at least twice
 * the share of the old full-wave rectifier (13 %). */
static void test_pbe_intermodulation(void) {
  const size_t n = 4 * 44100;
  int16_t *buf = malloc(n * 4);
  for (size_t i = 0; i < n; i++) {
    const double t = (double)i / FS;
    const int16_t v = (int16_t)lrint(
        0.25 * 32767 * (sin(2 * M_PI * 40 * t) + sin(2 * M_PI * 75 * t)));
    buf[2 * i] = buf[2 * i + 1] = v;
  }
  dsp_init(44100);
  dsp_config_t c = flat();
  c.bass.on = true;
  c.bass.f_low = 100; // harmonics from below 100 Hz, added above it
  c.bass.boost = 0;
  c.bass.harmonics = 1;
  dsp_set_config(&c);
  process_frames(buf, n);
  double harm = 0;
  double band = 0;
  for (int f = 81; f <= 500; f++) {
    const double a = chan_amp(buf, 44100, 3 * 44100, 0, f);
    band += a * a;
    for (int k = 2; k <= 6; k++) {
      if (f == 40 * k || f == 75 * k) {
        harm += a * a;
      }
    }
  }
  check(harm / band > 0.26, "harmonics: harmonic share, 40 + 75 Hz (%)",
        100 * harm / band, 33, 7);
  free(buf);
}

/* Level of a sine at @p f, @p in_db, after the bass block, f_low 120 Hz,
 * boost 0.5 (+6 dB), no harmonics, at output gain @p vol. */
static double extension_db_at(double f, double in_db, float vol) {
  const size_t n = 3 * 44100;
  int16_t *buf = malloc(n * 4);
  const double a = pow(10, in_db / 20);
  sine16(buf, n, f, a);
  dsp_init(44100);
  dsp_set_output_gain(vol);
  dsp_config_t c = flat();
  c.bass.on = true;
  c.bass.f_low = 120;
  c.bass.boost = 0.5f;
  c.bass.harmonics = 0;
  dsp_set_config(&c);
  process_frames(buf, n);
  // the DSP applies the volume itself: take it out to compare the boost
  const double out = db(chan_amp(buf, 2 * 44100, 44100, 0, f) / (a * vol));
  free(buf);
  dsp_set_output_gain(1.0f);
  return out;
}

static double extension_db(double in_db, float vol) {
  return extension_db_at(80, in_db, vol);
}

static void test_bass_extension(void) {
  // quiet: full boost (+6 dB shelf; the subsonic high-pass at 60 Hz and the
  // shelf's slope at 80 Hz take some of it)
  const double quiet = extension_db(-40, 1.0f);
  check(quiet > 3.0 && quiet < 6.5, "bass: quiet 80 Hz boosted (dB)", quiet,
        4.5, 1.5);
  // loud at full volume: the dynamics hold the boost back
  const double loud = extension_db(-6, 1.0f);
  check(loud < quiet - 3.0, "bass: loud 80 Hz held back (dB)", loud, 0, 2);
  // the same loud input at -26 dB volume: free again, plus loudness
  const double soft = extension_db(-6, 0.05f);
  check(soft > quiet + 2.0, "bass: low volume adds loudness (dB)", soft,
        quiet + 6, 4);
  // a real shelf: no dip above the limit (the parallel form dipped -1.5 dB)
  const double above = extension_db_at(200, -40, 1.0f);
  check(above > -0.2, "bass: no dip above the limit, 200 Hz (dB)", above, 1,
        1.2);
}

static void test_clamp(void) {
  dsp_init(44100);
  dsp_config_t c = flat();
  add_filter(&c, DSP_CH_L, DSP_FILTER_PEAK, 30000, 20, 50);
  add_filter(&c, DSP_CH_L, DSP_FILTER_PEAK, NAN, -40, 0.01f);
  c.preamp_db = 99;
  c.bass.f_low = 10;
  c.bass.boost = 3;
  c.bass.harmonics = -1;
  dsp_clamp_config(&c);
  check_near("clamp f to 0.45 fs", c.filters[0][0].f, 0.45 * 44100, 0.5);
  check_near("clamp gain max", c.filters[0][0].gain_db, DSP_GAIN_MAX, 0);
  check_near("clamp q max", c.filters[0][0].q, DSP_Q_MAX, 0);
  check(c.filters[0][1].f >= DSP_F_MIN, "NaN frequency lands in range",
        c.filters[0][1].f, DSP_F_MIN, 0);
  check_near("clamp gain min", c.filters[0][1].gain_db, DSP_GAIN_MIN, 0);
  check_near("clamp q min", c.filters[0][1].q, DSP_Q_MIN, 0.001);
  check_near("clamp preamp", c.preamp_db, DSP_PREAMP_MAX, 0);
  check_near("clamp bass f_low", c.bass.f_low, DSP_BASS_F_MIN, 0);
  check_near("clamp bass boost", c.bass.boost, 1, 0);
  check_near("clamp bass harmonics", c.bass.harmonics, 0, 0);
}

/* The device sweep must equal sweep() in the prototype, which evaluates the
 * closed form in double for every sample. */
static void test_sweep_matches_closed_form(void) {
  const double fs = FS;
  const double t = 4;
  const double f1 = 20;
  const double f2 = 20000;
  const float amp = 0.25f;
  dsp_sweep_t sw;
  dsp_sweep_init(&sw, fs, t, f1, f2, amp);
  const size_t n = (size_t)floor(t * fs + 0.5);
  const double l = t / log(f2 / f1);
  const double k = 2 * M_PI * f1 * l;
  const size_t fi = (size_t)floor(0.01 * fs + 0.5);
  const size_t fo = (size_t)floor(0.005 * fs + 0.5);
  float chunk[352];
  size_t at = 0;
  double worst = 0;
  size_t got;
  while ((got = dsp_sweep_fill(&sw, chunk, 352)) > 0) {
    for (size_t j = 0; j < got; j++) {
      const size_t i = at + j;
      double g = amp;
      if (i < fi) {
        g *= 0.5 - 0.5 * cos(M_PI * (double)i / (double)fi);
      }
      if (n - i < fo) {
        g *= 0.5 - 0.5 * cos(M_PI * (double)(n - i) / (double)fo);
      }
      const double want = g * sin(k * (exp((double)i / fs / l) - 1));
      const double err = fabs(chunk[j] - want);
      if (err > worst) {
        worst = err;
      }
    }
    at += got;
  }
  check(at == n, "sweep length (samples)", (double)at, (double)n, 0);
  check(worst < 1e-4, "sweep vs closed form, max abs error", worst, 0, 1e-4);
}

static void test_pink_level(void) {
  dsp_pink_t p;
  dsp_pink_init(&p, -15.0f, 777);
  static float buf[44100];
  double s = 0;
  size_t cnt = 0;
  for (int sec = 0; sec < 20; sec++) {
    dsp_pink_fill(&p, buf, 44100);
    if (sec == 0) {
      continue; // let the slow pole settle
    }
    for (size_t i = 0; i < 44100; i++) {
      s += (double)buf[i] * buf[i];
      cnt++;
    }
  }
  check_near("pink noise RMS (dBFS)", 10 * log10(s / (double)cnt), -15.0, 0.5);
}

int main(void) {
  test_biquad_magnitudes();
  test_identity_and_bypass();
  test_chain_level();
  test_param_change_is_smooth();
  test_pbe();
  test_pbe_intermodulation();
  test_bass_extension();
  test_clamp();
  test_sweep_matches_closed_form();
  test_pink_level();
  printf("%s: %d failure(s)\n", s_failures ? "FAILED" : "PASSED", s_failures);
  return s_failures ? 1 : 0;
}
