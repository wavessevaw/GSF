#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Collect one local, privacy-sensitive SteamOS/GFG diagnostic run. Nothing is uploaded.
set -euo pipefail
mode=passthrough
case "${1:-}" in
  --native) mode=native; shift ;;
  --passthrough) shift ;;
esac
if [[ $# -eq 0 ]]; then
  printf 'Usage: gfg-native --diagnose <game-command> [args...]\n' >&2
  exit 2
fi

if [[ "$1" == '%command%' ]]; then
  printf 'GSF diagnosis aborted: literal %%command%%, Steam command expansion did not occur.\n' >&2
  exit 2
fi

capture_unavailable() {
  printf 'GFG: diagnostic capture unavailable; starting the game without capture.\n' >&2
  exec "$@"
}
# Diagnostics are optional. A full/unwritable state directory or unavailable
# capture utilities must not turn the launch option into a game-start failure.
for utility in date mkdir mktemp chmod mkfifo tee; do
  command -v "$utility" >/dev/null 2>&1 || capture_unavailable "$@"
done
root="${XDG_DATA_HOME:-$HOME/.local/share}/gfg-native"
state="${XDG_STATE_HOME:-$HOME/.local/state}/gfg-native/diagnostics"
stamp="$(date -u '+%Y%m%dT%H%M%SZ')" || capture_unavailable "$@"
umask 077
mkdir -p "$state" 2>/dev/null || capture_unavailable "$@"
run="$(mktemp -d "$state/$stamp.XXXXXX")" || capture_unavailable "$@"
stamp="${run##*/}"
chmod 700 "$state" "$run" || capture_unavailable "$@"
tee /dev/null </dev/null >/dev/null || capture_unavailable "$@"
export GFG_DIAGNOSTIC_CAPTURE="$run"
if [[ "$mode" == native ]]; then
  export GFG_NATIVE_TELEMETRY="$run/pairs.jsonl"
  export GFG_NATIVE_TRACE=1
  export GFG_NATIVE_TIMING=1
  export GFG_NATIVE_PRESENT_TRACE=1
fi

{
  printf 'schema=1\nmode=%s\ncapture_utc=%s\n' "$mode" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  printf 'command_basename=%s\n' "${1##*/}"
  printf '\n--- kernel ---\n'
  uname -a
  printf '\n--- OS release ---\n'
  if [[ -r /etc/os-release ]]; then cat /etc/os-release; else printf 'missing\n'; fi
  if [[ -r /etc/steamos-release ]]; then
    printf '\n--- SteamOS release ---\n'
    cat /etc/steamos-release
  fi
  printf '\n--- installed graphics packages ---\n'
  if command -v pacman >/dev/null 2>&1; then
    pacman -Q gamescope mesa vulkan-radeon 2>&1 || true
  else
    printf 'pacman=not-installed\n'
  fi
  printf '\n--- selected runtime variables ---\n'
  for name in VK_INSTANCE_LAYERS VK_LAYER_PATH GFG_NATIVE_FG GFG_NATIVE_GPU_BRIDGE \
      GFG_NATIVE_GPU_SYNC GFG_NATIVE_ALGO GFG_NATIVE_LMC_TIER GFG_NATIVE_TRACE \
      GFG_NATIVE_TIMING GFG_NATIVE_PRESENT_TRACE GFG_NATIVE_MODE GFG_NATIVE_OUTPUT_HZ GFG_NATIVE_ASYNC GFG_NATIVE_FORCE_FIFO; do
    if [[ -v "$name" ]]; then printf '%s=%q\n' "$name" "${!name}"; fi
  done
  printf '\n--- installed GFG payload SHA-256 ---\n'
  for name in libVkLayer_gfg_native.so VkLayer_gfg_native.json diagnose.sh accept_report.py; do
    if [[ -f "$root/$name" ]]; then sha256sum "$root/$name"; fi
  done
  printf '\n--- memory ---\n'
  if [[ -r /proc/meminfo ]]; then grep -E '^(MemTotal|MemAvailable):' /proc/meminfo || true; fi
  printf '\n--- DRM device IDs ---\n'
  shopt -s nullglob
  for path in /sys/class/drm/card*/device/vendor /sys/class/drm/card*/device/device; do
    printf '%s=' "$path"; cat "$path" 2>/dev/null || true
  done
  printf '\n--- power / temperature snapshots (raw sysfs units) ---\n'
  for path in /sys/class/hwmon/hwmon*/name /sys/class/hwmon/hwmon*/temp*_label \
      /sys/class/hwmon/hwmon*/power*_label; do
    printf '%s=' "$path"; cat "$path" 2>/dev/null || true
  done
  for path in /sys/class/hwmon/hwmon*/temp*_input /sys/class/hwmon/hwmon*/power*_average \
      /sys/class/power_supply/BAT*/capacity /sys/class/power_supply/BAT*/status; do
    printf '%s=' "$path"; cat "$path" 2>/dev/null || true
  done
  printf '\n--- Vulkan summary (hardware UUID fields removed) ---\n'
  if command -v vulkaninfo >/dev/null 2>&1; then
    env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH GFG_NATIVE_FG=0 GFG_NATIVE_GPU_BRIDGE=0 \
      timeout 20s vulkaninfo --summary 2>&1 | sed -E '/(deviceUUID|driverUUID|deviceLUID)/Id' || true
  else
    printf 'vulkaninfo=not-installed\n'
  fi
} > "$run/system-start.txt" 2>&1 || capture_unavailable "$@"

cat > "$run/capture-info.txt" <<EOF || capture_unavailable "$@"
This local bundle contains game stdout/stderr and selected device metadata.
Review it for private game/account paths before sharing. It is never uploaded by this script.
The timestamps and bridge counters are WSI/API observations, not physical scanout.
EOF

mkfifo "$run/stdout.pipe" "$run/stderr.pipe" || capture_unavailable "$@"
tee "$run/app.stdout.log" < "$run/stdout.pipe" &
stdout_tee=$!
tee "$run/app.stderr.log" < "$run/stderr.pipe" >&2 &
stderr_tee=$!
set +e
"$@" > "$run/stdout.pipe" 2> "$run/stderr.pipe"
app_status=$?
set -e
wait "$stdout_tee" || :
wait "$stderr_tee" || :
rm -f "$run/stdout.pipe" "$run/stderr.pipe" || :
printf '%s\n' "$app_status" > "$run/process-status.txt" || :

{
  printf 'capture_end_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  printf '\n--- memory ---\n'
  if [[ -r /proc/meminfo ]]; then grep -E '^(MemTotal|MemAvailable):' /proc/meminfo || true; fi
  printf '\n--- power / temperature snapshots (raw sysfs units) ---\n'
  for path in /sys/class/hwmon/hwmon*/temp*_input /sys/class/hwmon/hwmon*/power*_average \
      /sys/class/power_supply/BAT*/capacity /sys/class/power_supply/BAT*/status; do
    printf '%s=' "$path"; cat "$path" 2>/dev/null || true
  done
} > "$run/system-end.txt" 2>&1 || :

if command -v python3 >/dev/null 2>&1 && [[ -r "$root/accept_report.py" ]]; then
  python3 "$root/accept_report.py" "$run" --output "$run/report.md" || :
else
  printf '# GFG Native diagnostic — NOT ACCEPTANCE\n\nPhysical display scanout is unobserved.\n' > "$run/report.md" || :
fi

bundle="$state/$stamp.tar.gz"
if tar -czf "$bundle" -C "$state" "$stamp"; then
  printf 'GFG diagnostic bundle: %s\n' "$bundle"
  sha256sum "$bundle" || :
else
  printf 'GFG: could not archive diagnostic capture in %s\n' "$run" >&2
fi
printf 'Physical display scanout: unobserved; hardware acceptance NOT COMPLETED.\n'
exit "$app_status"
