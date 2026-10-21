#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bundles a built KEDAVRA Gamescope with the shared libraries it was linked against,
# so it runs on SteamOS whose libraries are older than the build host's:
#   bundle.sh <gamescope build dir> <out dir> <version string>
# Every library from the build host is copied except glibc and the libraries that must
# match system daemons or GPU drivers (Vulkan loader, GBM/EGL/GL, udev, systemd, audio,
# PipeWire). The binaries get an absolute RUNPATH to the installed location; absolute
# paths still apply to a binary with file capabilities, $ORIGIN would not.
set -eu
BUILD=${1:?build dir}
OUT=${2:?out dir}
VERSION=${3:?version}
PREFIX=/var/lib/kedavra/runtime

SKIP='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libvulkan|libgbm|libEGL|libGL|libGLX|libGLdispatch|libOpenGL|libdrm_amdgpu|libdrm_radeon|libudev|libsystemd|libasound|libpulse|libpipewire-0\.3|libdbus-1)\.so'

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib"
cp "$BUILD/src/gamescope" "$BUILD/src/gamescopectl" "$OUT/bin/"
for b in "$OUT"/bin/*; do
	ldd "$b" | awk '/=> \// { print $3 }'
done | sort -u | while read -r lib; do
	name=$(basename "$lib")
	if echo "$name" | grep -Eq "$SKIP"; then
		continue
	fi
	cp -L "$lib" "$OUT/lib/$name"
done
for f in "$OUT"/bin/* "$OUT"/lib/*; do
	patchelf --set-rpath "$PREFIX/lib" "$f"
done
echo "$VERSION" > "$OUT/VERSION"
ls "$OUT/lib" > "$OUT/LIBS"
du -sh "$OUT"
