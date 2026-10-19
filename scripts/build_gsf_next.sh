#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo 'Commit tracked source changes before creating a distributable build' >&2
  exit 1
fi
commit=$(git rev-parse HEAD)
build=${GSF_NEXT_BUILD_DIR:-engine/gsf-next/build}
cmake -S engine/gsf-next -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGSF_SOURCE_COMMIT="$commit" -DVulkan_INCLUDE_DIR="${VULKAN_HEADERS:-/usr/include}"
cmake --build "$build" -j "${GSF_BUILD_JOBS:-4}"
