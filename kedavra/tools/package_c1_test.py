#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Label an already-built experimental C1 package; never publish or install it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import zipfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.plugin.is_file():
        parser.error("built plugin ZIP is missing")
    head = os.environ.get("C1_HEAD_SHA", "")
    build = os.environ.get("GITHUB_SHA", "")
    if len(head) != 40 or len(build) != 40 or any(c not in "0123456789abcdef" for c in head + build):
        parser.error("full Actions build and head SHAs are required")
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": 1, "experiment": "c1-post-history-v1",
        "hardware_runtime_version": 1,
        "features": ["c1_post_history", "direct_scan_sdr_sync0", "completion_deadline_analysis"],
        "diagnostic_logging_schema": 1,
        "head_commit": head, "build_commit": build,
        "base_release": "1.4.1", "official_release": False,
        "default_enabled": False, "scope": "SDR generation with active post filters",
        "plugin_controls": True,
        "controls": {
            "install": "Setup > Install / Reinstall Hardware Runtime > Reboot now",
            "filters": "Setup > Image filters",
            "enable": "Setup > C1 direct history: On",
            "disable": "Setup > C1 direct history: Off",
            "direct_scan": "Setup > DirectScan",
            "deadline": "Setup > Deadline-aware analysis",
            "logs": "Log / Setup > Save report to Desktop",
            "restore_stock": "Setup > Remove and restore stock Gamescope > Reboot now",
        },
        "hardware_performance_measured": False,
    }
    target = args.out / ("KEDAVRA-HardwareRuntime-test-" + head[:12] + ".zip")
    shutil.copyfile(args.plugin, target)
    with zipfile.ZipFile(target, "a", compression=zipfile.ZIP_DEFLATED) as archive:
        if "KEDAVRA/EXPERIMENT.json" in archive.namelist():
            parser.error("experimental manifest already exists")
        archive.writestr("KEDAVRA/EXPERIMENT.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        archive.writestr("KEDAVRA/runtime/EXPERIMENT.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        bad = archive.testzip()
        if bad:
            parser.error("corrupt ZIP member: " + bad)
    (args.out / "BUILD-INFO.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    readme = Path(__file__).resolve().parents[2] / "docs/kedavra/experiments/HARDWARE-RUNTIME.md"
    shutil.copyfile(readme, args.out / "README.ru.md")
    names = [target.name, "BUILD-INFO.json", "README.ru.md"]
    (args.out / "SHA256SUMS").write_text("".join(
        hashlib.sha256((args.out / name).read_bytes()).hexdigest() + "  " + name + "\n"
        for name in names), encoding="utf-8")
    print(target)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
