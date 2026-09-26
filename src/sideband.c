/*
 * sideband engine. See sideband.h for the signal flow and parameter layout.
 *
 * Why this is C: synth firmware and DSP libraries are written in C because
 * the inner loop has to be deterministic and allocation-free. Everything here
 * lives in static storage; sideband_render() touches no heap, takes no locks and
 * always does a bounded amount of work (8 voices x 6 operators x 128 samples).
 */
#include "sideband.h"
#include "dspmath.h"

/* ------------------------------------------------------------------------ */
/* Parameters                                                                */

typedef struct { int8_t min, max, def; } Spec;

static const Spec GLOBAL_SPEC[SIDEBAND_GLOBALS] = {
  {0, SIDEBAND_ALGOS - 1, 0}, /* algorithm */
  {0, 7, 0},               /* feedback on the algorithm's feedback operator */
};

static const Spec OP_SPEC[SIDEBAND_OP_PARAMS] = {
  {0, 1, 0},     /* mode */
  {0, 31, 1},    /* coarse */
  {0, 99, 0},    /* fine */
  {-20, 20, 0},  /* detune (cents) */
  {0, 99, 0},    /* level (OP1 defaults to 99, see param_spec) */
  {0, 99, 99}, {0, 99, 99}, {0, 99, 99}, {0, 99, 60}, /* R1..R4 */
  {0, 99, 99}, {0, 99, 99}, {0, 99, 99}, {0, 99, 0},  /* L1..L4 */
};

static int valid_id(int id) { return id >= 0 && id < SIDEBAND_PARAMS; }

static Spec param_spec(int id) {
  if (id < SIDEBAND_GLOBALS) return GLOBAL_SPEC[id];
  int op = (id - SIDEBAND_GLOBALS) / SIDEBAND_OP_PARAMS;
  int off = (id - SIDEBAND_GLOBALS) % SIDEBAND_OP_PARAMS;
  Spec s = OP_SPEC[off];
  if (off == SIDEBAND_OP_LEVEL && op == 0) s.def = 99; /* the init voice is one sine */
  return s;
}

static int8_t g_param[SIDEBAND_PARAMS];

#define OPP(op, off) (g_param[SIDEBAND_GLOBALS + (op) * SIDEBAND_OP_PARAMS + (off)])

/* ------------------------------------------------------------------------ */
/* Algorithms: which operator modulates which. Operators are numbered so that */
/* every modulator has a higher number than the operator it feeds, which lets */
/* one pass from OP6 down to OP1 compute each sample. The unit tests check    */
/* this ordering and the graphs themselves.                                   */

#define B(n) (1u << ((n) - 1)) /* bit for OPn */

typedef struct {
  uint8_t mod[SIDEBAND_OPS]; /* mod[k]: operators feeding OP(k+1) */
  uint8_t carriers;       /* operators summed to the output */
  uint8_t fb_op;          /* index (0..5) of the operator with self-feedback */
} Algo;

static const Algo ALGOS[SIDEBAND_ALGOS] = {
  /* 1 PAIRS       2>1  4>3  6>5                  */ {{B(2), 0, B(4), 0, B(6), 0}, B(1) | B(3) | B(5), 5},
  /* 2 TWIN STACKS 3>2>1  6>5>4                   */ {{B(2), B(3), 0, B(5), B(6), 0}, B(1) | B(4), 5},
  /* 3 PAIR+FOUR   2>1  6>5>4>3                   */ {{B(2), 0, B(4), B(5), B(6), 0}, B(1) | B(3), 5},
  /* 4 TOWER       6>5>4>3>2>1                    */ {{B(2), B(3), B(4), B(5), B(6), 0}, B(1), 5},
  /* 5 FAN         2>1  6>(3,4,5)                 */ {{B(2), 0, B(6), B(6), B(6), 0}, B(1) | B(3) | B(4) | B(5), 5},
  /* 6 BRANCH      (2, 4>3, 6>5)>1                */ {{B(2) | B(3) | B(5), 0, B(4), 0, B(6), 0}, B(1), 5},
  /* 7 STACK+SINES 6>5>4, 1 2 3 plain             */ {{0, 0, 0, B(5), B(6), 0}, B(1) | B(2) | B(3) | B(4), 5},
  /* 8 ORGAN       all six are carriers           */ {{0, 0, 0, 0, 0, 0}, 0x3F, 5},
};

/* ------------------------------------------------------------------------ */
/* Tunings                                                                   */

#define MOD_TURNS      2.0f     /* a full-level modulator deviates the phase by 2 turns (4 pi rad) */
#define FB_MAX_RAD     1.6f     /* feedback 7: self-modulation index in radians */
#define MIX_GAIN       0.3f     /* voice sum to limiter input */
#define ENV_SLOWEST_S  40.0f    /* rate 0: a full 0..99 sweep takes this long */
#define ENV_FASTEST_S  0.0015f  /* rate 99 */
#define LIMIT_RELEASE_S 0.15f
#define VOLUME_SMOOTH_S 0.02f
#define END_FADE_S     0.1f     /* a released voice that settles above silence fades out this fast */
#define STAGE_DONE     4

/* Rising segments ease in towards the top instead of moving linearly in dB.
 * Each step covers a share of the distance to RISE_TOP (above the highest
 * level), so an attack starts fast and lands gently: a straight line in dB
 * accelerates in amplitude and stops dead, which ticks on slow attacks.
 * RISE_K is chosen so a full 0..99 rise still takes the rate's sweep time. */
#define RISE_TOP 119.0
#define RISE_K   0.018014052 /* ln(119 / 20) / 99 */

/* 1/sqrt(carriers): switching algorithms keeps roughly the same loudness
 * without one carrier becoming inaudible next to six. */
static const float CARRIER_NORM[SIDEBAND_OPS + 1] = {
  0.0f, 1.0f, 0.70710678f, 0.57735027f, 0.5f, 0.44721360f, 0.40824829f,
};

typedef struct {
  uint32_t phase;
  double env;  /* envelope level, 0..99. Double, because at slow rates a float
                * would round each tiny step to whole ULPs and drift. */
  int stage;   /* 0 attack, 1 decay, 2 sustain, 3 release, 4 done */
  float gain;  /* smoothed output-level gain */
} OpState;

typedef struct {
  OpState op[SIDEBAND_OPS];
  float fb1, fb2;     /* last two outputs of the feedback operator */
  float vel_gain;
  float fade;         /* 1 while playing; ramps to 0 when ending */
  int note;
  int gate;           /* key held */
  int active;
  int ending;
  uint32_t age;
} Voice;

static float g_sr = 48000.0f;
static double g_rate_step[100];    /* envelope units per sample for each rate */
static Voice g_voice[SIDEBAND_VOICES];
static uint32_t g_age;
static float g_out[SIDEBAND_BLOCK];
static float g_mon[SIDEBAND_BLOCK];
static float g_mix[SIDEBAND_BLOCK];

static float g_volume, g_volume_target, g_volume_coef;
static float g_dc_x1, g_dc_y1, g_dc_r;
static float g_lim_env, g_lim_rel, g_lim_gain;
static float g_peak;
static float g_end_step;

/* ------------------------------------------------------------------------ */
/* Conversions                                                               */

/* Output level (or envelope level) 0..99 as a linear gain: 99 = 1.0, and each
 * step is 0.75 dB (8 steps per halving). 0 is silence. */
static inline float level_amp(float level) {
  if (level < 0.5f) return 0.0f;
  return exp2_approx((level - 99.0f) * 0.125f);
}

static float note_hz(int note) {
  return 440.0f * exp2_approx((float)(note - 69) * (1.0f / 12.0f));
}

/* The frequency of operator `op` for a key at `key_hz`. */
static float op_hz(int op, float key_hz) {
  float hz;
  int coarse = OPP(op, SIDEBAND_OP_COARSE);
  float fine = (float)OPP(op, SIDEBAND_OP_FINE) * 0.01f;
  if (OPP(op, SIDEBAND_OP_MODE) == 1) {
    /* Fixed: 10^((coarse mod 4) + fine/100) Hz, i.e. 1 Hz to about 9.8 kHz. */
    hz = exp2_approx(3.3219281f * ((float)(coarse & 3) + fine));
  } else {
    float ratio = coarse == 0 ? 0.5f : (float)coarse;
    hz = key_hz * ratio * (1.0f + fine);
  }
  return hz * exp2_approx((float)OPP(op, SIDEBAND_OP_DETUNE) * (1.0f / 1200.0f));
}

static uint32_t hz_to_inc(float hz) {
  float cycles = hz / g_sr;
  if (!(cycles > 0.0f)) return 0u;
  if (cycles > 0.49f) cycles = 0.49f; /* stay below Nyquist */
  return (uint32_t)(int64_t)(cycles * 4294967296.0f);
}

static float feedback_turns(void) {
  int fb = g_param[SIDEBAND_P_FEEDBACK];
  if (fb <= 0) return 0.0f;
  float rad = FB_MAX_RAD * exp2_approx((float)(fb - 7));
  return rad / (2.0f * (float)DSP_PI);
}

/* ------------------------------------------------------------------------ */
/* Envelope                                                                  */

static inline void env_tick(OpState *o, int op) {
  if (o->stage == STAGE_DONE || o->stage == 2) {
    /* Sustain and done both hold at their target; sustain may still be
     * gliding towards L3 if L3 was edited while the key is down. */
    double target = (double)OPP(op, o->stage == 2 ? SIDEBAND_OP_L3 : SIDEBAND_OP_L4);
    double step = g_rate_step[OPP(op, o->stage == 2 ? SIDEBAND_OP_R3 : SIDEBAND_OP_R4)];
    if (o->env < target) { o->env += step * RISE_K * (RISE_TOP - o->env); if (o->env > target) o->env = target; }
    else if (o->env > target) { o->env -= step; if (o->env < target) o->env = target; }
    return;
  }
  double target = (double)OPP(op, SIDEBAND_OP_L1 + o->stage);
  double step = g_rate_step[OPP(op, SIDEBAND_OP_R1 + o->stage)];
  if (o->env < target) {
    o->env += step * RISE_K * (RISE_TOP - o->env);
    if (o->env >= target) { o->env = target; o->stage++; }
  } else if (o->env > target) {
    o->env -= step;
    if (o->env <= target) { o->env = target; o->stage++; }
  } else {
    o->stage++;
  }
}

/* ------------------------------------------------------------------------ */
/* Voices                                                                    */

static void voice_reset(Voice *v) {
  for (int k = 0; k < SIDEBAND_OPS; k++) {
    v->op[k].phase = 0u;
    v->op[k].env = 0.0;
    v->op[k].stage = STAGE_DONE;
    v->op[k].gain = 0.0f;
  }
  v->fb1 = v->fb2 = 0.0f;
  v->vel_gain = 0.0f;
  v->fade = 0.0f;
  v->note = -1;
  v->gate = 0;
  v->active = 0;
  v->ending = 0;
  v->age = 0u;
}

static Voice *voice_pick(int note) {
  Voice *best = 0;
  for (int i = 0; i < SIDEBAND_VOICES; i++)
    if (g_voice[i].active && g_voice[i].note == note) return &g_voice[i];
  for (int i = 0; i < SIDEBAND_VOICES; i++)
    if (!g_voice[i].active) return &g_voice[i];
  /* All busy: steal the oldest released voice, else the oldest held one. */
  for (int i = 0; i < SIDEBAND_VOICES; i++)
    if (!g_voice[i].gate && (!best || g_voice[i].age < best->age)) best = &g_voice[i];
  if (best) return best;
  best = &g_voice[0];
  for (int i = 1; i < SIDEBAND_VOICES; i++)
    if (g_voice[i].age < best->age) best = &g_voice[i];
  return best;
}

static float velocity_gain(int velocity) {
  return 0.3f + 0.7f * (float)velocity * (1.0f / 127.0f);
}

void sideband_note_on(int note, int velocity) {
  if (velocity <= 0) { sideband_note_off(note); return; }
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  if (velocity > 127) velocity = 127;
  Voice *v = voice_pick(note);
  int was_active = v->active;
  v->note = note;
  v->gate = 1;
  v->active = 1;
  v->ending = 0;
  v->fade = 1.0f;
  v->age = ++g_age;
  v->vel_gain = velocity_gain(velocity);
  for (int k = 0; k < SIDEBAND_OPS; k++) {
    OpState *o = &v->op[k];
    o->stage = 0;
    if (!was_active) {
      /* Key sync: a fresh voice starts every operator at phase 0 and its
       * envelope at L4, so the same notes always give the same samples. */
      o->phase = 0u;
      o->env = (double)OPP(k, SIDEBAND_OP_L4);
      o->gain = level_amp((float)OPP(k, SIDEBAND_OP_LEVEL));
    }
    /* A retriggered or stolen voice keeps its phase and level to avoid a click. */
  }
  if (!was_active) v->fb1 = v->fb2 = 0.0f;
}

void sideband_note_off(int note) {
  for (int i = 0; i < SIDEBAND_VOICES; i++) {
    Voice *v = &g_voice[i];
    if (v->active && v->gate && v->note == note) {
      v->gate = 0;
      for (int k = 0; k < SIDEBAND_OPS; k++) v->op[k].stage = 3;
    }
  }
}

void sideband_all_notes_off(void) {
  for (int i = 0; i < SIDEBAND_VOICES; i++)
    if (g_voice[i].active && g_voice[i].gate) sideband_note_off(g_voice[i].note);
}

void sideband_panic(void) {
  for (int i = 0; i < SIDEBAND_VOICES; i++) voice_reset(&g_voice[i]);
  g_dc_x1 = g_dc_y1 = 0.0f;
  g_lim_env = 0.0f;
  g_lim_gain = 1.0f;
}

int sideband_active_voices(void) {
  int n = 0;
  for (int i = 0; i < SIDEBAND_VOICES; i++) n += g_voice[i].active;
  return n;
}

static void render_voice(Voice *v, float *mix) {
  const Algo *al = &ALGOS[g_param[SIDEBAND_P_ALGO]];
  const float fb_amt = feedback_turns() * 0.5f; /* averages the last two samples */
  const int fb_op = al->fb_op;
  const float key_hz = note_hz(v->note);
  int ncar = 0;
  for (int k = 0; k < SIDEBAND_OPS; k++) ncar += (al->carriers >> k) & 1;
  const float out_gain = v->vel_gain * CARRIER_NORM[ncar];

  uint32_t inc[SIDEBAND_OPS];
  float gain_step[SIDEBAND_OPS], gain_target[SIDEBAND_OPS];
  for (int k = 0; k < SIDEBAND_OPS; k++) {
    inc[k] = hz_to_inc(op_hz(k, key_hz));
    gain_target[k] = level_amp((float)OPP(k, SIDEBAND_OP_LEVEL));
    gain_step[k] = (gain_target[k] - v->op[k].gain) * (1.0f / (float)SIDEBAND_BLOCK);
  }

  for (int s = 0; s < SIDEBAND_BLOCK; s++) {
    float out[SIDEBAND_OPS];
    float sum = 0.0f;
    for (int k = SIDEBAND_OPS - 1; k >= 0; k--) {
      OpState *o = &v->op[k];
      env_tick(o, k);
      o->gain += gain_step[k];
      float amp = level_amp((float)o->env) * o->gain;
      float y = 0.0f;
      if (amp > 0.0f) {
        float mod = 0.0f;
        for (int j = k + 1; j < SIDEBAND_OPS; j++)
          if (al->mod[k] & (1u << j)) mod += out[j];
        float turns = mod * MOD_TURNS;
        if (k == fb_op) turns += (v->fb1 + v->fb2) * fb_amt;
        y = sine_lookup(o->phase + turns_to_phase(turns)) * amp;
      }
      o->phase += inc[k];
      out[k] = y;
      if (al->carriers & (1u << k)) sum += y;
    }
    v->fb2 = v->fb1;
    v->fb1 = out[fb_op];
    if (v->ending) {
      v->fade -= g_end_step;
      if (v->fade < 0.0f) v->fade = 0.0f;
    }
    mix[s] += sum * out_gain * v->fade;
  }
  for (int k = 0; k < SIDEBAND_OPS; k++) v->op[k].gain = gain_target[k];

  /* A released voice ends once every carrier envelope has finished. If a
   * carrier settles above silence (L4 > 0) it fades out, so no patch can
   * leave a note droning after the key is let go. */
  if (!v->gate) {
    int finished = 1, silent = 1;
    for (int k = 0; k < SIDEBAND_OPS; k++) {
      if (!(al->carriers & (1u << k))) continue;
      const OpState *o = &v->op[k];
      float a = level_amp((float)o->env) * o->gain;
      if (o->stage != STAGE_DONE && a > 1e-4f) finished = 0;
      if (a > 1e-4f) silent = 0;
    }
    if (finished && silent) voice_reset(v);
    else if (finished && !v->ending) v->ending = 1;
    if (v->ending && v->fade <= 0.0f) voice_reset(v);
  }
}

/* ------------------------------------------------------------------------ */
/* Master chain                                                              */

/* One sample through DC blocker, limiter, volume and ceiling. Returns the
 * output; *mon receives the post-limiter, pre-volume sample. */
static inline float master_sample(float x, float *mon) {
  if (!is_finite(x)) x = 0.0f;
  /* DC blocker: a one-pole high-pass at a few hertz. */
  float y = x - g_dc_x1 + g_dc_r * g_dc_y1;
  g_dc_x1 = x;
  if (absf(y) < 1e-20f) y = 0.0f;
  if (!is_finite(y)) y = 0.0f;
  g_dc_y1 = y;
  /* Peak limiter with instant attack: the envelope is never below the
   * current sample, so |y * gain| <= threshold on every sample. */
  float a = absf(y);
  float held = g_lim_env * g_lim_rel;
  g_lim_env = a > held ? a : held;
  if (g_lim_env < 1e-20f) g_lim_env = 0.0f;
  g_lim_gain = g_lim_env > SIDEBAND_LIMIT_THRESHOLD ? SIDEBAND_LIMIT_THRESHOLD / g_lim_env : 1.0f;
  y *= g_lim_gain;
  *mon = y;
  /* Volume (smoothed), then a hard ceiling as the last line of defence. */
  g_volume += (g_volume_target - g_volume) * g_volume_coef;
  float out = y * g_volume;
  if (out > SIDEBAND_CEILING) out = SIDEBAND_CEILING;
  if (out < -SIDEBAND_CEILING) out = -SIDEBAND_CEILING;
  return out;
}

float *sideband_render(void) {
  for (int s = 0; s < SIDEBAND_BLOCK; s++) g_mix[s] = 0.0f;
  for (int i = 0; i < SIDEBAND_VOICES; i++)
    if (g_voice[i].active) render_voice(&g_voice[i], g_mix);

  /* Should never happen with clamped parameters (the fuzz tests check), but
   * if a non-finite value ever appears, silence every voice at once. */
  int bad = 0;
  for (int s = 0; s < SIDEBAND_BLOCK; s++) if (!is_finite(g_mix[s])) bad = 1;
  if (bad) {
    sideband_panic();
    for (int s = 0; s < SIDEBAND_BLOCK; s++) g_mix[s] = 0.0f;
  }

  for (int s = 0; s < SIDEBAND_BLOCK; s++) {
    float out = master_sample(g_mix[s] * MIX_GAIN, &g_mon[s]);
    g_out[s] = out;
    float a = absf(out);
    if (a > g_peak) g_peak = a;
  }
  return g_out;
}

float *sideband_monitor(void) { return g_mon; }

float sideband_take_peak(void) {
  float p = g_peak;
  g_peak = 0.0f;
  return p;
}

float sideband_gain_reduction(void) { return g_lim_gain; }

float sideband_set_volume(float gain) {
  if (!is_finite(gain)) return g_volume_target;
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  g_volume_target = gain;
  return gain;
}

/* ------------------------------------------------------------------------ */
/* Parameters API                                                            */

float sideband_set_param(int id, float value) {
  if (!valid_id(id)) return 0.0f;
  if (!is_finite(value)) return (float)g_param[id];
  Spec s = param_spec(id);
  if (value < (float)s.min) value = (float)s.min;
  if (value > (float)s.max) value = (float)s.max;
  int v = (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
  if (v < s.min) v = s.min;
  if (v > s.max) v = s.max;
  g_param[id] = (int8_t)v;
  return (float)v;
}

float sideband_get_param(int id) { return valid_id(id) ? (float)g_param[id] : 0.0f; }
float sideband_param_min(int id) { return valid_id(id) ? (float)param_spec(id).min : 0.0f; }
float sideband_param_max(int id) { return valid_id(id) ? (float)param_spec(id).max : 0.0f; }
float sideband_param_default(int id) { return valid_id(id) ? (float)param_spec(id).def : 0.0f; }

int sideband_algo_mod_mask(int algo, int op) {
  if (algo < 0 || algo >= SIDEBAND_ALGOS || op < 0 || op >= SIDEBAND_OPS) return 0;
  return ALGOS[algo].mod[op];
}

int sideband_algo_carrier_mask(int algo) {
  return (algo >= 0 && algo < SIDEBAND_ALGOS) ? ALGOS[algo].carriers : 0;
}

int sideband_algo_feedback_op(int algo) {
  return (algo >= 0 && algo < SIDEBAND_ALGOS) ? ALGOS[algo].fb_op : -1;
}

float sideband_sample_rate(void) { return g_sr; }

/* ------------------------------------------------------------------------ */

void sideband_init(float sample_rate) {
  if (!is_finite(sample_rate)) sample_rate = 48000.0f;
  if (sample_rate < 8000.0f) sample_rate = 8000.0f;
  if (sample_rate > 192000.0f) sample_rate = 192000.0f;
  g_sr = sample_rate;

  sine_table_build();

  /* Rate r sweeps the full 0..99 range in T(r) seconds, from 40 s at rate 0
   * to 1.5 ms at rate 99, evenly spaced on a log scale. */
  const float log2_span = -14.702667f; /* log2(0.0015 / 40) */
  for (int r = 0; r < 100; r++) {
    double t = (double)ENV_SLOWEST_S * (double)exp2_approx(log2_span * (float)r / 99.0f);
    g_rate_step[r] = 99.0 / (t * (double)g_sr);
  }

  for (int id = 0; id < SIDEBAND_PARAMS; id++) g_param[id] = param_spec(id).def;
  for (int i = 0; i < SIDEBAND_VOICES; i++) voice_reset(&g_voice[i]);
  g_age = 0u;

  g_volume = 0.0f; /* fades in from silence */
  g_volume_target = SIDEBAND_DEFAULT_VOLUME;
  g_volume_coef = 1.0f / (VOLUME_SMOOTH_S * g_sr + 1.0f);
  g_dc_r = 1.0f - (2.0f * (float)DSP_PI * 4.0f / g_sr); /* ~4 Hz corner */
  g_dc_x1 = g_dc_y1 = 0.0f;
  /* Envelope falls by 1/e (8.7 dB) every LIMIT_RELEASE_S seconds. */
  g_lim_rel = exp2_approx(-1.442695f / (LIMIT_RELEASE_S * g_sr));
  g_lim_env = 0.0f;
  g_lim_gain = 1.0f;
  g_peak = 0.0f;
  g_end_step = 1.0f / (END_FADE_S * g_sr);
  for (int s = 0; s < SIDEBAND_BLOCK; s++) g_out[s] = g_mon[s] = g_mix[s] = 0.0f;
}
