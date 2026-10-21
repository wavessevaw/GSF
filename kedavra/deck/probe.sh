#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# KEDAVRA device probe. Read-only: prints what the install step depends on and
# changes nothing. Run in Desktop Mode (Konsole) or over SSH:
#   sh probe.sh > probe.txt
# Send probe.txt back; nothing in it is secret (no keys, no account data).
echo "# KEDAVRA probe $(date -u +%Y-%m-%dT%H:%M:%SZ)"
section() { echo; echo "## $1"; }

section os
cat /etc/os-release 2>/dev/null | grep -E '^(NAME|VERSION_ID|BUILD_ID|VARIANT_ID)='
uname -r
cat /sys/class/dmi/id/product_name 2>/dev/null

section gamescope
command -v gamescope && gamescope --version 2>&1 | head -2
command -v gamescopectl || echo "gamescopectl: not found"
pacman -Q gamescope mesa vulkan-radeon glibc 2>/dev/null
getcap "$(command -v gamescope)" 2>/dev/null || echo "getcap unavailable"
ls -l "$(command -v gamescope)"
findmnt -n -o SOURCE,TARGET,OPTIONS / 2>/dev/null
findmnt -n -o SOURCE,TARGET,OPTIONS --target "$HOME" 2>/dev/null
findmnt -n "$(command -v gamescope)" 2>/dev/null && echo "gamescope is bind-mounted (KEDAVRA installed?)"

section session
for f in /usr/bin/gamescope-session /usr/lib/steamos/gamescope-session /usr/share/gamescope-session-plus/gamescope-session-plus /usr/bin/steamos-session-select; do
	[ -e "$f" ] && { echo "--- $f"; grep -n -E 'gamescope( |$)|GAMESCOPE|environment.d|GAMESCOPECMD' "$f" | head -20; }
done
ls ~/.config/environment.d 2>/dev/null
systemctl --user list-units --all 2>/dev/null | grep -i gamescope

section running
pid=$(pgrep -x gamescope | head -1)
if [ -n "$pid" ]; then
	tr '\0' ' ' < /proc/$pid/cmdline; echo
	grep -E 'Cap(Eff|Prm)' /proc/$pid/status
	ls -l /proc/$pid/exe
else
	echo "gamescope not running (Desktop Mode?)"
fi

section display
for m in /sys/class/drm/card*-eDP-*/modes; do echo "$m"; sort -u "$m"; done 2>/dev/null
cat /sys/kernel/debug/dri/0/amdgpu_firmware_info 2>/dev/null | head -3 || true

section vulkan
command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo|apiVersion' | head -6

section kedavra
ls -l ~/.local/share/kedavra 2>/dev/null
cat "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/kedavra-status.json" 2>/dev/null || echo "no status file (stock Gamescope or engine off)"
