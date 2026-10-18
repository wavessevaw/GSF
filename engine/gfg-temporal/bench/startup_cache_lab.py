#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure actual GPU LMC startup with isolated Mesa disk-cache states.

Direct Temporal Engine compute, not Native WSI or physical display acceptance.
No performance threshold: correctness is strict; cold/warm costs are observations.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time

import numpy as np

PAIRS = 4
WIDTH, HEIGHT = 96, 64
PASSES = ("search_b", "search_a", "layers", "gpu_model", "field", "masked", "orphan", "field2")
PROFILE = re.compile(r"init_profile phase=(\S+) pipeline=(-?\d+) variant=(-?\d+) host_ms=([\d.]+) result=(-?\d+)")


def make_sequence():
    """Small textured integer translation, with exact intermediate ground truth."""
    y, x = np.mgrid[:HEIGHT, :WIDTH]
    base = np.stack(((x*13+y*7)%256, (x*3+y*17)%256, (x*19+y*5)%256), axis=-1).astype(np.uint8)
    return np.stack([np.roll(base, k, axis=1) for k in range(3*PAIRS+1)])


def validate_rows(rows):
    if len(rows) != PAIRS:
        raise ValueError(f"expected {PAIRS} complete GPU pairs, got {len(rows)}")
    for k, row in enumerate(rows):
        if row.get("algorithm") != 2 or row.get("outputs") != 2 or row.get("timed") != 1:
            raise ValueError("missing real two-output GPU LMC timestamps")
        if row.get("pair") != k or row.get("a") != k*3 or row.get("b") != (k+1)*3:
            raise ValueError("unordered or wrong source pair")
        values = [row.get(n) for n in ("cpu_submit_ms", "cpu_total_ms", "first_ready_ms", "pair_ready_ms", "pyramid_ms")]
        passes = row.get("lmc_ms", {})
        values += [passes.get(n) for n in PASSES]
        if any(not isinstance(v, (float, int)) or not math.isfinite(v) or v < 0 for v in values):
            raise ValueError("missing/invalid GPU or host stage time")
        if row["first_ready_ms"] > row["pair_ready_ms"] or row["cpu_submit_ms"] > row["cpu_total_ms"]:
            raise ValueError("invalid readiness/host timing order")


def summarize(rows, log, wall_ms):
    validate_rows(rows)
    events = [{"phase":p, "pipeline":int(i), "variant":int(v), "host_ms":float(ms), "result":int(r)}
              for p, i, v, ms, r in PROFILE.findall(log)]
    contexts = [e for e in events if e["phase"] == "context-total"]
    if len(contexts) != 1 or any(e["result"] != 0 for e in events):
        raise ValueError("missing successful context initialization profile")
    first = rows[0]
    stages = dict(first["lmc_ms"], pyramid=first["pyramid_ms"],
                  output1=first["frames"][0]["total_ms"], output2=first["frames"][1]["total_ms"])
    return {"wall_ms":round(wall_ms, 3), "context_host_ms":contexts[0]["host_ms"],
            "first_pair_host_ms":first["cpu_total_ms"], "first_pair_gpu_ms":first["pair_ready_ms"],
            "later_pair_gpu_median_ms":statistics.median(r["pair_ready_ms"] for r in rows[1:]),
            "largest_first_gpu_stage":max(stages, key=stages.get), "first_gpu_stages_ms":stages,
            "init_events":events}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1]/"build/te-bench")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sequence = make_sequence()
    np.save(out/"source.npy", sequence)
    report = {"scope":"direct GPU LMC startup, not WSI/Deck performance", "status":"FAIL", "runs":{}}
    # Fresh directory every invocation; never clear a caller's existing cache.
    with tempfile.TemporaryDirectory(prefix="mesa-cache-", dir=out) as cache:
        for label, disabled in (("empty", False), ("reused", False), ("disabled", True)):
            case = out/label
            case.mkdir(exist_ok=True)
            env = dict(os.environ, MESA_SHADER_CACHE_DIR=cache,
                       MESA_SHADER_CACHE_DISABLE="true" if disabled else "false",
                       MESA_SHADER_CACHE_SHOW_STATS="true", GSF_TE_PROFILE_INIT="1")
            # Avoid inherited layer overrides: this lab explicitly requests validation
            # through te-bench; the native layer is covered separately in the WSI suite.
            env.pop("VK_INSTANCE_LAYERS", None)
            cmd = [str(args.binary.resolve()), "--frames", str(out/"source.npy"), "--pairs", str(PAIRS),
                   "--step", "3", "--algo", "lmc-gpu", "--tier", "0", "--profile", "--validation",
                   "--out", str(case/"generated.npy"), "--json", str(case/"pairs.jsonl")]
            started = time.monotonic()
            try:
                proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, timeout=args.timeout, check=False)
                log = proc.stdout
                (case/"process.log").write_text(log)
                if proc.returncode or re.search(r"Validation Error|VUID-|UNASSIGNED-.*ERROR", log):
                    raise ValueError(f"process or Vulkan validation failure: exit {proc.returncode}")
                if "validation errors 0" not in log:
                    raise ValueError("missing validation completion counter")
                rows = [json.loads(line) for line in (case/"pairs.jsonl").read_text().splitlines()]
                result = summarize(rows, log, (time.monotonic()-started)*1000)
                generated = np.load(case/"generated.npy", allow_pickle=False)
                if generated.shape != (2*PAIRS, HEIGHT, WIDTH, 3) or generated.dtype != np.uint8:
                    raise ValueError("missing generated GPU pixel outputs")
                # Exact distinctness is a minimum anti-hold check, not a quality score.
                for k in range(PAIRS):
                    g1, g2 = generated[2*k:2*k+2]
                    if np.array_equal(g1, g2) or any(np.array_equal(g, sequence[s])
                            for g in (g1,g2) for s in (3*k,3*(k+1))):
                        raise ValueError(f"pair {k}: duplicated output or endpoint hold")
                result["pixel_sha256"] = hashlib.sha256(generated.tobytes()).hexdigest()
                result["cache_files_after"] = sum(p.is_file() for p in Path(cache).rglob("*"))
                result["status"] = "PASS"
                report["runs"][label] = result
                print(label, json.dumps({k:v for k,v in result.items() if k != "init_events"}), flush=True)
            except Exception as error:
                if isinstance(error, subprocess.TimeoutExpired):
                    (case/"process.log").write_text((error.stdout or b"").decode(errors="replace")
                        if isinstance(error.stdout, bytes) else (error.stdout or ""))
                report["runs"][label] = {"status":"FAIL", "error":repr(error)}
                break
        runs = report["runs"]
        if len(runs) == 3 and all(v["status"] == "PASS" for v in runs.values()):
            report["cache_pixel_equivalence"] = len({v["pixel_sha256"] for v in runs.values()}) == 1
            report["status"] = "PASS" if report["cache_pixel_equivalence"] else "FAIL"
    (out/"results.json").write_text(json.dumps(report, indent=2)+"\n")
    print("GSF_LMC_STARTUP_CACHE", report["status"], flush=True)
    if report["status"] != "PASS":
        print(json.dumps(report, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
