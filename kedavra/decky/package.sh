#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the KEDAVRA Decky plugin zip: package.sh [out.zip]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build/KEDAVRA.zip}
cd "$HERE"
npm ci --no-audit --no-fund
npm run -s build
STAGE=$(mktemp -d)
mkdir -p "$STAGE/KEDAVRA/bin" "$STAGE/KEDAVRA/dist"
cp main.py plugin.json package.json README.md "$STAGE/KEDAVRA/"
cp -r py_modules "$STAGE/KEDAVRA/"
rm -rf "$STAGE/KEDAVRA/py_modules/__pycache__"
cp dist/index.js "$STAGE/KEDAVRA/dist/"
cp ../tools/kfg_pacing.py "$STAGE/KEDAVRA/bin/"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
(cd "$STAGE" && python3 -m zipfile -c "$OUT" KEDAVRA)
rm -rf "$STAGE"
echo "$OUT"
