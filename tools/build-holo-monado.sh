#!/bin/bash
# Build Monado (Quest 1 driver) inside the Holo rootfs: run through tools/run-holo-local.sh.
# Environment:
#   MONADO_SRC=/build-sources/monado  source tree (cfa6078 + src/monado/quest1-monado.patch)
#   MONADO_BUILD_DIR=build-quest1     build directory, relative to MONADO_SRC
#   MONADO_SLAM=0                     1: OpenCV and SLAM tracking (t_slam loads libbasalt.so
#                                     through VIT); needs OpenCV in the rootfs
#   MONADO_INSTALL=1                  0: build only; no install to /opt/monado, no xrgears
set -euo pipefail
export CMAKE_BUILD_PARALLEL_LEVEL=8
MONADO_SRC=${MONADO_SRC:-/build-sources/monado}
MONADO_BUILD_DIR=${MONADO_BUILD_DIR:-build-quest1}
MONADO_SLAM=${MONADO_SLAM:-0}
MONADO_INSTALL=${MONADO_INSTALL:-1}
mkdir -p /opt/hybris/include/ahb/android /opt/hybris/include/linux
for source in /build-sources/quest1-headers/linux/*.h; do
 target=/opt/hybris/include/linux/${source##*/}
 cmp -s "$source" "$target" || cp "$source" "$target"
done
cmp -s /build-sources/android-headers/android/rect.h /opt/hybris/include/ahb/android/rect.h || cp /build-sources/android-headers/android/rect.h /opt/hybris/include/ahb/android/
# Only the public AHB declarations are needed under glibc: expose API 29 and
# strip bionic-only availability annotations from the Android header.
header_tmp=$(mktemp)
{ printf '#ifndef __ANDROID_API__\n#define __ANDROID_API__ 29\n#endif\n';
 sed -E 's/__INTRODUCED_IN\([0-9]+\)//g' /build-sources/android-headers/android/hardware_buffer.h;
} > "$header_tmp"
cmp -s "$header_tmp" /opt/hybris/include/ahb/android/hardware_buffer.h || cp "$header_tmp" /opt/hybris/include/ahb/android/hardware_buffer.h
rm "$header_tmp"
cd "$MONADO_SRC"
if git apply --check /prepare-monado/public-base-build.patch 2>/dev/null; then
 git apply /prepare-monado/public-base-build.patch
else
 git apply --reverse --check /prepare-monado/public-base-build.patch
fi
opts=()
while read -r option; do
 [ "$option" = XRT_BUILD_DRIVER_QUEST1 ] || opts+=("-D$option=OFF")
done < <(grep -oE 'XRT_BUILD_DRIVER_[A-Z0-9_]+' CMakeLists.txt | sort -u)
if [ "$MONADO_SLAM" = 1 ]; then
 # OpenCV's libraries pull in ffmpeg, whose libOpenCL.so.1 the linker would otherwise take from
 # the RPATH (/opt/hybris/lib: libhybris's, without symbol versions) instead of ocl-icd's.
 opts+=(-DXRT_HAVE_OPENCV=ON -DXRT_FEATURE_SLAM=ON
  -DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,/usr/lib -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-rpath-link,/usr/lib)
else
 opts+=(-DXRT_HAVE_OPENCV=OFF -DXRT_FEATURE_SLAM=OFF)
fi
cmake -S . -B "$MONADO_BUILD_DIR" -G Ninja \
 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/monado \
 -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
 -DCMAKE_INSTALL_RPATH=/opt/hybris/lib \
 -DVulkan_LIBRARY=/opt/hybris/lib/libvulkan.so \
 -DXRT_HYBRIS_AHB=ON -DXRT_BUILD_DRIVER_QUEST1=ON \
 -DXRT_FEATURE_STEAMVR_PLUGIN=OFF -DXRT_FEATURE_OPENVR=OFF \
 -DXRT_FEATURE_DEBUG_GUI=OFF -DXRT_FEATURE_WINDOW_PEEK=OFF \
 -DXRT_HAVE_SDL2=OFF -DBUILD_TESTING=OFF \
 -DXRT_BUILD_SAMPLES=OFF "${opts[@]}"
cmake --build "$MONADO_BUILD_DIR" --parallel 8 --target monado-service cli openxr_monado monado-ctl monado
[ "$MONADO_INSTALL" = 1 ] || exit 0
cmake --install "$MONADO_BUILD_DIR"

# xrgears demo (OpenXR client), installed to /usr/local
pacman -S --needed --noconfirm meson glm
xrgears_rev=51ef6c779c8d3134d8df9eca1294779e61c8243f
if [ ! -d /build-sources/xrgears/.git ]; then
 git clone https://gitlab.freedesktop.org/monado/demos/xrgears.git /build-sources/xrgears
fi
cd /build-sources/xrgears
[ "$(git rev-parse HEAD)" = "$xrgears_rev" ] || { git fetch origin "$xrgears_rev" 2>/dev/null || true; git checkout -q "$xrgears_rev"; }
[ -d build ] || meson setup build --prefix=/usr/local --buildtype=release
ninja -C build -j8
ninja -C build install
