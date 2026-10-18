#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"
sha256sum --check --status SHA256SUMS.txt || {
  echo 'GFG Native FG: package checksum verification failed.' >&2
  exit 1
}
target="${XDG_DATA_HOME:-$HOME/.local/share}/gfg-native"
bin="$HOME/.local/bin"
mkdir -p "$target" "$bin" "$target/vulkan-layers"
# Plugin startup and mode changes may stage the same package repeatedly. Do not
# replace the useful previous-version snapshot with the current installation.
unchanged=1
for name in libVkLayer_gfg_native.so VkLayer_gfg_native.json diagnose.sh accept_report.py; do
  cmp -s "$name" "$target/$name" || unchanged=0
done
cmp -s gfg-native.sh "$bin/gfg-native" || unchanged=0
cmp -s VkLayer_gfg_native.json "$target/vulkan-layers/VkLayer_gfg_native.json" || unchanged=0
[[ "$(readlink "$target/vulkan-layers/libVkLayer_gfg_native.so" 2>/dev/null)" == "../libVkLayer_gfg_native.so" ]] || unchanged=0
[[ -x "$bin/gfg-native" ]] || unchanged=0
if [[ -f MANIFEST.json ]]; then cmp -s MANIFEST.json "$target/MANIFEST.json" || unchanged=0; fi
if (( unchanged )); then
  printf 'GFG Native: verified component and executable launcher already installed.\n'
  exit 0
fi
# Preserve the immediately previous installation, including absent files.
backup="$target/.rollback"
stage="$(mktemp -d "$target/.install.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/previous/vulkan-layers"
for name in libVkLayer_gfg_native.so VkLayer_gfg_native.json diagnose.sh accept_report.py MANIFEST.json; do
  if [[ -e "$target/$name" ]]; then cp -p "$target/$name" "$stage/previous/$name"; fi
done
for name in VkLayer_gfg_native.json libVkLayer_gfg_native.so; do
  source="$target/vulkan-layers/$name"
  if [[ -e "$source" || -L "$source" ]]; then
    cp -Pp "$source" "$stage/previous/vulkan-layers/$name"
  fi
done
if [[ -e "$bin/gfg-native" ]]; then cp -p "$bin/gfg-native" "$stage/previous/gfg-native"; fi
printf 'gfg-native-rollback-v1\n' > "$stage/previous/IDENTITY"
install -m 0755 libVkLayer_gfg_native.so "$target/libVkLayer_gfg_native.so.tmp"
install -m 0644 VkLayer_gfg_native.json "$target/VkLayer_gfg_native.json.tmp"
install -m 0644 VkLayer_gfg_native.json "$target/vulkan-layers/VkLayer_gfg_native.json.tmp"
ln -sfn ../libVkLayer_gfg_native.so "$target/vulkan-layers/libVkLayer_gfg_native.so.tmp"
install -m 0755 diagnose.sh "$target/diagnose.sh.tmp"
install -m 0644 accept_report.py "$target/accept_report.py.tmp"
install -m 0755 gfg-native.sh "$bin/gfg-native.tmp"
if [[ -f MANIFEST.json ]]; then install -m 0644 MANIFEST.json "$target/MANIFEST.json.tmp"; fi
rm -rf -- "$backup"
mv "$stage/previous" "$backup"
trap 'bash "$root/rollback.sh" >&2 || true; exit 1' ERR
mv -f "$target/libVkLayer_gfg_native.so.tmp" "$target/libVkLayer_gfg_native.so"
mv -f "$target/VkLayer_gfg_native.json.tmp" "$target/VkLayer_gfg_native.json"
mv -f "$target/vulkan-layers/VkLayer_gfg_native.json.tmp" "$target/vulkan-layers/VkLayer_gfg_native.json"
mv -f "$target/vulkan-layers/libVkLayer_gfg_native.so.tmp" "$target/vulkan-layers/libVkLayer_gfg_native.so"
mv -f "$target/diagnose.sh.tmp" "$target/diagnose.sh"
mv -f "$target/accept_report.py.tmp" "$target/accept_report.py"
mv -f "$bin/gfg-native.tmp" "$bin/gfg-native"
if [[ -f MANIFEST.json ]]; then mv -f "$target/MANIFEST.json.tmp" "$target/MANIFEST.json"
else rm -f "$target/MANIFEST.json"; fi
printf 'GFG FG3 async candidate installed; physical 90 FPS acceptance NOT COMPLETED.\nSteam launch option:\n%s/.local/bin/gfg-native %%command%%\nRollback: bash %s/rollback.sh\n' "$HOME" "$root"
