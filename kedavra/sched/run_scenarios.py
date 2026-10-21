#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the KEDAVRA scheduling model over the directive's stress matrix.

Each scenario runs the compositor model (kfg_sim, which links the production
scheduler header) for the stated duration and several seeds, then evaluates
the trace with the same analyzer used for Steam Deck KMS traces.

What this proves: the scheduling policy meets the pacing targets *under the
modelled timing assumptions* (listed per scenario). What it does not prove:
anything about real Steam Deck timing. The output is labelled origin=model.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import kfg_pacing  # noqa: E402

# Common modelled Deck timing assumptions (see docs/kedavra/PHASE_C_PIPELINE.md):
# analysis median 2.0 ms / p99 3.5 ms, warp 0.45 ms, composition median 0.55 ms / p99 0.9 ms,
# compositor redzone 1.65 ms, 8 % game slowdown while generating.
SCENARIOS = [
    # --- Phase D / E target modes, stable input, 10 minutes each --------------------------------
    dict(name="x3_30to90_stable", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0",
         duration=600, mult=3, require="pass"),
    dict(name="x2_45to90_stable", args="--refresh 90 --mult 2 --cap 45 --game-ms 14 --game-p99-ms 18 --spike-prob 0",
         duration=600, mult=2, require="pass"),
    dict(name="x2_30to60_stable", args="--refresh 60 --mult 2 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0",
         duration=600, mult=2, require="pass"),
    dict(name="x2_40to80_stable", args="--refresh 80 --mult 2 --cap 40 --game-ms 16 --game-p99-ms 20 --spike-prob 0",
         duration=600, mult=2, require="pass"),
    # --- 30-minute extended stability runs for the critical modes --------------------------------
    dict(name="x3_30to90_30min", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0",
         duration=1800, mult=3, require="pass", seeds=1),
    dict(name="x2_45to90_30min", args="--refresh 90 --mult 2 --cap 45 --game-ms 14 --game-p99-ms 18 --spike-prob 0",
         duration=1800, mult=2, require="pass", seeds=1),
    # --- Stress matrix (directive 8.2) ----------------------------------------------------------
    # Occasional long game frames: misses are caused by the source; ordering and latency must hold.
    dict(name="x3_game_spikes", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0.003 --spike-ms 40",
         duration=600, mult=3, require=["no_reorder", "no_latency_accumulation", "no_oscillation"]),
    # Heavy motion scenes cost more analysis; analysis p99 rises to 6 ms.
    dict(name="x3_heavy_analysis", args="--refresh 90 --mult 3 --cap 30 --game-ms 22 --game-p99-ms 27 --me-ms 3.5 --me-p99-ms 6 --spike-prob 0",
         duration=600, mult=3, require="pass"),
    # Scene transitions: 1 % of pairs are cuts, never blended.
    dict(name="x3_scene_cuts", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0 --cut-prob 0.01",
         duration=600, mult=3, require=["no_reorder", "missed_below_0.5pct", "p95_interval_error", "p99_interval_error"]),
    # Sudden source FPS drop: the game cannot hold 30 for 10 s, then recovers.
    dict(name="x3_fps_drop", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0 --event 60:game_ms:38 --event 70:game_ms:20",
         duration=180, mult=3, exclude=[(58, 80)], require="pass"),
    # Toggle generation off and on during play.
    dict(name="toggle_off_on", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0 --event 60:off:0 --event 90:on:0",
         duration=180, mult=3, exclude=[(58, 95)], require="pass"),
    # Switch x3 (30 fps cap) to x2 (45 fps cap) and back.
    dict(name="switch_x3_x2", args="--refresh 90 --mult 3 --cap 30 --game-ms 13 --game-p99-ms 17 --spike-prob 0 --event 60:mult:2 --event 60:cap:45 --event 120:mult:3 --event 120:cap:30",
         duration=180, mult=None, exclude=[(58, 65), (118, 125)], require=["no_reorder", "missed_below_0.5pct", "p95_interval_error", "p99_interval_error"]),
    # Display refresh change 90 -> 60 Hz (x3 30 fps -> x2 30 fps).
    dict(name="refresh_90_to_60", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0 --event 60:refresh:60 --event 60:mult:2",
         duration=180, mult=None, exclude=[(58, 65)], require=["no_reorder", "missed_below_0.5pct", "p95_interval_error", "p99_interval_error"]),
    # Insufficient compute: analysis becomes 6x slower for 30 s. Must degrade to passthrough,
    # not present late frames, and recover afterwards.
    dict(name="insufficient_compute", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0 --event 60:me_scale:20 --event 90:me_scale:1",
         duration=180, mult=3, exclude=[(58, 100)], require="pass", expect_degrade=True),
    # Suspend / resume: 5 s without game frames.
    dict(name="suspend_resume", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0 --event 60:gap:5000",
         duration=180, mult=3, exclude=[(58, 70)], require="pass"),
    # Uncapped game at ~40 fps on 90 Hz with x3: not cadence-locked. Expected to FAIL the
    # locked-cadence check; it documents why the source cap must be refresh / multiplier.
    dict(name="uncapped_40fps_x3_informative", args="--refresh 90 --mult 3 --cap 0 --game-ms 25 --game-p99-ms 30 --spike-prob 0",
         duration=120, mult=3, require=["no_reorder"], informative=True),
]


def run(sim, sc, seed, outdir):
    trace = os.path.join(outdir, f"{sc['name']}_s{seed}.csv")
    srcs = os.path.join(outdir, f"{sc['name']}_s{seed}_src.csv")
    cmd = [sim] + sc["args"].split() + ["--duration", str(sc["duration"]), "--seed", str(seed), "--trace", trace, "--sources", srcs]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"{cmd}: {p.stderr}")
    rows = kfg_pacing.load_trace(trace)
    sources = kfg_pacing.load_sources(srcs)
    rep = kfg_pacing.analyze(rows, sources, skip_s=2.0, exclude=sc.get("exclude", ()), expect_mult=sc.get("mult"))
    # Global invariant over every state (passthrough, degraded, transitions): no reordering.
    full = kfg_pacing.analyze(rows, None, skip_s=0.0, exclude=(), expect_mult=None, only_active=False)
    rep["all_states_reorders"] = full.get("reorders")
    rep["degraded_rows"] = sum(1 for r in rows if r["state"] == "degraded")
    rep["sim_counts"] = p.stderr.strip()
    return rep, trace


def verdict(sc, rep):
    req = sc["require"]
    checks = rep.get("checks", {})
    if req == "pass":
        ok = bool(rep.get("pass"))
    else:
        ok = all(checks.get(c, False) for c in req)
    ok = ok and rep.get("all_states_reorders") == 0
    if sc.get("expect_degrade"):
        ok = ok and rep["degraded_rows"] > 0
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--out", required=True, help="summary JSON")
    ap.add_argument("--keep-traces", help="directory for raw traces")
    ap.add_argument("--only")
    a = ap.parse_args()
    outdir = a.keep_traces or tempfile.mkdtemp(prefix="kfg_scen_")
    os.makedirs(outdir, exist_ok=True)
    summary = {"origin": "model", "note": "kfg_sim compositor model; not a hardware measurement", "scenarios": []}
    all_ok = True
    for sc in SCENARIOS:
        if a.only and a.only not in sc["name"]:
            continue
        for seed in range(1, sc.get("seeds", a.seeds) + 1):
            rep, trace = run(a.sim, sc, seed, outdir)
            ok = verdict(sc, rep)
            if not sc.get("informative"):
                all_ok &= ok
            ie = rep.get("interval_error_ms", {})
            lat = rep.get("latency_ms") or {}
            line = (f"{'PASS' if ok else 'FAIL'} {sc['name']:30s} seed={seed} missed={rep.get('missed_fraction', 0):.4%} "
                    f"p95={ie.get('p95', float('nan')):.3f}ms p99={ie.get('p99', float('nan')):.3f}ms "
                    f"lat50={lat.get('p50') or 0:.1f}ms drift={rep.get('latency_drift_ms') or 0:.2f}ms "
                    f"reorder={rep.get('all_states_reorders')} degraded_rows={rep['degraded_rows']}")
            print(line, flush=True)
            summary["scenarios"].append({"name": sc["name"], "seed": seed, "args": sc["args"], "duration_s": sc["duration"],
                                         "required": sc["require"], "informative": bool(sc.get("informative")),
                                         "verdict": ok, "report": rep})
    summary["all_required_pass"] = all_ok
    with open(a.out, "w") as f:
        json.dump(summary, f, indent=1, sort_keys=True)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
