"""Merge bench result files into one table (markdown) with the winner per scene.

usage: python3 summary.py results/*.json > table.md
"""
import json
import sys

COLS = [("flip", "FLIP"), ("flip_hard", "FLIP motion"), ("flip_hud", "FLIP HUD"), ("worst", "FLIP p95"),
        ("temporal", "temporal")]


def main():
    data = {}
    for path in sys.argv[1:]:
        for scene, methods in json.load(open(path)).items():
            for m, r in methods.items():
                data.setdefault(scene, {})[m] = r
    methods = sorted({m for s in data.values() for m in s},
                     key=lambda m: ["repeat", "blend", "te2", "fsr3", "lmc", "lmc2"].index(m)
                     if m in ["repeat", "blend", "te2", "fsr3", "lmc", "lmc2"] else 99)
    for key, title in COLS:
        print(f"\n### {title} (lower is better)\n")
        print("| scene | " + " | ".join(methods) + " |")
        print("|---|" + "---|" * len(methods))
        for scene in data:
            vals = {m: data[scene][m][key] for m in methods if m in data[scene]}
            best = min(vals.values()) if vals else None
            cells = []
            for m in methods:
                if m not in vals:
                    cells.append("—")
                else:
                    v = f"{vals[m]:.4f}"
                    cells.append(f"**{v}**" if vals[m] == best else v)
            print(f"| {scene} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
