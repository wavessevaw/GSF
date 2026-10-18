#!/usr/bin/env bash
# Independent GFG GPU pipeline. DISPLAY refresh is configured by SteamOS.
set -euo pipefail
if (($# == 0)); then
    echo 'Usage: run_gfg_gsf.sh command [arguments...]' >&2
    exit 2
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
layer_dir=${GFG_NATIVE_LAYER_DIR:-"$script_dir/../engine/gfg-native/build-te"}
if [[ ! -f "$layer_dir/VkLayer_gfg_native.json" || ! -f "$layer_dir/libVkLayer_gfg_native.so" ]]; then
    echo "GFG native layer is missing from $layer_dir; build ENABLE_TE=1 BUILD=build-te first." >&2
    exit 1
fi
export GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 GFG_NATIVE_ASYNC=1
export GFG_NATIVE_GPU_SYNC=same-device GFG_NATIVE_ALGO=gsf
# Refresh is queried from WSI when available; OUTPUT_HZ is an explicit override.
export GFG_NATIVE_MODE=${GFG_NATIVE_MODE:-3}
export GFG_NATIVE_FORCE_FIFO=0
export VK_LAYER_PATH="$layer_dir${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
case ":${VK_INSTANCE_LAYERS:-}:" in
    *:VK_LAYER_GFG_native:*) ;;
    *) export VK_INSTANCE_LAYERS="VK_LAYER_GFG_native${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}" ;;
esac
exec "$@"
