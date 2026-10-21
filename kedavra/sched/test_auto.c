// SPDX-License-Identifier: GPL-3.0-or-later
// Closed-loop tests of kfg_auto.h with kfg_sched.h and a model of a game behind
// Gamescope's frame limiter (frame callback released every `mult` vblanks).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "kfg_auto.h"
#include "kfg_sched.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) / 9007199254740992.0; }

typedef struct {
	double x2_fraction;    // share of vblanks at x2 in the second half
	uint64_t switches;
	double late_fraction;  // source-late repeats per source frame, second half
	double x2_all;         // share of vblanks at x2 over the whole run
	double uneven;         // second half: outputs whose motion step is off by > 25%
} result;

// In-game frame limiter (the recommended setup): 0 = none. The game works for a part of
// the frame, then its limiter sleeps until the next deadline (period 1 / lock_fps on the
// game's own clock, which runs slightly off the display clock, so its phase against the
// vblanks drifts through every position) and presents. A deadline it misses resets it.
static double g_lock_fps;
static double g_lock_skew = 2e-4;   // game clock vs display clock
static double g_lock_sleep = 0.4e-3; // limiter wake-up jitter, +-s

// native_fps(t_s) gives the game's own frame rate at time t; jitter is relative.
static result run(double (*native_fps)(double), double seconds, double jitter)
{
	const uint32_t hz = 90;
	const double period = 1.0 / hz;
	kfg_auto a;
	kfg_auto_init(&a, hz);
	kfg_sched_config cfg = kfg_sched_default_config(a.mult);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint32_t mult = a.mult;
	const int64_t n = (int64_t)(seconds * hz);
	double busy_until = 0;      // game render end time
	bool rendering = false, callback = true;
	uint64_t id = 0;
	double deadline = 0;        // in-game limiter
	double *gt = calloc((size_t)n + 2, sizeof(double)); // game time of each source frame
	double shown_prev = -1, shown = -1;
	int64_t steps = 0, uneven = 0;
	int64_t x2 = 0, half = 0, x2_all = 0;
	uint64_t late_base = 0, src_half = 0, ulate_base = 0;
	for (int64_t k = 0; k < n; k++) {
		const double t = k * period;
		if (k == n / 2) {
			late_base = s.n_repeat_reason[KFG_REPEAT_SOURCE_LATE];
			ulate_base = a.lates;
			src_half = id;
		}
		// Latch a frame that finished before this vblank.
		if (rendering && busy_until <= t) {
			rendering = false;
			kfg_sched_on_latch(&s, ++id, k);
			gt[id] = busy_until;
			kfg_auto_on_latch(&a, k);
			kfg_sched_on_me(&s, id, k + 2, true, false);
		}
		// Limiter: one frame callback per mult vblanks.
		if (k % mult == 0)
			callback = true;
		if (!rendering && callback) {
			callback = false;
			rendering = true;
			const double f = native_fps(t);
			busy_until = t + (1.0 / f) * (1.0 + jitter * (2.0 * urand() - 1.0));
			if (g_lock_fps > 0) {
				const double p = (1.0 + g_lock_skew) / g_lock_fps;
				const double work = t + 0.6 * p * (1.0 + jitter * (2.0 * urand() - 1.0));
				deadline = deadline + p < work ? work : deadline + p;
				busy_until = deadline + g_lock_sleep * (2.0 * urand() - 1.0);
			}
		}
		const kfg_decision d = kfg_sched_decide(&s, k);
		if (d.show == KFG_SHOW_REAL)
			shown = gt[d.src_a];
		else if (d.show == KFG_SHOW_GEN)
			shown = gt[d.src_a] + (gt[d.src_b] - gt[d.src_a]) * d.phase_num / d.phase_den;
		if (k >= n / 2 && shown_prev >= 0 && d.state == KFG_STATE_ACTIVE) {
			steps++;
			if (fabs(shown - shown_prev - period) > 0.25 * period)
				uneven++;
		}
		shown_prev = shown;
		const uint32_t m = kfg_auto_step(&a, k);
		if (m != mult) {
			mult = m;
			kfg_sched_set_multiplier(&s, m);
		}
		x2_all += mult == 2;
		if (k >= n / 2) {
			half++;
			x2 += mult == 2;
		}
	}
	result r;
	r.x2_fraction = (double)x2 / (double)half;
	r.x2_all = (double)x2_all / (double)n;
	r.uneven = steps ? (double)uneven / (double)steps : 1.0;
	free(gt);
	r.switches = a.n_switch;
	const uint64_t src = id - src_half;
	(void)late_base;
	r.late_fraction = src ? (double)(a.lates - ulate_base) / (double)src : 1.0;
	return r;
}

static double fps60(double t) { (void)t; return 60.0; }
static double fps50(double t) { (void)t; return 50.0; }
static double fps38(double t) { (void)t; return 38.0; }
static double fps44(double t) { (void)t; return 44.0; }
static double heavy_scene(double t) { return t < 120 ? 60.0 : (t < 240 ? 36.0 : 60.0); }

int main(void)
{
	result r;
	r = run(fps60, 300, 0.10);
	printf("native 60:  x2 %.1f%%, switches %llu, late %.3f%%\n", 100 * r.x2_fraction, (unsigned long long)r.switches, 100 * r.late_fraction);
	CHECK(r.x2_fraction > 0.99, "a 60 fps game should settle at x2");
	CHECK(r.switches <= 2, "no oscillation at 60 fps");

	r = run(fps50, 300, 0.10);
	printf("native 50:  x2 %.1f%%, switches %llu, late %.3f%%\n", 100 * r.x2_fraction, (unsigned long long)r.switches, 100 * r.late_fraction);
	CHECK(r.x2_fraction > 0.95, "a 50 fps game should hold x2");

	r = run(fps38, 600, 0.10);
	printf("native 38:  x2 %.1f%%, switches %llu, late %.3f%%\n", 100 * r.x2_fraction, (unsigned long long)r.switches, 100 * r.late_fraction);
	CHECK(r.x2_fraction < 0.05, "a 38 fps game should stay at x3");
	CHECK(r.switches <= 12, "x2 trials must back off (%llu switches)", (unsigned long long)r.switches);
	CHECK(r.late_fraction < 0.01, "x3 at 30 fps should be clean for a 38 fps game");

	r = run(fps44, 600, 0.05);
	printf("native 44:  x2 %.1f%%, switches %llu, late %.3f%%\n", 100 * r.x2_fraction, (unsigned long long)r.switches, 100 * r.late_fraction);
	CHECK(r.x2_fraction < 0.1, "a game just below 45 fps should not sit at x2");

	// In-game FPS lock (the recommended setup), its phase drifting through the vblanks.
	g_lock_fps = 30;
	r = run(fps60, 600, 0.10);
	printf("lock 30:    x2 %.2f%% of the run, switches %llu, late %.3f%%, uneven motion %.2f%%\n", 100 * r.x2_all, (unsigned long long)r.switches, 100 * r.late_fraction, 100 * r.uneven);
	CHECK(r.x2_all < 0.002, "x2 trials on a game locked at 30 must be short (%.3f%% of the run)", 100 * r.x2_all);
	CHECK(r.late_fraction < 0.005, "a game locked at 30 is not late at x3 (%.3f%%)", 100 * r.late_fraction);
	CHECK(r.uneven < 0.005, "a game locked at 30 must move evenly at x3 (%.2f%% uneven)", 100 * r.uneven);

	g_lock_fps = 45;
	r = run(fps60, 600, 0.10);
	printf("lock 45:    x2 %.1f%%, switches %llu, late %.3f%%, uneven motion %.2f%%\n", 100 * r.x2_fraction, (unsigned long long)r.switches, 100 * r.late_fraction, 100 * r.uneven);
	CHECK(r.x2_fraction > 0.99, "a game locked at 45 should hold x2");
	CHECK(r.late_fraction < 0.005, "a game locked at 45 is not late at x2 (%.3f%%)", 100 * r.late_fraction);
	CHECK(r.uneven < 0.005, "a game locked at 45 must move evenly at x2 (%.2f%% uneven)", 100 * r.uneven);
	g_lock_fps = 0;

	r = run(heavy_scene, 360, 0.10);
	printf("60->36->60: x2 %.1f%% (second half), switches %llu\n", 100 * r.x2_fraction, (unsigned long long)r.switches);
	CHECK(r.x2_fraction > 0.5, "x2 should come back after the heavy scene");

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("kfg_auto: all tests passed\n");
	return 0;
}
