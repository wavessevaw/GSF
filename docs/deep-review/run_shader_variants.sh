#!/usr/bin/env bash
# From repository root. Requires a c264c28 baseline build + engine.json and glslangValidator.
# Pass that preserved build directory as argument 1; do not rebuild it with candidate shaders.
set -euo pipefail
build=${1:-kedavra/build}
out=${2:-/tmp/kfg-shader-review}
mkdir -p "$out"
for variant in repair-init squared raw-confidence; do
    mkdir -p "$out/$variant"
    cp "$build"/*.spv "$out/$variant/"
done
# The control shader has only the initialization fix; other control shaders
# come from the pinned baseline build, NOT from a rebuilt candidate directory.
glslangValidator -V --target-env vulkan1.1 -Ikedavra/shaders \
  -o "$out/repair-init/kfg_select.spv" docs/deep-review/optimization-evidence/repair-init.comp
for shader in select warp; do
    glslangValidator -V --target-env vulkan1.1 -Ikedavra/shaders \
      -o "$out/squared/kfg_${shader}.spv" "kedavra/shaders/kfg_${shader}.comp"
done
cp "$out/squared/kfg_warp.spv" "$out/raw-confidence/"
glslangValidator -V --target-env vulkan1.1 -DKFG_EXPERIMENT_RAW_CONFIDENCE=1 \
  -Ikedavra/shaders -o "$out/raw-confidence/kfg_select.spv" kedavra/shaders/kfg_select.comp
for variant in repair-init squared raw-confidence; do
    "$build/kfg_vktest" --spv "$out/$variant" --json "$out/$variant/engine.json" > "$out/$variant/test.log" 2>&1
    python3 docs/deep-review/compare_engine.py "$build/engine.json" "$out/$variant/engine.json" > "$out/$variant/comparison.json"
done
