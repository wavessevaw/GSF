#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Same-host historical A/B/A and candidate ablation; no Steam Deck claims."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics as st
import subprocess
import time

BASELINE = "5f22938b55f9853f764e927c847d9c1a8d9a8c04"
METRICS = ("vulkan_elapsed_ns", "wall_ns", "record_ns", "submit_wait_ns", "pass_sum_ns")


def run(command, log, timeout=300):
    print("RUN", " ".join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout, check=False)
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {command}; see {log}")
    return result.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def block_summary(directory):
    groups = {}
    with (directory / "samples.csv").open() as stream:
        for row in csv.DictReader(stream):
            groups.setdefault(row["case"], []).append(row)
    summary = {}
    for case, rows in groups.items():
        summary[case] = {metric: st.median(float(r[metric]) for r in rows) for metric in METRICS}
        summary[case]["passes"] = int(rows[0]["passes"])
        assert len({r["passes"] for r in rows}) == 1
        summary[case]["samples"] = len(rows)
        summary[case]["p95_vulkan_elapsed_ns"] = sorted(float(r["vulkan_elapsed_ns"]) for r in rows)[int(.95 * (len(rows) - 1))]
    hashes = {}
    for image in sorted(directory.glob("*.rgba")):
        hashes[image.name] = sha(image)
        image.unlink()  # Raw timing + cryptographic pixel hashes, not GBs of repeated images.
    (directory / "pixel-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    return summary, hashes


def delta_stats(values):
    rng = random.Random(141)
    boot = sorted(st.median(rng.choices(values, k=len(values))) for _ in range(10000))
    return {"median": st.median(values), "min": min(values), "max": max(values),
            "bootstrap95": [boot[249], boot[9749]], "blocks": values}


def compare(blocks, size, case, metric):
    old, new, historical, ablation, drift = [], [], [], [], []
    for cycle in blocks:
        a1 = cycle["A1"][case][metric]
        a2 = cycle["A2"][case][metric]
        b0 = cycle["B0"][case][metric]
        b = cycle["B"][case][metric]
        a = (a1 + a2) / 2
        old.append(a)
        new.append(b)
        historical.append(a - b)
        ablation.append(b0 - b)
        drift.append(a2 - a1)
    return {"size": size, "case": case, "metric": metric,
            "baseline_median_ns": st.median(old), "candidate_median_ns": st.median(new),
            "saved_ns": delta_stats(historical), "same_candidate_saved_ns": delta_stats(ablation),
            "baseline_drift_ns": delta_stats(drift),
            "saved_percent": 100 * st.median(historical) / st.median(old)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True, type=Path)
    ap.add_argument("--candidate", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--samples", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=9)
    args = ap.parse_args()
    args.baseline = args.baseline.resolve()
    args.candidate = args.candidate.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    baseline_sha = capture(["git", "-C", str(args.baseline), "rev-parse", "HEAD"])
    assert baseline_sha == BASELINE, baseline_sha
    metadata = {"baseline": baseline_sha,
                "candidate": capture(["git", "-C", str(args.candidate), "rev-parse", "HEAD"]),
                "platform": platform.platform(), "cpu": capture(["lscpu"]),
                "compiler": capture(["cc", "--version"]), "vulkan": capture(["vulkaninfo", "--summary"]),
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "environment": {k: os.environ.get(k) for k in ("LP_NUM_THREADS", "OMP_NUM_THREADS", "VK_ICD_FILENAMES", "MESA_SHADER_CACHE_DISABLE", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT")},
                "cycles": args.cycles, "samples_per_case_block": args.samples,
                "warmup_per_case_block": args.warmup,
                "order": "A1(old legacy), B0(new legacy), B(new optimized), A2(old legacy)"}
    (out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    # Exact source and SPIR-V identities are controls, not inferred from version names.
    source_hashes = {}
    for label, root in (("baseline", args.baseline), ("candidate", args.candidate)):
        files = list((root / "kedavra/build").glob("*.spv"))
        files += [root / "kedavra/engine" / f for f in ("kfg_vk.c", "kfg_vk.h", "kfg_plan.h", "kfg_post.h")]
        files += [root / "kedavra/sched/kfg_sched.h", root / "kedavra/gamescope/src/kfg/kfg.cpp"]
        source_hashes[label] = {str(p.relative_to(root)): sha(p) for p in files}
    (out / "source-sha256.json").write_text(json.dumps(source_hashes, indent=2) + "\n")
    for key in source_hashes["baseline"]:
        if key.endswith(".spv") or key.endswith(("kfg_vk.c", "kfg_vk.h", "kfg_plan.h", "kfg_sched.h")):
            assert source_hashes["baseline"][key] == source_hashes["candidate"][key], key

    # Both original quality suites, normal and lowbase; compare actual JSON metrics.
    correctness = {}
    for lowbase in (False, True):
        suffix = "lowbase" if lowbase else "normal"
        for label, root in (("baseline", args.baseline), ("candidate", args.candidate)):
            target = out / f"correctness-{label}-{suffix}.json"
            command = [root / "kedavra/build/kfg_vktest", "--spv", root / "kedavra/build",
                       "--size", "1280x800", "--json", target]
            if lowbase:
                command += ["--lowbase"]
            run(command, out / f"correctness-{label}-{suffix}.log", timeout=300)
            data = json.loads(target.read_text())
            assert data["failures"] == 0
            correctness[label, suffix] = data
        assert correctness["baseline", suffix] == correctness["candidate", suffix], suffix
    for label, root in (("baseline", args.baseline), ("candidate", args.candidate)):
        run([root / "kedavra/build/kfg_posttest", "--spv", root / "kedavra/build"], out / f"posttest-{label}.log")
        run([root / "kedavra/build/compare_sched"], out / f"completion-{label}.csv")

    # Existing scheduler matrix is a negative control: it has no GPU completion adapter.
    run(["python3", args.candidate / "kedavra/sched/compare_pacing.py", "--baseline",
         args.baseline / "kedavra/build/kfg_sim", "--candidate", args.candidate / "kedavra/build/kfg_sim",
         "--out", out / "pacing-negative-control.json"], out / "pacing-negative-control.log")
    pacing = json.loads((out / "pacing-negative-control.json").read_text())
    assert all(item["baseline"] == item["new"] for item in pacing)

    result = {"metadata": metadata, "blocks": {}, "comparisons": [], "pixel_checks": 0}
    for w, h in ((320, 200), (1280, 800)):
        size = f"{w}x{h}"
        blocks = []
        for cycle in range(args.cycles):
            summaries = {}
            hashes = {}
            for label, root, variant in (("A1", args.baseline, "legacy"), ("B0", args.candidate, "legacy"),
                                         ("B", args.candidate, "optimized"), ("A2", args.baseline, "legacy")):
                directory = out / f"{size}-cycle{cycle + 1}-{label}"
                directory.mkdir()
                run([root / "kedavra/build/kfg_compare", root / "kedavra/build", variant, w, h,
                     args.samples, args.warmup, directory, label], directory / "process.log", timeout=300)
                summaries[label], hashes[label] = block_summary(directory)
            for label in ("B0", "B", "A2"):
                assert hashes[label] == hashes["A1"], (size, cycle, label, "pixel difference")
                result["pixel_checks"] += len(hashes[label])
            for case in summaries["A1"]:
                a_pass = summaries["A1"][case]["passes"]
                assert summaries["B0"][case]["passes"] == a_pass
                removed = 1 if case in ("sharp", "vivid", "smooth", "display_copy_proxy") else 0
                assert summaries["B"][case]["passes"] == a_pass - removed
            blocks.append(summaries)
        result["blocks"][size] = blocks
        for case in blocks[0]["A1"]:
            for metric in METRICS:
                result["comparisons"].append(compare(blocks, size, case, metric))
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")

    report = ["# Сравнение Hardware Runtime с Kedavra 1.4.1 вне Steam Deck", "",
              f"Baseline: `{baseline_sha}`. Candidate: `{metadata['candidate']}`.", "",
              "Ubuntu GitHub Actions, software Vulkan lavapipe/llvmpipe, два worker threads; один host, последовательные прогоны. "
              "Это измеренные времена этой CPU Vulkan-среды, не AMD GPU и не прогноз Steam Deck.", "",
              f"{args.cycles} блоков A1/B0/B/A2 на размер; {args.warmup} warm-up и {args.samples} измерений на case/block. "
              "A1/A2 — исходная 1.4.1, B0 — новый engine с legacy path, B — новый engine с оптимизированным path. "
              "Baseline блока = среднее медиан A1/A2; итог — медиана блоков. В CSV сохранены все samples.", "",
              "Vulkan elapsed — timestamp от начала первого dispatch до конца последнего, включая промежуточные barriers. "
              "Wall — CPU begin/record/submit/wait; uploads/readbacks, allocations и pixel hashing исключены из окна. "
              "CPU record и submit/wait сохранены раздельно. Никакой параллельной нагрузки из нашего job нет; host общий для CI.", "",
              "## Измеренные времена", "",
              "Положительная экономия означает ускорение. 95% bootstrap interval строится по пяти блокам (разведочная оценка, не аппаратная гарантия). "
              "Дрейф A2−A1 и negative controls помогают видеть шум; изменившийся контроль не объявляется оптимизацией.", "",
              "| Размер | Case | 1.4.1, ms | Новый, ms | Экономия, ms | % | 95% interval экономии, ms | B0−B, ms | A2−A1, ms |",
              "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for r in result["comparisons"]:
        if r["metric"] != "vulkan_elapsed_ns":
            continue
        lo, hi = r["saved_ns"]["bootstrap95"]
        report.append(f"| {r['size']} | {r['case']} | {r['baseline_median_ns']/1e6:.3f} | "
                      f"{r['candidate_median_ns']/1e6:.3f} | {r['saved_ns']['median']/1e6:+.3f} | {r['saved_percent']:+.1f}% | "
                      f"[{lo/1e6:+.3f}; {hi/1e6:+.3f}] | {r['same_candidate_saved_ns']['median']/1e6:+.3f} | "
                      f"{r['baseline_drift_ns']['median']/1e6:+.3f} |")
    report += ["", "## Проверка результата и границы интерпретации", "",
               f"- {result['pixel_checks']} сравнений SHA256 readback-изображений: исходная 1.4.1 = новый legacy = новый optimized = повтор исходной. "
               "Проверяются RGBA целиком и три фазы каждого case/block/size, не только счётчики.",
               "- Original quality suites normal/lowbase: обе версии PASS, JSON quality metrics идентичны; post suites обеих версий PASS.",
               "- SPIR-V всех shaders, kfg_vk.c/.h, kfg_plan.h и kfg_sched.h идентичны. Analysis и warp без filters — negative controls, их изменение времени является шумом.",
               "- C1 cases sharp/vivid/smooth: реальные production post plans + тот же compute kfg_copy, что RecordCopy, либо production kfg_plan_post_into. "
               "Снято ровно одно dispatch; resource formats одинаковы. Это не полный Gamescope frame benchmark.",
               "- display_copy_proxy: warp + identity compute copy против warp. Этот проход является proxy переноса полного SDR изображения; "
               "он НЕ выполняет Gamescope BLIT/color management, импорт modifiers или DRM commit. Его экономию нельзя выдавать за измеренный выигрыш DirectScan.",
               "- На 1280×800 RGBA8 удалённый copy имеет логический payload 4 096 000 B read + 4 096 000 B write = 8 192 000 B/frame. "
               "Это расчёт обращений изображения, не измерение DRAM bandwidth: cache/compression/tiling не учтены. Дополнительная память pool DirectScan здесь не моделируется.",
               "- Старый scheduler matrix (5 scenarios × 3 seeds × 180 s) идентичен: он не моделирует новый completion adapter. "
               "Нет заявленного повышения FPS из изменения scheduler kernel.", "", "## Контролируемые задержки completion", "",
               "Production scheduler обеих версий получает одинаковые latch/finish traces. Legacy bridge сообщает latch+2; новый bridge сообщает наблюдаемое completion. "
               "Это воспроизводимый model counterexample корректности готовности, не GPU timing, не физически показанные кадры и не обещание более высокого FPS.", "",
               "| × | Completion, vblanks | Premature GEN: 1.4.1 → новый | GEN decisions: 1.4.1 → новый | Repeat: 1.4.1 → новый |",
               "|---|---:|---:|---:|---:|"]
    with (out / "completion-baseline.csv").open() as f:
        old_sched = list(csv.DictReader(f))
    with (out / "completion-candidate.csv").open() as f:
        new_sched = list(csv.DictReader(f))
    for a, b in zip(old_sched, new_sched):
        assert a["multiplier"] == b["multiplier"] and a["completion_delay_vblanks"] == b["completion_delay_vblanks"]
        report.append(f"| {a['multiplier']} | {a['completion_delay_vblanks']} | {a['premature_generated']} → {b['premature_generated']} | "
                      f"{a['generated']} → {b['generated']} | {a['repeats']} → {b['repeats']} |")
    report += ["", "## Воспроизведение", "", "Workflow `.github/workflows/kedavra-compare.yml` закрепляет исходную 1.4.1 по SHA, "
               "собирает один harness против обеих версий с одинаковыми `cc -O2`, запускает `compare_versions.py`. "
               "Среда, hashes, CSV каждого блока, correctness JSON, completion CSV и исходный отчёт находятся рядом с этим файлом.", "",
               "GPU submission-to-start delay, KMS pacing, физический scanout, энергопотребление и эффект deadline admission "
               "на реальную очередь этой средой не измеряются. Нулевой результат и регрессии сохраняются в таблице."]
    (out / "REPORT.ru.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("PASS: comparisons, exact pixels, original quality suites and controls", flush=True)


if __name__ == "__main__":
    main()
