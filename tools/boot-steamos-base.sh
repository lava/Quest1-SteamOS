#!/usr/bin/env bash
# Invoke manually when ready to test. Never flashes, erases, or switches slots.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
# STEAMOS_ARTIFACT selects another build directory holding boot-steamos-base.img + boot.sha256
artifact="${STEAMOS_ARTIFACT:-$project/build/steamos}"
image="$artifact/boot-steamos-base.img"
(cd "$artifact" && sha256sum -c boot.sha256)
if [[ ${1:-} == --check ]]; then
    printf 'Image checksum verified; no device accessed.\n'
    exit 0
fi
if [[ $# != 0 ]]; then
    printf 'Usage: %s [--check]\n' "$0" >&2
    exit 2
fi
fb=${FASTBOOT:-}
if [[ -z "$fb" ]]; then
    fb=$(command -v fastboot || true)
fi
if [[ -z "$fb" ]]; then
    fb="$HOME/.local/share/QuestStack/platform-tools/37.0.1/linux-x64/fastboot"
fi
[[ -x "$fb" ]] || { echo 'Set FASTBOOT to the path of the fastboot executable.' >&2; exit 1; }
device_output=$("$fb" devices)
mapfile -t devices < <(printf '%s\n' "$device_output" | awk '$2 == "fastboot" {print $1}')
if [[ ${#devices[@]} == 0 ]]; then
    if [[ "$device_output" == *"no permissions"* ]]; then
        printf '%s\n' "$device_output" >&2
        echo 'Linux USB permissions prevent fastboot access; check the headset udev rules.' >&2
        exit 1
    fi
    adb="$(dirname "$fb")/adb"
    if [[ -x "$adb" ]]; then
        adb_output=$("$adb" devices 2>/dev/null || true)
        if printf '%s\n' "$adb_output" | awk '$2 == "sideload" {found=1} END {exit !found}'; then
            echo 'The headset is in ADB sideload mode (firmware-update receiver), not fastboot.' >&2
            echo 'Return to the USB Update Mode menu and stay there; do not select Sideload Update.' >&2
            echo 'If needed, hold Power to turn it off, then hold Volume Down and press Power.' >&2
            exit 1
        fi
    fi
    echo 'No fastboot device found. From power off, hold Volume Down and press Power.' >&2
    echo 'Stay at the USB Update Mode menu; do not select Sideload Update.' >&2
    exit 1
elif [[ ${#devices[@]} != 1 ]]; then
    echo 'More than one fastboot device is connected. Leave only the Quest connected.' >&2
    exit 1
fi
serial=${devices[0]}
product=$("$fb" -s "$serial" getvar product 2>&1)
[[ "$product" =~ product:[[:space:]]*monterey([[:space:]]|$) ]] || {
    echo 'The connected fastboot device is not a Quest 1 (monterey).' >&2; exit 1;
}
unlocked=$("$fb" -s "$serial" getvar unlocked 2>&1)
[[ "$unlocked" =~ unlocked:[[:space:]]*(yes|true)([[:space:]]|$) ]] || {
    echo 'The bootloader did not report an unlocked state.' >&2; exit 1;
}
slot=$("$fb" -s "$serial" getvar current-slot 2>&1)
[[ "$slot" =~ current-slot:[[:space:]]*([ab])([[:space:]]|$) ]] || {
    echo 'Could not read the current slot; no boot attempted.' >&2; exit 1;
}
echo "Current slot: ${BASH_REMATCH[1]}"
echo 'Loading the SteamOS base kernel into RAM using fastboot boot.'
echo 'An installed /data/steamos/holo.img will be mounted read-write; otherwise this starts a rescue shell.' 
"$fb" -s "$serial" boot "$image"
printf '\nConnect with: telnet 192.168.77.1\n'
printf 'Fallback: USB serial /dev/ttyACM* (115200 baud).\n'
printf 'In the shell: uname -a; cat /proc/mounts; dmesg\n'
printf 'In SteamOS, return cleanly with: systemctl reboot\n'
printf 'If still in the initramfs rescue shell, use: reboot -f\n'
