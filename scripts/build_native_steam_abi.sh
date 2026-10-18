#!/usr/bin/env bash
# Build an independent component against an explicitly supplied older glibc SDK.
# The SDK is a build dependency only; no libc is shipped or installed system-wide.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${GFG_GLIBC_SYSROOT:?set to a verified relocatable glibc 2.31 SDK}"
: "${VULKAN_HEADERS:?set to Vulkan-Headers v1.4.365 include directory}"
sdk=$(cd -- "$GFG_GLIBC_SYSROOT" && pwd)
core=${GFG_CORE_BUILD:-"$root/engine/gfg-temporal/build-steam"}
native=${GFG_NATIVE_BUILD:-"$root/engine/gfg-native/build-steam"}
unset C_INCLUDE_PATH
export LIBRARY_PATH="$sdk/usr/lib/x86_64-linux-gnu:$sdk/lib/x86_64-linux-gnu"
flags="-std=c11 -O2 -Wall -Wextra -Werror --sysroot=$sdk -L$sdk/usr/lib/x86_64-linux-gnu -L$sdk/lib/x86_64-linux-gnu"
make -C "$root/engine/gfg-temporal" BUILD="$core" CFLAGS="$flags" VK_CFLAGS="-I$VULKAN_HEADERS" lib
make -C "$root/engine/gfg-native" ENABLE_TE=1 BUILD="$native" TE_LIB="$core/libgfg-temporal.a" \
  CFLAGS="$flags -fPIC" VULKAN_HEADERS="$VULKAN_HEADERS" layer abi
