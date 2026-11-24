#include <math.h>

#include "dsp_biquad.h"

void dsp_biquad_design(dsp_filter_type_t type, double f, double gain_db,
                       double q, double fs, dsp_coeffs_t *out) {
  const double a = pow(10.0, gain_db / 40.0);
  const double w = 2.0 * M_PI * f / fs;
  const double c = cos(w);
  const double s = sin(w);
  const bool shelf =
      type == DSP_FILTER_LOW_SHELF || type == DSP_FILTER_HIGH_SHELF;
  const double alpha = s / (2.0 * (shelf ? M_SQRT1_2 : q));
  double b0;
  double b1;
  double b2;
  double a0;
  double a1;
  double a2;

  switch (type) {
  case DSP_FILTER_PEAK:
    b0 = 1 + alpha * a;
    b1 = -2 * c;
    b2 = 1 - alpha * a;
    a0 = 1 + alpha / a;
    a1 = -2 * c;
    a2 = 1 - alpha / a;
    break;
  case DSP_FILTER_LOW_SHELF: {
    const double r = 2 * sqrt(a) * alpha;
    b0 = a * ((a + 1) - (a - 1) * c + r);
    b1 = 2 * a * ((a - 1) - (a + 1) * c);
    b2 = a * ((a + 1) - (a - 1) * c - r);
    a0 = (a + 1) + (a - 1) * c + r;
    a1 = -2 * ((a - 1) + (a + 1) * c);
    a2 = (a + 1) + (a - 1) * c - r;
    break;
  }
  case DSP_FILTER_HIGH_SHELF: {
    const double r = 2 * sqrt(a) * alpha;
    b0 = a * ((a + 1) + (a - 1) * c + r);
    b1 = -2 * a * ((a - 1) + (a + 1) * c);
    b2 = a * ((a + 1) + (a - 1) * c - r);
    a0 = (a + 1) - (a - 1) * c + r;
    a1 = 2 * ((a - 1) - (a + 1) * c);
    a2 = (a + 1) - (a - 1) * c - r;
    break;
  }
  case DSP_FILTER_HIGH_PASS:
    b0 = (1 + c) / 2;
    b1 = -(1 + c);
    b2 = (1 + c) / 2;
    a0 = 1 + alpha;
    a1 = -2 * c;
    a2 = 1 - alpha;
    break;
  case DSP_FILTER_LOW_PASS:
  default:
    b0 = (1 - c) / 2;
    b1 = 1 - c;
    b2 = (1 - c) / 2;
    a0 = 1 + alpha;
    a1 = -2 * c;
    a2 = 1 - alpha;
    break;
  }

  out->b0 = (float)(b0 / a0);
  out->b1 = (float)(b1 / a0);
  out->b2 = (float)(b2 / a0);
  out->a1 = (float)(a1 / a0);
  out->a2 = (float)(a2 / a0);
}

double dsp_biquad_mag_db(const dsp_coeffs_t *k, double f, double fs) {
  const double w = 2.0 * M_PI * f / fs;
  const double c1 = cos(w);
  const double s1 = sin(w);
  const double c2 = cos(2 * w);
  const double s2 = sin(2 * w);
  const double nr = k->b0 + k->b1 * c1 + k->b2 * c2;
  const double ni = -(k->b1 * s1 + k->b2 * s2);
  const double dr = 1 + k->a1 * c1 + k->a2 * c2;
  const double di = -(k->a1 * s1 + k->a2 * s2);
  return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

static inline float flush(float z) {
  return fabsf(z) < 1e-15f ? 0.0f : z;
}

void dsp_biquad_run2(const dsp_coeffs_t *ka, dsp_state_t *sa, float *xa,
                     const dsp_coeffs_t *kb, dsp_state_t *sb, float *xb,
                     size_t n) {
  const float ab0 = ka->b0, ab1 = ka->b1, ab2 = ka->b2, aa1 = ka->a1,
              aa2 = ka->a2;
  const float bb0 = kb->b0, bb1 = kb->b1, bb2 = kb->b2, ba1 = kb->a1,
              ba2 = kb->a2;
  float az1 = sa->z1, az2 = sa->z2, bz1 = sb->z1, bz2 = sb->z2;
  for (size_t i = 0; i < n; i++) {
    const float ina = xa[i];
    const float inb = xb[i];
    const float ya = ab0 * ina + az1;
    const float yb = bb0 * inb + bz1;
    az1 = ab1 * ina - aa1 * ya + az2;
    bz1 = bb1 * inb - ba1 * yb + bz2;
    az2 = ab2 * ina - aa2 * ya;
    bz2 = bb2 * inb - ba2 * yb;
    xa[i] = ya;
    xb[i] = yb;
  }
  sa->z1 = flush(az1);
  sa->z2 = flush(az2);
  sb->z1 = flush(bz1);
  sb->z2 = flush(bz2);
}

void dsp_biquad_run(const dsp_coeffs_t *k, dsp_state_t *s, float *x, size_t n) {
  const float b0 = k->b0;
  const float b1 = k->b1;
  const float b2 = k->b2;
  const float a1 = k->a1;
  const float a2 = k->a2;
  float z1 = s->z1;
  float z2 = s->z2;
  for (size_t i = 0; i < n; i++) {
    const float in = x[i];
    const float y = b0 * in + z1;
    z1 = b1 * in - a1 * y + z2;
    z2 = b2 * in - a2 * y;
    x[i] = y;
  }
  // Flush denormals once per block rather than per sample: a decaying tail
  // would otherwise sit in the denormal range and cost far more per sample.
  s->z1 = flush(z1);
  s->z2 = flush(z2);
}
