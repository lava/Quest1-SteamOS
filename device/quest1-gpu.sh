#!/bin/sh
# Make Meta's Adreno Vulkan driver and gralloc the HAL defaults for libhybris. Android picks them
# by ro.hardware/ro.board.platform; libhybris loads the *.default.so. The overlay lives in RAM;
# system_b stays read-only. Needs quest1-modem's Android mounts (/system on system_b).
# Installed as /usr/local/sbin/quest1-gpu, run once per boot by quest1-gpu.service, before the
# hwservicemanager, gralloc allocator and configstore services.
set -eu
H=/system/vendor/lib64/hw
mkdir -p /run/quest1-hw/up /run/quest1-hw/work
mountpoint -q $H || mount -t overlay overlay -o lowerdir=$H,upperdir=/run/quest1-hw/up,workdir=/run/quest1-hw/work $H
ln -sf vulkan.msm8998.so $H/vulkan.default.so
ln -sf gralloc.msm8998.so $H/gralloc.default.so
