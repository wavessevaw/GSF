#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bundles a built KEDAVRA Gamescope with the shared libraries it was linked against,
# so it runs on SteamOS whose libraries are older than the build host's:
#   bundle.sh <gamescope build dir> <out dir> <version string>
# Every library from the build host is copied, glibc and its dynamic loader included
# (the build host's libraries need a newer glibc than SteamOS ships). Not copied are the
# libraries that must match system daemons or GPU drivers (Vulkan loader, GBM/EGL/GL,
# udev, systemd, audio, PipeWire); they load from the system against the newer, backward
# compatible glibc. The binaries get the bundled loader as interpreter and an absolute
# RUNPATH to the installed location; absolute paths still apply to a binary with file
# capabilities, $ORIGIN would not.
set -eu
BUILD=${1:?build dir}
OUT=${2:?out dir}
VERSION=${3:?version}
PREFIX=/var/lib/kedavra/runtime
LOADER=ld-linux-x86-64.so.2

SKIP='^(libvulkan|libgbm|libEGL|libGL|libGLX|libGLdispatch|libOpenGL|libdrm_amdgpu|libdrm_radeon|libudev|libsystemd|libasound|libpulse|libpipewire-0\.3|libdbus-1)\.so'
GLIBC='^(ld-linux.*|libc|libm|libmvec|libdl|libpthread|librt|libresolv|libutil|libanl|libBrokenLocale|libnss_.*|libthread_db)\.so'

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib"
cp "$BUILD/src/gamescope" "$BUILD/src/gamescopectl" "$OUT/bin/"
for b in "$OUT"/bin/*; do
	ldd "$b" | awk '/=> \// { print $3 } /^[ \t]*\/[^ ]*ld-linux/ { print $1 }'
done | sort -u | while read -r lib; do
	name=$(basename "$lib")
	if echo "$name" | grep -Eq "$SKIP"; then
		continue
	fi
	cp -L "$lib" "$OUT/lib/$name"
done
[ -f "$OUT/lib/$LOADER" ] || { echo "no $LOADER in the ldd closure" >&2; exit 1; }
for f in "$OUT"/bin/* "$OUT"/lib/*; do
	if basename "$f" | grep -Eq "$GLIBC"; then
		continue
	fi
	patchelf --set-rpath "$PREFIX/lib" "$f"
done
for b in "$OUT"/bin/*; do
	patchelf --set-interpreter "$PREFIX/lib/$LOADER" "$b"
done
echo "$VERSION" > "$OUT/VERSION"
ls "$OUT/lib" > "$OUT/LIBS"
du -sh "$OUT"
