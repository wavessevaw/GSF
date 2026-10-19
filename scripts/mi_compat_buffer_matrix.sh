#!/bin/sh
# Ordinary mutable-format WSI with real source-image retention. No virtual IDs,
# test-only source facade, Steam display or physical-FPS claim.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
build=${GSF_NATIVE_BUILD:-build-mi}
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}
export VK_LAYER_PATH="$root/engine/gfg-native/$build:/usr/share/vulkan/explicit_layer.d"
export VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation
export GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 GFG_NATIVE_GPU_SYNC=same-device
export GFG_NATIVE_ASYNC=1 GFG_NATIVE_FORCE_FIFO=1 GFG_NATIVE_ALGO=gsf
export GFG_NATIVE_FLOW_LITE2=1 GFG_NATIVE_PACING=1 GFG_NATIVE_OUTPUT_HZ=90
export GFG_NATIVE_COMPAT_BUFFER=1 GFG_TEST_MUTABLE_SWAPCHAIN=1
export GFG_TEST_EXPECT_GPU=1 GFG_TEST_ALLOW_SKIPS=1 GFG_TEST_VALIDATION=1
export GFG_TEST_FRAMES=24 GFG_TEST_SOURCE_HZ=5
unset GFG_NATIVE_BUFFERED_OUTPUT GFG_NATIVE_SOURCE_CONTRACT GFG_TEST_BUFFERED_OWNER
mkdir -p artifacts/compat-buffers
run_case() {
    name=$1
    export GFG_NATIVE_MODE=$2 GFG_NATIVE_BUFFER_DEPTH=$3
    export GFG_NATIVE_GPU_BRIDGE_INJECT_SKIP=${4:-0}
    export GFG_NATIVE_TELEMETRY="$root/artifacts/compat-buffers/$name.jsonl"
    : > "$GFG_NATIVE_TELEMETRY"
    timeout 90 xvfb-run -a engine/gfg-native/$build/wsi-gpu-smoke > "artifacts/compat-buffers/$name.log" 2>&1
    echo "Compatible mutable WSI $name PASS"
}
run_case x2-buffer0 2 0
run_case x2-buffer1 2 1
run_case x2-buffer2 2 2
run_case x3-buffer2 3 2
run_case x4-buffer2 4 2
run_case x4-buffer2-injected 4 2 5
python3 - <<'PY'
import json
from pathlib import Path
for path in sorted(Path('artifacts/compat-buffers').glob('*.jsonl')):
    rows=[json.loads(line) for line in path.read_text().splitlines() if line.startswith('{')]
    pairs=[e for e in rows if e.get('event')=='pair']
    buffers=[e for e in rows if e.get('event')=='buffer_state']
    slots=[e for e in rows if e.get('event')=='synthetic_lifecycle']
    depth=int(path.stem.split('buffer')[1][0])
    mode=int(path.stem[1])
    assert len(pairs)==len(buffers)==24, (path,len(pairs),len(buffers))
    context=next(e for e in rows if e.get('event')=='bridge_context')
    effective=depth if context['compositor_timing_mode']==2 else 0
    assert all(e['effective_depth']==effective and not e['independent_owner'] for e in buffers),path
    assert any(e['dispatched_outputs']==mode-1 for e in pairs),path
    assert any(e['slot_id']==mode-2 and e['wsi_accepted_ns'] for e in slots),path
    assert sum(bool(e['wsi_accepted_ns']) for e in slots)>=(24-1)*(mode-1)//2,path
    assert all(len(e['gpu_budget_all_ns'])==3 and len(e['target_all_ns'])==4 for e in buffers)
    assert all(e['queue_depth']<=64 for e in buffers)
    log=path.with_suffix('.log').read_text()
    assert 'validation errors=0' in log and 'buffered-owner active' not in log,path
    print('COMPAT_BUFFER_VALIDATED',path.stem,'effective_depth',effective,'dispatched',sum(e['dispatched_outputs'] for e in pairs),
          'accepted',sum(bool(e['wsi_accepted_ns']) for e in slots),'physical_displayed','UNVERIFIED')
PY
