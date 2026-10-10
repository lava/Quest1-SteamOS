#!/bin/sh
# Run an OpenXR client against the Monado service (monado.service): xr-run xrgears,
# xr-run hello_xr -g Vulkan2. Clients need root (the GPU device nodes are root-only).
# Installed as /usr/local/bin/xr-run.
export XR_RUNTIME_JSON=/opt/monado/share/openxr/1/openxr_monado.json
export XDG_RUNTIME_DIR=/run/monado
exec hybris-run "$@"
