#include "dsp_signal.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI (2.0 * M_PI)

/* RMS of the Kellet filter's output for uniform white input in [-1, 1)
 * (measured over 60 s; host_test.c checks the resulting level). */
#define PINK_RAW_RMS 1.7659f

static size_t round_pos(double v) {
  return (size_t)floor(v + 0.5); // Math.round() for positive values
}

void dsp_sweep_init(dsp_sweep_t *s, double fs, double seconds, double f1,
                    double f2, float amp) {
  const double l = seconds / log(f2 / f1);
  s->fs = fs;
  s->inv_fs_l = 1.0 / (fs * l);
  s->k = TWO_PI * f1 * l;
  s->ratio = exp(s->inv_fs_l);
  s->pos = 0;
  s->len = round_pos(seconds * fs);
  s->fade_in = round_pos(0.01 * fs);
  s->fade_out = round_pos(0.005 * fs);
  s->amp = amp;
}

/* phi(n) = K (exp(n / (fs L)) - 1). The S3 has no double FPU, so instead of
 * a soft-float exp() per sample the closed form is evaluated exactly once
 * per call and stepped inside it: phi(n+1) - phi(n) = K e(n) (r - 1), with
 * e(n+1) = e(n) r. Error over a 352-sample chunk stays far below 1e-6 rad. */
size_t dsp_sweep_fill(dsp_sweep_t *s, float *out, size_t n) {
  if (s->pos >= s->len) {
    return 0;
  }
  if (n > s->len - s->pos) {
    n = s->len - s->pos;
  }
  double e = exp((double)s->pos * s->inv_fs_l);
  double ph = fmod(s->k * (e - 1.0), TWO_PI);
  const double dk = s->k * (s->ratio - 1.0);
  for (size_t i = 0; i < n; i++) {
    const size_t idx = s->pos + i;
    float g = s->amp;
    if (idx < s->fade_in) {
      g *= 0.5f - 0.5f * cosf((float)M_PI * (float)idx / (float)s->fade_in);
    }
    if (s->len - idx < s->fade_out) {
      g *= 0.5f - 0.5f * cosf((float)M_PI * (float)(s->len - idx) /
                              (float)s->fade_out);
    }
    out[i] = g * sinf((float)ph);
    ph += dk * e;
    e *= s->ratio;
    if (ph >= TWO_PI) {
      ph -= TWO_PI; // the per-sample step is < pi below Nyquist
    }
  }
  s->pos += n;
  return n;
}

void dsp_pink_init(dsp_pink_t *p, float rms_dbfs, uint32_t seed) {
  for (int i = 0; i < 7; i++) {
    p->b[i] = 0.0f;
  }
  p->gain = powf(10.0f, rms_dbfs / 20.0f) / PINK_RAW_RMS;
  p->rng = seed ? seed : 1u;
}

void dsp_pink_fill(dsp_pink_t *p, float *out, size_t n) {
  float *b = p->b;
  uint32_t x = p->rng;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    const float w = (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.969f * b[2] + w * 0.153852f;
    b[3] = 0.8665f * b[3] + w * 0.3104856f;
    b[4] = 0.55f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.016898f;
    out[i] = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) *
             p->gain;
    b[6] = w * 0.115926f;
  }
  p->rng = x;
}
