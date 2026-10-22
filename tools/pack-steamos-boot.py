#!/usr/bin/env python3
"""Package the SteamOS base boot image using the local stock boot header and a built kernel.

Does not access a connected headset. The output can be started with fastboot boot or flashed
to boot_a (docs/holo-image.md).
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--stock-boot', type=Path, required=True)
p.add_argument('--kernel-out', type=Path, required=True)
p.add_argument('--busybox', type=Path, required=True)
p.add_argument('--init', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
stock = a.stock_boot.read_bytes()
assert stock[:8] == b'ANDROID!', 'Expected Android boot image'
ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, version, osv = struct.unpack_from('<10I', stock, 8)
assert version == 0 and page == 4096 and ssize == 0, 'Unexpected stock header layout'
manifest = a.stock_boot.parent / 'manifest.json'
if manifest.exists():
    expected = next(x['Sha256'] for x in json.loads(manifest.read_text())['Partitions'] if x['Name'] == 'boot')
    assert hashlib.sha256(stock).hexdigest().lower() == expected.lower(), 'Stock backup hash mismatch'

image = (a.kernel_out / 'arch/arm64/boot/Image.gz-dtb').read_bytes()
d = zlib.decompressobj(31)
raw = d.decompress(image) + d.flush()
assert d.eof and raw[56:60] == b'ARM\x64', 'Expected gzip-compressed ARM64 kernel'
dtbs = d.unused_data
remaining = dtbs
count = 0
while remaining:
    assert remaining[:4] == b'\xd0\x0d\xfe\xed', 'Invalid device-tree magic'
    size = struct.unpack_from('>I', remaining, 4)[0]
    assert 40 <= size <= len(remaining), 'Invalid device-tree length'
    remaining = remaining[size:]
    count += 1
assert count > 0
assert raw.count(b'skip_initramfs\0') == 1, 'Expected exactly one legacy-SAR argument handler'
# Quest ABL appends skip_initramfs. Neutralize it only in this packaged kernel.
raw = raw.replace(b'skip_initramfs\0', b'want_initramfs\0')
kernel = gzip.compress(raw, compresslevel=9, mtime=0) + dtbs

busybox = a.busybox.read_bytes()
assert busybox[:4] == b'\x7fELF' and struct.unpack_from('<H', busybox, 18)[0] == 183
init = a.init.read_text()
assert 'switch_root' in init and '/data/steamos' in init
a.output.parent.mkdir(parents=True, exist_ok=True)
spec = a.output.parent / 'initramfs.spec'
entries = [f'dir /{name} 755 0 0' for name in ('dev','bin','etc','proc','sys','run','tmp','data','newroot')]
entries += ['nod /dev/console 600 0 0 c 5 1', 'nod /dev/kmsg 644 0 0 c 1 11',
            f'file /bin/busybox {a.busybox.resolve()} 755 0 0',
            'slink /bin/sh busybox 777 0 0', f'file /init {a.init.resolve()} 755 0 0']
spec.write_text('\n'.join(entries) + '\n')
archive = subprocess.check_output([str(a.kernel_out / 'usr/gen_init_cpio'), '-t', '0', str(spec)])
ramdisk = gzip.compress(archive, compresslevel=9, mtime=0)
(a.output.parent / 'ramdisk.cpio.gz').write_bytes(ramdisk)
header = bytearray(stock[:1632])
# Match the port's tested entropy configuration for systemd startup.
cmdline = stock[64:576].split(b'\0',1)[0] + stock[608:1632].split(b'\0',1)[0]
if b'rng_core.default_quality=' not in cmdline:
    cmdline += b' rng_core.default_quality=700'
assert len(cmdline) < 1535
header[64:576] = cmdline[:511].ljust(512,b'\0')
header[608:1632] = cmdline[511:].ljust(1024,b'\0')
struct.pack_into('<I', header, 8, len(kernel))
struct.pack_into('<I', header, 16, len(ramdisk))
digest = hashlib.sha1()
for blob in (kernel, ramdisk, b''):
    digest.update(blob)
    digest.update(struct.pack('<I', len(blob)))
header[576:608] = digest.digest() + bytes(12)
pad = lambda b: b + bytes(-len(b) % page)
output = pad(header) + pad(kernel) + pad(ramdisk)
a.output.write_bytes(output)
info = {'stock_boot_sha256': hashlib.sha256(stock).hexdigest(),
        'input_kernel_sha256': hashlib.sha256(image).hexdigest(),
        'busybox_sha256': hashlib.sha256(busybox).hexdigest(),
        'init_sha256': hashlib.sha256(a.init.read_bytes()).hexdigest(),
        'unsigned_boot_sha256': hashlib.sha256(output).hexdigest(),
        'header_version': version, 'page_size': page, 'device_trees': count,
        'kernel_bytes': len(kernel), 'ramdisk_bytes': len(ramdisk),
        'kernel_patch': 'skip_initramfs -> want_initramfs (packaged copy only)',
        'cmdline': cmdline.decode(), 'boot_tested': False}
(a.output.parent / 'image-info.json').write_text(json.dumps(info, indent=2) + '\n')
print(json.dumps(info, indent=2))
