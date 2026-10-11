#!/bin/sh
# Audio: boot the ADSP, after which the msm8998-cm710x sound card registers and the CM710X codec
# loads its firmware from /run/fw (set up by quest1-modem); then Android's mixer defaults and the
# speaker playback route (PRI_MI2S_RX <- MultiMedia1 = hw:0,0) from system_b's mixer_paths.xml.
# Installed as /usr/local/sbin/quest1-audio, run once per boot by quest1-audio.service.
set -eu
log() { echo "quest1-audio: $*"; }

# Once per boot. Never write 2 (unload) to boot_adsp: reloading a subsystem after a shutdown from
# Linux hangs the SoC (seen with the modem). The kernel's adsp-loader keeps the ADSP referenced.
if ! [ -e /dev/snd/controlC0 ]; then
	echo 1 > /sys/kernel/boot_adsp/boot
	for i in $(seq 1 40); do [ -e /dev/snd/controlC0 ] && break; sleep 0.25; done
fi
[ -e /dev/snd/controlC0 ] || { log "no sound card after booting the ADSP"; exit 1; }
log "$(head -1 /proc/asound/cards)"
python3 /usr/local/lib/quest1/mixer-paths.py /system/vendor/etc/mixer_paths.xml deep-buffer-playback speaker
