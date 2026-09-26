/*
 * sideband: a six-operator FM synthesizer engine.
 *
 * Freestanding C11. No libc, no libm, no heap: every buffer is static, so the
 * inner loops never allocate. The same file compiles to WebAssembly (for the
 * browser's AudioWorklet) and natively (for the unit tests).
 *
 * Signal flow per sample:
 *   voices (6 operators each, routed by one of 8 algorithms)
 *   -> mix -> DC blocker -> peak limiter (-3 dBFS) -> monitor tap
 *   -> smoothed volume -> hard ceiling (-1 dBFS) -> output
 */
#ifndef SIDEBAND_H
#define SIDEBAND_H

#include <stdint.h>

#define SIDEBAND_BLOCK      128 /* samples per render call (one Web Audio render quantum) */
#define SIDEBAND_OPS        6
#define SIDEBAND_VOICES     8
#define SIDEBAND_ALGOS      8
#define SIDEBAND_GLOBALS    2
#define SIDEBAND_OP_PARAMS  13
#define SIDEBAND_PARAMS     (SIDEBAND_GLOBALS + SIDEBAND_OPS * SIDEBAND_OP_PARAMS) /* 80 */

/* Global parameter ids. */
enum { SIDEBAND_P_ALGO = 0, SIDEBAND_P_FEEDBACK = 1 };

/* Per-operator parameters: id = SIDEBAND_GLOBALS + op * SIDEBAND_OP_PARAMS + offset,
 * where op 0 is OP1 and op 5 is OP6. */
enum {
  SIDEBAND_OP_MODE,   /* 0 = ratio to the key, 1 = fixed frequency */
  SIDEBAND_OP_COARSE, /* ratio mode: 0 = x0.5, 1..31 = x1..x31; fixed mode: decade (mod 4) */
  SIDEBAND_OP_FINE,   /* 0..99: ratio x(1 + fine/100); fixed x10^(fine/100) */
  SIDEBAND_OP_DETUNE, /* -20..+20 cents */
  SIDEBAND_OP_LEVEL,  /* 0..99 output level, 0.75 dB per step, 0 = off */
  SIDEBAND_OP_R1, SIDEBAND_OP_R2, SIDEBAND_OP_R3, SIDEBAND_OP_R4, /* envelope rates 0..99 */
  SIDEBAND_OP_L1, SIDEBAND_OP_L2, SIDEBAND_OP_L3, SIDEBAND_OP_L4  /* envelope levels 0..99 */
};

/* Loudness guarantees (linear full-scale units). */
#define SIDEBAND_LIMIT_THRESHOLD 0.70794578f /* -3 dBFS: the limiter's ceiling */
#define SIDEBAND_CEILING         0.89125094f /* -1 dBFS: nothing rendered is louder */
#define SIDEBAND_DEFAULT_VOLUME  0.25f       /* -12 dB */

void   sideband_init(float sample_rate);
float  sideband_sample_rate(void);

float  sideband_set_param(int id, float value); /* returns the stored (clamped) value */
float  sideband_get_param(int id);
float  sideband_param_min(int id);
float  sideband_param_max(int id);
float  sideband_param_default(int id);

void   sideband_note_on(int note, int velocity);
void   sideband_note_off(int note);
void   sideband_all_notes_off(void);
void   sideband_panic(void);
float  sideband_set_volume(float gain);        /* linear 0..1, returns the stored value */

float *sideband_render(void);                  /* renders SIDEBAND_BLOCK samples */
float *sideband_monitor(void);                 /* the same block before the volume stage */
float  sideband_take_peak(void);               /* output peak since the last call */
float  sideband_gain_reduction(void);          /* limiter gain at the end of the last block */
int    sideband_active_voices(void);

int    sideband_algo_mod_mask(int algo, int op); /* bit j set = OP(j+1) modulates OP(op+1) */
int    sideband_algo_carrier_mask(int algo);
int    sideband_algo_feedback_op(int algo);

#endif
