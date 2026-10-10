#!/usr/bin/env bash
# Run only after public-userspace builds have stopped; creates a new holo.img.
# Usage: tools/finalize-holo-rootfs.sh [OUTPUT]   (default build/steamos/holo.img; must not exist.
# rootfs-info.json is written next to it, so give each new image its own directory.)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
W=$P/build/steamos
OUT=$(realpath -m "${1:-$W/holo.img}")
exec 9>/tmp/cxx-build.lock
flock 9
# PRoot creates inaccessible empty placeholders for synthetic bind points.
# Remove only these known empty placeholders and restore ordinary runtime dirs.
for path in build-sources prepare-tools host-rootfs; do
 if [[ -d "$W/rootfs/$path" && ! -L "$W/rootfs/$path" ]]; then
  chmod 755 "$W/rootfs/$path"
  rmdir "$W/rootfs/$path"
 fi
done
for path in dev proc sys; do chmod 755 "$W/rootfs/$path"; done
# PRoot masks this file with /dev/null and creates an empty mode-000 placeholder.
preload=$W/rootfs/etc/ld.so.preload
if [[ -f $preload && ! -L $preload && ! -s $preload && $(stat -c %a "$preload") == 0 ]]; then
 rm "$preload"
fi
fakeroot -- python3 "$P/tools/pack-holo-rootfs.py" \
 --rootfs "$W/rootfs" --base "$W/downloads/system.rootfs.zst" \
 --packages "$W/downloads/packages" --output "$OUT"
