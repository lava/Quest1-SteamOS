# SteamOS from Valve's public Holo base, installed on slot A

This fork's own path to a SteamOS system on the Quest 1. It runs next to the upstream one
(`initrd/init`, `device/holo-*.sh`, the hand-assembled image of [rootfs-build.md](rootfs-build.md)):

| | Upstream | This path |
|---|---|---|
| System image | Assembled by hand on the headset | Built on a PC from Valve's public Holo Core ARM64 preview (`mash-20251118.3`) |
| Steam, SteamVR | Valve's arm64 builds (not redistributable) | Not included: Monado and OpenXR clients only |
| Android files | Slot A; the modem setup writes `persist`, `modemst1/2` | Slot B, all read-only; the modem's writable partitions are RAM copies |
| Boot | `fastboot boot` every time | `fastboot boot`, or flashed to `boot_a` with stock Android kept on slot B |
| Head tracking | 3DoF | 6DoF with Basalt SLAM on the bottom camera pair ([tracking-6dof.md](tracking-6dof.md)) |

Tested on one headset whose stock system runs on slot B (firmware 49845030443200410), with slot A
holding the older firmware left by the unlock. Every step below that writes to the headset is
marked as such.

## What runs

- `initrd/steamos-base-init`: USB network (`192.168.77.1`, DHCP for the PC), a root telnet
  shell and a USB serial shell, then `/data/steamos/holo.img` on a loop device and Holo's systemd.
  Without an image it stays in the RAM rescue shell.
- `device/quest1-modem.sh`: modem and Wi-Fi firmware, with every Android partition set read-only.
- `device/quest1-gpu.sh`, `hybris-run`: the Adreno Vulkan driver through libhybris.
- `device/quest1-audio.sh`: ADSP and the CM710X sound card with Android's speaker route.
- Monado (`monado.service`) with the quest1 driver: display, IMU, tracking cameras, SLAM.
- `device/quest1-slot.py` (`quest1-slot.service`): marks the booted slot successful.
- Key-only SSH for root, Wi-Fi from a local configuration (`tools/add-holo-remote.sh`).

## Building

The image is not fully scripted yet. What is:

1. Kernels: Meta's kernel at the commit in `tools/build-boot.sh`, patched and configured, built with
   `tools/build-kernel-local.sh SRC OUT`. SteamOS kernel: `kernel/quest1-kernel.patch` and the
   other `kernel/*.patch` (as `tools/build-boot.sh kernel` applies them). Minimal shell: Meta's
   kernel with only `kernel/minimal-shell.patch`.
2. Boot images: `tools/pack-steamos-boot.py` (SteamOS) and `tools/pack-minimal-shell.py`; both take
   the header values from a backup of the stock boot image, then `magiskboot sign`.
3. Rootfs, in `build/steamos/rootfs`, through `tools/run-holo-local.sh` (QEMU user mode and proot,
   no root needed): libhybris (`tools/build-holo-public.sh`), Monado
   (`tools/build-holo-monado.sh`), Basalt (`tools/build-basalt.sh aarch64`), then
   `tools/add-holo-remote.sh SECRETS_DIR` with `wpa_supplicant-wlan0.conf` and `root.pub`.
4. `tools/finalize-holo-rootfs.sh build/steamos/images/<name>/holo.img` packs the ext4 image.

Still manual: downloading and extracting `system.rootfs.zst`, running `tools/prepare-holo-base.py`
on it, the first package installs and fetching the libhybris, Monado and proot sources. A
`holo.img` built this way contains the Wi-Fi password: never publish it.

## Installing

Back up first. From the minimal shell (`tools/boot-minimal-shell.sh`, RAM only), set every disk
read-only (`blockdev --setro`) and copy `boot_a`, `boot_b`, `misc`, the GPT of each disk and the
modem partitions to the PC, checking each against a hash taken on the headset.

1. Image (writes userdata): boot the minimal shell, then `python3.12 tools/install-holo-base.py`.
   It copies `holo.img` to `/data/steamos/` and checks it on the headset. A factory reset deletes it.
2. Try it (writes only the image): from fastboot, `tools/boot-steamos-base.sh`. Connect with
   `ssh root@192.168.77.1` or `telnet 192.168.77.1`.
3. Permanent (writes `boot_a` and the slot flags): from fastboot,

   ```sh
   fastboot flash boot_a build/steamos/boot-steamos-base.img
   fastboot set_active a
   fastboot reboot
   ```

   The headset now starts SteamOS without a PC. `quest1-slot` marks slot A successful once the
   system is up; without that the bootloader falls back to slot B after 7 boots. Slot A's own,
   older firmware runs everything above, so nothing else on slot A needs changing.

## Getting back to stock

From SteamOS: `systemctl --no-block --reboot-argument=bootloader reboot` reaches fastboot (the
`systemctl reboot bootloader` form fails on systemd 258). Then:

```sh
fastboot set_active b
fastboot reboot
```

Stock Android marks slot B successful again on its first boot. From stock,
`adb reboot bootloader` reaches fastboot without the button combination.
