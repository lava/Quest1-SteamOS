#!/bin/sh
# Run a program against Meta's Android libraries through libhybris (Vulkan on the Adreno, gralloc,
# libsyncboss): hybris-run vulkaninfo --summary. The device nodes (kgsl, ion, hwbinder, syncboss)
# are root-only, so run it as root.
# Installed as /usr/local/bin/hybris-run.
export LD_LIBRARY_PATH=/opt/hybris/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
# bionic code needs its TLS slot
export LD_PRELOAD=/opt/hybris/lib/libbionictls.so${LD_PRELOAD:+:$LD_PRELOAD}
export ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime
exec "$@"
