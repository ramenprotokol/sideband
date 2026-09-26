/*
 * sideband engine. See sideband.h for the signal flow and parameter layout.
 *
 * Why this is C: synth firmware and DSP libraries are written in C because
 * the inner loop has to be deterministic and allocation-free. Everything here
 * lives in static storage; sideband_render() touches no heap, takes no locks and
 * always does a bounded amount of work (8 voices x 6 operators x 128 samples,
 * twice the sine lookups during a 10 ms algorithm crossfade).
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

/* OP6 carries the feedback in every algorithm and is never modulated itself,
 * so its output is the same under any two algorithms. The crossfade between
 * algorithms relies on that, and the unit tests check it. */
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
#define MIX_GAIN       0.6f     /* voice sum to the master chain */
#define ENV_SLOWEST_S  40.0f    /* rate 0: a full 0..99 sweep takes this long */
#define ENV_FASTEST_S  0.0015f  /* rate 99 */
#define LIMIT_RELEASE_S 0.15f
#define VOLUME_SMOOTH_S 0.02f
#define LEVEL_SMOOTH_S 0.01f    /* each of two one-pole stages smoothing a level edit */
#define PITCH_SMOOTH_S 0.015f   /* one-pole glide for a ratio or fixed-frequency edit */
#define ALGO_XFADE_S   0.01f    /* crossfade when the algorithm changes under held notes */
#define END_FADE_S     0.1f     /* a released voice that settles above silence fades out this fast */
#define PANIC_FADE_S   0.005f   /* panic: every voice fades out this fast */
#define RISE_S         0.01f    /* a fading voice that is played again comes back up this fast */
#define STAGE_DONE     4

/* Operators close to Nyquist would only alias, so they fade out between
 * these frequencies (as fractions of the sample rate). The phase increment
 * is still clamped below Nyquist as a last line of defence. */
#define NYQ_FADE_START 0.43f
#define NYQ_FADE_END   0.47f
#define MAX_CYCLES     0.49f

/* The compressor: a slow RMS compressor with a wide soft knee, ahead of the
 * peak limiter. Its detector measures the mean square of the mix (with the
 * lowest bass filtered out of the measurement, so a bass note does not pump
 * the mix), which tracks loudness rather than peaks. A single note of a
 * factory voice sits in the lower half of the knee, where the ratio is gentle
 * (under about 2:1); only dense chords and extreme patches reach the full 4:1,
 * so they land much closer to the presets than the limiter alone would put
 * them. It can only lower the gain. */
#define COMP_THRESHOLD_DB -16.0f /* mean-square level, dB re full scale */
#define COMP_RATIO        4.0f   /* reached only above the knee */
#define COMP_KNEE_DB      12.0f  /* the ratio eases in from 1:1 over this range */
#define COMP_DETECT_S     0.04f
#define COMP_ATTACK_S     0.015f
#define COMP_RELEASE_S    0.3f
#define COMP_SIDECHAIN_HZ 120.0f

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
  double inc;   /* phase increment per sample, gliding towards its target.
                 * Double holds every 32-bit increment exactly, so a settled
                 * glide lands on the exact target. */
  double env;   /* envelope level, 0..99. Double, because at slow rates a float
                 * would round each tiny step to whole ULPs and drift. */
  int stage;    /* 0 attack, 1 decay, 2 sustain, 3 release, 4 done */
  double g1;    /* first smoothing stage of the output-level gain */
  double gain;  /* smoothed output-level gain (level x Nyquist fade). The
                 * smoothers run in double so they settle exactly instead of
                 * stalling a few float ULPs short of the target. */
} OpState;

typedef struct {
  OpState op[SIDEBAND_OPS];
  float fb1, fb2;     /* last two outputs of the feedback operator */
  double vel_gain;    /* smoothed towards vel_target */
  double vel_target;
  float fade;         /* voice gain: 1 while playing, 0 when it has faded out */
  float fade_target;
  float fade_step;    /* per-sample move of fade towards fade_target */
  int note;
  int gate;           /* key held */
  int active;
  int ending;         /* fading out; the voice is freed when fade reaches 0 */
  int snap;           /* next block: set frequencies without a glide (new note) */
  uint32_t age;
} Voice;

static float g_sr = 48000.0f;
static double g_rate_step[100];    /* envelope units per sample for each rate */
static Voice g_voice[SIDEBAND_VOICES];
static uint32_t g_age;
static float g_out[SIDEBAND_BLOCK];
static float g_mon[SIDEBAND_BLOCK];
static float g_mix[SIDEBAND_BLOCK];

/* Algorithm crossfade: g_algo is what the voices play; g_algo_from fades out
 * while g_xf (the weight of g_algo) rises from 0 to 1. */
static int g_algo, g_algo_from;
static float g_xf, g_xf_step;

static double g_level_coef, g_pitch_coef;
static float g_volume, g_volume_target, g_volume_coef;
static float g_dc_x1, g_dc_y1, g_dc_r;
static float g_sc_lp, g_sc_coef, g_det, g_det_coef, g_comp_gr, g_comp_att, g_comp_rel, g_comp_gain;
static float g_lim_env, g_lim_rel, g_lim_gain;
static float g_peak;
static float g_end_step, g_panic_step, g_rise_step;

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
  if (cycles > MAX_CYCLES) cycles = MAX_CYCLES; /* stay below Nyquist */
  return (uint32_t)(int64_t)(cycles * 4294967296.0f);
}

/* 1 below NYQ_FADE_START x sample rate, falling linearly to 0 at NYQ_FADE_END. */
static float nyquist_fade(float hz) {
  float cycles = hz / g_sr;
  if (!(cycles > NYQ_FADE_START)) return 1.0f;
  if (cycles >= NYQ_FADE_END) return 0.0f;
  return (NYQ_FADE_END - cycles) * (1.0f / (NYQ_FADE_END - NYQ_FADE_START));
}

/* The gain an operator is heading for: its level, faded out near Nyquist. */
static float op_target_gain(int op, float hz) {
  return level_amp((float)OPP(op, SIDEBAND_OP_LEVEL)) * nyquist_fade(hz);
}

static float feedback_turns(void) {
  int fb = g_param[SIDEBAND_P_FEEDBACK];
  if (fb <= 0) return 0.0f;
  float rad = FB_MAX_RAD * exp2_approx((float)(fb - 7));
  return rad / (2.0f * (float)DSP_PI);
}

static int carrier_count(const Algo *al) {
  int n = 0;
  for (int k = 0; k < SIDEBAND_OPS; k++) n += (al->carriers >> k) & 1;
  return n;
}

/* A one-pole smoothing coefficient for a time constant of `seconds`. */
static float smooth_coef(float seconds) { return 1.0f / (seconds * g_sr + 1.0f); }
static double smooth_coef_d(float seconds) { return 1.0 / ((double)seconds * (double)g_sr + 1.0); }

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
    v->op[k].inc = 0.0;
    v->op[k].env = 0.0;
    v->op[k].stage = STAGE_DONE;
    v->op[k].g1 = 0.0;
    v->op[k].gain = 0.0;
  }
  v->fb1 = v->fb2 = 0.0f;
  v->vel_gain = v->vel_target = 0.0;
  v->fade = v->fade_target = 0.0f;
  v->fade_step = 0.0f;
  v->note = -1;
  v->gate = 0;
  v->active = 0;
  v->ending = 0;
  v->snap = 0;
  v->age = 0u;
}

static int any_voice_active(void) {
  for (int i = 0; i < SIDEBAND_VOICES; i++) if (g_voice[i].active) return 1;
  return 0;
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
  if (was_active && v->note != note) v->snap = 1; /* a stolen voice jumps to its new pitch */
  v->note = note;
  v->gate = 1;
  v->active = 1;
  v->age = ++g_age;
  v->vel_target = (double)velocity_gain(velocity);
  if (was_active) {
    /* Retriggered or stolen: phases, levels and velocity carry on and glide,
     * and a voice that was fading out comes back up from where it is. */
    v->ending = 0;
    v->fade_target = 1.0f;
    v->fade_step = g_rise_step;
  } else {
    /* Key sync: a fresh voice starts every operator at phase 0 and its
     * envelope at L4, so the same notes always give the same samples. */
    const float key_hz = note_hz(note);
    for (int k = 0; k < SIDEBAND_OPS; k++) {
      OpState *o = &v->op[k];
      float hz = op_hz(k, key_hz);
      o->phase = 0u;
      o->inc = (double)hz_to_inc(hz);
      o->env = (double)OPP(k, SIDEBAND_OP_L4);
      o->g1 = o->gain = (double)op_target_gain(k, hz);
    }
    v->fb1 = v->fb2 = 0.0f;
    v->vel_gain = v->vel_target;
    v->fade = v->fade_target = 1.0f;
    v->ending = 0;
    v->snap = 0;
  }
  for (int k = 0; k < SIDEBAND_OPS; k++) v->op[k].stage = 0;
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

/* Panic: every sounding voice fades to silence over 5 ms and is freed. A fade
 * this short still stops everything at once, without the step of a cut. */
void sideband_panic(void) {
  for (int i = 0; i < SIDEBAND_VOICES; i++) {
    Voice *v = &g_voice[i];
    if (!v->active) continue;
    v->gate = 0;
    v->ending = 1;
    v->fade_target = 0.0f;
    v->fade_step = g_panic_step;
  }
}

/* The emergency stop for a non-finite mix: silence at once. */
static void hard_reset(void) {
  for (int i = 0; i < SIDEBAND_VOICES; i++) voice_reset(&g_voice[i]);
  g_dc_x1 = g_dc_y1 = 0.0f;
  g_sc_lp = g_det = g_comp_gr = 0.0f;
  g_comp_gain = 1.0f;
  g_lim_env = 0.0f;
  g_lim_gain = 1.0f;
  g_algo = g_algo_from = g_param[SIDEBAND_P_ALGO];
  g_xf = 1.0f;
}

int sideband_active_voices(void) {
  int n = 0;
  for (int i = 0; i < SIDEBAND_VOICES; i++) n += g_voice[i].active;
  return n;
}

/* Renders one voice into mix. While an algorithm crossfade is running
 * (xf0 < 1), every operator is computed under both routings from the same
 * phases and envelopes, and the two carrier sums are crossfaded. */
static void render_voice(Voice *v, float *mix, float xf0) {
  const Algo *al = &ALGOS[g_algo];
  const Algo *af = xf0 < 1.0f ? &ALGOS[g_algo_from] : 0;
  const float fb_amt = feedback_turns() * 0.5f; /* averages the last two samples */
  const int fb_op = al->fb_op;
  const float key_hz = note_hz(v->note);
  const float norm = CARRIER_NORM[carrier_count(al)];
  const float norm_from = af ? CARRIER_NORM[carrier_count(af)] : 0.0f;
  const double lc = g_level_coef, pc = g_pitch_coef;

  double inc_target[SIDEBAND_OPS], gain_target[SIDEBAND_OPS];
  for (int k = 0; k < SIDEBAND_OPS; k++) {
    float hz = op_hz(k, key_hz);
    inc_target[k] = (double)hz_to_inc(hz);
    gain_target[k] = (double)op_target_gain(k, hz);
    if (v->snap) v->op[k].inc = inc_target[k];
  }
  v->snap = 0;

  for (int s = 0; s < SIDEBAND_BLOCK; s++) {
    float out[SIDEBAND_OPS], out_from[SIDEBAND_OPS];
    float sum = 0.0f, sum_from = 0.0f;
    for (int k = SIDEBAND_OPS - 1; k >= 0; k--) {
      OpState *o = &v->op[k];
      env_tick(o, k);
      /* Level edits pass through two one-pole stages, frequency edits
       * through one, every sample: a fader drag is a smooth curve. */
      o->g1 += (gain_target[k] - o->g1) * lc;
      o->gain += (o->g1 - o->gain) * lc;
      if (o->inc != inc_target[k]) {
        /* Lands exactly once within a millionth (0.002 cents) of the target. */
        o->inc += (inc_target[k] - o->inc) * pc;
        if (absd(inc_target[k] - o->inc) <= 1e-6 * inc_target[k] + 0.5) o->inc = inc_target[k];
      }
      float amp = level_amp((float)o->env) * (float)o->gain;
      float y = 0.0f, y_from = 0.0f;
      if (amp > 0.0f) {
        float fb = k == fb_op ? (v->fb1 + v->fb2) * fb_amt : 0.0f;
        float mod = 0.0f;
        for (int j = k + 1; j < SIDEBAND_OPS; j++)
          if (al->mod[k] & (1u << j)) mod += out[j];
        y = sine_lookup(o->phase + turns_to_phase(mod * MOD_TURNS + fb)) * amp;
        if (af) {
          float mod_from = 0.0f;
          for (int j = k + 1; j < SIDEBAND_OPS; j++)
            if (af->mod[k] & (1u << j)) mod_from += out_from[j];
          y_from = sine_lookup(o->phase + turns_to_phase(mod_from * MOD_TURNS + fb)) * amp;
        }
      }
      o->phase += (uint32_t)(int64_t)o->inc;
      out[k] = y;
      out_from[k] = y_from;
      if (al->carriers & (1u << k)) sum += y;
      if (af && (af->carriers & (1u << k))) sum_from += y_from;
    }
    v->fb2 = v->fb1;
    v->fb1 = out[fb_op];
    v->vel_gain += (v->vel_target - v->vel_gain) * lc;
    if (v->fade < v->fade_target) {
      v->fade += v->fade_step;
      if (v->fade > v->fade_target) v->fade = v->fade_target;
    } else if (v->fade > v->fade_target) {
      v->fade -= v->fade_step;
      if (v->fade < v->fade_target) v->fade = v->fade_target;
    }
    float voice_out = sum * norm;
    if (af) {
      float w = xf0 + (float)(s + 1) * g_xf_step;
      if (w > 1.0f) w = 1.0f;
      voice_out = voice_out * w + sum_from * norm_from * (1.0f - w);
    }
    mix[s] += voice_out * (float)v->vel_gain * v->fade;
  }
  /* Settle the smoothers exactly once they are within a hair of the target. */
  for (int k = 0; k < SIDEBAND_OPS; k++) {
    OpState *o = &v->op[k];
    if (absd(o->g1 - gain_target[k]) < 1e-7 && absd(o->gain - gain_target[k]) < 1e-7)
      o->g1 = o->gain = gain_target[k];
  }
  if (absd(v->vel_gain - v->vel_target) < 1e-7) v->vel_gain = v->vel_target;

  /* A released voice ends once every carrier envelope has finished. If a
   * carrier settles above silence (L4 > 0) it fades out, so no patch can
   * leave a note droning after the key is let go. */
  if (!v->gate) {
    int finished = 1, silent = 1;
    for (int k = 0; k < SIDEBAND_OPS; k++) {
      if (!(al->carriers & (1u << k))) continue;
      const OpState *o = &v->op[k];
      float a = level_amp((float)o->env) * (float)o->gain;
      if (o->stage != STAGE_DONE && a > 1e-4f) finished = 0;
      if (a > 1e-4f) silent = 0;
    }
    if (finished && silent) {
      voice_reset(v);
      return;
    }
    if (finished && !v->ending) {
      v->ending = 1;
      v->fade_target = 0.0f;
      v->fade_step = g_end_step;
    }
  }
  if (v->ending && v->fade <= 0.0f) voice_reset(v);
}

/* ------------------------------------------------------------------------ */
/* Master chain                                                              */

/* The compressor's gain for one sample (always <= 1). */
static inline float compressor_gain(float y) {
  /* Sidechain: take the lows out of the measurement (one-pole high-pass). */
  float s = y - g_sc_lp;
  g_sc_lp += s * g_sc_coef;
  if (absf(g_sc_lp) < 1e-20f) g_sc_lp = 0.0f;
  if (s > 16.0f) s = 16.0f;
  if (s < -16.0f) s = -16.0f;
  g_det += (s * s - g_det) * g_det_coef;
  if (g_det < 1e-12f) g_det = 1e-12f; /* -120 dB floor: no denormals, log2 defined */
  float level_db = 3.0103000f * log2_approx(g_det); /* 10 log10(mean square) */
  float over = level_db - COMP_THRESHOLD_DB;
  const float half = 0.5f * COMP_KNEE_DB, slope = 1.0f - 1.0f / COMP_RATIO;
  float want; /* gain reduction the level asks for, in dB (soft knee) */
  if (over <= -half) want = 0.0f;
  else if (over < half) want = slope * (over + half) * (over + half) * (1.0f / (2.0f * COMP_KNEE_DB));
  else want = slope * over;
  g_comp_gr += (want - g_comp_gr) * (want > g_comp_gr ? g_comp_att : g_comp_rel);
  if (g_comp_gr < 1e-6f) g_comp_gr = 0.0f;
  return exp2_approx(g_comp_gr * -0.16609640f); /* 10^(-dB/20) */
}

/* One sample through DC blocker, compressor, limiter, volume and ceiling.
 * Returns the output; *mon receives the post-limiter, pre-volume sample. */
static inline float master_sample(float x, float *mon) {
  if (!is_finite(x)) x = 0.0f;
  /* DC blocker: a one-pole high-pass at a few hertz. */
  float y = x - g_dc_x1 + g_dc_r * g_dc_y1;
  g_dc_x1 = x;
  if (absf(y) < 1e-20f) y = 0.0f;
  if (!is_finite(y)) y = 0.0f;
  g_dc_y1 = y;
  g_comp_gain = compressor_gain(y);
  y *= g_comp_gain;
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
  /* An algorithm change under sounding notes starts a crossfade; one that
   * arrives mid-crossfade waits for it to finish, so the sound never jumps. */
  if (g_xf >= 1.0f && g_algo != g_param[SIDEBAND_P_ALGO]) {
    g_algo_from = g_algo;
    g_algo = g_param[SIDEBAND_P_ALGO];
    g_xf = any_voice_active() ? 0.0f : 1.0f;
  }
  const float xf0 = g_xf;

  for (int s = 0; s < SIDEBAND_BLOCK; s++) g_mix[s] = 0.0f;
  for (int i = 0; i < SIDEBAND_VOICES; i++)
    if (g_voice[i].active) render_voice(&g_voice[i], g_mix, xf0);
  if (g_xf < 1.0f) {
    g_xf += (float)SIDEBAND_BLOCK * g_xf_step;
    if (g_xf > 1.0f) g_xf = 1.0f;
  }

  /* Should never happen with clamped parameters (the fuzz tests check), but
   * if a non-finite value ever appears, silence every voice at once. */
  int bad = 0;
  for (int s = 0; s < SIDEBAND_BLOCK; s++) if (!is_finite(g_mix[s])) bad = 1;
  if (bad) {
    hard_reset();
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
float sideband_comp_gain(void) { return g_comp_gain; }

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
  /* With nothing sounding there is nothing to crossfade. */
  if (id == SIDEBAND_P_ALGO && g_xf >= 1.0f && !any_voice_active()) g_algo = g_algo_from = v;
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
  if (sample_rate < SIDEBAND_MIN_RATE) sample_rate = SIDEBAND_MIN_RATE;
  if (sample_rate > SIDEBAND_MAX_RATE) sample_rate = SIDEBAND_MAX_RATE;
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
  g_algo = g_algo_from = g_param[SIDEBAND_P_ALGO];
  g_xf = 1.0f;
  g_xf_step = 1.0f / (ALGO_XFADE_S * g_sr);

  g_level_coef = smooth_coef_d(LEVEL_SMOOTH_S);
  g_pitch_coef = smooth_coef_d(PITCH_SMOOTH_S);
  g_volume = 0.0f; /* fades in from silence */
  g_volume_target = SIDEBAND_DEFAULT_VOLUME;
  g_volume_coef = smooth_coef(VOLUME_SMOOTH_S);
  g_dc_r = 1.0f - (2.0f * (float)DSP_PI * 4.0f / g_sr); /* ~4 Hz corner */
  g_dc_x1 = g_dc_y1 = 0.0f;
  g_sc_coef = 2.0f * (float)DSP_PI * COMP_SIDECHAIN_HZ / g_sr;
  g_det_coef = smooth_coef(COMP_DETECT_S);
  g_comp_att = smooth_coef(COMP_ATTACK_S);
  g_comp_rel = smooth_coef(COMP_RELEASE_S);
  g_sc_lp = g_det = g_comp_gr = 0.0f;
  g_comp_gain = 1.0f;
  /* Envelope falls by 1/e (8.7 dB) every LIMIT_RELEASE_S seconds. */
  g_lim_rel = exp2_approx(-1.442695f / (LIMIT_RELEASE_S * g_sr));
  g_lim_env = 0.0f;
  g_lim_gain = 1.0f;
  g_peak = 0.0f;
  g_end_step = 1.0f / (END_FADE_S * g_sr);
  g_panic_step = 1.0f / (PANIC_FADE_S * g_sr);
  g_rise_step = 1.0f / (RISE_S * g_sr);
  for (int s = 0; s < SIDEBAND_BLOCK; s++) g_out[s] = g_mon[s] = g_mix[s] = 0.0f;
}
