/*
 * Unit tests for the sideband engine, compiled natively with `zig cc` and run by
 * scripts/test-c.mjs. The engine is included directly (a "unity build") so the
 * tests can reach its static helpers. The engine itself never uses libc or
 * libm; only this test file does, as an independent reference.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../src/sideband.c"

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

static int pid(int op, int off) { return SIDEBAND_GLOBALS + op * SIDEBAND_OP_PARAMS + off; }
static void setp(int op, int off, int v) { sideband_set_param(pid(op, off), (float)v); }

/* An operator with instant attack, full sustain and a quick release. */
static void op_organ(int op, int level, int coarse) {
  setp(op, SIDEBAND_OP_MODE, 0);
  setp(op, SIDEBAND_OP_COARSE, coarse);
  setp(op, SIDEBAND_OP_FINE, 0);
  setp(op, SIDEBAND_OP_DETUNE, 0);
  setp(op, SIDEBAND_OP_LEVEL, level);
  setp(op, SIDEBAND_OP_R1, 99); setp(op, SIDEBAND_OP_R2, 99);
  setp(op, SIDEBAND_OP_R3, 99); setp(op, SIDEBAND_OP_R4, 80);
  setp(op, SIDEBAND_OP_L1, 99); setp(op, SIDEBAND_OP_L2, 99);
  setp(op, SIDEBAND_OP_L3, 99); setp(op, SIDEBAND_OP_L4, 0);
}

static void fresh(int algo, int feedback) {
  sideband_init((float)SR);
  sideband_set_param(SIDEBAND_P_ALGO, (float)algo);
  sideband_set_param(SIDEBAND_P_FEEDBACK, (float)feedback);
  for (int k = 0; k < SIDEBAND_OPS; k++) op_organ(k, 0, 1);
}

/* Render n samples (a multiple of the block) of output and monitor. */
static void render(float *out, float *mon, int n) {
  for (int i = 0; i < n; i += SIDEBAND_BLOCK) {
    float *o = sideband_render();
    for (int s = 0; s < SIDEBAND_BLOCK && i + s < n; s++) {
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
static double measure_hz_at(const float *x, int n, double rate) {
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
  return (count - 1) * rate / (last - first);
}

static double measure_hz(const float *x, int n) { return measure_hz_at(x, n, (double)SR); }

/* Largest sample-to-sample step in x[a..b). */
static float max_step(const float *x, int a, int b) {
  float m = 0.0f;
  for (int i = a > 0 ? a : 1; i < b; i++) if (fabsf(x[i] - x[i - 1]) > m) m = fabsf(x[i] - x[i - 1]);
  return m;
}

static Voice *voice_for(int note) {
  for (int i = 0; i < SIDEBAND_VOICES; i++) if (g_voice[i].active && g_voice[i].note == note) return &g_voice[i];
  return NULL;
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
  sideband_init((float)SR);
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

static void test_log2_accuracy(void) {
  double worst = 0.0;
  for (int i = 0; i <= 40000; i++) {
    float x = exp2f(-40.0f + 50.0f * (float)i / 40000.0f) * (1.0f + 0.37f * (float)(i % 7) / 7.0f);
    double err = fabs((double)log2_approx(x) - log2((double)x));
    if (err > worst) worst = err;
  }
  CHECKF(worst < 3e-6, "worst log2 error %g", worst);
  CHECK(log2_approx(1.0f) == 0.0f);
  CHECK(log2_approx(0.0f) == -126.0f && log2_approx(-1.0f) == -126.0f && log2_approx(NAN) == -126.0f);
  CHECK(log2_approx(INFINITY) == 128.0f);
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
  sideband_init((float)SR);
  CHECK(fabsf(note_hz(69) - 440.0f) < 1e-3f);
  CHECK(fabsf(note_hz(81) - 880.0f) < 1e-3f);
  CHECKF(fabs(note_hz(60) - 261.6256) < 1e-3, "C4 %f", (double)note_hz(60));

  setp(0, SIDEBAND_OP_COARSE, 0);
  CHECK(fabsf(op_hz(0, 440.0f) - 220.0f) < 1e-3f);   /* coarse 0 = x0.5 */
  setp(0, SIDEBAND_OP_COARSE, 3); setp(0, SIDEBAND_OP_FINE, 50);
  CHECK(fabsf(op_hz(0, 440.0f) - 1980.0f) < 1e-2f);  /* 3 x 1.5 */
  setp(0, SIDEBAND_OP_FINE, 0); setp(0, SIDEBAND_OP_COARSE, 1); setp(0, SIDEBAND_OP_DETUNE, 12);
  CHECK(fabs(op_hz(0, 440.0f) - 440.0 * pow(2.0, 12.0 / 1200.0)) < 1e-3);
  setp(0, SIDEBAND_OP_DETUNE, 0);

  setp(0, SIDEBAND_OP_MODE, 1);
  setp(0, SIDEBAND_OP_COARSE, 2); setp(0, SIDEBAND_OP_FINE, 0);
  CHECKF(fabs(op_hz(0, 440.0f) - 100.0) < 1e-3, "fixed 100 Hz -> %f", (double)op_hz(0, 440.0f));
  CHECK(fabs(op_hz(0, 30.0f) - 100.0) < 1e-3);       /* fixed ignores the key */
  setp(0, SIDEBAND_OP_COARSE, 3); setp(0, SIDEBAND_OP_FINE, 99);
  CHECK(fabs(op_hz(0, 440.0f) - pow(10.0, 3.99)) < 0.05);
  setp(0, SIDEBAND_OP_COARSE, 7);                        /* decade is coarse mod 4 */
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
    setp(0, SIDEBAND_OP_FINE, cases[c].fine);
    setp(0, SIDEBAND_OP_DETUNE, cases[c].detune);
    setp(0, SIDEBAND_OP_MODE, cases[c].mode);
    sideband_note_on(69, 100);
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
  sideband_init((float)SR);
  setp(0, SIDEBAND_OP_L4, 0);
  setp(0, SIDEBAND_OP_L1, 99); setp(0, SIDEBAND_OP_R1, 50);
  setp(0, SIDEBAND_OP_L2, 49); setp(0, SIDEBAND_OP_R2, 30);
  setp(0, SIDEBAND_OP_L3, 49); setp(0, SIDEBAND_OP_R3, 10);
  setp(0, SIDEBAND_OP_R4, 70);
  OpState o = {.env = 0.0, .stage = 0, .gain = 1.0f};

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
  sideband_init((float)SR);
  double fastest = 99.0 / g_rate_step[99] / SR, slowest = 99.0 / g_rate_step[0] / SR;
  /* The slowest rate keeps its timing at the highest sample rate too. */
  sideband_init(384000.0f);
  setp(0, SIDEBAND_OP_L4, 0); setp(0, SIDEBAND_OP_L1, 99); setp(0, SIDEBAND_OP_R1, 0);
  OpState slow = {.env = 0.0, .stage = 0, .gain = 1.0f};
  int slow_ticks = ticks_until_stage_changes(&slow, 0, 384000 * 60);
  CHECKF(fabs(slow_ticks / 384000.0 - 40.0) < 0.02, "rate 0 at 384 kHz took %g s", slow_ticks / 384000.0);
  sideband_init((float)SR);
  CHECKF(fabs(fastest - 0.0015) < 1e-6, "rate 99 sweep %g s", fastest);
  CHECKF(fabs(slowest - 40.0) < 1e-3, "rate 0 sweep %g s", slowest);
  int increasing = 1;
  for (int r = 1; r < 100; r++) if (!(g_rate_step[r] > g_rate_step[r - 1])) increasing = 0;
  CHECK(increasing);
}

/* The attack lands gently: its last step is much smaller than its first. */
static void test_attack_eases_in(void) {
  sideband_init((float)SR);
  setp(0, SIDEBAND_OP_L4, 0); setp(0, SIDEBAND_OP_L1, 99); setp(0, SIDEBAND_OP_R1, 40);
  OpState o = {.env = 0.0, .stage = 0, .gain = 1.0f};
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
    sideband_init((float)SR);
    for (int off = SIDEBAND_OP_R1; off <= SIDEBAND_OP_L4; off++) setp(0, off, rng_int(0, 99));
    OpState o = {.env = (double)OPP(0, SIDEBAND_OP_L4), .stage = 0, .gain = 1.0f};
    for (int i = 0; i < 20000; i++) {
      if (i == 12000) o.stage = 3;
      if (i % 3000 == 0) setp(0, SIDEBAND_OP_L1 + rng_int(0, 3), rng_int(0, 99)); /* live edits */
      env_tick(&o, 0);
      if (!(o.env >= 0.0 && o.env <= 99.0) || o.stage < 0 || o.stage > STAGE_DONE) ok = 0;
    }
  }
  CHECK(ok);
}

/* ---------------------------------------------------------------------- */
/* Algorithms                                                              */

/* The intended graphs, written out independently of the engine's table. */
static const char *EXPECTED_EDGES[SIDEBAND_ALGOS] = {
  "2>1 4>3 6>5",
  "3>2 2>1 6>5 5>4",
  "2>1 6>5 5>4 4>3",
  "6>5 5>4 4>3 3>2 2>1",
  "2>1 6>3 6>4 6>5",
  "2>1 3>1 5>1 4>3 6>5",
  "6>5 5>4",
  "",
};
static const char *EXPECTED_CARRIERS[SIDEBAND_ALGOS] = {
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
  for (int mid = 1; mid <= SIDEBAND_OPS; mid++)
    if (expected_edge(algo, from, mid) && expected_path(algo, mid, to)) return 1;
  return 0;
}

static void test_algorithm_tables(void) {
  for (int a = 0; a < SIDEBAND_ALGOS; a++) {
    int carriers = sideband_algo_carrier_mask(a);
    CHECK(carriers != 0);
    CHECK(sideband_algo_feedback_op(a) == 5);
    for (int t = 0; t < SIDEBAND_OPS; t++) {
      int mask = sideband_algo_mod_mask(a, t);
      CHECKF(((carriers >> t) & 1) == expected_carrier(a, t + 1), "algo %d op %d carrier", a + 1, t + 1);
      for (int f = 0; f < SIDEBAND_OPS; f++) {
        int edge = (mask >> f) & 1;
        CHECKF(edge == expected_edge(a, f + 1, t + 1), "algo %d edge %d>%d", a + 1, f + 1, t + 1);
        if (edge) CHECKF(f > t, "algo %d: OP%d feeds OP%d, breaking the OP6..OP1 order", a + 1, f + 1, t + 1);
      }
      /* Every operator is heard: it is a carrier or feeds another operator. */
      int feeds = 0;
      for (int u = 0; u < SIDEBAND_OPS; u++) feeds |= (sideband_algo_mod_mask(a, u) >> t) & 1;
      CHECK(((carriers >> t) & 1) || feeds);
      /* A carrier is never also a modulator in this set of eight. */
      CHECK(!(((carriers >> t) & 1) && feeds));
    }
  }
  CHECK(sideband_algo_carrier_mask(-1) == 0 && sideband_algo_carrier_mask(8) == 0);
  CHECK(sideband_algo_mod_mask(0, 6) == 0 && sideband_algo_feedback_op(99) == -1);
}

/* Only carriers make sound on their own. */
static void test_only_carriers_are_audible(void) {
  for (int a = 0; a < SIDEBAND_ALGOS; a++) {
    for (int k = 0; k < SIDEBAND_OPS; k++) {
      fresh(a, 0);
      op_organ(k, 99, 1);
      sideband_note_on(69, 100);
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
  for (int a = 0; a < SIDEBAND_ALGOS; a++) {
    for (int c = 1; c <= SIDEBAND_OPS; c++) {
      if (!expected_carrier(a, c)) continue;
      for (int m = 1; m <= SIDEBAND_OPS; m++) {
        for (int pass = 0; pass < 2; pass++) {
          fresh(a, 0);
          for (int k = 1; k <= SIDEBAND_OPS; k++) {
            int level = expected_carrier(a, k) ? (k == c ? 99 : 0) : 80;
            if (pass == 1 && k == m) level = 0;
            op_organ(k - 1, level, k); /* distinct ratios */
          }
          sideband_note_on(57, 100);
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
  sideband_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double pure = power_fraction(buf_m + SR / 4, 4800, 440.0);
  CHECKF(pure > 0.999, "carrier alone: %.5f of power at 440 Hz", pure);

  fresh(0, 0);
  op_organ(0, 99, 1);
  op_organ(1, 85, 1);
  sideband_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double modulated = power_fraction(buf_m + SR / 4, 4800, 440.0);
  double second = power_fraction(buf_m + SR / 4, 4800, 880.0);
  CHECKF(modulated < 0.7 && second > 0.05, "modulated: fundamental %.3f, 2nd %.3f", modulated, second);
}

static void test_feedback(void) {
  /* Algorithm 8, OP6 alone: feedback 0 is a sine, feedback 7 is bright. */
  fresh(7, 0);
  op_organ(5, 99, 1);
  sideband_note_on(69, 100);
  render(NULL, buf_m, SR / 2);
  double clean = power_fraction(buf_m + SR / 4, 4800, 440.0);
  fresh(7, 7);
  op_organ(5, 99, 1);
  sideband_note_on(69, 100);
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
  sideband_init((float)SR);
  sideband_set_volume(1.0f);
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
  CHECKF(worst <= SIDEBAND_CEILING, "output peak %g", (double)worst);
  CHECKF(worst < 1.0f, "output peak %g", (double)worst);
  CHECKF(worst_mon <= SIDEBAND_LIMIT_THRESHOLD * 1.000001f, "monitor peak %g", (double)worst_mon);
}

static void set_extreme_patch(int algo) {
  fresh(algo, 7);
  for (int k = 0; k < SIDEBAND_OPS; k++) {
    op_organ(k, 99, 31);
    setp(k, SIDEBAND_OP_FINE, 99);
    if (expected_carrier(algo, k + 1)) setp(k, SIDEBAND_OP_COARSE, 1);
  }
}

static void test_limiter_under_extreme_patches(void) {
  for (int a = 0; a < SIDEBAND_ALGOS; a++) {
    set_extreme_patch(a);
    sideband_set_volume(1.0f);
    for (int n = 0; n < 12; n++) sideband_note_on(36 + n * 3, 127);  /* more notes than voices */
    render(buf_a, buf_m, SR);
    float p = peak(buf_a, SR), pm = peak(buf_m, SR);
    CHECKF(p <= SIDEBAND_CEILING && p < 1.0f, "algo %d: output peak %g", a + 1, (double)p);
    CHECKF(pm <= SIDEBAND_LIMIT_THRESHOLD * 1.000001f, "algo %d: monitor peak %g", a + 1, (double)pm);
    CHECKF(p > 0.3f, "algo %d: extreme patch should be loud, got %g", a + 1, (double)p);
    CHECK(sideband_active_voices() == SIDEBAND_VOICES);
  }
  /* The limiter was actually working, not just the final clamp. */
  set_extreme_patch(7);
  sideband_set_volume(1.0f);
  for (int n = 0; n < 8; n++) sideband_note_on(48 + n, 127);
  render(NULL, NULL, SR / 4);
  CHECKF(sideband_gain_reduction() < 0.9f, "gain %g", (double)sideband_gain_reduction());
}

static void test_volume(void) {
  sideband_init((float)SR);
  CHECK(g_volume_target == SIDEBAND_DEFAULT_VOLUME);
  CHECK(SIDEBAND_DEFAULT_VOLUME <= 0.2512f);           /* -12 dB or quieter */
  CHECK(sideband_set_volume(2.0f) == 1.0f);
  CHECK(sideband_set_volume(-1.0f) == 0.0f);
  CHECK(sideband_set_volume(NAN) == 0.0f);
  CHECK(sideband_set_volume(INFINITY) == 0.0f);
  CHECK(sideband_set_volume(0.5f) == 0.5f);

  /* Starts from silence and glides: no jump on the first sample. */
  fresh(7, 0);
  op_organ(0, 99, 1);
  sideband_note_on(69, 127);
  render(buf_a, buf_m, SR / 2);
  CHECKF(fabsf(buf_a[1]) < 0.01f, "first samples %g", (double)buf_a[1]);
  CHECKF(fabsf(peak(buf_a + SR / 4, SR / 4) - 0.25f * peak(buf_m + SR / 4, SR / 4)) < 1e-3f,
         "steady state is monitor x 0.25");

  /* Volume 0 is silence. */
  sideband_set_volume(0.0f);
  render(buf_a, NULL, SR / 2);
  CHECK(peak(buf_a + SR / 4, SR / 4) < 1e-6f);
}

static void test_silence_without_notes(void) {
  sideband_init((float)SR);
  render(buf_a, buf_m, SR / 4);
  CHECK(peak(buf_a, SR / 4) == 0.0f && peak(buf_m, SR / 4) == 0.0f);
}


/* ---------------------------------------------------------------------- */
/* Smoothness: edits, algorithm changes, panic and retriggers never step    */

/* An operator above Nyquist would only alias: it fades out between 0.43 and
 * 0.47 of the sample rate instead of being pinned just below Nyquist. */
static void test_operators_fade_out_near_nyquist(void) {
  sideband_init((float)SR);
  CHECK(nyquist_fade(0.40f * SR) == 1.0f);
  CHECKF(fabsf(nyquist_fade(0.45f * SR) - 0.5f) < 1e-4f, "fade at 0.45 sr %g", (double)nyquist_fade(0.45f * SR));
  CHECK(nyquist_fade(0.47f * SR) == 0.0f && nyquist_fade(3.0f * SR) == 0.0f);
  /* A carrier at x31.99 on C7 (67 kHz) is silent, not a tone pinned at 23.5 kHz. */
  fresh(7, 0);
  op_organ(0, 99, 31); setp(0, SIDEBAND_OP_FINE, 99);
  sideband_note_on(96, 127);
  render(NULL, buf_m, SR / 4);
  CHECKF(rms(buf_m, SR / 4) == 0.0, "ultrasonic carrier rms %g", rms(buf_m, SR / 4));
  /* The same carrier on C2 (2.1 kHz) plays at full level. */
  fresh(7, 0);
  op_organ(0, 99, 31); setp(0, SIDEBAND_OP_FINE, 99);
  sideband_note_on(36, 127);
  render(NULL, buf_m, SR / 4);
  CHECK(rms(buf_m + SR / 8, SR / 8) > 0.1);
}

/* A level edit glides over about 20 ms (two 10 ms one-pole stages, every
 * sample) instead of stepping at the next block. */
static void test_level_edits_are_smoothed(void) {
  fresh(7, 0);
  op_organ(0, 99, 1);
  sideband_note_on(69, 127);
  render(NULL, NULL, SR / 4);
  Voice *v = voice_for(69);
  CHECK(v != NULL);
  if (!v) return;
  double start = v->op[0].gain, target = (double)level_amp(40.0f);
  setp(0, SIDEBAND_OP_LEVEL, 40);
  render(NULL, NULL, SIDEBAND_BLOCK);
  double moved = (start - v->op[0].gain) / (start - target);
  CHECKF(moved > 0.0f && moved < 0.1f, "after one block the gain moved %.3f of the way", (double)moved);
  render(NULL, NULL, SR / 4);
  CHECKF(v->op[0].gain == target, "after 250 ms gain %g, target %g", v->op[0].gain, target);
  /* The same for a ratio edit: the frequency glides, then lands exactly. */
  uint32_t want = hz_to_inc(880.0f);
  setp(0, SIDEBAND_OP_COARSE, 2);
  render(NULL, NULL, SIDEBAND_BLOCK);
  CHECKF(v->op[0].inc > (double)hz_to_inc(440.0f) && v->op[0].inc < (double)want, "increment mid-glide %g", v->op[0].inc);
  render(NULL, NULL, SR / 4);
  CHECKF(v->op[0].inc == (double)want, "increment %f, want %u", v->op[0].inc, want);
}

/* Changing the algorithm under a held note crossfades over 10 ms: the
 * waveform never steps further than either sound does on its own. */
#define T0 (SIDEBAND_BLOCK * 188) /* about 0.5 s, a whole number of blocks */
static void test_algorithm_change_crossfades(void) {
  fresh(1, 6);
  for (int k = 0; k < SIDEBAND_OPS; k++) op_organ(k, k == 0 || k == 3 ? 99 : 80, 1);
  sideband_note_on(60, 100);
  render(buf_a, NULL, T0);
  sideband_set_param(SIDEBAND_P_ALGO, 3); /* TOWER: a very different sound */
  CHECK(g_algo == 1);                     /* nothing jumps before the next block */
  render(buf_a + T0, NULL, T0);
  CHECK(g_algo == 3 && g_xf == 1.0f);
  float old_sound = max_step(buf_a, T0 - SR / 20, T0);
  float new_sound = max_step(buf_a, T0 + SR / 10, T0 + SR / 5);
  float bound = old_sound > new_sound ? old_sound : new_sound;
  float during = max_step(buf_a, T0, T0 + SR / 50);
  CHECKF(during <= 1.1f * bound, "max step during the crossfade %g; old sound %g, new sound %g",
         (double)during, (double)old_sound, (double)new_sound);
  CHECKF(fabsf(buf_a[T0] - buf_a[T0 - 1]) <= 1.1f * old_sound, "step at the switch %g",
         (double)fabsf(buf_a[T0] - buf_a[T0 - 1]));
  /* A second change during the crossfade waits for it to finish. */
  sideband_set_param(SIDEBAND_P_ALGO, 0);
  render(NULL, NULL, SIDEBAND_BLOCK);
  CHECK(g_algo == 0 && g_xf < 1.0f);
  sideband_set_param(SIDEBAND_P_ALGO, 7);
  render(NULL, NULL, SIDEBAND_BLOCK);
  CHECK(g_algo == 0);
  render(NULL, NULL, SR / 20);
  CHECK(g_algo == 7 && g_xf == 1.0f);
  /* With nothing sounding, a change applies at once. */
  sideband_panic();
  render(NULL, NULL, SR / 10);
  CHECK(sideband_active_voices() == 0);
  sideband_set_param(SIDEBAND_P_ALGO, 4);
  CHECK(g_algo == 4 && g_xf == 1.0f);
}

/* Panic fades every voice out over 5 ms instead of cutting it. */
static void test_panic_fades_out(void) {
  fresh(7, 0);
  op_organ(0, 99, 1);
  sideband_set_volume(1.0f);
  sideband_note_on(69, 127);
  sideband_note_on(76, 127);
  render(buf_a, NULL, T0);
  sideband_panic();
  render(buf_a + T0, NULL, T0);
  float before = max_step(buf_a, T0 - SR / 20, T0);
  float after = max_step(buf_a, T0, T0 + SR / 10);
  CHECKF(after <= before * 1.001f, "max step after panic %g, before %g", (double)after, (double)before);
  int fade_len = (int)(0.005 * SR);
  CHECKF(fabsf(buf_a[T0 + fade_len / 2]) > 0.0f, "still fading halfway through");
  CHECK(peak(buf_a + T0 + fade_len + SIDEBAND_BLOCK, SR / 20) < 1e-3f);
  CHECK(sideband_active_voices() == 0);
}

/* A drone (L4 > 0) played again while it fades out comes back up from where
 * it is instead of jumping to full level. */
static void test_retrigger_during_end_fade_is_smooth(void) {
  const int half_fade = SIDEBAND_BLOCK * 19; /* about 50 ms of the 0.1 s end fade */
  fresh(7, 0);
  op_organ(0, 99, 1); setp(0, SIDEBAND_OP_L4, 99); setp(0, SIDEBAND_OP_R4, 99);
  sideband_set_volume(1.0f);
  sideband_note_on(60, 100);
  render(buf_a, NULL, T0);
  sideband_note_off(60);
  render(buf_a + T0, NULL, half_fade);
  Voice *v = voice_for(60);
  CHECK(v != NULL && v->ending && v->fade < 0.8f && v->fade > 0.2f);
  if (!v) return;
  int at = T0 + half_fade;
  sideband_note_on(60, 100);
  render(buf_a + at, NULL, T0);
  float before = max_step(buf_a, T0 - SR / 20, T0);
  float after = max_step(buf_a, at, at + SR / 100);
  CHECKF(after <= 1.01f * before, "max step after retrigger %g, steady %g", (double)after, (double)before);
  CHECK(v->fade == 1.0f && !v->ending);
}

/* The compressor leaves a quiet note alone, pulls a dense chord down, never
 * adds gain, and keeps louder input louder. */
static void test_compressor(void) {
  fresh(7, 0);
  op_organ(0, 80, 1);
  sideband_note_on(69, 100);
  render(NULL, NULL, SR / 2);
  CHECKF(sideband_comp_gain() == 1.0f, "quiet sine: comp gain %g", (double)sideband_comp_gain());

  fresh(7, 0);
  for (int k = 0; k < SIDEBAND_OPS; k++) op_organ(k, 99, k + 1);
  for (int n = 0; n < 8; n++) sideband_note_on(48 + 4 * n, 127);
  render(NULL, NULL, SR / 2);
  CHECKF(sideband_comp_gain() < 0.5f, "dense chord: comp gain %g", (double)sideband_comp_gain());

  double prev = 0.0;
  int monotonic = 1;
  for (int level = 60; level <= 99; level += 3) {
    fresh(7, 0);
    op_organ(0, level, 1);
    sideband_note_on(69, 100);
    render(NULL, buf_m, SR);
    double r = rms(buf_m + SR / 2, SR / 2);
    if (!(r > prev)) monotonic = 0;
    prev = r;
    if (!(sideband_comp_gain() <= 1.0f)) monotonic = 0;
  }
  CHECK(monotonic);
}

/* Up to 384 kHz is supported and stays in tune. */
static void test_high_sample_rate(void) {
  sideband_init(384000.0f);
  sideband_set_param(SIDEBAND_P_ALGO, 7);
  sideband_note_on(69, 100);
  static float hi[384000 / 2];
  for (int i = 0; i < 384000 / 2; i += SIDEBAND_BLOCK) {
    sideband_render();
    for (int s = 0; s < SIDEBAND_BLOCK; s++) hi[i + s] = g_mon[s];
  }
  double hz = measure_hz_at(hi + 38400, 384000 / 2 - 38400, 384000.0);
  CHECKF(fabs(hz - 440.0) < 0.02, "A4 at 384 kHz measured %.4f Hz", hz);
}

/* ---------------------------------------------------------------------- */
/* Parameters, voices, determinism                                         */

static void test_parameter_clamping(void) {
  sideband_init((float)SR);
  int lvl = pid(2, SIDEBAND_OP_LEVEL), det = pid(4, SIDEBAND_OP_DETUNE);
  CHECK(sideband_set_param(SIDEBAND_P_ALGO, 1e9f) == 7.0f);
  CHECK(sideband_set_param(SIDEBAND_P_ALGO, -1e9f) == 0.0f);
  CHECK(sideband_set_param(SIDEBAND_P_FEEDBACK, 99.0f) == 7.0f);
  CHECK(sideband_set_param(lvl, 3.6f) == 4.0f);
  CHECK(sideband_set_param(lvl, NAN) == 4.0f);          /* NaN is ignored */
  CHECK(sideband_set_param(lvl, INFINITY) == 4.0f);
  CHECK(sideband_set_param(lvl, 250.0f) == 99.0f);
  CHECK(sideband_set_param(det, -30.0f) == -20.0f);
  CHECK(sideband_set_param(det, -2.5f) == -3.0f);       /* rounds half away from zero */
  CHECK(sideband_set_param(-1, 5.0f) == 0.0f);
  CHECK(sideband_set_param(SIDEBAND_PARAMS, 5.0f) == 0.0f);
  CHECK(sideband_get_param(SIDEBAND_PARAMS) == 0.0f);
  CHECK(sideband_param_min(det) == -20.0f && sideband_param_max(det) == 20.0f);
  CHECK(sideband_param_default(pid(0, SIDEBAND_OP_LEVEL)) == 99.0f);
  CHECK(sideband_param_default(pid(1, SIDEBAND_OP_LEVEL)) == 0.0f);
  int all_in_range = 1;
  for (int id = 0; id < SIDEBAND_PARAMS; id++) {
    float mn = sideband_param_min(id), mx = sideband_param_max(id), df = sideband_param_default(id);
    if (!(mn <= df && df <= mx)) all_in_range = 0;
  }
  CHECK(all_in_range);
}

static void test_voice_allocation(void) {
  fresh(7, 0);
  op_organ(0, 99, 1);
  for (int n = 0; n < 10; n++) sideband_note_on(60 + n, 100);
  CHECK(sideband_active_voices() == SIDEBAND_VOICES);
  sideband_note_on(69, 100);                              /* already sounding: reuse */
  CHECK(sideband_active_voices() == SIDEBAND_VOICES);
  sideband_all_notes_off();
  render(NULL, NULL, SR);
  CHECK(sideband_active_voices() == 0);

  sideband_note_on(300, 100);
  CHECK(g_voice[0].note == 127 || sideband_active_voices() == 1);
  int found = 0;
  for (int i = 0; i < SIDEBAND_VOICES; i++) if (g_voice[i].active && g_voice[i].note == 127) found = 1;
  CHECK(found);
  sideband_note_on(127, 0);                               /* velocity 0 = note off */
  render(NULL, NULL, SR);
  CHECK(sideband_active_voices() == 0);
}

static void test_released_drone_still_ends(void) {
  fresh(7, 0);
  for (int k = 0; k < SIDEBAND_OPS; k++) { op_organ(k, 99, k + 1); setp(k, SIDEBAND_OP_L4, 99); }
  sideband_note_on(60, 100);
  render(NULL, NULL, SR / 10);
  sideband_note_off(60);
  render(NULL, NULL, SR / 2);
  CHECK(sideband_active_voices() == 0);
}

static void play_sequence(float *out, int n) {
  sideband_note_on(60, 100);
  render(out, NULL, n / 4);
  sideband_note_on(64, 90);
  sideband_note_on(67, 80);
  render(out + n / 4, NULL, n / 4);
  sideband_note_off(60);
  render(out + n / 2, NULL, n / 4);
  sideband_all_notes_off();
  render(out + 3 * n / 4, NULL, n / 4);
}

static void ep_like_patch(void) {
  fresh(0, 5);
  op_organ(0, 99, 1); setp(0, SIDEBAND_OP_R2, 30); setp(0, SIDEBAND_OP_L3, 0); setp(0, SIDEBAND_OP_L2, 80);
  op_organ(1, 70, 14); setp(1, SIDEBAND_OP_R2, 60); setp(1, SIDEBAND_OP_L2, 0); setp(1, SIDEBAND_OP_L3, 0);
  op_organ(2, 95, 1); setp(2, SIDEBAND_OP_DETUNE, 5);
  op_organ(3, 75, 1);
  op_organ(4, 90, 1); setp(4, SIDEBAND_OP_DETUNE, -5);
  op_organ(5, 70, 1);
}

static uint32_t g_golden_hash;
#define GOLDEN_HASH 0x900e4f46u

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
  setp(1, SIDEBAND_OP_COARSE, 13);
  play_sequence(buf_b, n);
  CHECK(memcmp(buf_a, buf_b, sizeof(float) * n) != 0);

  /* Determinism holds at other sample rates too. */
  for (int pass = 0; pass < 2; pass++) {
    sideband_init(44100.0f);
    op_organ(0, 99, 1); op_organ(1, 80, 3);
    play_sequence(pass == 0 ? buf_a : buf_b, n);
  }
  CHECK(memcmp(buf_a, buf_b, sizeof(float) * n) == 0);
}

static void test_sample_rate_clamping(void) {
  sideband_init(0.0f);      CHECK(sideband_sample_rate() == 8000.0f);
  sideband_init(NAN);       CHECK(sideband_sample_rate() == 48000.0f);
  sideband_init(1e9f);      CHECK(sideband_sample_rate() == 384000.0f);
  sideband_init(384000.0f); CHECK(sideband_sample_rate() == 384000.0f);
  sideband_init(192000.0f); CHECK(sideband_sample_rate() == 192000.0f);
  sideband_init(-INFINITY); CHECK(sideband_sample_rate() == 48000.0f);
  sideband_init(44100.0f);  CHECK(sideband_sample_rate() == 44100.0f);
}

/* Random patches, including out-of-range and non-finite values, random
 * notes, velocities, volumes and sample rates. Every sample must be finite
 * and inside the ceiling. */
static void test_fuzz_no_nan_no_overs(void) {
  static const float rates[] = {8000.0f, 22050.0f, 44100.0f, 48000.0f, 96000.0f, 192000.0f, 384000.0f};
  static const float specials[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 3.4e38f, -0.0f};
  rng_state = 20260926u;
  int finite = 1, within = 1, env_ok = 1;
  float worst = 0.0f;
  const int iterations = 600;
  for (int it = 0; it < iterations; it++) {
    sideband_init(rates[rng_int(0, 6)]);
    for (int id = 0; id < SIDEBAND_PARAMS; id++) {
      int kind = rng_int(0, 9);
      float mn = sideband_param_min(id), mx = sideband_param_max(id);
      float v;
      if (kind == 0) v = specials[rng_int(0, 6)];
      else if (kind == 1) v = mn;
      else if (kind == 2) v = mx;
      else if (kind == 3) v = (float)rng_int(-1000, 1000);
      else v = mn + (mx - mn) * (float)(rng() % 10001) / 10000.0f;
      sideband_set_param(id, v);
    }
    int vk = rng_int(0, 3);
    sideband_set_volume(vk == 0 ? 1.0f : vk == 1 ? specials[rng_int(0, 6)] : (float)(rng() % 1001) / 1000.0f);
    for (int blk = 0; blk < 24; blk++) {
      if (blk % 4 == 0) {
        int notes = rng_int(1, 10);
        for (int i = 0; i < notes; i++) sideband_note_on(rng_int(-10, 140), rng_int(-5, 200));
      }
      if (blk % 6 == 5) sideband_note_off(rng_int(0, 127));
      if (blk == 12) sideband_set_param(rng_int(0, SIDEBAND_PARAMS - 1), (float)rng_int(-50, 150));
      float *o = sideband_render();
      for (int s = 0; s < SIDEBAND_BLOCK; s++) {
        if (!is_finite(o[s]) || !is_finite(g_mon[s])) finite = 0;
        if (fabsf(o[s]) > SIDEBAND_CEILING || fabsf(g_mon[s]) > SIDEBAND_LIMIT_THRESHOLD * 1.000001f) within = 0;
        if (fabsf(o[s]) > worst) worst = fabsf(o[s]);
      }
      if (!(sideband_comp_gain() <= 1.0f && sideband_comp_gain() > 0.0f)) within = 0;
      for (int i = 0; i < SIDEBAND_VOICES; i++)
        for (int k = 0; k < SIDEBAND_OPS; k++)
          if (!(g_voice[i].op[k].env >= 0.0 && g_voice[i].op[k].env <= 99.0)) env_ok = 0;
    }
  }
  CHECK(finite);
  CHECKF(within, "worst output %g", (double)worst);
  CHECK(env_ok);
  printf("    fuzz: %d random patches, worst output peak %.4f (ceiling %.4f)\n",
         iterations, (double)worst, (double)SIDEBAND_CEILING);
}

int main(void) {
  RUN(test_sine_table_accuracy);
  RUN(test_exp2_accuracy);
  RUN(test_log2_accuracy);
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
  RUN(test_operators_fade_out_near_nyquist);
  RUN(test_level_edits_are_smoothed);
  RUN(test_algorithm_change_crossfades);
  RUN(test_panic_fades_out);
  RUN(test_retrigger_during_end_fade_is_smooth);
  RUN(test_compressor);
  RUN(test_high_sample_rate);
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
