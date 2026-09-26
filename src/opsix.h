/*
 * op-six: a six-operator FM synthesizer engine.
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
#ifndef OPSIX_H
#define OPSIX_H

#include <stdint.h>

#define OPSIX_BLOCK      128 /* samples per render call (one Web Audio render quantum) */
#define OPSIX_OPS        6
#define OPSIX_VOICES     8
#define OPSIX_ALGOS      8
#define OPSIX_GLOBALS    2
#define OPSIX_OP_PARAMS  13
#define OPSIX_PARAMS     (OPSIX_GLOBALS + OPSIX_OPS * OPSIX_OP_PARAMS) /* 80 */

/* Global parameter ids. */
enum { OPSIX_P_ALGO = 0, OPSIX_P_FEEDBACK = 1 };

/* Per-operator parameters: id = OPSIX_GLOBALS + op * OPSIX_OP_PARAMS + offset,
 * where op 0 is OP1 and op 5 is OP6. */
enum {
  OPSIX_OP_MODE,   /* 0 = ratio to the key, 1 = fixed frequency */
  OPSIX_OP_COARSE, /* ratio mode: 0 = x0.5, 1..31 = x1..x31; fixed mode: decade (mod 4) */
  OPSIX_OP_FINE,   /* 0..99: ratio x(1 + fine/100); fixed x10^(fine/100) */
  OPSIX_OP_DETUNE, /* -20..+20 cents */
  OPSIX_OP_LEVEL,  /* 0..99 output level, 0.75 dB per step, 0 = off */
  OPSIX_OP_R1, OPSIX_OP_R2, OPSIX_OP_R3, OPSIX_OP_R4, /* envelope rates 0..99 */
  OPSIX_OP_L1, OPSIX_OP_L2, OPSIX_OP_L3, OPSIX_OP_L4  /* envelope levels 0..99 */
};

/* Loudness guarantees (linear full-scale units). */
#define OPSIX_LIMIT_THRESHOLD 0.70794578f /* -3 dBFS: the limiter's ceiling */
#define OPSIX_CEILING         0.89125094f /* -1 dBFS: nothing rendered is louder */
#define OPSIX_DEFAULT_VOLUME  0.25f       /* -12 dB */

void   opsix_init(float sample_rate);
float  opsix_sample_rate(void);

float  opsix_set_param(int id, float value); /* returns the stored (clamped) value */
float  opsix_get_param(int id);
float  opsix_param_min(int id);
float  opsix_param_max(int id);
float  opsix_param_default(int id);

void   opsix_note_on(int note, int velocity);
void   opsix_note_off(int note);
void   opsix_all_notes_off(void);
void   opsix_panic(void);
float  opsix_set_volume(float gain);        /* linear 0..1, returns the stored value */

float *opsix_render(void);                  /* renders OPSIX_BLOCK samples */
float *opsix_monitor(void);                 /* the same block before the volume stage */
float  opsix_take_peak(void);               /* output peak since the last call */
float  opsix_gain_reduction(void);          /* limiter gain at the end of the last block */
int    opsix_active_voices(void);

int    opsix_algo_mod_mask(int algo, int op); /* bit j set = OP(j+1) modulates OP(op+1) */
int    opsix_algo_carrier_mask(int algo);
int    opsix_algo_feedback_op(int algo);

#endif
