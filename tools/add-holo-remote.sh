#!/usr/bin/env bash
# Add remote access to the local Holo rootfs (build/steamos/rootfs) before finalize-holo-rootfs.sh:
#  - Wi-Fi without persistent writes (quest1-modem.service, wpa_supplicant, dhcpcd on wlan0)
#  - the GPU through libhybris (quest1-gpu.service, Android HAL daemons, hybris-run)
#  - iw, ip (iproute2) and hostname (inetutils) for work on the headset
#  - network sockets for non-root (Android "inet" group), so timesyncd sets the clock; networkd on 4.4
#  - OpenSSH: root login with a key only
# Secrets come from SECRETS_DIR (default ~/.local/share/QuestStack/quest1-remote), never the repo:
#   wpa_supplicant-wlan0.conf (Wi-Fi network and password)  root.pub (SSH key for root)
# The resulting holo.img contains the Wi-Fi password: keep it local, never publish it.
# Usage: tools/add-holo-remote.sh [SECRETS_DIR]
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd -P)
W=$P/build/steamos
R=$W/rootfs
S=${1:-$HOME/.local/share/QuestStack/quest1-remote}
REPO=https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3
# repo filename sha256 (from the snapshot's core.db/extra.db)
PKGS="core openssh-10.2p1-2-aarch64.pkg.tar.zst b96ab7c370317b95543420e34b544317272d48560d0a559d83de8dc501073713
core libnl-3.11.0-1-aarch64.pkg.tar.zst 2c7d4af1dec8c1f444635c4d890b75a9c0cf035565a5eecbea8d4f01deaba959
extra duktape-2.7.0-7-aarch64.pkg.tar.zst 24807fc3fa8d08db6ae5b8fd262ffcbb957a2380ed6113e5f30dccd5e1ff1935
extra polkit-126-2.2-aarch64.pkg.tar.zst fb172ac3222cfa49538b1afb0433ff7b50a355af4d82c229a40c9cc5fb62689e
extra pcsclite-2.4.0-3-aarch64.pkg.tar.zst 1660db51aa01e87ad8a3530c2b4a3483c8d0fc58c7266979adf26efd1fb3f65c
core wpa_supplicant-2:2.11-3-aarch64.pkg.tar.zst 3201a7af3766be5f15e8899d31a61d4fe30b4133e46b22312e12c8c9d79a213b
core iw-6.17-1-aarch64.pkg.tar.zst f586ce187e398fff8cb8994c0709d2566e65de53e71f0040cb6c29a23958f0a7
extra dhcpcd-10.3.0-1-aarch64.pkg.tar.zst 6e5d59463cec53b7e450d7435ee296c320599c5dfbea7a9e42eee5a9d9142e7c
core libmnl-1.0.5-2-aarch64.pkg.tar.zst fa3c76a8c9072cbf32dcf59245a0cd9a4a2922f00c84a21cb01f7495aa8c190d
core libnftnl-1.3.0-1-aarch64.pkg.tar.zst be0acce068196ce2682c90612120f6e57c2ba93a93b0f86ea7f1d1b5da68d937
core libpcap-1.10.5-3-aarch64.pkg.tar.zst b799454c2162f56acfe630df20ee73c05007d0ccd79825a53a8c261d0b6d0f4b
core libnfnetlink-1.0.2-2-aarch64.pkg.tar.zst eb2836eb830b2a587e293ce89548ba822c14c92cd385ee70666c346e2146c3a4
core libnetfilter_conntrack-1.0.9-2-aarch64.pkg.tar.zst 708988b0ec002283b30d7e948f87c4217a1ced878be8d3fa1659d22290a2f50b
core iptables-1:1.8.11-2-aarch64.pkg.tar.zst 35171376ec63b9f78427d7465da4f7f7d7adc51970233580161a57d97962c476
core libbpf-1.5.1-1-aarch64.pkg.tar.zst 130e09fb57d4b26342829793d62f9b56814d2b8da1e53252fed66c05445e2f17
core iproute2-6.17.0-1-aarch64.pkg.tar.zst b05e855eb60c40f2b353f7e3ab24e24e6093bd7ac80e733c3b579555c8182e55
core inetutils-2.6-1-aarch64.pkg.tar.zst 0f7237debe5e205e942edff6ef76506bdb85d55c03ddcd2e54a362974b4688cf"

[[ -f $R/etc/quest1-base-release ]] || { echo "No prepared rootfs at $R" >&2; exit 1; }
for f in wpa_supplicant-wlan0.conf root.pub; do
	[[ -s $S/$f ]] || { echo "Missing secret $S/$f" >&2; exit 1; }
done
for k in root.pub; do
	grep -qE '^(ssh-|ecdsa-)' "$S/$k" || { echo "Not a public key: $S/$k" >&2; exit 1; }
done

# 1. Packages: download, verify against the snapshot's database hashes, install in the rootfs
D=$W/downloads/packages
I=$W/sources/quest1-remote-pkgs
mkdir -p "$D" && rm -rf "$I" && mkdir -p "$I"
files=()
while read -r repo file sha; do
	[[ -f $D/$file ]] || curl -fsSL -o "$D/$file" "$REPO/$repo/os/aarch64/${file//:/%3A}"
	echo "$sha  $D/$file" | sha256sum -c --quiet
	cp "$D/$file" "$I/"
	files+=("/build-sources/quest1-remote-pkgs/$file")
done <<< "$PKGS"
exec 9>/tmp/cxx-build.lock
flock 9
"$P/tools/run-holo-local.sh" /usr/bin/pacman -U --needed --noconfirm "${files[@]}"
"$P/tools/run-holo-local.sh" /bin/sh -c '
	# CONFIG_ANDROID_PARANOID_NETWORK: only root and GID 3003 (AID_INET) may open IPv4/IPv6
	# sockets; anyone else gets EACCES ("Temporary failure in name resolution").
	getent group inet >/dev/null || groupadd -r -g 3003 inet
	usermod -aG inet alpm  # pacman DownloadUser, should it be enabled'
rm -rf "$I"

# 2. Files. pack-holo-rootfs.py makes everything not owned by a package root:root, modes kept.
put() { install -D -m "$1" "$2" "$R/$3"; }
link() { mkdir -p "$(dirname "$R/$1")"; ln -sfn "$2" "$R/$1"; }
unit() { printf '%s\n' "$2" > "$R/etc/systemd/system/$1"; chmod 644 "$R/etc/systemd/system/$1"; }

put 755 "$P/device/quest1-modem.sh" usr/local/sbin/quest1-modem
put 644 "$P/tools/mkproparea.py" usr/local/lib/quest1/mkproparea.py
mkdir -p "$R/android" "$R/system" "$R/firmware" "$R/apex" "$R/persist" "$R/mnt/vendor/persist" "$R/data"
link vendor /system/vendor
unit quest1-modem.service '[Unit]
Description=Modem subsystem and WCN3990 Wi-Fi firmware (no persistent writes)
# Never stopped from Linux: closing /dev/subsys_modem shuts the modem down and the next load hangs
# the SoC. Not part of the shutdown transaction (no Conflicts=shutdown.target), and quest1-modem
# marks its cgroup so that systemd-shutdown spares the daemons until the final reboot().
DefaultDependencies=no
After=local-fs.target sysinit.target
IgnoreOnIsolate=yes
# Only tries user.* (rejected by kernel 4.4), but keeps systemd from removing the trusted.* xattr
SurviveFinalKillSignal=yes

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/quest1-modem
TimeoutStartSec=120

[Install]
WantedBy=multi-user.target'
link etc/systemd/system/multi-user.target.wants/quest1-modem.service /etc/systemd/system/quest1-modem.service

# GPU: Meta's Adreno Vulkan driver through libhybris (/opt/hybris, from the base build). It needs
# the HAL overlay (quest1-gpu) and three Android daemons from system_b: the HIDL registry, the
# graphics allocator (gralloc buffers) and configstore (libvulkan waits forever without it).
[[ -f $R/opt/hybris/lib/libbionictls.so ]] || { echo "No libhybris in $R/opt/hybris" >&2; exit 1; }
put 755 "$P/device/quest1-gpu.sh" usr/local/sbin/quest1-gpu
put 755 "$P/device/hybris-run.sh" usr/local/bin/hybris-run
unit quest1-gpu.service '[Unit]
Description=Adreno Vulkan and gralloc as the libhybris HAL defaults
# quest1-modem mounts system_b; it may still fail later, on the Wi-Fi
Wants=quest1-modem.service
After=quest1-modem.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/quest1-gpu'
AENV='Environment=ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime'
unit android-hwservicemanager.service "[Unit]
Description=Android hwservicemanager (HIDL registry)
Requires=quest1-gpu.service
After=quest1-gpu.service

[Service]
$AENV
ExecStart=/system/bin/hwservicemanager
Restart=on-failure

[Install]
WantedBy=multi-user.target"
hal() { # name description binary
	unit "android-$1.service" "[Unit]
Description=Android HAL: $2
Requires=android-hwservicemanager.service
After=android-hwservicemanager.service

[Service]
$AENV
# hwservicemanager.ready is preset in the property area: give the registry time to start
ExecStartPre=/bin/sleep 0.5
ExecStart=$3
Restart=on-failure
RestartSec=1

[Install]
WantedBy=multi-user.target"
	link "etc/systemd/system/multi-user.target.wants/android-$1.service" "/etc/systemd/system/android-$1.service"
}
link etc/systemd/system/multi-user.target.wants/android-hwservicemanager.service /etc/systemd/system/android-hwservicemanager.service
hal gralloc "graphics allocator" /system/vendor/bin/hw/android.hardware.graphics.allocator@2.0-service
hal configstore configstore /system/vendor/bin/hw/android.hardware.configstore@1.1-service

# wpa_supplicant-nl80211@wlan0 reads wpa_supplicant-nl80211-wlan0.conf
put 600 "$S/wpa_supplicant-wlan0.conf" etc/wpa_supplicant/wpa_supplicant-nl80211-wlan0.conf
rm -f "$R/etc/wpa_supplicant/wpa_supplicant-wlan0.conf"
# Started when the driver creates wlan0
link etc/systemd/system/sys-subsystem-net-devices-wlan0.device.wants/wpa_supplicant-nl80211@wlan0.service /usr/lib/systemd/system/wpa_supplicant-nl80211@.service
link etc/systemd/system/sys-subsystem-net-devices-wlan0.device.wants/dhcpcd@wlan0.service /usr/lib/systemd/system/dhcpcd@.service

# SSH: keys only, root with the key from root.pub.
put 644 /dev/stdin etc/ssh/sshd_config.d/10-quest1.conf <<'EOF'
PermitRootLogin prohibit-password
PasswordAuthentication no
KbdInteractiveAuthentication no
EOF
link etc/systemd/system/multi-user.target.wants/sshd.service /usr/lib/systemd/system/sshd.service
put 600 "$S/root.pub" root/.ssh/authorized_keys
chmod 700 "$R/root/.ssh"

# Time: timesyncd runs as systemd-timesync and needs inet (see above) to resolve and reach NTP
# servers. Without it the clock stays wrong (the RTC reads 1970) and TLS fails, e.g. in pacman.
put 644 /dev/stdin etc/systemd/system/systemd-timesyncd.service.d/10-android-inet.conf <<'EOF'
[Service]
SupplementaryGroups=inet
EOF
# networkd: kernel 4.4 has no nexthop objects; enumerating them fails fatally
# ("Could not enumerate nexthops: Operation not supported").
put 644 /dev/stdin etc/systemd/networkd.conf.d/10-quest1-kernel-4.4.conf <<'EOF'
[Network]
ManageForeignNextHops=no
EOF
# usb0: keep the initramfs address even when networkd stops; Wi-Fi (dhcpcd) decides "online",
# so a missing USB cable doesn't stop timesyncd.
n=$R/etc/systemd/network/10-usb0.network
grep -q '^KeepConfiguration=' "$n" || sed -i 's/^ConfigureWithoutCarrier=yes$/&\nKeepConfiguration=yes/' "$n"
grep -q '^RequiredForOnline=' "$n" || sed -i 's/^\[DHCPServer\]$/[Link]\nRequiredForOnline=no\n&/' "$n"
# Kernel 4.4 lacks what networkd needs for IPv6 router discovery and its DHCP server (both fail
# with "Protocol not available" and put usb0 in the failed state). The initramfs keeps busybox
# udhcpd running for the host instead.
sed -i 's/^DHCPServer=yes$/DHCPServer=no/' "$n"
grep -q '^IPv6AcceptRA=' "$n" || sed -i 's/^KeepConfiguration=yes$/&\nIPv6AcceptRA=no/' "$n"

# Shutdown: without kernel/cgroup-events-frozen.patch systemd never notices that a cgroup became
# empty, so every stop that waits for leftover processes runs into its timeout twice. Short
# timeouts bound that; harmless with the patch.
put 644 /dev/stdin etc/systemd/system.conf.d/90-quest1-shutdown.conf <<'EOF'
[Manager]
DefaultTimeoutStopSec=10s
EOF
put 644 /dev/stdin etc/systemd/system/user@.service.d/90-quest1-shutdown.conf <<'EOF'
[Service]
TimeoutStopSec=10s
EOF
# dhcpcd -x (ExecStop) and then SIGTERM each hang for the full timeout at shutdown, once wlan0 is
# going away; nothing needs the lease released.
put 644 /dev/stdin etc/systemd/system/dhcpcd@wlan0.service.d/90-quest1-shutdown.conf <<'EOF'
[Service]
ExecStop=
TimeoutStopSec=3s
EOF
# udev's v4l_id hangs for minutes on the msm camera nodes (workers killed after 3 min), and its open
# of the Venus nodes starts a firmware load before /run/fw exists. Nothing needs v4l names.
link etc/udev/rules.d/60-persistent-v4l.rules /dev/null

f=$R/etc/quest1-base-release
grep -q '^REMOTE_ACCESS=' "$f" || cat >> "$f" <<'EOF'
REMOTE_ACCESS=wifi+ssh (image contains the Wi-Fi password: do not publish)
EOF
echo "Remote access added to $R. Next: tools/finalize-holo-rootfs.sh build/steamos/images/<name>/holo.img"
