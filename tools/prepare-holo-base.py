#!/usr/bin/env python3
"""Configure an extracted public Holo rootfs for a USB-only first boot.
Run under the same fakeroot ownership database as extraction and mkfs.
No headset access, package installation, modem or Android HAL services.
"""
from pathlib import Path
import argparse, os
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('rootfs', type=Path)
a=p.parse_args(); root=a.rootfs.resolve()
assert (root/'usr/lib/systemd/systemd').is_file()
def write(path, text, mode=0o644):
 f=root/path; f.parent.mkdir(parents=True,exist_ok=True)
 if f.is_symlink(): f.unlink()
 f.write_text(text); f.chmod(mode); os.chown(f,0,0)
def link(path, target):
 f=root/path; f.parent.mkdir(parents=True,exist_ok=True)
 if f.exists() or f.is_symlink(): f.unlink()
 f.symlink_to(target); os.lchown(f,0,0)
write('etc/hostname','holo-quest\n')
write('etc/fstab','# Root is an ext4 loop image mounted by the Quest initramfs.\n')
write('etc/machine-id','uninitialized\n')
write('etc/resolv.conf','nameserver 1.1.1.1\nnameserver 8.8.8.8\n')
write('etc/systemd/network/10-usb0.network','''[Match]
Name=usb0
[Network]
Address=192.168.77.1/24
DHCPServer=yes
ConfigureWithoutCarrier=yes
[DHCPServer]
PoolOffset=2
PoolSize=1
EmitDNS=no
EmitRouter=no
''')
write('etc/systemd/system/serial-getty@ttyGS0.service.d/autologin.conf','''[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 115200,57600,38400,9600 - $TERM
''')
link('etc/systemd/system/default.target','/usr/lib/systemd/system/multi-user.target')
for service in ['systemd-networkd.service','systemd-networkd.socket']:
 target='sockets.target' if service.endswith('.socket') else 'multi-user.target'
 if (root/'usr/lib/systemd/system'/service).exists():
  link(f'etc/systemd/system/{target}.wants/{service}',f'/usr/lib/systemd/system/{service}')
link('etc/systemd/system/getty.target.wants/serial-getty@ttyGS0.service','/usr/lib/systemd/system/serial-getty@.service')
for service in ['systemd-resolved.service','systemd-resolved-varlink.socket','systemd-resolved-monitor.socket','systemd-firstboot.service','NetworkManager.service','systemd-networkd-wait-online.service']:
 link('etc/systemd/system/'+service,'/dev/null')
# This image contains no network credentials and permits root only on the USB console.
# Early initramfs telnet remains available, bound to 192.168.77.1.
(root/'userdata').mkdir(exist_ok=True)
write('etc/quest1-base-release','''SOURCE=holo-core-aarch64-preview/mash-20251118.3
PURPOSE=USB-only first systemd boot
STEAM_CLIENT=absent
STEAMVR=absent
MONADO_AUTOSTART=disabled
ANDROID_HAL_AUTOSTART=disabled
EXPECTED_ANDROID_SLOT=_b
''')
# Pacman's sandbox requires newer kernel APIs unavailable on Quest's 4.4.
f=root/'etc/pacman.conf'; text=f.read_text()
if '\nDisableSandbox\n' not in text: f.write_text(text.replace('[options]','[options]\nDisableSandbox',1))
for directory in root.rglob('*'):
 if directory.is_dir() and not directory.is_symlink(): os.chown(directory,0,0)
print('Configured USB-only Holo base at',root)
