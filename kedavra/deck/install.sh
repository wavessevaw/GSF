#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# KEDAVRA install (reversible). Run in Desktop Mode:
#   sh install.sh /path/to/gamescope-kedavra
# What it does:
#   1. copies the binary to ~/.local/share/kedavra/gamescope;
#   2. refuses if the binary misses a library or does not start (--version);
#   3. grants it CAP_SYS_NICE like the stock binary (realtime GPU queue);
#   4. bind-mounts it over /usr/bin/gamescope. A bind mount lives in memory only:
#      a reboot always returns the stock Gamescope. rollback.sh undoes it at once.
# The read-only root filesystem is never unlocked and no system file is changed.
set -eu
SRC=${1:?usage: install.sh <patched gamescope binary>}
DIR=$HOME/.local/share/kedavra
DST=$DIR/gamescope
STOCK=$(command -v gamescope)

[ "$STOCK" = /usr/bin/gamescope ] || { echo "unexpected gamescope path: $STOCK" >&2; exit 1; }
if findmnt -n "$STOCK" >/dev/null 2>&1; then
	echo "already installed; run rollback.sh first" >&2
	exit 1
fi
if pgrep -x gamescope >/dev/null; then
	echo "Gamescope is running. Switch to Desktop Mode first (Steam > Power > Switch to Desktop)." >&2
	exit 1
fi

mkdir -p "$DIR"
cp "$SRC" "$DST"
chmod 0755 "$DST"
missing=$(ldd "$DST" | grep 'not found' || true)
if [ -n "$missing" ]; then
	echo "refusing: missing libraries:" >&2
	echo "$missing" >&2
	exit 1
fi
"$DST" --version >/dev/null 2>&1 || { echo "refusing: $DST --version failed" >&2; exit 1; }

sudo setcap cap_sys_nice=eip "$DST"
if findmnt -n -o OPTIONS --target "$DST" | grep -q nosuid; then
	echo "warning: $DIR is on a nosuid mount; CAP_SYS_NICE will be ignored and the GPU queue will not be realtime" >&2
fi
sudo mount --bind "$DST" "$STOCK"
date -u +%Y-%m-%dT%H:%M:%SZ > "$DIR/installed_at"
echo "installed. Return to Gaming Mode; Gamescope starts the KEDAVRA build."
echo "If Gaming Mode does not come up: hold the power button, reboot (stock Gamescope returns)."
