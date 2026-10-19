#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
export VK_LAYER_PATH="$root/engine/gfg-native/build-mi:/usr/share/vulkan/explicit_layer.d"
export VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation
export GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 GFG_NATIVE_GPU_SYNC=same-device GFG_NATIVE_ASYNC=1
export GFG_NATIVE_ALGO=gsf GFG_NATIVE_FLOW_LITE2=1 GFG_NATIVE_MODE=4
export GFG_NATIVE_BUFFERED_OUTPUT=1 GFG_NATIVE_SOURCE_CONTRACT=barriers GFG_TEST_BUFFERED_OWNER=1
export GFG_NATIVE_OUTPUT_HZ=90 GFG_TEST_FRAMES=32 GFG_TEST_SOURCE_HZ=5
export GFG_TEST_ALLOW_SKIPS=1 GFG_TEST_EXPECT_GPU=1 GFG_TEST_VALIDATION=1
mkdir -p artifacts/buffers
for depth in 0 1 2; do
    export GFG_NATIVE_BUFFER_DEPTH=$depth
    export GFG_NATIVE_TELEMETRY="$root/artifacts/buffers/buffer$depth.jsonl"
    : > "$GFG_NATIVE_TELEMETRY"
    : > "$GFG_NATIVE_TELEMETRY.buffer.jsonl"
    timeout 90 xvfb-run -a engine/gfg-native/build-mi/wsi-gpu-smoke > "artifacts/buffers/buffer$depth.log" 2>&1
    echo "Buffer $depth Vulkan ownership PASS (inspect generation counts separately)"
done

python3 - <<'PYCHECK'
import json
from pathlib import Path
for depth in (0,1,2):
    log=Path(f'artifacts/buffers/buffer{depth}.log').read_text()
    assert 'buffered-owner active' in log and 'validation errors=0' in log
    rows=[json.loads(x) for x in Path(f'artifacts/buffers/buffer{depth}.jsonl').read_text().splitlines() if x.startswith('{')]
    pairs=[x for x in rows if x.get('event')=='pair']
    assert len(pairs)==32
    assert any(x['dispatched_outputs']==3 for x in pairs), (depth, 'no three-output pair')
    print('BUFFER_CONTRACT_PASS',depth,'source',len(pairs),'dispatched',sum(x['dispatched_outputs'] for x in pairs))
PYCHECK
