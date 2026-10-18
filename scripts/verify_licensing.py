#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guardrails against accidentally deleting GSF's existing license notices.

This is a presence check, not a legal audit or an SPDX/REUSE classifier.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    "LICENSE.md": (
        "GPL-3.0-or-later",
        "# GNU GENERAL PUBLIC LICENSE",
        "Version 3, 29 June 2007",
        "END OF TERMS AND CONDITIONS",
        "# Third-party notices",
        "Eugenio Segala",
        "lsfg-vk renderer lineage",
        "toml++",
        "Steam Deck Homebrew components",
    ),
    "COPYRIGHT.md": (
        "The project does not claim ownership of third-party code.",
        "1b600fbc4e6a03a9616fd65ee2f5b16f5224da00",
    ),
    "NOTICE.md": ("MAKO", "AFMF-Linux", "FidelityFX", "BSD-3-Clause"),
    "TRADEMARKS.md": ("not", "GPL", "independent"),
    "CONTRIBUTING.md": (
        "GPL-3.0-or-later",
        "git commit -s",
        "not",
    ),
    "README.md": ("LICENSE.md", "COPYRIGHT.md", "NOTICE.md", "TRADEMARKS.md"),
    "docs/LICENSING.ru.md": ("GPL-3.0-or-later", "автор"),
    "MIGRATION.md": ("GFG Extreme", "234", "source"),
    "engine/gfg-native/src/layer.c": ("SPDX-License-Identifier: GPL-3.0-or-later",),
}

failures = []
for relative_path, markers in REQUIRED.items():
    path = ROOT / relative_path
    if not path.is_file():
        failures.append(f"missing: {relative_path}")
        continue
    data = path.read_text(encoding="utf-8")
    for marker in markers:
        if marker not in data:
            failures.append(f"{relative_path}: absent marker {marker!r}")

# Historic third-party notice contents must still be embedded after the
# unchanged full-text GNU GPL, not replaced by a short summary/link.
license_text = (ROOT / "LICENSE.md").read_text(encoding="utf-8") if (ROOT / "LICENSE.md").exists() else ""
if license_text.count("Copyright (c)") < 3:
    failures.append("LICENSE.md: historical MIT/BSD attribution sections reduced")
if license_text.find("END OF TERMS AND CONDITIONS") > license_text.find("# Third-party notices"):
    failures.append("LICENSE.md: GPL full text must precede third-party notices")
if failures:
    for failure in failures:
        print("LICENSING ERROR:", failure, file=sys.stderr)
    sys.exit(1)
print("GSF license/notice safeguard PASS (presence checks only, not legal clearance)")
