#!/usr/bin/env bash
# Render the bench scenes at 1280x800, 90 FPS (Movie Maker, fixed timestep).
# usage: render.sh <godot binary> <out dir> [scene...]
set -euo pipefail
godot=$1; out=$2; shift 2
scenes=("$@"); [ ${#scenes[@]} -gt 0 ] || scenes=(pan sword specular particles walk orbit)
here=$(cd "$(dirname "$0")/scenes" && pwd)
for s in "${scenes[@]}"; do
  mkdir -p "$out/$s"
  xvfb-run -a -s "-screen 0 1280x800x24" "$godot" --path "$here" --rendering-driver opengl3 \
    --write-movie "$out/$s/f.png" --fixed-fps 90 --quit-after 190 -- --scene="$s" >"$out/$s/render.log" 2>&1
  rm -f "$out/$s/f.wav"
  echo "$s: $(ls "$out/$s" | grep -c png) frames"
done
