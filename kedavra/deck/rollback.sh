#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# KEDAVRA rollback: removes the bind mount. Run in Desktop Mode. A reboot does the same.
set -eu
STOCK=/usr/bin/gamescope
if findmnt -n "$STOCK" >/dev/null 2>&1; then
	sudo umount "$STOCK"
	echo "stock Gamescope restored"
else
	echo "nothing mounted over $STOCK; stock Gamescope is active"
fi
rm -f "$HOME/.local/share/kedavra/installed_at"
