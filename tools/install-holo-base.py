#!/usr/bin/env python3
"""Install the prepared Holo image through the proven Quest USB RAM shell.
This writes one new image file on userdata; never flashes, changes slots or reboots.
Requires Python 3.12 (stdlib telnetlib). --check performs local checks only.
"""
import argparse, hashlib, json, re, socket, sys, time, uuid, warnings
from pathlib import Path
with warnings.catch_warnings():
 warnings.simplefilter('ignore',DeprecationWarning)
 try:
  import telnetlib
 except ImportError:
  raise SystemExit('This host helper requires Python 3.12: run python3.12 tools/install-holo-base.py')

class Shell:
 def __init__(self, host): self.connection=telnetlib.Telnet(host,23,timeout=10)
 def run(self, body, timeout=30):
  marker='Q1_DONE_'+uuid.uuid4().hex
  command=f"( set -eu; {body}\n); status=$?; printf '\\n{marker}:%s\\n' \"$status\"\n"
  self.connection.write(command.encode())
  pattern=re.compile(rb'(?:\r?\n)'+marker.encode()+rb':([0-9]+)\r?\n')
  index, match, output=self.connection.expect([pattern],timeout)
  if index<0: raise RuntimeError('USB shell command timed out: '+output.decode(errors='replace')[-1500:])
  if int(match.group(1))!=0: raise RuntimeError('USB shell command failed: '+output.decode(errors='replace')[-2000:])
  return output.decode(errors='replace')
 def close(self): self.connection.close()

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--image',type=Path,default=Path(__file__).resolve().parents[1]/'build/steamos/holo.img')
 p.add_argument('--host',default='192.168.77.1')
 p.add_argument('--port',type=int,default=4242)
 p.add_argument('--check',action='store_true')
 p.add_argument('--replace',action='store_true',help='replace an existing holo.img, keeping it as holo.img.prev (replacing an older .prev)')
 a=p.parse_args(); image=a.image.resolve(); info=json.loads(image.with_name('rootfs-info.json').read_text())
 if image.name!=info['image'] or image.stat().st_size!=info['bytes']: raise SystemExit('Image metadata mismatch')
 if not 1024<=a.port<=65535: raise SystemExit('Invalid transfer port')
 print(f'Checking local image ({info["bytes"]//1024**2} MiB)...',flush=True)
 with image.open('rb') as f: checksum=hashlib.file_digest(f,'sha256').hexdigest()
 if checksum!=info['sha256']: raise SystemExit('Image checksum mismatch')
 print(f'Verified {image.name}: {checksum}',flush=True)
 if a.check:
  print('Local checks passed; no device accessed.');return
 print('Installing one new file on userdata through the minimal USB shell.',flush=True)
 token=uuid.uuid4().hex
 exists_check=('[ ! -L /data/steamos/holo.img ]' if a.replace else
  "[ ! -e /data/steamos/holo.img ] && [ ! -L /data/steamos/holo.img ] || { echo 'holo.img already exists; refusing overwrite (use --replace)'; exit 1; }")
 publish=('rm -f /data/steamos/holo.img.prev\n[ ! -e /data/steamos/holo.img ] || mv /data/steamos/holo.img /data/steamos/holo.img.prev\n' if a.replace else '')
 partial=f'/data/steamos/.holo-{token}.partial'
 pidfile=f'/run/holo-install-{token}.pid'
 result=f'/run/holo-install-{token}.result'
 mountmarker=f'/run/holo-install-{token}.mounted'
 shell=Shell(a.host)
 try:
  # Refuse stock Android, a different kernel, or a pre-existing userdata mount.
  shell.run(f'''[ "$(uname -r)" = 4.4.205-perf-shell+ ] || {{ echo 'Boot the proven minimal shell first'; exit 1; }}
[ "$(id -u)" = 0 ]
if awk '$2=="/data" {{found=1}} END {{exit !found}}' /proc/mounts; then echo '/data is already mounted; refusing to take ownership'; exit 1; fi
UD=
for event in /sys/class/block/sd*/uevent; do
 if grep -q '^PARTNAME=userdata$' "$event"; then
  [ -z "$UD" ] || {{ echo 'Multiple userdata devices'; exit 1; }}
  name=${{event%/uevent}}; UD=/dev/${{name##*/}}
 fi
done
[ -b "$UD" ]
mkdir -p /data
mount -t ext4 -o rw,noatime "$UD" /data
touch {mountmarker} || {{ umount /data; exit 1; }}
''')
  shell.run(f'''[ ! -L /data/steamos ]
{exists_check}
available=$(df -Pk /data | tail -1 | awk '{{print $4}}')
[ "$available" -ge {(info['bytes']+1023)//1024+128*1024} ] || {{ echo 'Insufficient free userdata space'; exit 1; }}
mkdir -p /data/steamos
[ ! -e {partial} ]
setsid sh -c 'busybox nc -l -p {a.port} -w 60 < /dev/null > {partial}; echo $? > {result}' < /dev/null > /dev/null 2>&1 &
echo $! > {pidfile}
''')
  connection=None
  for _ in range(30):
   try: connection=socket.create_connection((a.host,a.port),timeout=5);break
   except ConnectionRefusedError: time.sleep(0.2)
  if connection is None: raise RuntimeError('USB file receiver did not start')
  connection.settimeout(60)
  with connection, image.open('rb') as source:
   sent=0;last=0
   while chunk:=source.read(1024*1024):
    connection.sendall(chunk);sent+=len(chunk)
    if sent-last>=256*1024**2 or sent==info['bytes']:
     print(f'Transferred {sent//1024**2}/{info["bytes"]//1024**2} MiB',flush=True);last=sent
   connection.shutdown(socket.SHUT_WR)
   while connection.recv(1024): pass
  print('Verifying the received image on the headset...',flush=True)
  shell.run(f'''for i in $(seq 1 30); do [ -f {result} ] && break; sleep 1; done
[ "$(cat {result})" = 0 ]
[ "$(stat -c %s {partial})" = {info['bytes']} ]
printf '%s  %s\\n' {checksum} {partial} | sha256sum -c -
# Publication on the same ext4 filesystem; with --replace the old image becomes holo.img.prev.
{publish}ln {partial} /data/steamos/holo.img
rm {partial}
sync
''',timeout=600)
  shell.run(f'umount /data && rm {mountmarker}',timeout=60)
  print('Installed and verified /data/steamos/holo.img; userdata is unmounted. No reboot performed.',flush=True)
 finally:
  try:
   shell.run(f'''if [ -f {pidfile} ] && [ ! -f {result} ]; then kill -TERM "-$(cat {pidfile})" 2>/dev/null || true; fi
rm -f {pidfile} {result}
''')
   shell.run(f'if [ -f {mountmarker} ]; then rm -f {partial}; sync; umount /data && rm {mountmarker}; fi',timeout=60)
  except Exception as error:
   print('Cleanup could not be confirmed; inspect the USB shell before reboot: '+str(error),file=sys.stderr)
  shell.close()

if __name__=='__main__': main()
