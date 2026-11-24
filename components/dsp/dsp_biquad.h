#pragma once

#include <stddef.h>

#include "dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

/** Normalised biquad coefficients (a0 == 1). */
typedef struct {
  float b0, b1, b2, a1, a2;
} dsp_coeffs_t;

/** Transposed direct form II state. */
typedef struct {
  float z1, z2;
} dsp_state_t;

/**
 * RBJ cookbook design, ported from coeffs() in
 * docs/web_dsp/calibration-prototype.html. Shelves use Q = 1/sqrt(2)
 * whatever @p q says; @p gain_db is ignored for high/low pass.
 */
void dsp_biquad_design(dsp_filter_type_t type, double f, double gain_db,
                       double q, double fs, dsp_coeffs_t *out);

/** Magnitude response in dB at @p f (magDb() in the prototype). */
double dsp_biquad_mag_db(const dsp_coeffs_t *k, double f, double fs);

/** Filter @p n samples of @p x in place. */
void dsp_biquad_run(const dsp_coeffs_t *k, dsp_state_t *s, float *x, size_t n);

/**
 * Two independent biquads over two buffers in one loop. Same result as two
 * dsp_biquad_run() calls, but the two dependency chains interleave, which
 * nearly halves the time on the ESP32-S3's FPU.
 */
void dsp_biquad_run2(const dsp_coeffs_t *ka, dsp_state_t *sa, float *xa,
                     const dsp_coeffs_t *kb, dsp_state_t *sb, float *xb,
                     size_t n);
