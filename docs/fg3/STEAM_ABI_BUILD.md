# Experimental package ABI build

Host: Debian, GCC14.2.0, glslang15.2.0, Vulkan-Headers v1.4.365
(c46850864f4661461b0f6cb9922c058ffea4915e). Host glibc2.41 headers initially
introduced __isoc23_strtol/strtoul/strtoull@GLIBC_2.38. Do not ship that result
as Steam Runtime compatible merely because it loads on the build host.

A separate SDK was extracted from Debian bullseye packages downloaded through
APT's signed InRelease/Packages verification (no signature/hash/TLS bypass):

| Package | Version | SHA-256 of original .deb |
|---|---|---|
| libc6 | 2.31-13+deb11u11 | 05f7264da867b37f4c5ce49266b558ea1e81e05a9464f623152fca70f3550282 |
| libc6-dev | 2.31-13+deb11u11 | e7f7b45d9c5cfcf37609f0b6efd3c645272c812144703af89dfd32218fcb0fd3 |
| libc-dev-bin | 2.31-13+deb11u11 | 289800bcbc9e3f2718c2e13e4f2c90f15843ffcf46b26d95a15b276fe2f9ac87 |
| linux-libc-dev | 5.10.223-1 | e3be603bd12377bb90fcfb0f2048ca3a81c886e49d6f09945b82159d1c54f7b2 |

Repository: https://deb.debian.org/debian bullseye main.
`dpkg-deb -x` extracts into a dedicated build sysroot; nothing is installed globally.
Absolute symlinks to SDK libraries must be relocated relative to their own directory,
otherwise the host filesystem can satisfy libpthread/libdl despite --sysroot.
Library search explicitly places SDK directories before GCC host search paths.
Original binaries remain unchanged and original package hashes are retained.

Build with scripts/build_native_steam_abi.sh, setting GFG_GLIBC_SYSROOT,
VULKAN_HEADERS, GFG_CORE_BUILD and GFG_NATIVE_BUILD. Pin glslang and compiler for
byte-identical rebuilds. The Makefile ABI check rejects requirements above2.31.
The production artifact requires GLIBC2.17 or older symbols and loads under the
actual2.31 loader using RTLD_NOW/dlsym. This is an ABI/load check, not Steam Deck
execution. Vulkan/WSI validation still runs against the cloud's host Mesa/loader.
The installed package uses system libc; it does not ship old glibc binaries.
