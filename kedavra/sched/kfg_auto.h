// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA automatic source cap and multiplier.
//
// Generation needs a source that arrives on a fixed vblank cadence. Instead of a manual
// FPS limit, the engine caps the game itself (Gamescope's frame limiter releases the
// game's frame callback every `mult` vblanks) and picks the multiplier from what the
// game actually delivers:
//
//   * it starts at x3 (source = refresh / 3), the cadence most games can hold;
//   * after a window with no late source frame it tries x2 (source = refresh / 2);
//   * as soon as more than late_limit of a window's expected source frames came late
//     it drops back to x3 (without waiting for the window to end); a failed x2 trial doubles the wait before the next one
//     (up to backoff_max), a passed one resets it.
//
// A late source frame is a latch interval longer than `mult` vblanks: under the cap a
// game that keeps up is latched exactly every `mult` vblanks. Jitter around the cap is not
// late: an interval that, with its neighbour before or after it, still fits two cap periods
// (2 then 4, or 4 then 2, at x3) is what a game locked in its own settings looks like when
// its presents sit near a vblank boundary, and the scheduler keeps such frames on the grid. (The scheduler's own
// repeats are not the signal: it absorbs an irregular source by filling longer intervals
// with more intermediates, which keeps flips regular but makes motion uneven.)
// Self-contained C99/C++ like kfg_sched.h, so the compositor and the tests share it.
#ifndef KFG_AUTO_H
#define KFG_AUTO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct kfg_auto {
	uint32_t mult;           // 2 or 3
	int64_t window_start;    // vblank of the current evaluation window; -1 = not started
	uint64_t late_base;
	uint64_t lates;          // latch intervals longer than mult
	int64_t last_latch;      // -1 = none yet
	int64_t prev_latch;      // the latch before last_latch; -1 = none
	bool pending;            // the last interval was long; the next one decides if it was late
	uint32_t window_vbl;     // vblanks per evaluation window
	uint32_t probe_vbl;      // base wait at x3 before trying x2 again
	uint32_t backoff;        // multiplies probe_vbl after failed trials
	uint32_t backoff_max;
	int64_t probe_at;        // first vblank at which x2 may be tried
	bool trial;              // x2 is being tried
	double late_limit;       // tolerated fraction of late source frames per window
	uint64_t n_switch;       // multiplier changes, for status
} kfg_auto;

static inline void kfg_auto_init(kfg_auto *a, uint32_t refresh_hz)
{
	if (refresh_hz < 30)
		refresh_hz = 30;
	a->mult = 3;
	a->window_start = -1;
	a->late_base = 0;
	a->lates = 0;
	a->last_latch = -1;
	a->prev_latch = -1;
	a->pending = false;
	a->window_vbl = 2u * refresh_hz;   // 2 s
	a->probe_vbl = 20u * refresh_hz;   // 20 s
	a->backoff = 1;
	a->backoff_max = 8;
	a->probe_at = 0;                   // first clean window may try x2
	a->trial = false;
	a->late_limit = 0.03;
	a->n_switch = 0;
}

// A source frame was latched at vblank vbl.
static inline void kfg_auto_on_latch(kfg_auto *a, int64_t vbl)
{
	const int64_t m = (int64_t)a->mult;
	if (a->last_latch >= 0) {
		const int64_t iv = vbl - a->last_latch;
		const bool pair_long = a->prev_latch < 0 || vbl - a->prev_latch > 2 * m;
		if (a->pending) {
			// The long interval before this one: late unless this one gave the time back.
			if (pair_long)
				a->lates++;
			a->pending = false;
		}
		// Long, and not compensating an early frame before it: the next interval decides.
		if (iv > m && pair_long)
			a->pending = true;
	}
	a->prev_latch = a->last_latch;
	a->last_latch = vbl;
}

// The source stopped (pause, menu, focus change): its gap is not a late frame.
static inline void kfg_auto_reset_source(kfg_auto *a)
{
	a->last_latch = -1;
	a->prev_latch = -1;
	a->pending = false;
}

// Called once per vblank. Returns the multiplier (and so the source cap refresh / mult)
// for the following vblanks.
static inline uint32_t kfg_auto_step(kfg_auto *a, int64_t vbl)
{
	const uint64_t lates = a->lates;
	if (a->window_start < 0 || vbl < a->window_start) {
		a->window_start = vbl;
		a->late_base = lates;
		return a->mult;
	}
	const uint64_t late = lates - a->late_base;
	const double expected = (double)a->window_vbl / (double)a->mult;
	const bool bad = (double)late > a->late_limit * expected;
	// At x2 a window that is already over the limit cannot recover: lates only grow. Drop
	// back at once instead of at the end of the window, so a failed trial (a game locked
	// at refresh / 3 in its own settings, say) costs a few frames instead of two seconds.
	if (vbl - a->window_start < (int64_t)a->window_vbl && !(a->mult == 2 && bad))
		return a->mult;
	const uint32_t before = a->mult;

	if (a->mult == 2) {
		if (bad) {
			a->mult = 3;
			a->n_switch++;
			if (a->trial && a->backoff < a->backoff_max)
				a->backoff *= 2;
			a->trial = false;
			a->probe_at = vbl + (int64_t)a->probe_vbl * a->backoff;
		} else if (a->trial) {
			a->trial = false;  // x2 held for a whole window
			a->backoff = 1;
		}
	} else if (late == 0 && vbl >= a->probe_at) {
		a->mult = 2;
		a->n_switch++;
		a->trial = true;
	}
	if (a->mult != before)
		kfg_auto_reset_source(a);  // the first interval after a switch follows the old cap
	a->window_start = vbl;
	a->late_base = lates;
	return a->mult;
}

#endif
