#!/usr/bin/env python3
"""Create the prepared ext4 image without root mounts. Invoke under fakeroot.
Restores source/package ownership metadata and capability xattrs explicitly.
"""
import argparse, hashlib, json, os, subprocess, tarfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--rootfs',type=Path,required=True)
p.add_argument('--base',type=Path,required=True)
p.add_argument('--packages',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args(); root=a.rootfs.resolve(); out=a.output.resolve()
assert os.getuid()==0, 'Run under fakeroot to preserve Unix ownership'
assert (root/'etc/quest1-base-release').is_file()
assert not out.exists(), 'Refusing to overwrite an existing image; choose a new output'
# Local files default to root. Source tar/package metadata overrides packaged owners.
os.lchown(root,0,0)
for d,dirs,files in os.walk(root):
 for name in dirs+files: os.lchown(Path(d)/name,0,0)
xattrs={}; archives=[a.base]+sorted(a.packages.glob('*.pkg.tar.zst'))
for archive in archives:
 stream=subprocess.Popen(['zstd','-dc',str(archive)],stdout=subprocess.PIPE)
 with tarfile.open(fileobj=stream.stdout,mode='r|') as tar:
  for item in tar:
   rel=item.name.removeprefix('./'); path=root/rel
   assert not rel.startswith('/') and '..' not in Path(rel).parts
   assert path == root or path.parent.resolve().is_relative_to(root), f'Archive ancestor escapes root: {rel}'
   if not path.exists() and not path.is_symlink(): continue
   os.lchown(path,item.uid,item.gid)
   # The extraction/install preserves permission bits; don't overwrite modified config modes.
   for key,value in item.pax_headers.items():
    if key.startswith('SCHILY.xattr.'):
     xattrs[(rel,key.removeprefix('SCHILY.xattr.'))]=value.encode('utf-8','surrogateescape')
 # Drain archive padding before waiting, or a full pipe can deadlock zstd.
 while stream.stdout.read(1024*1024): pass
 assert stream.wait()==0
out.parent.mkdir(parents=True,exist_ok=True)
with out.open('wb') as f: f.truncate(4*1024**3)
subprocess.run(['mke2fs','-q','-t','ext4','-L','holo-quest','-m','0','-O','^orphan_file,^metadata_csum_seed,^metadata_csum','-d',str(root),str(out)],check=True)
attrs_dir=out.parent/'rootfs-xattrs';attrs_dir.mkdir(exist_ok=True)
restored=[]
for index,((rel,key),value) in enumerate(sorted(xattrs.items())):
 assert '"' not in rel and '\n' not in rel and ' ' not in key
 blob=attrs_dir/f'{index}.bin';blob.write_bytes(value)
 command=f'ea_set -f {blob} /{rel} {key}'
 subprocess.run(['debugfs','-w','-R',command,str(out)],check=True)
 check=attrs_dir/f'{index}.verified.bin'
 subprocess.run(['debugfs','-R',f'ea_get -f {check} /{rel} {key}',str(out)],check=True)
 assert check.read_bytes()==value, f'Extended attribute did not round trip: {rel} {key}'
 restored.append({'path':rel,'name':key,'hex':value.hex()})
subprocess.run(['e2fsck','-fn',str(out)],check=True)
metadata={'image':out.name,'bytes':out.stat().st_size,'sha256':hashlib.file_digest(out.open('rb'),'sha256').hexdigest(),'base_sha256':hashlib.sha256(a.base.read_bytes()).hexdigest(),'restored_xattrs':restored,'boot_tested':False}
(out.parent/'rootfs-info.json').write_text(json.dumps(metadata,indent=2)+'\n')
print(json.dumps(metadata,indent=2))
