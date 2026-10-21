#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame pacing analyzer for KEDAVRA flip traces.

Reads one CSV row per display refresh, as written by the patched compositor
(times from DRM page-flip events) or by kfg_sim. Computes the acceptance
metrics of the Operation KEDAVRA directive, section 4.2:

  * P95 / P99 absolute presentation interval error between consecutive
    newly presented frames (target interval = one refresh),
  * fraction of refresh slots that did not present a new frame (missed),
  * recurring short/long oscillation,
  * temporal reordering of content,
  * latency (flip - source GPU completion) and its drift over the run,
  * generated outputs per source interval (proof that x2 shows one and x3
    shows two distinct intermediates).

The analyzer never fills in missing data: a metric whose inputs are absent is
reported as null. Whether the trace came from hardware or a model is recorded
in the report only from what the trace says about itself (`source` column or
--origin), never inferred.
"""
import argparse
import csv
import json
import math
import sys
from collections import Counter, defaultdict

LIMITS = {
    "p95_interval_error_ms": 1.0,
    "p99_interval_error_ms": 2.0,
    "missed_fraction": 0.005,
    "oscillation_fraction": 0.005,
    "reorders": 0,
    "latency_drift_ms": None,  # set from refresh: one refresh period
}


def pct(values, q):
    if not values:
        return None
    v = sorted(values)
    pos = (len(v) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return v[lo]
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


def median(values):
    return pct(values, 0.5)


def load_trace(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def load_sources(path):
    done = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            done[int(r["id"])] = float(r["done_ns"])
    return done


def in_windows(t_s, windows):
    return any(a <= t_s < b for a, b in windows)


def analyze(rows, sources=None, skip_s=2.0, exclude=(), expect_mult=None, only_active=True):
    if not rows:
        return {"error": "empty trace"}
    t0 = float(rows[0]["flip_ns"])
    sel = []
    for r in rows:
        t_s = (float(r["flip_ns"]) - t0) / 1e9
        if t_s < skip_s or in_windows(t_s, exclude):
            continue
        if only_active and r.get("state", "active") != "active":
            continue
        sel.append(r)
    if len(sel) < 3:
        return {"error": "too few rows in evaluation window", "rows": len(sel)}

    refresh_ns = median([float(r["refresh_ns"]) for r in sel])
    n = len(sel)
    kinds = Counter(r["kind"] for r in sel)
    reasons = Counter(r["reason"] for r in sel if r["kind"] == "repeat")
    new = [r for r in sel if r["kind"] in ("real", "gen")]

    # Interval error between consecutive newly presented frames. Consecutive rows
    # must be adjacent in the selection (an excluded window breaks the chain).
    idx = {id(r): i for i, r in enumerate(sel)}
    errors, devs = [], []
    for a, b in zip(new, new[1:]):
        dt = float(b["flip_ns"]) - float(a["flip_ns"])
        if dt > 20 * float(b["refresh_ns"]) or a["refresh_ns"] != b["refresh_ns"]:
            continue  # chain broken by an exclusion window or a state change
        target = float(b["refresh_ns"])
        errors.append(abs(dt - target) / 1e6)
        devs.append((dt - target) / 1e6)

    # Recurring short/long alternation: consecutive deviations of opposite sign above 1 ms.
    osc = sum(1 for x, y in zip(devs, devs[1:]) if abs(x) > 1.0 and abs(y) > 1.0 and x * y < 0)

    # Temporal order: content index of new frames strictly increases.
    reorders = 0
    steps = Counter()
    prev = None
    for r in new:
        c = int(r["content"])
        if prev is not None:
            if c <= prev:
                reorders += 1
            steps[c - prev] += 1
        prev = c

    # Intermediates per source interval (by later endpoint), excluding the first and last
    # intervals which may be cut by the evaluation window.
    per_pair = defaultdict(set)
    for r in new:
        if r["kind"] == "gen":
            per_pair[int(r["src_b"])].add((int(r["phase_num"]), int(r["phase_den"])))
    pairs = sorted(per_pair)[1:-1]
    gen_hist = Counter(len(per_pair[p]) for p in pairs)
    # Pairs whose source interval equals the multiplier (cadence-locked intervals).
    locked = [p for p in pairs if expect_mult and all(den == expect_mult for _, den in per_pair[p])]
    locked_hist = Counter(len(per_pair[p]) for p in locked)
    phases = Counter()
    for p in pairs:
        for num, den in per_pair[p]:
            phases[f"{num}/{den}"] += 1

    lat = []
    if sources:
        for r in new:
            if r["kind"] == "real":
                sid = int(r["src_a"])
                if sid in sources:
                    lat.append(((float(r["flip_ns"]) - sources[sid]) / 1e6, (float(r["flip_ns"]) - t0) / 1e9))
    lat_ms = [x for x, _ in lat]
    drift = None
    if len(lat) >= 30:
        third = len(lat) // 3
        drift = median(lat_ms[-third:]) - median(lat_ms[:third])

    missed = kinds.get("repeat", 0) / n
    report = {
        "rows": n,
        "duration_s": (float(sel[-1]["flip_ns"]) - float(sel[0]["flip_ns"])) / 1e9,
        "refresh_hz": 1e9 / refresh_ns,
        "kinds": dict(kinds),
        "repeat_reasons": dict(reasons),
        "missed_fraction": missed,
        "interval_error_ms": {
            "p50": pct(errors, 0.5), "p95": pct(errors, 0.95), "p99": pct(errors, 0.99), "max": max(errors) if errors else None,
        },
        "oscillation_fraction": osc / max(1, len(devs) - 1),
        "reorders": reorders,
        "content_steps": {str(k): v for k, v in sorted(steps.items())},
        "generated_per_interval": {str(k): v for k, v in sorted(gen_hist.items())},
        "generated_per_locked_interval": {str(k): v for k, v in sorted(locked_hist.items())},
        "locked_interval_fraction": len(locked) / len(pairs) if pairs and expect_mult else None,
        "content_step_irregular_fraction": (sum(v for k, v in steps.items() if k != 1) / max(1, sum(steps.values()))),
        "phases": dict(phases),
        "latency_ms": {"p50": median(lat_ms), "p95": pct(lat_ms, 0.95), "p99": pct(lat_ms, 0.99)} if lat_ms else None,
        "latency_drift_ms": drift,
        "skips": sum(1 for r in sel if r.get("skipped") == "1"),
        "compose_late": sum(1 for r in sel if r.get("compose_late") == "1"),
    }
    checks = {
        "p95_interval_error": report["interval_error_ms"]["p95"] is not None and report["interval_error_ms"]["p95"] <= LIMITS["p95_interval_error_ms"],
        "p99_interval_error": report["interval_error_ms"]["p99"] is not None and report["interval_error_ms"]["p99"] <= LIMITS["p99_interval_error_ms"],
        "missed_below_0.5pct": missed < LIMITS["missed_fraction"],
        "no_oscillation": report["oscillation_fraction"] < LIMITS["oscillation_fraction"],
        "no_reorder": reorders == 0,
        "uniform_content_steps": report["content_step_irregular_fraction"] < 0.005,
    }
    if drift is not None:
        checks["no_latency_accumulation"] = abs(drift) <= refresh_ns / 1e6
    if expect_mult:
        want = expect_mult - 1
        total = sum(locked_hist.values())
        checks[f"x{expect_mult}_intermediates_99.5pct"] = total > 0 and locked_hist.get(want, 0) / total >= 0.995
        checks["cadence_locked_99.5pct"] = bool(pairs) and len(locked) / len(pairs) >= 0.995
    report["checks"] = checks
    report["pass"] = all(checks.values())
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--sources", help="source CSV (id,done_ns,...) for latency")
    ap.add_argument("--skip", type=float, default=2.0, help="warm-up seconds excluded")
    ap.add_argument("--exclude", action="append", default=[], help="A:B seconds excluded (transitions)")
    ap.add_argument("--expect-mult", type=int)
    ap.add_argument("--all-states", action="store_true", help="include passthrough/degraded rows")
    ap.add_argument("--origin", default=None, help="label recorded in the report, e.g. 'model' or 'deck-kms'")
    ap.add_argument("--json", help="write report JSON here")
    ap.add_argument("--assert-pass", action="store_true", help="exit 1 unless every check passes")
    a = ap.parse_args()
    rows = load_trace(a.trace)
    sources = load_sources(a.sources) if a.sources else None
    windows = []
    for w in a.exclude:
        lo, hi = w.split(":")
        windows.append((float(lo), float(hi)))
    rep = analyze(rows, sources, a.skip, windows, a.expect_mult, not a.all_states)
    rep["origin"] = a.origin or (rows[0].get("origin") if rows and rows[0].get("origin") else "unlabelled")
    out = json.dumps(rep, indent=2, sort_keys=True)
    if a.json:
        with open(a.json, "w") as f:
            f.write(out + "\n")
    print(out)
    if a.assert_pass and not rep.get("pass"):
        sys.exit(1)


if __name__ == "__main__":
    main()
