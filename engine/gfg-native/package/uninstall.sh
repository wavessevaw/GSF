#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
target="${XDG_DATA_HOME:-$HOME/.local/share}/gfg-native"
rm -f -- "$target/libVkLayer_gfg_native.so" "$target/VkLayer_gfg_native.json" \
  "$target/diagnose.sh" "$target/accept_report.py" "$target/MANIFEST.json"
rm -rf -- "$target/.rollback"
rmdir -- "$target" 2>/dev/null || :
# The installed wrapper already falls through without the component and still
# understands --off. Preserve it instead of installing an option-blind stub.
echo 'GFG Native FG removed; transparent launcher retained for existing Steam launch options.'
