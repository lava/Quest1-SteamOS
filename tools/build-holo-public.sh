#!/bin/bash
set -euo pipefail
export CMAKE_BUILD_PARALLEL_LEVEL=8
cd /build-sources/android-headers
sed -i 's/ANDROID_VERSION_MAJOR 11/ANDROID_VERSION_MAJOR 10/' android-version.h
make install PREFIX=/opt/hybris
cd /build-sources/libhybris
if git apply --check /prepare-libhybris/vulkan-arm64.patch 2>/dev/null; then
 git apply /prepare-libhybris/vulkan-arm64.patch
else
 git apply --reverse --check /prepare-libhybris/vulkan-arm64.patch
fi
cd hybris
./autogen.sh --prefix=/opt/hybris --enable-arch=arm64 --enable-adreno-quirks --enable-wayland \
 --with-android-headers=/opt/hybris/include/android \
 --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/apex/com.android.runtime/lib64/bionic \
 CFLAGS='-g -O2 -std=gnu11 -DVK_ENABLE_BETA_EXTENSIONS' CXXFLAGS='-g -O2 -std=gnu++17'
make -j8
make install
cc -O2 -fPIC -shared -o /opt/hybris/lib/libbionictls.so /build-sources/bionictls.c -ldl -pthread
