#!/usr/bin/env bash
# Invoke manually when ready to test. Never flashes, erases, or switches slots.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
artifact="$project/build/minimal-shell"
image="$artifact/boot-minimal-shell.img"
ssh=0
if [[ ${1:-} == --ssh ]]; then
    # Same kernel, plus fbtest and key-only SSH (tools/build-minimal-shell-ssh.sh).
    ssh=1
    artifact="$project/build/minimal-shell-ssh"
    image="$artifact/boot-minimal-shell-ssh.img"
    shift
fi
(cd "$artifact" && sha256sum -c SHA256SUMS)
if [[ ${1:-} == --check ]]; then
    printf 'Image checksum verified; no device accessed.\n'
    exit 0
fi
if [[ $# != 0 ]]; then
    printf 'Usage: %s [--ssh] [--check]\n' "$0" >&2
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
echo 'Loading the minimal shell into RAM using fastboot boot.'
"$fb" -s "$serial" boot "$image"
printf '\nConnect with: telnet 192.168.77.1\n'
[[ $ssh == 1 ]] && printf 'Or: ssh -o HostKeyAlias=quest-shell root@192.168.77.1\n'
printf 'Fallback: USB serial /dev/ttyACM* (115200 baud).\n'
printf 'In the shell: uname -a; cat /proc/mounts; dmesg\n'
printf 'Return to the normal OS with: reboot -f\n'
