#!/usr/bin/env python3
"""Standalone GSF X11/Mesa Vulkan testing (NOT physical Steam Deck scanout).

Exercises TEST-ONLY loader-installed layer and real Vulkan WSI with X11
pixel sampling. Intermediate images are split A/B samples, not full LMC.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
VALIDATION_ERROR = re.compile(r"Validation Error|VUID-|UNASSIGNED-.*ERROR")

# Reuse EXACTLY the asserted observable evidence from the existing output-
# owner workflow, including post-X11-composition pixel checks.
CASES = (
    ("real-only", (), (
        "GSF_LOADER_LIVE_APP 8 GPU-rendered real source frames visible through one output WSI PASS",
    )),
    ("gpu-a-b-split", ("GFG_NATIVE_TEST_OUTPUT_INTERLEAVE",), (
        "GSF_LOADER_LIVE_INIT virtual_images=3 one_real_output=1 GPU_worker=1",
        "GSF_LOADER_SPLIT_APP output_visible=15 original=8 intermediate=7 x11_distinct_pixels=1 PASS",
        "GSF_LOADER_LIVE AB_SPLIT output_present=14 originals=7 intermediate=7",
        "GSF_LOADER_LIVE ORIGINAL output_present=15 originals=8 intermediate=7",
    )),
    ("paced-30-to-60", ("GFG_NATIVE_TEST_OUTPUT_INTERLEAVE", "GFG_NATIVE_TEST_PACED_30_60"), (
        "GSF_PACED_30_TO_60_X11 paced_source=8 originals=8 intermediate=7 visible=15 app_to_worker_distinct_frames=1 PASS",
        "GSF_OUTPUT_PACER rate=60 emitted=15 originals=8 intermediate=7",
        "GSF_LOADER_SPLIT_APP output_visible=15 original=8 intermediate=7 x11_distinct_pixels=1 PASS",
    )),
    ("paced-45-to-90", ("GFG_NATIVE_TEST_OUTPUT_INTERLEAVE", "GFG_NATIVE_TEST_PACED_45_90"), (
        "GSF_PACED_45_TO_90_X11 paced_source=8 originals=8 intermediate=7 visible=15 app_to_worker_distinct_frames=1 PASS",
        "GSF_OUTPUT_PACER rate=90 emitted=15 originals=8 intermediate=7",
    )),
    ("sustained-45-to-90", ("GFG_NATIVE_TEST_OUTPUT_INTERLEAVE", "GFG_NATIVE_TEST_PACED_45_90", "GFG_NATIVE_TEST_SUSTAINED"), (
        "GSF_PACED_45_TO_90_X11 paced_source=120 originals=120 intermediate=119 visible=239 app_to_worker_distinct_frames=1 PASS",
        "GSF_OUTPUT_PACER rate=90 emitted=239 originals=120 intermediate=119",
        "GSF_LOADER_SPLIT_APP output_visible=239 original=120 intermediate=119 x11_distinct_pixels=1 PASS",
    )),
)

def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def lavapipe():
    configured = os.environ.get("VK_ICD_FILENAMES")
    if configured:
        path = Path(configured)
        if not path.is_file():
            raise RuntimeError("Missing pinned ICD: " + configured)
        return path
    files = sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
    files = [f for f in files if "x86_64" in f.name] or files
    if not files:
        raise RuntimeError("No lavapipe ICD: install mesa-vulkan-drivers")
    return files[0]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--profile", choices=("short", "sustained"), default="sustained")
    p.add_argument("--layer-order", choices=("native-first", "validation-first"), default="native-first")
    p.add_argument("--build-dir", default="engine/gfg-native/build-lab")
    p.add_argument("--output", default="gsf-lab-results")
    p.add_argument("--timeout", type=int, default=45)
    args = p.parse_args()
    if args.timeout < 5 or args.timeout > 600:
        p.error("timeout must be 5..600 seconds")
    logs = Path(args.output).resolve()
    logs.mkdir(parents=True, exist_ok=True)
    binary_dir = (ROOT / args.build_dir).resolve()
    layer_dir = binary_dir / "loader-probe"
    binary = binary_dir / "loader-live-output"
    layer = layer_dir / "libVkLayer_gfg_native.so"
    manifest = layer_dir / "VkLayer_gfg_native.json"
    result = {
        "type": "software_vulkan_x11_test_lab",
        "date_utc": datetime.now(timezone.utc).isoformat(),
        "sha": os.environ.get("GITHUB_SHA", "local"),
        "system": platform.platform(),
        "profile": args.profile,
        "layer_order": args.layer_order,
        "physical_oled_scanout": "UNVERIFIED",
        "gamescope_commit": "UNVERIFIED",
        "proton_game_compatibility": "UNVERIFIED",
        "gpu_lmc_pixel_quality": "NOT_TESTED",
        "cases": [],
        "status": "FAIL"
    }
    try:
        icd = lavapipe()
        result["icd"] = str(icd)
        for executable in ("timeout", "xvfb-run"):
            if not shutil.which(executable):
                raise RuntimeError("Missing executable: " + executable)
        for path in (binary, layer, manifest):
            if not path.is_file():
                raise RuntimeError("Build first, missing " + str(path))
        result["artifact_sha256"] = {str(path.relative_to(ROOT)): sha256(path) for path in (binary, layer, manifest)}
        result["icd_sha256"] = sha256(icd)
        for name, flags, expected in (CASES if args.profile == "sustained" else CASES[:-1]):
            env = dict(os.environ)
            env.update({
                "GFG_NATIVE_FG": "1",
                "GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT": "1",
                "GFG_NATIVE_TEST_PROXY_CREATE": "1",
                "GFG_NATIVE_TRACE": "1",
                "VK_LAYER_PATH": str(layer_dir) + ":/usr/share/vulkan/explicit_layer.d",
                "VK_ICD_FILENAMES": str(icd),
                "VK_INSTANCE_LAYERS": (
                    "VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation"
                    if args.layer_order == "native-first"
                    else "VK_LAYER_KHRONOS_validation:VK_LAYER_GFG_native"
                )
            })
            # No accidental inheritance of another scenario's switches.
            for _, all_flags, _ in CASES:
                for flag in all_flags:
                    env.pop(flag, None)
            for flag in flags:
                env[flag] = "1"
            command = ["timeout", str(args.timeout) + "s", "xvfb-run", "-a", "-s",
                       "-screen 0 1024x768x24", str(binary)]
            started = time.monotonic()
            try:
                completed = subprocess.run(
                    command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, errors="replace",
                    timeout=args.timeout + 10, check=False)
                output = completed.stdout
                exitcode = completed.returncode
            except subprocess.TimeoutExpired as exc:
                output = ((exc.stdout or b"").decode(errors="replace")
                          if isinstance(exc.stdout, bytes) else (exc.stdout or ""))
                output += "\nGSF_LAB_PYTHON_TIMEOUT\n"
                exitcode = 124
            elapsed = round((time.monotonic() - started) * 1000)
            (logs / (name + ".log")).write_text(output, encoding="utf-8")
            missing = [marker for marker in expected if marker not in output]
            errors = VALIDATION_ERROR.findall(output)
            ok = exitcode == 0 and not missing and not errors
            result["cases"].append({
                "case": name,
                "status": "PASS" if ok else "FAIL",
                "exit_code": exitcode,
                "elapsed_ms": elapsed,
                "missing_markers": missing,
                "vulkan_validation_errors": len(errors),
                "log": name + ".log",
            })
            print("GSF_LAB case={} status={} elapsed_ms={} exit={}".format(
                name, "PASS" if ok else "FAIL", elapsed, exitcode), flush=True)
            if not ok:
                print("\n".join(output.splitlines()[-65:]), file=sys.stderr)
        if result["cases"] and all(c["status"] == "PASS" for c in result["cases"]):
            result["status"] = "PASS"
    except Exception as exc:
        result["setup_error"] = "{}: {}".format(type(exc).__name__, exc)
        print("GSF_LAB_SETUP_ERROR " + result["setup_error"], file=sys.stderr)
    finally:
        (logs / "results.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        lines = [
            "# GSF Autonomous Test Lab",
            "",
            "**Mesa lavapipe + X11 test ONLY. Not Gamescope, Steam Deck OLED or true LMC quality.**",
            "",
            "Verdict: **{}**. Layer order: {}. Profile: {}.".format(
                result["status"], args.layer_order, args.profile),
            "",
            "| Scenario | Result | Elapsed |", "|---|---|---:|"
        ]
        for case in result["cases"]:
            lines.append("| {} | {} | {} ms |".format(
                case["case"], case["status"], case["elapsed_ms"]))
        if "setup_error" in result:
            lines.extend(("", "Setup error: " + result["setup_error"]))
        text = "\n".join(lines) + "\n"
        (logs / "SUMMARY.md").write_text(text, encoding="utf-8")
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as file:
                file.write(text)
    return 0 if result["status"] == "PASS" else 1

if __name__ == "__main__":
    sys.exit(main())
