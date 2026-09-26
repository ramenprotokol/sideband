/*
 * Small, freestanding maths for the synth: a sine table and a fast exp2.
 * No libm. Everything here is plain IEEE float/double arithmetic, so it gives
 * the same bits on every WebAssembly engine.
 */
#ifndef SIDEBAND_DSPMATH_H
#define SIDEBAND_DSPMATH_H

#include <stdint.h>

#define SINE_BITS 12
#define SINE_SIZE (1 << SINE_BITS)           /* 4096 points per turn */
#define SINE_FRAC_BITS (32 - SINE_BITS)

static float sine_table[SINE_SIZE + 1];      /* +1 guard point for interpolation */

static const double DSP_PI = 3.14159265358979323846;

/* sin(x) for |x| <= pi by its Taylor series. At |x| = pi the first omitted
 * term (x^35/35!) is below 1e-22, far under double precision. Only used to
 * fill the table once at start-up. */
static double taylor_sin(double x) {
  double x2 = x * x, term = x, sum = x;
  for (int n = 1; n <= 16; n++) {
    term *= -x2 / (double)((2 * n) * (2 * n + 1));
    sum += term;
  }
  return sum;
}

static void sine_table_build(void) {
  for (int i = 0; i <= SINE_SIZE; i++) {
    double x = 2.0 * DSP_PI * (double)i / (double)SINE_SIZE;
    if (x > DSP_PI) x -= 2.0 * DSP_PI;
    sine_table[i] = (float)taylor_sin(x);
  }
  /* Pin the exact zeros and peaks so the table is perfectly symmetric. */
  sine_table[0] = 0.0f;
  sine_table[SINE_SIZE / 4] = 1.0f;
  sine_table[SINE_SIZE / 2] = 0.0f;
  sine_table[3 * SINE_SIZE / 4] = -1.0f;
  sine_table[SINE_SIZE] = 0.0f;
}

/* sin(2*pi*phase/2^32) by table lookup with linear interpolation.
 * Worst-case error is about 3e-7, below 16-bit audio resolution. */
static inline float sine_lookup(uint32_t phase) {
  uint32_t i = phase >> SINE_FRAC_BITS;
  float frac = (float)(phase & ((1u << SINE_FRAC_BITS) - 1u)) * (1.0f / (float)(1u << SINE_FRAC_BITS));
  float a = sine_table[i];
  return a + (sine_table[i + 1] - a) * frac;
}

/* A phase offset in turns (1.0 = one full cycle) as a 32-bit phase increment.
 * Out-of-range or NaN input gives 0, so the float-to-int conversion below is
 * always defined behaviour. */
static inline uint32_t turns_to_phase(float t) {
  if (!(t > -1024.0f && t < 1024.0f)) return 0u;
  return (uint32_t)(int64_t)(t * 4294967296.0f);
}

/* 2^x. Relative error around 1e-7 across the range the synth uses.
 * NaN or very negative input gives 0; very large input saturates. */
static inline float exp2_approx(float x) {
  if (!(x > -126.0f)) return 0.0f;
  if (x > 126.0f) x = 126.0f;
  int i = (int)x;
  if ((float)i > x) i--;                       /* floor */
  float f = x - (float)i;                      /* [0, 1) */
  /* Taylor series of e^(f ln 2) to degree 8. */
  float p = 1.3215487e-6f;
  p = p * f + 1.5252734e-5f;
  p = p * f + 1.5403530e-4f;
  p = p * f + 1.3333558e-3f;
  p = p * f + 9.6181291e-3f;
  p = p * f + 5.5504109e-2f;
  p = p * f + 2.4022651e-1f;
  p = p * f + 6.9314718e-1f;
  p = p * f + 1.0f;
  union { uint32_t u; float f; } scale;
  scale.u = (uint32_t)(i + 127) << 23;
  return p * scale.f;
}

static inline float absf(float x) { return x < 0.0f ? -x : x; }

static inline int is_finite(float x) {
  return x == x && x < 3.0e38f && x > -3.0e38f;
}

#endif
