#!/usr/bin/env bash
# Run commands in the local ARM64 rootfs under QEMU; no host service starts.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
W=$P/build/steamos
ROOT=$W/rootfs
QEMU=$P/build/minimal-shell/downloads/qemu/usr/bin/qemu-aarch64-static
if [[ ! -x $QEMU ]]; then
 QEMU=$(find "$P/build/minimal-shell/downloads" -name qemu-aarch64-static -type f -print -quit)
fi
mkdir -p "$W/proot-tmp"
exec bwrap --die-with-parent --unshare-user --uid 0 --gid 0 --unshare-pid --unshare-ipc \
 --ro-bind / / --bind "$ROOT" "$ROOT" --bind "$W/sources" "$W/sources" --bind "$W/proot-tmp" /tmp --proc /proc --dev /dev \
 --chdir "$ROOT" \
 "$W/sources/proot/src/proot" -0 -r "$ROOT" -b /dev -b /proc -b /sys \
 -b /etc/resolv.conf:/etc/resolv.conf -b "$W/sources":/build-sources -b "$P/tools":/prepare-tools -b "$P/src/monado":/prepare-monado -b "$P/src/libhybris":/prepare-libhybris -q "$QEMU" -w / "$@"
