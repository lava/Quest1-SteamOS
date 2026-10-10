#!/usr/bin/env bash
# Install OpenSSH on the running Holo (over the USB telnet shell) and let in one public key.
# Usage: tools/install-holo-ssh.sh [PUBKEY]   (default ~/.ssh/id_ed25519.pub)
# Key-only login as root; passwords are refused. Safe to rerun.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
KEY=${1:-$HOME/.ssh/id_ed25519.pub}
PKG=openssh-10.2p1-2-aarch64.pkg.tar.zst
SHA=b96ab7c370317b95543420e34b544317272d48560d0a559d83de8dc501073713
URL=https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/core/os/aarch64/$PKG
D=$P/build/steamos/downloads/ssh
qsh() { uv run --no-project python "$P/tools/qsh.py" "$@"; }

mkdir -p "$D"
[[ -f $D/$PKG ]] || curl -fsSL -o "$D/$PKG" "$URL"
echo "$SHA  $D/$PKG" | sha256sum -c --quiet
pub=$(head -1 "$KEY")
[[ $pub == ssh-* && $pub != *"'"* ]] || { echo "Not a usable public key: $KEY" >&2; exit 1; }

qsh --put "$D/$PKG" "/tmp/$PKG"
qsh - <<EOF
set -e
echo '$SHA  /tmp/$PKG' | sha256sum -c
pacman -Q openssh >/dev/null 2>&1 || pacman -U --noconfirm /tmp/$PKG
rm -f /tmp/$PKG
install -d -m 700 /root/.ssh
touch /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
grep -qxF '$pub' /root/.ssh/authorized_keys || echo '$pub' >> /root/.ssh/authorized_keys
grep -q '^Include /etc/ssh/sshd_config.d/' /etc/ssh/sshd_config
install -d /etc/ssh/sshd_config.d
cat > /etc/ssh/sshd_config.d/10-quest1.conf <<'CONF'
PermitRootLogin prohibit-password
PasswordAuthentication no
KbdInteractiveAuthentication no
CONF
ssh-keygen -A
sshd -t
systemctl enable --now sshd.service
systemctl is-active sshd.service
EOF
echo "Done: ssh root@192.168.77.1"
