#pragma once

/**
 * @file dsp_signal.h
 * @brief Measurement signals generated on the fly (no large buffers).
 *
 * The sweep must match sweep() in docs/web_dsp/calibration-prototype.html
 * sample for sample: the page deconvolves the recording with its own copy.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Exponential sine sweep (Farina) with Hann fade-in/-out. */
typedef struct {
  double fs;
  double inv_fs_l; // 1 / (fs * L)
  double k;        // 2 pi f1 L
  double ratio;    // exp(1 / (fs * L)), per-sample growth of exp(t / L)
  size_t pos;
  size_t len;
  size_t fade_in;
  size_t fade_out;
  float amp;
} dsp_sweep_t;

/** Prototype defaults: 4 s, 20 Hz .. 20 kHz, amplitude 0.25. */
void dsp_sweep_init(dsp_sweep_t *s, double fs, double seconds, double f1,
                    double f2, float amp);

/** Fill up to @p n samples; returns how many were written (0 = finished). */
size_t dsp_sweep_fill(dsp_sweep_t *s, float *out, size_t n);

/** Pink noise (Paul Kellet's filter) at a fixed RMS level. */
typedef struct {
  float b[7];
  float gain;
  uint32_t rng;
} dsp_pink_t;

void dsp_pink_init(dsp_pink_t *p, float rms_dbfs, uint32_t seed);
void dsp_pink_fill(dsp_pink_t *p, float *out, size_t n);

#ifdef __cplusplus
}
#endif
