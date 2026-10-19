#!/bin/sh
# Lavapipe functional validation; never a scanout or Deck performance claim.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
mkdir -p artifacts/wsi
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}
export VK_LAYER_PATH="$root/engine/gfg-native/build-mi:/usr/share/vulkan/explicit_layer.d"
export VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation
export GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 GFG_NATIVE_GPU_SYNC=same-device
export GFG_NATIVE_ASYNC=1 GFG_NATIVE_FORCE_FIFO=1 GFG_NATIVE_ALGO=gsf
export GFG_NATIVE_FLOW_LITE2=1 GFG_NATIVE_PACING=1 GFG_NATIVE_OUTPUT_HZ=90
export GFG_TEST_EXPECT_GPU=1 GFG_TEST_ALLOW_SKIPS=1 GFG_TEST_VALIDATION=1
export GFG_TEST_FRAMES=12 GFG_TEST_SOURCE_HZ=5
for mode in 2 3 4; do
  export GFG_NATIVE_MODE=$mode
  export GFG_NATIVE_TELEMETRY="$root/artifacts/wsi/x$mode.jsonl"
  : > "$GFG_NATIVE_TELEMETRY"
  timeout 90 xvfb-run -a -s '-screen 0 1024x768x24' \
    engine/gfg-native/build-mi/wsi-gpu-smoke > "artifacts/wsi/x$mode.log" 2>&1
  echo "WSI x$mode PASS"
done
python3 - <<'PY'
import json
from pathlib import Path
for mode in (2,3,4):
    rows=[json.loads(x) for x in Path(f'artifacts/wsi/x{mode}.jsonl').read_text().splitlines() if x.startswith('{')]
    pairs=[x for x in rows if x.get('event')=='pair']
    mapping=[x for x in rows if x.get('event')=='slot_mapping']
    slots=[x for x in rows if x.get('event')=='synthetic_lifecycle']
    assert len(pairs)==12
    assert any(x['dispatched_outputs']==mode-1 for x in pairs), (mode, 'never generated all outputs')
    assert all(0<x['tau_applied']<1 for x in mapping)
    assert all(abs(x['tau_applied']-x['tau_requested'])<1e-6 for x in mapping)
    for pair in pairs:
        accepted=[x for x in slots if x['pair']==pair['pair'] and x['wsi_accepted_ns']]
        assert [x['target_ns'] for x in accepted]==sorted(x['target_ns'] for x in accepted)
    assert any(x['slot_id']==mode-2 and x['wsi_accepted_ns'] for x in slots), (mode, 'last synthetic never accepted')
    assert all(x['physical_displayed'] is None for x in pairs)
    print('ACTUAL_VULKAN_MULTIFRAME_PASS',mode,len(mapping),sum(bool(x['wsi_accepted_ns']) for x in slots))
PY
