#!/bin/sh
# Start the modem subsystem, which hosts the WCN3990 Wi-Fi firmware, and the qcacld driver (wlan0).
# Derived from holo-android-modem.sh, but writes nothing persistent:
#  - every Android partition (all of sd* except userdata's disk) is set read-only in the kernel;
#  - the partitions the modem side writes (modemst1/2, fsg, fsc via rmt_storage; "private" = /persist
#    via tftp_server) are RAM copies on loop devices, so changes are dropped at reboot;
#  - Android's system_b and modem_b are mounted read-only.
# Installed as /usr/local/sbin/quest1-modem, run once per boot by quest1-modem.service.
set -eu
Q=/run/quest1-modem
SLOT=_b
E="env ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime"
VNDK="LD_LIBRARY_PATH=/system/lib64/vndk-29:/system/lib64/vndk-sp-29:/system/lib64"
V=/system/vendor/bin
part() { grep -l "^PARTNAME=$1\$" /sys/class/block/sd*/uevent | head -1 | cut -d/ -f5; }
ismnt() { grep -q " $1 " /proc/mounts; }
log() { echo "quest1-modem: $*"; }

# Once per boot only: reloading the modem after a shutdown from Linux hangs the SoC.
[ -e $Q/started ] && { log "already started this boot"; exit 0; }
mkdir -p $Q

# 1. Kernel-level write protection. Userdata (holo.img) and its whole disk stay writable.
ud=$(part userdata)
uddisk=$(basename "$(readlink -f /sys/class/block/$ud/..)")
for d in /sys/class/block/sd*; do
	n=${d##*/}
	[ "$n" = "$ud" ] || [ "$n" = "$uddisk" ] || blockdev --setro /dev/$n
done

# 2. RAM copies of the partitions the modem side writes
mkdir -p /dev/block/bootdevice/by-name
for n in modemst1 modemst2 fsg fsc private; do
	[ -f $Q/$n.img ] && continue
	dd if=/dev/$(part $n) of=$Q/$n.img bs=1M 2>/dev/null
	l=$(losetup -f --show $Q/$n.img)
	echo "$n $l" >> $Q/loops
	[ $n = private ] || ln -sf $l /dev/block/bootdevice/by-name/$n
done

# 3. Android layout: system_b (system-as-root) and modem_b read-only, /data in RAM
ismnt /android || mount -t ext4 -o ro,noload /dev/$(part system$SLOT) /android
ismnt /system || mount --bind /android/system /system
ismnt /apex || mount -t tmpfs -o mode=755 tmpfs /apex
for a in /android/system/apex/*; do
	n=${a##*/}; n=${n%.release}
	mkdir -p /apex/$n; ismnt /apex/$n || mount --bind $a /apex/$n
done
# fstab.monterey options: tftp_server (uid vendor_rfs) must read wlanmdsp.mbn
ismnt /firmware || mount -t vfat -o ro,shortname=lower,uid=1000,gid=1000,dmask=222,fmask=333 /dev/$(part modem$SLOT) /firmware
ismnt /system/vendor/firmware_mnt || mount --bind /firmware /system/vendor/firmware_mnt
ismnt /data || mount -t tmpfs -o mode=771 tmpfs /data
mkdir -p /run/fw
for f in /system/vendor/firmware/* /firmware/image/*; do ln -sf $f /run/fw/; done

# 4. /persist = RAM copy of "private"; a journal replay, if any, touches only the copy
ismnt /persist || mount -t ext4 -o nosuid,nodev $(awk '$1=="private"{print $2}' $Q/loops) /persist
ismnt /mnt/vendor/persist || mount --bind /persist /mnt/vendor/persist
mkdir -p /data/vendor/tombstones/rfs/modem /data/vendor/wifi/wpa/sockets /data/vendor/wifi/sockets /dev/socket
chown -R 2951:2951 /data/vendor/tombstones/rfs
chmod 0771 /data/vendor/tombstones/rfs /data/vendor/tombstones/rfs/modem

# 5. bionic property area
python3 /usr/local/lib/quest1/mkproparea.py /dev/__properties__ \
	-s ro.hardware=monterey -s ro.boot.hardware=monterey -s ro.board.platform=msm8998 \
	-s hwservicemanager.ready=true -s ro.boot.slot_suffix=$SLOT \
	/android/system/build.prop /android/system/vendor/build.prop /android/system/etc/prop.default /android/default.prop

# 6. Daemons, in init.monterey.rc order
touch $Q/started
# systemd-shutdown must not kill them either: spare this service's cgroup until reboot(). systemd
# itself only sets the user.* xattr, which kernel 4.4 rejects on cgroupfs; systemd-shutdown also
# reads trusted.*.
if [ -n "${INVOCATION_ID:-}" ]; then
	cg=/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)
	setfattr -n trusted.survive_final_kill_signal -v 1 "$cg" || log "cannot mark $cg to survive shutdown"
fi
echo -n /run/fw > /sys/module/firmware_class/parameters/path
echo 2 > /sys/class/firmware/timeout   # no ueventd to answer the user-helper fallback
$E $V/irsc_util /vendor/etc/sec_config > $Q/irsc_util.log 2>&1 || true
# pd-mapper must be up before the modem boots: icnss looks up the Wi-Fi PD once
setsid $E $V/qrtr-ns -f > $Q/qrtr-ns.log 2>&1 < /dev/null &
setsid $E $V/pd-mapper > $Q/pd-mapper.log 2>&1 < /dev/null &
sleep 2
setsid $E $V/rmt_storage > $Q/rmt_storage.log 2>&1 < /dev/null &
setsid $E $V/tftp_server > $Q/tftp_server.log 2>&1 < /dev/null &
sleep 1
# Holding /dev/subsys_modem open boots the modem (pm-service does this on Android)
setsid sh -c 'exec 3</dev/subsys_modem; exec sleep infinity' > $Q/subsys.log 2>&1 < /dev/null &
# cnss-daemon blocks in libbinder until a context manager exists, then sends bdwlan.bin
setsid $E /system/bin/servicemanager > $Q/servicemanager.log 2>&1 < /dev/null &
setsid $E $V/vndservicemanager /dev/vndbinder > $Q/vndservicemanager.log 2>&1 < /dev/null &
sleep 1
setsid $E $VNDK $V/cnss-daemon -n -l > $Q/cnss-daemon.log 2>&1 < /dev/null &
sleep 2
echo 1 > /sys/kernel/boot_wlan/boot_wlan
for i in $(seq 1 30); do [ -e /sys/class/net/wlan0 ] && break; sleep 1; done
[ -e /sys/class/net/wlan0 ] && log "wlan0 up" || { log "wlan0 did not appear"; exit 1; }
