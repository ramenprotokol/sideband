/*
 * Unit tests for the op-six engine, compiled natively with `zig cc` and run by
 * scripts/test-c.mjs. The engine is included directly (a "unity build") so the
 * tests can reach its static helpers. The engine itself never uses libc or
 * libm; only this test file does, as an independent reference.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../src/opsix.c"

static int g_checks, g_failed, g_test_failed;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; g_test_failed = 1; \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

#define CHECKF(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; g_test_failed = 1; \
      printf("    FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

#define RUN(fn) do { g_test_failed = 0; fn(); \
    printf("%s %s\n", g_test_failed ? "FAIL" : "ok  ", #fn); } while (0)

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */

#define SR 48000
#define MAXN (SR * 4)
static float buf_a[MAXN], buf_b[MAXN], buf_m[MAXN];

static int pid(int op, int off) { return OPSIX_GLOBALS + op * OPSIX_OP_PARAMS + off; }
static void setp(int op, int off, int v) { opsix_set_param(pid(op, off), (float)v); }

/* An operator with instant attack, full sustain and a quick release. */
static void op_organ(int op, int level, int coarse) {
  setp(op, OPSIX_OP_MODE, 0);
  setp(op, OPSIX_OP_COARSE, coarse);
  setp(op, OPSIX_OP_FINE, 0);
  setp(op, OPSIX_OP_DETUNE, 0);
  setp(op, OPSIX_OP_LEVEL, level);
  setp(op, OPSIX_OP_R1, 99); setp(op, OPSIX_OP_R2, 99);
  setp(op, OPSIX_OP_R3, 99); setp(op, OPSIX_OP_R4, 80);
  setp(op, OPSIX_OP_L1, 99); setp(op, OPSIX_OP_L2, 99);
  setp(op, OPSIX_OP_L3, 99); setp(op, OPSIX_OP_L4, 0);
}

static void fresh(int algo, int feedback) {
  opsix_init((float)SR);
  opsix_set_param(OPSIX_P_ALGO, (float)algo);
  opsix_set_param(OPSIX_P_FEEDBACK, (float)feedback);
  for (int k = 0; k < OPSIX_OPS; k++) op_organ(k, 0, 1);
}

/* Render n samples (a multiple of the block) of output and monitor. */
static void render(float *out, float *mon, int n) {
  for (int i = 0; i < n; i += OPSIX_BLOCK) {
    float *o = opsix_render();
    for (int s = 0; s < OPSIX_BLOCK && i + s < n; s++) {
      if (out) out[i + s] = o[s];
      if (mon) mon[i + s] = g_mon[s];
    }
  }
}

static float peak(const float *x, int n) {
  float p = 0.0f;
  for (int i = 0; i < n; i++) if (fabsf(x[i]) > p) p = fabsf(x[i]);
  return p;
}

static double rms(const float *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
  return sqrt(s / n);
}

/* Frequency from interpolated rising zero crossings. */
static double measure_hz(const float *x, int n) {
  double first = -1.0, last = -1.0;
  int count = 0;
  for (int i = 1; i < n; i++) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      double t = (i - 1) + (double)(-x[i - 1]) / (double)(x[i] - x[i - 1]);
      if (first < 0.0) first = t;
      last = t;
      count++;
    }
  }
  if (count < 2) return 0.0;
  return (count - 1) * (double)SR / (last - first);
}

/* Fraction of the signal's power at exactly `hz` (the window must hold a
 * whole number of cycles, e.g. 440 Hz over 4800 samples at 48 kHz). */
static double power_fraction(const float *x, int n, double hz) {
  double re = 0.0, im = 0.0, total = 0.0;
  for (int i = 0; i < n; i++) {
    double w = 2.0 * M_PI * hz * i / SR;
    re += x[i] * cos(w);
    im -= x[i] * sin(w);
    total += (double)x[i] * x[i];
  }
  double amp = 2.0 * sqrt(re * re + im * im) / n;
  return (amp * amp / 2.0) / (total / n);
}

static uint32_t fnv1a(const float *x, int n) {
  const unsigned char *p = (const unsigned char *)x;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < (size_t)n * sizeof(float); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void) {
  uint32_t x = rng_state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return rng_state = x;
}
static int rng_int(int lo, int hi) { return lo + (int)(rng() % (uint32_t)(hi - lo + 1)); }

/* ---------------------------------------------------------------------- */
/* Maths                                                                   */

static void test_sine_table_accuracy(void) {
  opsix_init((float)SR);
  double worst = 0.0;
  for (uint32_t i = 0; i < 200000; i++) {
    uint32_t phase = i * 21474u + (i * 2654435761u >> 7);
    double ref = sin(2.0 * M_PI * (double)phase / 4294967296.0);
    double err = fabs((double)sine_lookup(phase) - ref);
    if (err > worst) worst = err;
  }
  CHECKF(worst < 1e-6, "worst sine error %g", worst);
  CHECK(sine_lookup(0) == 0.0f);
  CHECK(sine_lookup(1u << 30) == 1.0f);
  CHECK(sine_lookup(3u << 30) == -1.0f);
}

static void test_exp2_accuracy(void) {
  double worst = 0.0;
  for (int i = 0; i <= 30000; i++) {
    float x = -20.0f + 30.0f * (float)i / 30000.0f;
    double ref = exp2((double)x);
    double rel = fabs((double)exp2_approx(x) - ref) / ref;
    if (rel > worst) worst = rel;
  }
  CHECKF(worst < 5e-7, "worst exp2 relative error %g", worst);
  CHECK(exp2_approx(0.0f) == 1.0f);
  CHECK(exp2_approx(-1.0f) == 0.5f);
  CHECK(exp2_approx(NAN) == 0.0f);
  CHECK(exp2_approx(-1000.0f) == 0.0f);
  CHECK(is_finite(exp2_approx(1000.0f)));
}

static void test_turns_to_phase(void) {
  CHECK(turns_to_phase(0.25f) == (1u << 30));
  CHECK(turns_to_phase(1.25f) == (1u << 30));        /* whole turns wrap away */
  CHECK(turns_to_phase(-0.25f) == (3u << 30));
  CHECK(turns_to_phase(NAN) == 0u);
  CHECK(turns_to_phase(INFINITY) == 0u);
  CHECK(turns_to_phase(1e30f) == 0u);
}

static void test_level_to_amplitude(void) {
  CHECK(level_amp(99.0f) == 1.0f);
  CHECK(level_amp(91.0f) == 0.5f);                   /* 8 steps = 6.02 dB */
  CHECK(level_amp(83.0f) == 0.25f);
  CHECK(level_amp(0.0f) == 0.0f);
  float prev = 0.0f;
  int monotonic = 1;
  for (int l = 1; l <= 99; l++) {
    float a = level_amp((float)l);
    if (!(a > prev)) monotonic = 0;
    prev = a;
  }
  CHECK(monotonic);
  double db_step = 20.0 * log10((double)level_amp(60.0f) / (double)level_amp(59.0f));
  CHECKF(fabs(db_step - 0.7526) < 1e-3, "dB per level step %g", db_step);
}

/* ---------------------------------------------------------------------- */
/* Operators                                                               */

static void test_operator_frequency_maths(void) {
  opsix_init((float)SR);
  CHECK(fabsf(note_hz(69) - 440.0f) < 1e-3f);
  CHECK(fabsf(note_hz(81) - 880.0f) < 1e-3f);
  CHECKF(fabs(note_hz(60) - 261.6256) < 1e-3, "C4 %f", (double)note_hz(60));

  setp(0, OPSIX_OP_COARSE, 0);
  CHECK(fabsf(op_hz(0, 440.0f) - 220.0f) < 1e-3f);   /* coarse 0 = x0.5 */
  setp(0, OPSIX_OP_COARSE, 3); setp(0, OPSIX_OP_FINE, 50);
  CHECK(fabsf(op_hz(0, 440.0f) - 1980.0f) < 1e-2f);  /* 3 x 1.5 */
  setp(0, OPSIX_OP_FINE, 0); setp(0, OPSIX_OP_COARSE, 1); setp(0, OPSIX_OP_DETUNE, 12);
  CHECK(fabs(op_hz(0, 440.0f) - 440.0 * pow(2.0, 12.0 / 1200.0)) < 1e-3);
  setp(0, OPSIX_OP_DETUNE, 0);

  setp(0, OPSIX_OP_MODE, 1);
  setp(0, OPSIX_OP_COARSE, 2); setp(0, OPSIX_OP_FINE, 0);
  CHECKF(fabs(op_hz(0, 440.0f) - 100.0) < 1e-3, "fixed 100 Hz -> %f", (double)op_hz(0, 440.0f));
  CHECK(fabs(op_hz(0, 30.0f) - 100.0) < 1e-3);       /* fixed ignores the key */
  setp(0, OPSIX_OP_COARSE, 3); setp(0, OPSIX_OP_FINE, 99);
  CHECK(fabs(op_hz(0, 440.0f) - pow(10.0, 3.99)) < 0.05);
  setp(0, OPSIX_OP_COARSE, 7);                        /* decade is coarse mod 4 */
  CHECK(fabs(op_hz(0, 440.0f) - pow(10.0, 3.99)) < 0.05);

  /* Increments never reach Nyquist, whatever the request. */
  CHECK(hz_to_inc(1e9f) == hz_to_inc(0.49f * SR));
  CHECK(hz_to_inc(-5.0f) == 0u);
  CHECK(hz_to_inc(NAN) == 0u);
}

static void test_measured_pitch(void) {
  static const struct { int coarse, fine, detune, mode; double hz; } cases[] = {
    {1, 0, 0, 0, 440.0},
    {2, 0, 0, 0, 880.0},
    {0, 0, 0, 0, 220.0},
    {1, 50, 0, 0, 660.0},
    {1, 0, 20, 0, 0.0}, /* 440 x 2^(20/1200), computed below */
    {2, 0, 0, 1, 100.0},
  };
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    fresh(7, 0);
    op_organ(0, 99, cases[c].coarse);
    setp(0, OPSIX_OP_FINE, cases[c].fine);
    setp(0, OPSIX_OP_DETUNE, cases[c].detune);
    setp(0, OPSIX_OP_MODE, cases[c].mode);
    opsix_note_on(69, 100);
    render(NULL, buf_m, SR);
    double hz = measure_hz(buf_m + SR / 10, SR - SR / 10);
    double want = cases[c].hz > 0.0 ? cases[c].hz : 440.0 * pow(2.0, cases[c].detune / 1200.0);
    CHECKF(fabs(hz - want) < 0.02, "case %zu: measured %.4f Hz, want %.4f", c, hz, want);
  }
}

/* ---------------------------------------------------------------------- */
/* Envelopes                                                               */

static double sweep_seconds(int rate) { return 40.0 * pow(0.0015 / 40.0, rate / 99.0); }

static int ticks_until_stage_changes(OpState *o, int op, int limit) {
  int start = o->stage, n = 0;
  while (o->stage == start && n < limit) { env_tick(o, op); n++; }
  return n;
}

static void test_envelope_stage_times(void) {
  opsix_init((float)SR);
  setp(0, OPSIX_OP_L4, 0);
  setp(0, OPSIX_OP_L1, 99); setp(0, OPSIX_OP_R1, 50);
  setp(0, OPSIX_OP_L2, 49); setp(0, OPSIX_OP_R2, 30);
  setp(0, OPSIX_OP_L3, 49); setp(0, OPSIX_OP_R3, 10);
  setp(0, OPSIX_OP_R4, 70);
  OpState o = {0u, 0.0, 0, 1.0f};

  /* Rising segments close a fixed share of the gap to RISE_TOP each sample:
   * gap_n = gap_0 (1 - a)^n, so the attack takes ln(gap_0/gap_end)/-ln(1-a). */
  int attack = ticks_until_stage_changes(&o, 0, SR * 60);
  double a = (99.0 / (sweep_seconds(50) * SR)) * log(119.0 / 20.0) / 99.0;
  double want = log(119.0 / 20.0) / -log(1.0 - a);
  CHECKF(fabs(attack - want) <= 2.0 + want * 1e-5, "attack %d ticks, want %.1f", attack, want);
  CHECKF(fabs(want / SR - sweep_seconds(50)) < 1e-3 * sweep_seconds(50), "full rise %.5f s, rate time %.5f s",
         want / SR, sweep_seconds(50));
  CHECK(o.env == 99.0 && o.stage == 1);

  int decay = ticks_until_stage_changes(&o, 0, SR * 60);
  want = sweep_seconds(30) * SR * 50.0 / 99.0;
  CHECKF(fabs(decay - want) <= 2.0 + want * 1e-5, "decay %d ticks, want %.1f", decay, want);
  CHECK(o.env == 49.0 && o.stage == 2);

  /* Sustain holds exactly at L3 for as long as the key is down. */
  for (int i = 0; i < SR * 3; i++) env_tick(&o, 0);
  CHECK(o.env == 49.0 && o.stage == 2);

  o.stage = 3; /* key up */
  int release = ticks_until_stage_changes(&o, 0, SR * 60);
  want = sweep_seconds(70) * SR * 49.0 / 99.0;
  CHECKF(fabs(release - want) <= 2.0 + want * 1e-5, "release %d ticks, want %.1f", release, want);
  CHECK(o.env == 0.0 && o.stage == STAGE_DONE);
}

static void test_envelope_rate_extremes(void) {
  opsix_init((float)SR);
  double fastest = 99.0 / g_rate_step[99] / SR, slowest = 99.0 / g_rate_step[0] / SR;
  /* The slowest rate keeps its timing at the highest sample rate too. */
  opsix_init(192000.0f);
  setp(0, OPSIX_OP_L4, 0); setp(0, OPSIX_OP_L1, 99); setp(0, OPSIX_OP_R1, 0);
  OpState slow = {0u, 0.0, 0, 1.0f};
  int slow_ticks = ticks_until_stage_changes(&slow, 0, 192000 * 60);
  CHECKF(fabs(slow_ticks / 192000.0 - 40.0) < 0.02, "rate 0 at 192 kHz took %g s", slow_ticks / 192000.0);
  opsix_init((float)SR);
  CHECKF(fabs(fastest - 0.0015) < 1e-6, "rate 99 sweep %g s", fastest);
  CHECKF(fabs(slowest - 40.0) < 1e-3, "rate 0 sweep %g s", slowest);
  int increasing = 1;
  for (int r = 1; r < 100; r++) if (!(g_rate_step[r] > g_rate_step[r - 1])) increasing = 0;
  CHECK(increasing);
}

/* The attack lands gently: its last step is much smaller than its first. */
static void test_attack_eases_in(void) {
  opsix_init((float)SR);
  setp(0, OPSIX_OP_L4, 0); setp(0, OPSIX_OP_L1, 99); setp(0, OPSIX_OP_R1, 40);
  OpState o = {0u, 0.0, 0, 1.0f};
  double first = 0.0, last = 0.0, prev = 0.0;
  while (o.stage == 0) {
    env_tick(&o, 0);
    double d = o.env - prev;
    if (first == 0.0) first = d;
    if (o.stage == 0) last = d;
    prev = o.env;
  }
  CHECKF(first / last > 5.0 && first / last < 6.5, "first/last step %g", first / last);
}

static void test_envelope_stays_in_range(void) {
  rng_state = 99u;
  int ok = 1;
  for (int trial = 0; trial < 400; trial++) {
    opsix_init((float)SR);
    for (int off = OPSIX_OP_R1; off <= OPSIX_OP_L4; off++) setp(0, off, rng_int(0, 99));
    OpState o = {0u, (double)OPP(0, OPSIX_OP_L4), 0, 1.0f};
    for (int i = 0; i < 20000; i++) {
      if (i == 12000) o.stage = 3;
      if (i % 3000 == 0) setp(0, OPSIX_OP_L1 + rng_int(0, 3), rng_int(0, 99)); /* live edits */
      env_tick(&o, 0);
      if (!(o.env >= 0.0 && o.env <= 99.0) || o.stage < 0 || o.stage > STAGE_DONE) ok = 0;
    }
  }
  CHECK(ok);
}

/* ---------------------------------------------------------------------- */
/* Algorithms                                                              */

/* The intended graphs, written out independently of the engine's table. */
static const char *EXPECTED_EDGES[OPSIX_ALGOS] = {
  "2>1 4>3 6>5",
  "3>2 2>1 6>5 5>4",
  "2>1 6>5 5>4 4>3",
  "6>5 5>4 4>3 3>2 2>1",
  "2>1 6>3 6>4 6>5",
  "2>1 3>1 5>1 4>3 6>5",
  "6>5 5>4",
  "",
};
static const char *EXPECTED_CARRIERS[OPSIX_ALGOS] = {
  "135", "14", "13", "1", "1345", "1", "1234", "123456",
};

static int expected_edge(int algo, int from, int to) { /* 1-based operator numbers */
  const char *s = EXPECTED_EDGES[algo];
  for (; *s; s++)
    if (s[0] == '0' + from && s[1] == '>' && s[2] == '0' + to) return 1;
  return 0;
}

static int expected_carrier(int algo, int op) { return strchr(EXPECTED_CARRIERS[algo], '0' + op) != NULL; }

static int expected_path(int algo, int from, int to) {
  if (from == to) return 1;
  for (int mid = 1; mid <= OPSIX_OPS; mid++)
    if (expected_edge(algo, from, mid) && expected_path(algo, mid, to)) return 1;
  return 0;
}

static void test_algorithm_tables(void) {
  for (int a = 0; a < OPSIX_ALGOS; a++) {
    int carriers = opsix_algo_carrier_mask(a);
    CHECK(carriers != 0);
    CHECK(opsix_algo_feedback_op(a) == 5);
    for (int t = 0; t < OPSIX_OPS; t++) {
      int mask = opsix_algo_mod_mask(a, t);
      CHECKF(((carriers >> t) & 1) == expected_carrier(a, t + 1), "algo %d op %d carrier", a + 1, t + 1);
      for (int f = 0; f < OPSIX_OPS; f++) {
        int edge = (mask >> f) & 1;
        CHECKF(edge == expected_edge(a, f + 1, t + 1), "algo %d edge %d>%d", a + 1, f + 1, t + 1);
        if (edge) CHECKF(f > t, "algo %d: OP%d feeds OP%d, breaking the OP6..OP1 order", a + 1, f + 1, t + 1);
      }
      /* Every operator is heard: it is a carrier or feeds another operator. */
      int feeds = 0;
      for (int u = 0; u < OPSIX_OPS; u++) feeds |= (opsix_algo_mod_mask(a, u) >> t) & 1;
      CHECK(((carriers >> t) & 1) || feeds);
      /* A carrier is never also a modulator in this set of eight. */
      CHECK(!(((carriers >> t) & 1) && feeds));
    }
  }
  CHECK(opsix_algo_carrier_mask(-1) == 0 && opsix_algo_carrier_mask(8) == 0);
  CHECK(opsix_algo_mod_mask(0, 6) == 0 && opsix_algo_feedback_op(99) == -1);
}

/* Only carriers make sound on their own. */
static void test_only_carriers_are_audible(void) {
  for (int a = 0; a < OPSIX_ALGOS; a++) {
    for (int k = 0; k < OPSIX_OPS; k++) {
      fresh(a, 0);
      op_organ(k, 99, 1);
      opsix_note_on(69, 100);
      render(NULL, buf_m, SR / 5);
      double r = rms(buf_m + SR / 10, SR / 10);
      int carrier = expected_carrier(a, k + 1);
      CHECKF(carrier ? r > 0.02 : r == 0.0, "algo %d op %d rms %g", a + 1, k + 1, r);
    }
  }
}

/* An operator changes a carrier's sound exactly when the intended graph has
 * a path from it to that carrier. */
static void test_modulation_follows_the_graph(void) {
  static float solo[SR / 5];
  for (int a = 0; a < OPSIX_ALGOS; a++) {
    for (int c = 1; c <= OPSIX_OPS; c++) {
      if (!expected_carrier(a, c)) continue;
      for (int m = 1; m <= OPSIX_OPS; m++) {
        for (int pass = 0; pass < 2; pass++) {
          fresh(a, 0);
          for (int k = 1; k <= OPSIX_OPS; k++) {
            int level = expected_carrier(a, k) ? (k == c ? 99 : 0) : 80;
            if (pass == 1 && k == m) level = 0;
            op_organ(k - 1, level, k); /* distinct ratios */
          }
          opsix_note_on(57, 100);
          render(NULL, pass == 0 ? solo : buf_m, SR / 5);
        }
        float diff = 0.0f;
        for (int i = 0; i < SR / 5; i++) if (fabsf(solo[i] - buf_m[i]) > diff) diff = fabsf(solo[i] - buf_m[i]);
        int want = expected_path(a, m, c);
        CHECKF(want ? diff > 1e-3f : diff == 0.0f, "algo %d: OP%d -> carrier OP%d diff %g", a + 1, m, c, (double)diff);
      }
    }
  }
}

static void test_modulation_adds_harmonics(void) {
  /* Algorithm 1: OP2 modulates OP1. OP1 alone is a pure sine; adding OP2
   * moves power out of the fundamental into sidebands. */
  fresh(0, 0);
  op_organ(0, 99, 1);
  opsix_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double pure = power_fraction(buf_m + SR / 4, 4800, 440.0);
  CHECKF(pure > 0.999, "carrier alone: %.5f of power at 440 Hz", pure);

  fresh(0, 0);
  op_organ(0, 99, 1);
  op_organ(1, 85, 1);
  opsix_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double modulated = power_fraction(buf_m + SR / 4, 4800, 440.0);
  double second = power_fraction(buf_m + SR / 4, 4800, 880.0);
  CHECKF(modulated < 0.7 && second > 0.05, "modulated: fundamental %.3f, 2nd %.3f", modulated, second);
}

static void test_feedback(void) {
  /* Algorithm 8, OP6 alone: feedback 0 is a sine, feedback 7 is bright. */
  fresh(7, 0);
  op_organ(5, 99, 1);
  opsix_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double clean = power_fraction(buf_m + SR / 4, 4800, 440.0);
  fresh(7, 7);
  op_organ(5, 99, 1);
  opsix_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double bright = power_fraction(buf_m + SR / 4, 4800, 440.0);
  CHECKF(clean > 0.999, "fb 0: %.5f", clean);
  CHECKF(bright < 0.9, "fb 7: %.5f", bright);
  double third = power_fraction(buf_m + SR / 4, 4800, 1320.0);
  CHECKF(third > 0.01, "fb 7 3rd harmonic %.4f", third);
  CHECK(fabs(feedback_turns() * 2.0 * M_PI - 1.6) < 1e-5);
}

/* ---------------------------------------------------------------------- */
/* Safety                                                                  */

static void test_master_chain_hard_limits(void) {
  opsix_init((float)SR);
  opsix_set_volume(1.0f);
  const float nasty[] = {1e6f, -1e6f, 3e38f, -3e38f, INFINITY, -INFINITY, NAN, 1.0f, -1.0f, 50.0f};
  float worst = 0.0f, worst_mon = 0.0f;
  int finite = 1;
  for (int i = 0; i < SR; i++) {
    float mon;
    float x = nasty[i % 10] * (i % 7 == 0 ? 1.0f : (float)((i % 13) - 6));
    float y = master_sample(x, &mon);
    if (!is_finite(y) || !is_finite(mon)) finite = 0;
    if (fabsf(y) > worst) worst = fabsf(y);
    if (fabsf(mon) > worst_mon) worst_mon = fabsf(mon);
  }
  CHECK(finite);
  CHECKF(worst <= OPSIX_CEILING, "output peak %g", (double)worst);
  CHECKF(worst < 1.0f, "output peak %g", (double)worst);
  CHECKF(worst_mon <= OPSIX_LIMIT_THRESHOLD * 1.000001f, "monitor peak %g", (double)worst_mon);
}

static void set_extreme_patch(int algo) {
  fresh(algo, 7);
  for (int k = 0; k < OPSIX_OPS; k++) {
    op_organ(k, 99, 31);
    setp(k, OPSIX_OP_FINE, 99);
    if (expected_carrier(algo, k + 1)) setp(k, OPSIX_OP_COARSE, 1);
  }
}

static void test_limiter_under_extreme_patches(void) {
  for (int a = 0; a < OPSIX_ALGOS; a++) {
    set_extreme_patch(a);
    opsix_set_volume(1.0f);
    for (int n = 0; n < 12; n++) opsix_note_on(36 + n * 3, 127);  /* more notes than voices */
    render(buf_a, buf_m, SR);
    float p = peak(buf_a, SR), pm = peak(buf_m, SR);
    CHECKF(p <= OPSIX_CEILING && p < 1.0f, "algo %d: output peak %g", a + 1, (double)p);
    CHECKF(pm <= OPSIX_LIMIT_THRESHOLD * 1.000001f, "algo %d: monitor peak %g", a + 1, (double)pm);
    CHECKF(p > 0.3f, "algo %d: extreme patch should be loud, got %g", a + 1, (double)p);
    CHECK(opsix_active_voices() == OPSIX_VOICES);
  }
  /* The limiter was actually working, not just the final clamp. */
  set_extreme_patch(7);
  opsix_set_volume(1.0f);
  for (int n = 0; n < 8; n++) opsix_note_on(48 + n, 127);
  render(NULL, NULL, SR / 4);
  CHECKF(opsix_gain_reduction() < 0.9f, "gain %g", (double)opsix_gain_reduction());
}

static void test_volume(void) {
  opsix_init((float)SR);
  CHECK(g_volume_target == OPSIX_DEFAULT_VOLUME);
  CHECK(OPSIX_DEFAULT_VOLUME <= 0.2512f);           /* -12 dB or quieter */
  CHECK(opsix_set_volume(2.0f) == 1.0f);
  CHECK(opsix_set_volume(-1.0f) == 0.0f);
  CHECK(opsix_set_volume(NAN) == 0.0f);
  CHECK(opsix_set_volume(INFINITY) == 0.0f);
  CHECK(opsix_set_volume(0.5f) == 0.5f);

  /* Starts from silence and glides: no jump on the first sample. */
  fresh(7, 0);
  op_organ(0, 99, 1);
  opsix_note_on(69, 127);
  render(buf_a, buf_m, SR / 2);
  CHECKF(fabsf(buf_a[1]) < 0.01f, "first samples %g", (double)buf_a[1]);
  CHECKF(fabsf(peak(buf_a + SR / 4, SR / 4) - 0.25f * peak(buf_m + SR / 4, SR / 4)) < 1e-3f,
         "steady state is monitor x 0.25");

  /* Volume 0 is silence. */
  opsix_set_volume(0.0f);
  render(buf_a, NULL, SR / 2);
  CHECK(peak(buf_a + SR / 4, SR / 4) < 1e-6f);
}

static void test_silence_without_notes(void) {
  opsix_init((float)SR);
  render(buf_a, buf_m, SR / 4);
  CHECK(peak(buf_a, SR / 4) == 0.0f && peak(buf_m, SR / 4) == 0.0f);
}

/* ---------------------------------------------------------------------- */
/* Parameters, voices, determinism                                         */

static void test_parameter_clamping(void) {
  opsix_init((float)SR);
  int lvl = pid(2, OPSIX_OP_LEVEL), det = pid(4, OPSIX_OP_DETUNE);
  CHECK(opsix_set_param(OPSIX_P_ALGO, 1e9f) == 7.0f);
  CHECK(opsix_set_param(OPSIX_P_ALGO, -1e9f) == 0.0f);
  CHECK(opsix_set_param(OPSIX_P_FEEDBACK, 99.0f) == 7.0f);
  CHECK(opsix_set_param(lvl, 3.6f) == 4.0f);
  CHECK(opsix_set_param(lvl, NAN) == 4.0f);          /* NaN is ignored */
  CHECK(opsix_set_param(lvl, INFINITY) == 4.0f);
  CHECK(opsix_set_param(lvl, 250.0f) == 99.0f);
  CHECK(opsix_set_param(det, -30.0f) == -20.0f);
  CHECK(opsix_set_param(det, -2.5f) == -3.0f);       /* rounds half away from zero */
  CHECK(opsix_set_param(-1, 5.0f) == 0.0f);
  CHECK(opsix_set_param(OPSIX_PARAMS, 5.0f) == 0.0f);
  CHECK(opsix_get_param(OPSIX_PARAMS) == 0.0f);
  CHECK(opsix_param_min(det) == -20.0f && opsix_param_max(det) == 20.0f);
  CHECK(opsix_param_default(pid(0, OPSIX_OP_LEVEL)) == 99.0f);
  CHECK(opsix_param_default(pid(1, OPSIX_OP_LEVEL)) == 0.0f);
  int all_in_range = 1;
  for (int id = 0; id < OPSIX_PARAMS; id++) {
    float mn = opsix_param_min(id), mx = opsix_param_max(id), df = opsix_param_default(id);
    if (!(mn <= df && df <= mx)) all_in_range = 0;
  }
  CHECK(all_in_range);
}

static void test_voice_allocation(void) {
  fresh(7, 0);
  op_organ(0, 99, 1);
  for (int n = 0; n < 10; n++) opsix_note_on(60 + n, 100);
  CHECK(opsix_active_voices() == OPSIX_VOICES);
  opsix_note_on(69, 100);                              /* already sounding: reuse */
  CHECK(opsix_active_voices() == OPSIX_VOICES);
  opsix_all_notes_off();
  render(NULL, NULL, SR);
  CHECK(opsix_active_voices() == 0);

  opsix_note_on(300, 100);
  CHECK(g_voice[0].note == 127 || opsix_active_voices() == 1);
  int found = 0;
  for (int i = 0; i < OPSIX_VOICES; i++) if (g_voice[i].active && g_voice[i].note == 127) found = 1;
  CHECK(found);
  opsix_note_on(127, 0);                               /* velocity 0 = note off */
  render(NULL, NULL, SR);
  CHECK(opsix_active_voices() == 0);
}

static void test_released_drone_still_ends(void) {
  fresh(7, 0);
  for (int k = 0; k < OPSIX_OPS; k++) { op_organ(k, 99, k + 1); setp(k, OPSIX_OP_L4, 99); }
  opsix_note_on(60, 100);
  render(NULL, NULL, SR / 10);
  opsix_note_off(60);
  render(NULL, NULL, SR / 2);
  CHECK(opsix_active_voices() == 0);
}

static void play_sequence(float *out, int n) {
  opsix_note_on(60, 100);
  render(out, NULL, n / 4);
  opsix_note_on(64, 90);
  opsix_note_on(67, 80);
  render(out + n / 4, NULL, n / 4);
  opsix_note_off(60);
  render(out + n / 2, NULL, n / 4);
  opsix_all_notes_off();
  render(out + 3 * n / 4, NULL, n / 4);
}

static void ep_like_patch(void) {
  fresh(0, 5);
  op_organ(0, 99, 1); setp(0, OPSIX_OP_R2, 30); setp(0, OPSIX_OP_L3, 0); setp(0, OPSIX_OP_L2, 80);
  op_organ(1, 70, 14); setp(1, OPSIX_OP_R2, 60); setp(1, OPSIX_OP_L2, 0); setp(1, OPSIX_OP_L3, 0);
  op_organ(2, 95, 1); setp(2, OPSIX_OP_DETUNE, 5);
  op_organ(3, 75, 1);
  op_organ(4, 90, 1); setp(4, OPSIX_OP_DETUNE, -5);
  op_organ(5, 70, 1);
}

static uint32_t g_golden_hash;
#define GOLDEN_HASH 0x4917c7b1u

static void test_determinism(void) {
  const int n = SR; /* 1 s */
  ep_like_patch();
  play_sequence(buf_a, n);
  ep_like_patch();
  play_sequence(buf_b, n);
  CHECK(memcmp(buf_a, buf_b, sizeof(float) * n) == 0);
  CHECK(peak(buf_a, n) > 0.01f);
  g_golden_hash = fnv1a(buf_a, n);
  /* The same scenario rendered by the WebAssembly build in Node
   * (tests/render.test.mjs) must hash to this same value: native and WASM
   * are bit-identical. If an intended engine change moves it, update both. */
  CHECKF(g_golden_hash == GOLDEN_HASH, "hash 0x%08x, golden 0x%08x", g_golden_hash, GOLDEN_HASH);

  /* A different patch gives different samples. */
  ep_like_patch();
  setp(1, OPSIX_OP_COARSE, 13);
  play_sequence(buf_b, n);
  CHECK(memcmp(buf_a, buf_b, sizeof(float) * n) != 0);

  /* Determinism holds at other sample rates too. */
  for (int pass = 0; pass < 2; pass++) {
    opsix_init(44100.0f);
    op_organ(0, 99, 1); op_organ(1, 80, 3);
    play_sequence(pass == 0 ? buf_a : buf_b, n);
  }
  CHECK(memcmp(buf_a, buf_b, sizeof(float) * n) == 0);
}

static void test_sample_rate_clamping(void) {
  opsix_init(0.0f);      CHECK(opsix_sample_rate() == 8000.0f);
  opsix_init(NAN);       CHECK(opsix_sample_rate() == 48000.0f);
  opsix_init(1e9f);      CHECK(opsix_sample_rate() == 192000.0f);
  opsix_init(-INFINITY); CHECK(opsix_sample_rate() == 48000.0f);
  opsix_init(44100.0f);  CHECK(opsix_sample_rate() == 44100.0f);
}

/* Random patches, including out-of-range and non-finite values, random
 * notes, velocities, volumes and sample rates. Every sample must be finite
 * and inside the ceiling. */
static void test_fuzz_no_nan_no_overs(void) {
  static const float rates[] = {8000.0f, 22050.0f, 44100.0f, 48000.0f, 96000.0f, 192000.0f};
  static const float specials[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 3.4e38f, -0.0f};
  rng_state = 20260926u;
  int finite = 1, within = 1, env_ok = 1;
  float worst = 0.0f;
  const int iterations = 600;
  for (int it = 0; it < iterations; it++) {
    opsix_init(rates[rng_int(0, 5)]);
    for (int id = 0; id < OPSIX_PARAMS; id++) {
      int kind = rng_int(0, 9);
      float mn = opsix_param_min(id), mx = opsix_param_max(id);
      float v;
      if (kind == 0) v = specials[rng_int(0, 6)];
      else if (kind == 1) v = mn;
      else if (kind == 2) v = mx;
      else if (kind == 3) v = (float)rng_int(-1000, 1000);
      else v = mn + (mx - mn) * (float)(rng() % 10001) / 10000.0f;
      opsix_set_param(id, v);
    }
    int vk = rng_int(0, 3);
    opsix_set_volume(vk == 0 ? 1.0f : vk == 1 ? specials[rng_int(0, 6)] : (float)(rng() % 1001) / 1000.0f);
    for (int blk = 0; blk < 24; blk++) {
      if (blk % 4 == 0) {
        int notes = rng_int(1, 10);
        for (int i = 0; i < notes; i++) opsix_note_on(rng_int(-10, 140), rng_int(-5, 200));
      }
      if (blk % 6 == 5) opsix_note_off(rng_int(0, 127));
      if (blk == 12) opsix_set_param(rng_int(0, OPSIX_PARAMS - 1), (float)rng_int(-50, 150));
      float *o = opsix_render();
      for (int s = 0; s < OPSIX_BLOCK; s++) {
        if (!is_finite(o[s]) || !is_finite(g_mon[s])) finite = 0;
        if (fabsf(o[s]) > OPSIX_CEILING || fabsf(g_mon[s]) > OPSIX_LIMIT_THRESHOLD * 1.000001f) within = 0;
        if (fabsf(o[s]) > worst) worst = fabsf(o[s]);
      }
      for (int i = 0; i < OPSIX_VOICES; i++)
        for (int k = 0; k < OPSIX_OPS; k++)
          if (!(g_voice[i].op[k].env >= 0.0 && g_voice[i].op[k].env <= 99.0)) env_ok = 0;
    }
  }
  CHECK(finite);
  CHECKF(within, "worst output %g", (double)worst);
  CHECK(env_ok);
  printf("    fuzz: %d random patches, worst output peak %.4f (ceiling %.4f)\n",
         iterations, (double)worst, (double)OPSIX_CEILING);
}

int main(void) {
  RUN(test_sine_table_accuracy);
  RUN(test_exp2_accuracy);
  RUN(test_turns_to_phase);
  RUN(test_level_to_amplitude);
  RUN(test_operator_frequency_maths);
  RUN(test_measured_pitch);
  RUN(test_envelope_stage_times);
  RUN(test_envelope_rate_extremes);
  RUN(test_attack_eases_in);
  RUN(test_envelope_stays_in_range);
  RUN(test_algorithm_tables);
  RUN(test_only_carriers_are_audible);
  RUN(test_modulation_follows_the_graph);
  RUN(test_modulation_adds_harmonics);
  RUN(test_feedback);
  RUN(test_master_chain_hard_limits);
  RUN(test_limiter_under_extreme_patches);
  RUN(test_volume);
  RUN(test_silence_without_notes);
  RUN(test_parameter_clamping);
  RUN(test_voice_allocation);
  RUN(test_released_drone_still_ends);
  RUN(test_determinism);
  RUN(test_sample_rate_clamping);
  RUN(test_fuzz_no_nan_no_overs);
  printf("determinism hash (EP-like sequence, 48 kHz): 0x%08x\n", g_golden_hash);
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
