#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# KEDAVRA engine benchmark on the Steam Deck GPU (RADV). Run in Desktop Mode from the
# unpacked "kedavra-deck-bench" CI artifact:   sh bench.sh
# Writes bench.json / bench.txt: correctness on RADV plus median GPU time of every pass.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
chmod +x kfg_vktest
./kfg_vktest --spv . --bench 300 --json bench.json | tee bench.txt
echo "send bench.json and bench.txt"
