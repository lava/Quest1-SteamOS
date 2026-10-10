#!/usr/bin/env bash
# Package the minimal RAM shell plus fbtest and key-only dropbear SSH.
# Reuses the proven minimal-shell kernel; writes only build/minimal-shell-ssh/.
# Usage: STOCK_BOOT=<backup of the stock boot image> tools/build-minimal-shell-ssh.sh [PUBKEY]
#   (PUBKEY default ~/.ssh/id_ed25519.pub). Run tools/build-shell-extras.sh first.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
M=$P/build/minimal-shell
O=$P/build/minimal-shell-ssh
KEY=${1:-$HOME/.ssh/id_ed25519.pub}
STOCK=${STOCK_BOOT:?Set STOCK_BOOT to a backup of the stock boot image}

(cd "$M/extras" && sha256sum -c --quiet SHA256SUMS)
mkdir -p "$O"
head -1 "$KEY" > "$O/authorized_keys"
python3 "$P/tools/pack-minimal-shell.py" \
    --stock-boot "$STOCK" --kernel-out "$M/kernel-out" \
    --busybox "$M/downloads/busybox/bin/busybox" --init "$P/initrd/shell-init" \
    --dir /root:700 --dir /root/.ssh:700 \
    --add "/root/.ssh/authorized_keys:$O/authorized_keys:600" \
    --add "/bin/dropbearmulti:$M/extras/dropbearmulti:755" \
    --add "/bin/fbtest:$M/extras/fbtest:755" \
    --output "$O/boot-minimal-shell-ssh.img" > "$O/pack.log"
"$M/downloads/magiskboot" sign "$O/boot-minimal-shell-ssh.img"
"$M/downloads/magiskboot" verify "$O/boot-minimal-shell-ssh.img"
(cd "$O" && sha256sum boot-minimal-shell-ssh.img > SHA256SUMS && cat SHA256SUMS)
