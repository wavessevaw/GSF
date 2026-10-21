// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame-generation presentation scheduler.
//
// The scheduler runs inside the compositor once per display refresh. All
// times are integer vblank indices of one fixed-refresh output, because the
// compositor latches application commits only on its own vblank wakeups.
//
//   latch L_i      : vblank index at which source frame i was latched.
//   content c      : vblank index (in source time) that the next output shows.
//   delay d        : L_latest - c, how far the display runs behind the newest source.
//
// For each output vblank k the scheduler advances c by one when that content
// can be shown in time, and otherwise repeats the previous output (a missed
// cadence slot) without advancing c. The delay therefore settles at the
// smallest value at which every intermediate is ready, and grows by one
// vblank when the source arrives late. After a long stable streak in which
// the next content position would also have been ready, one intermediate is
// skipped to give that latency back. c never decreases, so displayed content
// is always temporally ordered.
//
// The header is self-contained C99/C++ so the same code runs in the
// compositor, in the simulator and in the unit tests.
#ifndef KFG_SCHED_H
#define KFG_SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFG_SCHED_RING 16u
#define KFG_MAX_MULTIPLIER 4u

enum kfg_show {
	KFG_SHOW_NONE = 0,  // nothing latched yet
	KFG_SHOW_REAL,      // an unmodified source frame
	KFG_SHOW_GEN,       // generated intermediate between src_a and src_b
	KFG_SHOW_REPEAT,    // previous output repeated: a missed cadence slot
};

enum kfg_repeat_reason {
	KFG_REPEAT_NONE = 0,
	KFG_REPEAT_SOURCE_LATE,   // the source frame that bounds this content is not latched
	KFG_REPEAT_ME_LATE,       // motion analysis for the pair is not complete
	KFG_REPEAT_CADENCE,       // source interval longer than the multiplier can fill
	KFG_REPEAT_WARMUP,        // building the delay after enabling generation
};

enum kfg_state {
	KFG_STATE_OFF = 0,        // passthrough: newest frame, no added latency
	KFG_STATE_ACTIVE,         // generating
	KFG_STATE_DEGRADED,       // generation suspended because analysis cannot keep up
};

typedef struct kfg_sched_config {
	uint32_t multiplier;        // 1 = off, 2 = x2, 3 = x3
	uint32_t catchup_streak;    // stable vblanks required before giving latency back
	uint32_t max_delay;         // vblanks; larger delay resynchronises to the newest source
	uint32_t me_lag_budget;     // vblanks from latch to usable analysis that are tolerated
	uint32_t degrade_misses;    // sources over budget within the last degrade_window sources
	uint32_t degrade_window;    // that suspend generation (<= 64)
	uint32_t recover_vblanks;   // suspension length before generation is retried
	uint32_t max_fill_span;     // late source: fill intervals up to this many vblanks with
	                            // one intermediate per vblank; longer ones quantise to 1/multiplier
} kfg_sched_config;

typedef struct kfg_source_slot {
	uint64_t id;
	int64_t latch;       // vblank index of the latch
	int64_t me_ready;    // first vblank whose decision may use the pair (prev, this); INT64_MAX = pending
	bool me_ok;          // analysis succeeded (false: failure, show reals only for this pair)
	bool cut;            // scene cut between prev and this
	bool valid;
} kfg_source_slot;

typedef struct kfg_decision {
	enum kfg_show show;
	enum kfg_repeat_reason reason;
	enum kfg_state state;
	uint64_t src_a;      // REAL: the frame. GEN: earlier endpoint.
	uint64_t src_b;      // GEN: later endpoint.
	uint32_t phase_num;  // GEN: phase = num / den in (0, 1)
	uint32_t phase_den;
	int64_t content;     // content vblank index shown (unchanged for REPEAT)
	int64_t delay;       // L_latest - content after this decision
	bool skipped;        // this decision gave back one vblank of latency
} kfg_decision;

typedef struct kfg_sched {
	kfg_sched_config cfg;
	kfg_source_slot ring[KFG_SCHED_RING];
	uint32_t head;          // next write position
	uint32_t count;
	int64_t content;        // last shown content index; INT64_MIN before any output
	int64_t last_vblank;
	uint64_t last_gen_pair; // src_b of the last generated pair (cadence de-duplication)
	uint32_t last_gen_phase_num;
	uint32_t ahead_streak;
	int64_t wall_ref;       // established k - content of the steady cadence; -1 unknown
	int64_t realign_k;      // vblank of the last realignment
	enum kfg_state state;
	bool want_on;
	bool warming;           // generation enabled, first intermediate not shown yet
	int64_t degraded_until;
	uint8_t me_late_hist[64];  // per-source ring: analysis over budget
	uint32_t me_late_pos;
	uint32_t me_late_count;
	bool degrade_pending;
	// Counters for telemetry.
	uint64_t n_real, n_gen, n_repeat, n_skip, n_resync, n_degrade;
	uint64_t n_repeat_reason[5];
} kfg_sched;

static inline kfg_sched_config kfg_sched_default_config(uint32_t multiplier)
{
	kfg_sched_config c;
	c.multiplier = multiplier;
	c.catchup_streak = 90;   // 1 s at 90 Hz
	c.max_delay = 12;
	c.me_lag_budget = 2;  // analysis is split over two vblanks after the latch
	c.degrade_misses = 8;
	c.degrade_window = 32;
	c.recover_vblanks = 180;
	c.max_fill_span = 2u * multiplier + 2u;
	return c;
}

static inline void kfg_sched_init(kfg_sched *s, const kfg_sched_config *cfg)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	if (s->cfg.multiplier > KFG_MAX_MULTIPLIER)
		s->cfg.multiplier = KFG_MAX_MULTIPLIER;
	if (s->cfg.degrade_window > 64)
		s->cfg.degrade_window = 64;
	s->content = INT64_MIN;
	s->last_vblank = INT64_MIN;
	s->wall_ref = -1;
	s->realign_k = INT64_MIN / 2;
	s->want_on = s->cfg.multiplier > 1;
	s->state = KFG_STATE_OFF;
}

static inline const kfg_source_slot *kfg_sched_slot(const kfg_sched *s, uint32_t back)
{
	// back = 0 is the newest latched source.
	if (back >= s->count)
		return NULL;
	return &s->ring[(s->head + KFG_SCHED_RING - 1u - back) % KFG_SCHED_RING];
}

static inline kfg_source_slot *kfg_sched_find(kfg_sched *s, uint64_t id)
{
	for (uint32_t i = 0; i < s->count; i++) {
		kfg_source_slot *slot = &s->ring[(s->head + KFG_SCHED_RING - 1u - i) % KFG_SCHED_RING];
		if (slot->id == id)
			return slot;
	}
	return NULL;
}

// Forget history: called on enable/disable, refresh change, resize, suspend.
static inline void kfg_sched_reset_history(kfg_sched *s)
{
	s->count = 0;
	s->head = 0;
	s->ahead_streak = 0;
	s->last_gen_pair = 0;
	s->last_gen_phase_num = 0;
	s->wall_ref = -1;
	s->warming = true;
}

// Source frame `id` was latched at vblank `vbl`. Latches must be strictly increasing.
// Returns false (and ignores the frame) when the latch is not newer than the last one.
static inline bool kfg_sched_on_latch(kfg_sched *s, uint64_t id, int64_t vbl)
{
	const kfg_source_slot *newest = kfg_sched_slot(s, 0);
	if (newest && vbl <= newest->latch)
		return false;
	kfg_source_slot *slot = &s->ring[s->head];
	slot->id = id;
	slot->latch = vbl;
	slot->me_ready = INT64_MAX;
	slot->me_ok = false;
	slot->cut = false;
	slot->valid = true;
	s->head = (s->head + 1u) % KFG_SCHED_RING;
	if (s->count < KFG_SCHED_RING)
		s->count++;
	return true;
}

// Motion analysis for the pair ending at `id` completes in time for decisions at
// vblank >= ready_vbl. ok=false marks analysis failure; cut=true marks a scene cut.
static inline void kfg_sched_on_me(kfg_sched *s, uint64_t id, int64_t ready_vbl, bool ok, bool cut)
{
	kfg_source_slot *slot = kfg_sched_find(s, id);
	if (!slot)
		return;
	slot->me_ready = ready_vbl;
	slot->me_ok = ok;
	slot->cut = cut;
	if (s->state != KFG_STATE_ACTIVE)
		return;
	const uint32_t w = s->cfg.degrade_window ? s->cfg.degrade_window : 1u;
	const bool late = !ok || ready_vbl - slot->latch > (int64_t)s->cfg.me_lag_budget;
	const uint32_t idx = s->me_late_pos++ % w;
	if (s->me_late_hist[idx])
		s->me_late_count--;
	s->me_late_hist[idx] = late ? 1u : 0u;
	if (late)
		s->me_late_count++;
	if (s->me_late_count >= s->cfg.degrade_misses)
		s->degrade_pending = true;
}

// Change the multiplier at runtime. 1 disables generation.
static inline void kfg_sched_set_multiplier(kfg_sched *s, uint32_t multiplier)
{
	if (multiplier > KFG_MAX_MULTIPLIER)
		multiplier = KFG_MAX_MULTIPLIER;
	s->cfg.multiplier = multiplier;
	s->cfg.max_fill_span = 2u * multiplier + 2u;
	s->cfg.me_lag_budget = 2;
	s->want_on = multiplier > 1;
	// The steady delay depends on the multiplier; rebuild it.
	s->wall_ref = -1;
	s->warming = true;
}

// Locate the source interval containing content position c.
// Returns the index (back) of the earlier endpoint, or -1 when c is newer than the newest latch
// or older than retained history.
static inline int kfg_sched_locate(const kfg_sched *s, int64_t c, const kfg_source_slot **a, const kfg_source_slot **b)
{
	*a = *b = NULL;
	for (uint32_t back = 0; back < s->count; back++) {
		const kfg_source_slot *slot = kfg_sched_slot(s, back);
		if (slot->latch <= c) {
			*a = slot;
			*b = back ? kfg_sched_slot(s, back - 1u) : NULL;
			return (int)back;
		}
	}
	return -1;
}

typedef struct kfg_candidate {
	bool ok;
	enum kfg_show show;
	enum kfg_repeat_reason reason;
	uint64_t src_a, src_b;
	uint32_t num, den;
} kfg_candidate;

// Can content position c be shown at the decision for vblank k?
static inline kfg_candidate kfg_sched_eval(const kfg_sched *s, int64_t c, int64_t k)
{
	kfg_candidate r;
	memset(&r, 0, sizeof(r));
	const kfg_source_slot *a, *b;
	if (kfg_sched_locate(s, c, &a, &b) < 0) {
		r.reason = KFG_REPEAT_SOURCE_LATE;
		return r;
	}
	if (a->latch == c) {
		r.ok = true;
		r.show = KFG_SHOW_REAL;
		r.src_a = a->id;
		return r;
	}
	if (!b) {
		// c lies after the newest latch: the bounding frame has not arrived.
		r.reason = KFG_REPEAT_SOURCE_LATE;
		return r;
	}
	const int64_t span = b->latch - a->latch;
	const int64_t off = c - a->latch;
	if (b->me_ready > k) {
		r.reason = KFG_REPEAT_ME_LATE;
		return r;
	}
	if (b->cut || !b->me_ok) {
		// No valid motion: hold the nearer real frame instead of blending across a cut.
		r.ok = true;
		r.show = KFG_SHOW_REAL;
		r.src_a = (2 * off < span) ? a->id : b->id;
		return r;
	}
	const uint32_t n = s->cfg.multiplier;
	uint32_t num = (uint32_t)off, den = (uint32_t)span;
	if (span > (int64_t)n && span > (int64_t)s->cfg.max_fill_span) {
		// Interval longer than the multiplier can fill: quantise to multiples of 1/n.
		const uint32_t j = (uint32_t)((off * (int64_t)n + span / 2) / span);
		if (j == 0 || j >= n) {
			r.reason = KFG_REPEAT_CADENCE;
			return r;
		}
		num = j;
		den = n;
	}
	r.ok = true;
	r.show = KFG_SHOW_GEN;
	r.src_a = a->id;
	r.src_b = b->id;
	r.num = num;
	r.den = den;
	return r;
}

// Decide the output for vblank k (strictly increasing between calls).
static inline kfg_decision kfg_sched_decide(kfg_sched *s, int64_t k)
{
	kfg_decision d;
	memset(&d, 0, sizeof(d));
	const kfg_source_slot *newest = kfg_sched_slot(s, 0);
	s->last_vblank = k;

	if (!newest) {
		d.show = KFG_SHOW_NONE;
		d.state = s->state;
		d.content = s->content;
		return d;
	}

	// State transitions.
	if (!s->want_on) {
		s->state = KFG_STATE_OFF;
	} else if (s->state == KFG_STATE_ACTIVE && s->degrade_pending) {
		// Analysis cannot keep up: stop generating rather than keep producing late frames.
		s->state = KFG_STATE_DEGRADED;
		s->degraded_until = k + (int64_t)s->cfg.recover_vblanks;
		s->degrade_pending = false;
		s->n_degrade++;
	} else if (s->state == KFG_STATE_DEGRADED) {
		if (k >= s->degraded_until) {
			s->state = KFG_STATE_ACTIVE;
			s->warming = true;
			s->wall_ref = -1;
			memset(s->me_late_hist, 0, sizeof(s->me_late_hist));
			s->me_late_count = 0;
			s->me_late_pos = 0;
		}
	} else if (s->state == KFG_STATE_OFF) {
		s->state = KFG_STATE_ACTIVE;
		s->ahead_streak = 0;
		s->warming = true;
		s->wall_ref = -1;
	}

	if (s->state != KFG_STATE_ACTIVE) {
		// Passthrough: newest frame. Content only moves forward.
		d.state = s->state;
		if (s->content != INT64_MIN && newest->latch <= s->content) {
			d.show = KFG_SHOW_REPEAT;
			d.reason = KFG_REPEAT_SOURCE_LATE;
			s->n_repeat++;
		} else {
			d.show = KFG_SHOW_REAL;
			d.src_a = newest->id;
			s->content = newest->latch;
			s->n_real++;
		}
		d.content = s->content;
		d.delay = newest->latch - s->content;
		return d;
	}

	d.state = KFG_STATE_ACTIVE;
	if (s->content == INT64_MIN)
		s->content = newest->latch - 1; // first output shows the newest frame

	// Too far behind (long stall, suspend): resynchronise forward.
	if (newest->latch - s->content > (int64_t)s->cfg.max_delay) {
		s->content = newest->latch - 1;
		s->ahead_streak = 0;
		s->wall_ref = -1;
		s->warming = true;
		s->n_resync++;
	}

	int64_t c = s->content + 1;
	kfg_candidate cand;
	memset(&cand, 0, sizeof(cand));
	// Realign with the wall clock. When the display fell further behind than its established
	// cadence (it waited for a late source or analysis, or a late flip displaced one decision),
	// show the position the steady cadence shows now, so the hitch the viewer already saw does
	// not leave extra latency behind. Fall back to smaller jumps when that is not ready yet.
	bool realigned = false;
	if (s->wall_ref >= 0 && !s->warming) {
		for (int64_t t = k - s->wall_ref; t > c; t--) {
			cand = kfg_sched_eval(s, t, k);
			if (cand.ok) {
				c = t;
				realigned = true;
				s->realign_k = k;
				d.skipped = true;
				break;
			}
		}
	}
	if (!realigned)
		cand = kfg_sched_eval(s, c, k);

	// Latency give-back: after a long streak in which c+1 was also ready, skip one
	// intermediate (never a real frame).
	if (cand.ok) {
		kfg_candidate next = kfg_sched_eval(s, c + 1, k);
		if (next.ok)
			s->ahead_streak++;
		else
			s->ahead_streak = 0;
		if (s->ahead_streak >= s->cfg.catchup_streak && cand.show == KFG_SHOW_GEN) {
			c += 1;
			cand = next;
			s->ahead_streak = 0;
			d.skipped = true;
			s->n_skip++;
		}
	} else {
		s->ahead_streak = 0;
	}

	if (!cand.ok && s->warming && cand.reason == KFG_REPEAT_SOURCE_LATE)
		cand.reason = KFG_REPEAT_WARMUP;

	if (!cand.ok && s->wall_ref >= 0 && (cand.reason == KFG_REPEAT_SOURCE_LATE || cand.reason == KFG_REPEAT_ME_LATE) &&
	    k - s->realign_k <= 2 * (int64_t)s->cfg.multiplier + 2) {
		// The last realignment was too aggressive for current conditions: back off one vblank.
		if (s->wall_ref < (int64_t)s->cfg.max_delay)
			s->wall_ref++;
		s->realign_k = INT64_MIN / 2;
	}

	if (!cand.ok) {
		d.show = KFG_SHOW_REPEAT;
		d.reason = cand.reason;
		s->n_repeat++;
		s->n_repeat_reason[cand.reason]++;
	} else if (cand.show == KFG_SHOW_GEN && cand.src_b == s->last_gen_pair && cand.num == s->last_gen_phase_num) {
		// Same quantised phase as the previous output (cadence mismatch): count as a repeat
		// but let content advance so the display does not fall behind.
		s->content = c;
		d.show = KFG_SHOW_REPEAT;
		d.reason = KFG_REPEAT_CADENCE;
		s->n_repeat++;
		s->n_repeat_reason[KFG_REPEAT_CADENCE]++;
	} else {
		s->content = c;
		d.show = cand.show;
		d.src_a = cand.src_a;
		d.src_b = cand.src_b;
		d.phase_num = cand.num;
		d.phase_den = cand.den;
		if (cand.show == KFG_SHOW_GEN) {
			s->last_gen_pair = cand.src_b;
			s->last_gen_phase_num = cand.num;
			s->warming = false;
			s->n_gen++;
		} else {
			s->n_real++;
		}
	}
	if (cand.reason == KFG_REPEAT_CADENCE && !cand.ok)
		s->content = c; // content advances on a cadence hole
	if (cand.ok && !s->warming && (s->wall_ref < 0 || k - s->content < s->wall_ref || realigned || d.skipped))
		s->wall_ref = k - s->content;
	d.state = s->state;
	d.content = s->content;
	d.delay = newest->latch - s->content;
	return d;
}

static inline const char *kfg_show_name(enum kfg_show s)
{
	switch (s) {
	case KFG_SHOW_REAL: return "real";
	case KFG_SHOW_GEN: return "gen";
	case KFG_SHOW_REPEAT: return "repeat";
	default: return "none";
	}
}

static inline const char *kfg_reason_name(enum kfg_repeat_reason r)
{
	switch (r) {
	case KFG_REPEAT_SOURCE_LATE: return "source_late";
	case KFG_REPEAT_ME_LATE: return "me_late";
	case KFG_REPEAT_CADENCE: return "cadence";
	case KFG_REPEAT_WARMUP: return "warmup";
	default: return "";
	}
}

static inline const char *kfg_state_name(enum kfg_state s)
{
	switch (s) {
	case KFG_STATE_ACTIVE: return "active";
	case KFG_STATE_DEGRADED: return "degraded";
	default: return "off";
	}
}

#ifdef __cplusplus
}
#endif
#endif
