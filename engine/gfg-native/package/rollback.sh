#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
target="${XDG_DATA_HOME:-$HOME/.local/share}/gfg-native"
bin="$HOME/.local/bin"
backup="$target/.rollback"
[[ -f "$backup/IDENTITY" ]] && [[ "$(cat "$backup/IDENTITY")" == gfg-native-rollback-v1 ]] || {
  echo 'No valid GFG Native rollback snapshot.' >&2; exit 1;
}
for name in libVkLayer_gfg_native.so VkLayer_gfg_native.json diagnose.sh accept_report.py MANIFEST.json; do
  if [[ -f "$backup/$name" ]]; then
    cp -p "$backup/$name" "$target/$name.restore"
    mv -f "$target/$name.restore" "$target/$name"
  else
    rm -f "$target/$name"
  fi
done
# The loader directory is separate from provenance metadata. Restore it exactly
# (or remove only the two files we created when the previous version had none).
mkdir -p "$target/vulkan-layers"
for name in VkLayer_gfg_native.json libVkLayer_gfg_native.so; do
  previous="$backup/vulkan-layers/$name"
  current="$target/vulkan-layers/$name"
  if [[ -e "$previous" || -L "$previous" ]]; then
    cp -Pp "$previous" "$current.restore"
    mv -f "$current.restore" "$current"
  else
    rm -f "$current"
  fi
done
rmdir "$target/vulkan-layers" 2>/dev/null || :
if [[ -f "$backup/gfg-native" ]]; then
  cp -p "$backup/gfg-native" "$bin/gfg-native.restore"
  mv -f "$bin/gfg-native.restore" "$bin/gfg-native"
else
  # With no component present the current wrapper is a passthrough, including
  # --off / --diagnose. A bare `exec "$@"` stub would try to execute --off.
  mkdir -p "$bin"
  install -m 0755 "$(dirname -- "${BASH_SOURCE[0]}")/gfg-native.sh" "$bin/gfg-native"
fi
echo 'Previous GFG Native installation restored.'
