#!/usr/bin/env python3
"""Render a conservative report from one GFG Native diagnostic capture."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import statistics
import re
from pathlib import Path
import tarfile


def load_logs(source: Path) -> dict[str, str]:
    files: dict[str, str] = {}
    if source.is_dir():
        for path in source.rglob("*"):
            if path.is_file() and path.stat().st_size <= 32 * 1024 * 1024:
                files[path.name] = path.read_text(errors="replace")
        return files
    with tarfile.open(source, "r:gz") as archive:
        for member in archive.getmembers():
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"unsafe path in diagnostic archive: {member.name}")
            if not member.isfile() or member.size > 32 * 1024 * 1024:
                continue
            stream = archive.extractfile(member)
            if stream is not None:
                files[path.name] = stream.read().decode(errors="replace")
    return files


def summarize_trace(text: str) -> dict:
    """API timestamps measured in the capture, never physical display FPS."""
    records, invalid = [], 0
    for line in text.splitlines():
        try:
            record = json.loads(line)
            if not isinstance(record, dict) or record.get("schema") != 1:
                raise ValueError("schema")
            records.append(record)
        except (ValueError, TypeError):
            invalid += 1
    pairs = [r for r in records if r.get("event") == "pair"]
    drops, detail = Counter(), Counter()
    rates = {"source": [], "real_wsi": [], "generated_wsi": [], "all_wsi": []}
    capture_age, mailbox_wait, admission_wait = [], [], []
    identities = set()
    errors = []
    for pair in pairs:
        try:
            identity = (int(pair["pid"]), int(pair["generation"]), int(pair["pair"]))
            if identity in identities:
                raise ValueError("duplicate pair identity")
            identities.add(identity)
            source = int(pair["source1_ns"])
            worker = int(pair["worker_start_ns"])
            admitted = int(pair["admitted_ns"])
            if source > worker or not source:
                raise ValueError("source timestamp after worker")
            capture_age.append((worker-source)/1e6)
            if admitted:
                mailbox_wait.append((worker-admitted)/1e6)
                admission_wait.append((admitted-source)/1e6)
            group = identity[:2]
            rates["source"].append((group, source))
            accepted = pair["wsi_accepted_ns"]
            for k in range(3):
                at = int(accepted[k])
                if at:
                    rates["all_wsi"].append((group, at))
                    if k == 2 or pair["drop_code"][k] == -1:
                        rates["real_wsi" if k == 2 else "generated_wsi"].append((group, at))
            for k in range(int(pair["multiplier"])-1 if pair.get("source0_id", 1) else 0):
                code = pair["drop_code"][k]
                if code != -1:
                    drops[str(code)] += 1
                    detail[pair.get("drop_detail", ["unknown", "unknown"])[k]] += 1
        except (ValueError, KeyError, TypeError, IndexError) as error:
            errors.append(str(error))
    def measured_rate(events):
        # Multiple processes/swapchains are reported separately, never merged
        # into one fictitious game FPS. Include each observed interval once.
        groups = {}
        for key, at in events:
            groups.setdefault(key, []).append(at)
        return [{"pid": key[0], "generation": key[1], "count": len(values),
                 "fps": (len(values)-1)*1e9/(max(values)-min(values)) if len(values)>1 and max(values)>min(values) else None}
                for key, values in sorted(groups.items())]
    def quantiles(values):
        if not values:
            return None
        ordered = sorted(values)
        return {"median": statistics.median(ordered), "p95": ordered[int(.95*(len(ordered)-1))], "max": ordered[-1]}
    gpu = [r for r in records if r.get("event") == "gpu_pair" and r.get("timed")]
    return {"scope": "source/API/WSI observations; no physical scanout or uniqueness proof",
            "pairs": len(pairs), "invalid_records": invalid, "invalid_pairs": errors,
            "rates": {key: measured_rate(events) for key, events in rates.items()},
            "drop_codes": dict(drops), "drop_details": dict(detail),
            "capture_to_worker_ms": quantiles(capture_age), "mailbox_wait_ms": quantiles(mailbox_wait),
            "capture_to_admission_ms": quantiles(admission_wait),
            "gpu_samples": len(gpu),
            "gpu_first_ready_ms": quantiles([g["ready_ms"][0] for g in gpu]),
            "gpu_last_ready_ms": quantiles([g["ready_ms"][int(g["outputs"])-1] for g in gpu]),
            "compositor_feedback": sum(r.get("event") == "compositor_feedback" for r in records),
            "physical_displayed_fps": None, "unique_displayed_fps": None}


def render(files: dict[str, str]) -> str:
    stderr = files.get("app.stderr.log", "")
    start = files.get("system-start.txt", "")
    status = files.get("process-status.txt", "unknown")
    active = "yes" if "GPU bridge active:" in stderr else "no / not observed"
    def last(pattern: str, text: str):
        matches = list(re.finditer(pattern, text))
        return matches[-1] if matches else None

    algo_match = last(r"GPU bridge active:.*?\balgo=([^\s]+)", stderr)
    retired = last(
        r"(?:GPU bridge|gpu-bridge) retired real=(\d+) synthetic=(\d+) skipped=(\d+).*?errors=(\d+).*?"
        r"present_path_host_pixel_reads=(\d+) present_path_cpu_waits=(\d+)", stderr)
    stats = last(
        r"gpu stats bridges=(\d+).*?real=(\d+) synthetic=(\d+) skipped=(\d+).*?errors=(\d+)",
        stderr + "\n" + files.get("app.stdout.log", ""))
    count = retired or stats
    present_done = re.findall(r"present_done id=(\d+) kind=([^ ]+) t_ns=(\d+)", stderr)
    intervals_us = []
    for previous, current in zip(present_done, present_done[1:]):
        a, b = int(previous[2]), int(current[2])
        if b > a:
            intervals_us.append((b - a) / 1000.0)
    timings = last(
        r"present_cpu frames=(\d+).*?total_median_us=([0-9.]+).*?total_p95_us=([0-9.]+).*?"
        r"layer_own_median_us=([0-9.]+).*?layer_own_p95_us=([0-9.]+)", stderr)
    worker = last(r"output-worker retired submitted=(\d+) completed=(\d+) stale=(\d+) backpressure=(\d+) error=(-?\d+)", stderr)
    app_cpu = last(r"present-timing path=gpu-async[^\n]*?frames=(\d+) median_us=([0-9.]+) p95_us=([0-9.]+) max_us=([0-9.]+)",stderr)
    gpu_line = last(r"gpu-bridge gpu_elapsed[^\n]*", stderr)
    gpu = re.search(
        r"samples=(\d+) held_samples=(\d+) output1_median_ms=([0-9.]+) output1_p95_ms=([0-9.]+) "
        r"output2_median_ms=([0-9.]+) output2_p95_ms=([0-9.]+)",
        gpu_line.group(0) if gpu_line else "")
    losses = last(r"gpu-bridge skipped_outputs ring=(\d+) te=(\d+) history=(\d+) "
                  r"acquire=(\d+) bypass=(\d+) error=(\d+) ring_reclaimed=(\d+).*?bridge_frames=(\d+)", stderr)

    lines = [
        "# GFG Native diagnostic report — NOT ACCEPTANCE",
        "",
        "This report summarizes application/WSI instrumentation only. It does not prove physical OLED scanout.",
        "",
        f"- Generator bridge initialized in captured log: **{active}**",
        f"- Algorithm: **{algo_match.group(1) if algo_match else 'not observed'}**",
        f"- Launched command exit code (may be a Steam wrapper): **{status.strip()}**",
        "- Physical display / KMS scanout: **unobserved**",
        "- 30 real → 90 distinct physical FPS acceptance: **false / not established**",
    ]
    if count:
        if retired:
            names = ("real presents", "synthetic presents", "skipped outputs", "errors",
                     "host pixel reads on present path", "CPU waits on present path")
            values = retired.groups()
        else:
            names = ("bridges", "real presents", "synthetic presents", "skipped outputs", "errors")
            values = stats.groups()
        synthetic = int(retired.group(2) if retired else stats.group(3))
        lines.append(f"- Synthetic WSI handoffs observed: **{'yes' if synthetic else 'no — zero generated outputs'}**")
        lines.extend(["", "## Latest captured cumulative counters", "",
                      "Latest process snapshot across swapchain recreations; earlier snapshots are not added again.", ""])
        lines.extend(f"- {name}: {value}" for name, value in zip(names, values))
        real = int(retired.group(1) if retired else stats.group(2))
        skipped = int(retired.group(3) if retired else stats.group(4))
        if real:
            lines.append(f"- API handoffs per real present: {(real + synthetic) / real:.3f}× (not displayed FPS)")
        if synthetic + skipped:
            lines.append(f"- Synthetic output opportunities handed to WSI: {100 * synthetic / (synthetic + skipped):.2f}%")
        if retired:
            events = re.search(r"te_busy=(\d+) ring_busy=(\d+) acquire_not_ready=(\d+) holds=(\d+)", retired.group(0))
            if events:
                lines.extend(["", "Backpressure event counts (events, not lost output counts):", ""])
                lines.extend(f"- {name}: {value}" for name, value in zip(
                    ("TE busy", "ring busy", "image acquire not ready", "history holds including initialization"), events.groups()))
        if synthetic == 0:
            lines.extend(["", "Bridge initialization succeeded, but generation was not observed. CPU passthrough timings do not measure generation cost."])
    bypass = list(dict.fromkeys(re.findall(r"fg_bypass reason=([^\s]+)", stderr)))
    if bypass:
        lines.extend(["", "## Captured bypass reasons", ""])
        lines.extend(f"- {reason}" for reason in bypass)
    if losses:
        lines.extend(["", "## Latest bridge output losses", "",
                      f"This bridge saw {losses.group(8)} real presents. These counts are per bridge, not process totals.", ""])
        lines.extend(f"- {name}: {value}" for name, value in zip(
            ("ring unavailable", "TE unavailable", "history restart after a gap", "image unavailable",
             "metadata/disabled bypass", "error recovery"), losses.groups()[:6]))
        lines.append(f"- Reservations rescued by selecting another free slot: {losses.group(7)}")
    if timings:
        lines.extend([
            "", "## Output worker CPU call time" if worker else "## CPU time inside layer", "",
            f"- Samples: {timings.group(1)}",
            f"- Full present call median / p95: {timings.group(2)} / {timings.group(3)} µs",
            f"- Layer-owned CPU median / p95: {timings.group(4)} / {timings.group(5)} µs",
            "- These are output-thread call timings, including its resource polling and downstream WSI. They are not the game's API-call time or display latency."
            if worker else "- These are API-path CPU timings, not end-to-end display latency.",
        ])
    if app_cpu:
        lines.extend(["", "## Game present-call enqueue/backpressure time", "",
                      f"- Samples: {app_cpu.group(1)}; median / p95 / max: {app_cpu.group(2)} / {app_cpu.group(3)} / {app_cpu.group(4)} µs",
                      "- Includes waiting for a bounded mailbox slot; excludes GPU completion and physical scanout."])
    if worker:
        lines.extend(["", "## Bounded output worker", "",
                      f"- Submitted / finished jobs: {worker.group(1)} / {worker.group(2)}",
                      f"- Stale jobs reset to real-only history: {worker.group(3)}",
                      f"- Mailbox backpressure events: {worker.group(4)}; deferred WSI error: {worker.group(5)}",
                      "- At most one queued and one executing job; unknown metadata drains earlier jobs before synchronous forwarding."])
    if gpu:
        lines.extend(["", "## Latest bridge GPU timestamp samples", "",
                      f"- Completed generated-pair samples: {gpu.group(1)}; held samples excluded: {gpu.group(2)}",
                      f"- Analysis start → first output ready, median / p95: {gpu.group(3)} / {gpu.group(4)} ms",
                      f"- Analysis start → second output ready, median / p95: {gpu.group(5)} / {gpu.group(6)} ms",
                      "- Sampled completed pairs, not an exhaustive trace. Elapsed GPU timestamps include queue/scheduling gaps; game rendering, transport copies and physical scanout are outside this interval."])
        stage_text = stderr[gpu_line.end():].split("GPU bridge active:", 1)[0]
        stages = re.findall(r"gpu_lmc_mean_ms stage=([^ ]+) ms=([0-9.]+) samples=(\d+)", stage_text)
        if stages:
            lines.extend(["", "LMC analysis stage means (overlap with the ready intervals above; do not add twice):", ""])
            lines.extend(f"- {name}: {ms} ms ({samples} samples)" for name, ms, samples in stages)
    else:
        lines.extend(["", "## GPU generation cost", "",
                      "- GPU timestamp samples unavailable in this capture. CPU present-call timings cannot establish GPU generation cost."])
    if len(present_done) > 1 and intervals_us:
        mean = sum(intervals_us) / len(intervals_us)
        fps = 1_000_000.0 / mean if mean else 0.0
        lines.extend([
            "", "## Optional Vulkan present-wait observations", "",
            f"- Completed IDs observed: {len(present_done)}",
            f"- Mean interval between present-wait completions: {mean:.1f} µs ({fps:.2f} completions/s)",
            "- Present-wait completion is not physical panel scanout.",
        ])
    else:
        app_owned = "present-ids policy=application-owned" in stderr
        lines.extend(["", "## Optional Vulkan present-wait observations", "",
                      "- Application owns present IDs; synthetic IDs and the layer's completion monitor are disabled. No completion cadence was measured."
                      if app_owned else
                      "- No `present_done` records were captured; completed monitored presents are not established (a monitor may have started without receiving IDs)."])
    trace = files.get("pairs.jsonl")
    if trace is not None:
        summary = summarize_trace(trace)
        lines.extend(["", "## Pair-level measured trace", "",
                      "Source FPS and accepted WSI rates below come from timestamps. No Real × multiplier estimate.",
                      "Drop codes: 0 ring; 1 TE busy; 2 history; 3 WSI acquire; 4 bypass; 5 error; 6 deadline; 7 cut; 8 display budget; 9 duplicate.",
                      "GPU durations are sampled device timestamps; readiness observations include CPU/queue waits.",
                      "```json", json.dumps(summary, indent=2), "```"])
    lines.extend(["", "## Device snapshot", "", "```text", start.rstrip(), "```", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path, help="diagnostic directory or .tar.gz archive")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = render(load_logs(args.capture))
    if args.output:
        args.output.write_text(report)
    else:
        print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
