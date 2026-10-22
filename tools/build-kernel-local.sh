#!/usr/bin/env bash
# Build a Quest kernel from a prepared, configured source tree with Android's gcc 4.9 toolchains.
# Usage: tools/build-kernel-local.sh SRC OUT
#   SRC  Meta's kernel (see tools/build-boot.sh for the commit) with the patches applied:
#        kernel/quest1-kernel.patch and the other kernel/*.patch for the SteamOS kernel, or only
#        kernel/minimal-shell.patch for the minimal shell
#   OUT  build directory with a .config (updated with olddefconfig)
# TOOLCHAINS (default build/toolchains) holds aarch64-linux-android-4.9 and
# arm-linux-androideabi-4.9; tools/build-boot.sh kernel downloads the same ones.
# STOCK_CONFIG, when set, is compared with the result into OUT/config-diff.txt.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
[[ $# == 2 ]] || { echo "Usage: $0 SRC OUT" >&2; exit 2; }
src=$(realpath "$1")
out=$(realpath "$2")
tc=${TOOLCHAINS:-$P/build/toolchains}
args=(-C "$src" O="$out" ARCH=arm64
  CROSS_COMPILE="$tc/aarch64-linux-android-4.9/bin/aarch64-linux-android-"
  CROSS_COMPILE_ARM32="$tc/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-"
  CC="$tc/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc-4.9.x"
  HOSTCC=/usr/bin/gcc HOSTCXX=/usr/bin/g++
  'DTC_FLAGS=-Wno-unit_address_vs_reg -Wno-avoid_default_addr_size -Wno-reg_format'
  'HOSTCFLAGS=-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89 -fcommon')
export KBUILD_BUILD_USER=quest1 KBUILD_BUILD_HOST=quest-kernel-local
make "${args[@]}" olddefconfig
if [[ -n ${STOCK_CONFIG:-} ]]; then
  "$src/scripts/diffconfig" "$STOCK_CONFIG" "$out/.config" > "$out/config-diff.txt"
fi
make "${args[@]}" -j"${JOBS:-$(nproc)}" Image.gz-dtb modules
sha256sum "$out/arch/arm64/boot/Image.gz-dtb" > "$out/kernel.sha256"
printf 'Build finished successfully\n'
