#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
disabled=0
diagnose=0
while [[ "${1:-}" == --off || "${1:-}" == --diagnose ]]; do
  case "$1" in
    --off) disabled=1 ;;
    --diagnose) diagnose=1 ;;
  esac
  shift
done
if (( $# < 1 )); then
  printf 'Usage: gfg-native [--off] [--diagnose] <game-command> [args...]\n' >&2
  exit 2
fi
# A literal Steam placeholder is NOT an executable and must not be counted as
# a Vulkan startup failure or recorded as a partially launched game.
if [[ "$1" == '%command%' ]]; then
  printf 'GSF: Steam did not expand %%command%%. Set this launcher in the game Steam Launch Options, not Konsole.\n' >&2
  exit 2
fi
# Bypass before reading settings or invoking any optional helper. Steam/Proton
# may inherit our activation variables from a parent process or another wrapper.
passthrough() {
  unset GFG_NATIVE_FG GFG_NATIVE_GPU GFG_NATIVE_GPU_BRIDGE GFG_NATIVE_CPU_BRIDGE GFG_NATIVE_ASYNC GFG_NATIVE_FORCE_FIFO
  export DISABLE_GFG_NATIVE_FG=1
  local variable value item joined separator
  local -a entries
  for variable in VK_INSTANCE_LAYERS VK_LOADER_LAYERS_ENABLE; do
    [[ -v "$variable" ]] || continue
    value=${!variable}; joined=
    if [[ "$variable" == VK_INSTANCE_LAYERS ]]; then separator=:
    else separator=,; fi
    IFS="$separator" read -r -a entries <<< "$value"
    for item in "${entries[@]}"; do
      [[ -z "$item" || "$item" == VK_LAYER_GFG_native ]] && continue
      joined+="${joined:+$separator}$item"
    done
    if [[ -n "$joined" ]]; then export "$variable=$joined"; else unset "$variable"; fi
  done
  if (( diagnose )) && [[ -r "$root/diagnose.sh" ]]; then
    exec bash "$root/diagnose.sh" --passthrough "$@"
  fi
  exec "$@"
}
root="${XDG_DATA_HOME:-$HOME/.local/share}/gfg-native"
if (( disabled )); then passthrough "$@"; fi
# Decky writes its marker under the resolved game user's home even when a
# service/runtime exports another XDG_CONFIG_HOME. Keep the UI's existing Off
# authoritative; retain the XDG location for standalone users without it.
mode_file="${GFG_NATIVE_MODE_FILE:-$HOME/.config/mako-render/runtime-state/native-gsf.mode}"
if [[ -z "${GFG_NATIVE_MODE_FILE:-}" && ! -r "$mode_file" && -n "${XDG_CONFIG_HOME:-}" ]]; then
  mode_file="$XDG_CONFIG_HOME/mako-render/runtime-state/native-gsf.mode"
fi
mode="${GFG_NATIVE_MODE:-}"
if [[ -z "$mode" && -r "$mode_file" ]]; then
  IFS= read -r -n 8 mode < "$mode_file" || :
fi
mode=${mode:-3} # invoking this explicit launcher opts in; plugin defaults off
case "$mode" in off|2|3) ;; *) mode=off; printf 'GFG: invalid native mode, using real-only.\n' >&2 ;; esac
if [[ "$mode" == off ]] || \
   [[ ! -r "$root/vulkan-layers/VkLayer_gfg_native.json" || ! -r "$root/libVkLayer_gfg_native.so" ]]; then
  passthrough "$@"
fi
# Only Vulkan layer JSON manifests may be scanned. Keeping MANIFEST.json in
# VK_LAYER_PATH breaks Steam pressure-vessel's Vulkan loader inspection.
export VK_LAYER_PATH="$root/vulkan-layers${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
case ":${VK_INSTANCE_LAYERS:-}:" in
  *:VK_LAYER_GFG_native:*) ;;
  *) export VK_INSTANCE_LAYERS="VK_LAYER_GFG_native${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}" ;;
esac
unset DISABLE_GFG_NATIVE_FG
export GFG_NATIVE_FG=1
export GFG_NATIVE_GPU_BRIDGE=1
export GFG_NATIVE_GPU_SYNC=same-device
export GFG_NATIVE_ALGO=gsf
export GFG_NATIVE_MODE="$mode"
export GFG_NATIVE_ASYNC=1
export GFG_NATIVE_FORCE_FIFO=0
if [[ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" && -z "${GFG_NATIVE_GAMESCOPE_MANIFEST:-}" ]]; then
  for candidate in /usr/share/vulkan/implicit_layer.d/VkLayer_FROG_gamescope_wsi.x86_64.json \
      "$HOME/.local/share/mako-render/vulkan/gamescope_wsi_compatibility.d/VkLayer_FROG_gamescope_wsi.x86_64.json"; do
    if [[ -r "$candidate" ]]; then export GFG_NATIVE_GAMESCOPE_MANIFEST="$candidate"; break; fi
  done
fi
# GFG must be above Gamescope WSI. Its public FIFO contract maps to a
# compositor-managed FIFO / driver MAILBOX transport internally.
if [[ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" && -n "${GFG_NATIVE_GAMESCOPE_MANIFEST:-}" && -r "$GFG_NATIVE_GAMESCOPE_MANIFEST" ]]; then
  gamescope_dir=$(dirname -- "$GFG_NATIVE_GAMESCOPE_MANIFEST")
  export VK_LAYER_PATH="$VK_LAYER_PATH:$gamescope_dir"
  export VK_INSTANCE_LAYERS="VK_LAYER_GFG_native:VK_LAYER_FROG_gamescope_wsi_x86_64${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
  export DISABLE_GAMESCOPE_WSI=1
fi
status_dir="${XDG_STATE_HOME:-$HOME/.local/state}/gfg-native/status"
if mkdir -p "$status_dir" 2>/dev/null; then
  export GFG_NATIVE_STATUS_DIR="$status_dir"
else
  # Optional telemetry must never prevent a game from starting.
  unset GFG_NATIVE_STATUS_DIR
  printf 'GFG: native status directory unavailable; starting without live status.\n' >&2
fi
layers=:
IFS=: read -r -a layer_names <<< "$VK_INSTANCE_LAYERS"
for layer in "${layer_names[@]}"; do
  [[ -z "$layer" || "$layers" == *":$layer:"* ]] || layers+="$layer:"
done
export VK_INSTANCE_LAYERS="${layers#:}"; export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS%:}"
unset GFG_NATIVE_GPU GFG_NATIVE_CPU_BRIDGE
export DISABLE_MAKO=1
export DISABLE_MAKO_SPATIAL_SCALING=1
if (( diagnose )); then
  exec bash "$root/diagnose.sh" --native "$@"
fi
exec "$@"
