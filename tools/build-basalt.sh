#!/usr/bin/env bash
# Build Basalt (Monado's fork, VIT interface 2.0.1 as in Monado cfa6078) for SLAM tracking.
#   tools/build-basalt.sh host     x86-64 tools (basalt_vio, basalt_calibrate, ... with the
#                                  Pangolin GUI) into build/basalt-host/install, for offline
#                                  evaluation of EuRoC datasets. Dependencies come from a pinned
#                                  nixpkgs (binary cache only, nothing system-wide).
#   tools/build-basalt.sh aarch64  libbasalt.so (for Monado) and the tools (basalt_vio, run with
#                                  --show-gui 0) for the headset, cross-compiled with a pinned
#                                  clang against the Holo rootfs (build/steamos/rootfs), into
#                                  build/basalt-aarch64/install/opt/basalt.
# All compilation runs in the shared cxx-build.slice, serialized by /tmp/cxx-build.lock.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd -P)
SRC=$P/build/steamos/sources/basalt
BASALT_URL=https://gitlab.freedesktop.org/mateosss/basalt.git
BASALT_REV=df6e970c8da7636eb401a09e3317fbeaaf829b9a  # main, 2026-07-24, VIT 2.0.1
NIXPKGS=github:NixOS/nixpkgs/8edc0c72e3a38faf5434e40d1f19431125fe4b30
JOBS=8
R=$P/build/steamos/rootfs
HOLO=https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3
# Build-only packages for the cross build: extracted into build/basalt-aarch64/deps, next to the
# rootfs sysroot, never installed in the image. At runtime libbasalt.so needs onetbb, hwloc, fmt
# and OpenCV in the image; basalt_vio also boost-libs and libepoxy. OpenCV: Basalt is built
# against Arch's 4.12.0 package (only its libopencv_* are linked, not their dependencies); the
# image needs an OpenCV 4.12 with the same sonames (libopencv_core.so.412, ...).
# repo filename sha256 [subtree to extract]
CROSS_PKGS="extra onetbb-2022.3.0-2-aarch64.pkg.tar.zst f08ac1cad31d7f12c25ab979e214ff22c1b370b7656a29d1b2fd3c011e6cb063
extra hwloc-2.12.2-1-aarch64.pkg.tar.zst f811b5b7ff4c86e0353427b0463982041ddc26f75c7b4bd8e05cd89db6a74199
extra fmt-12.1.0-1-aarch64.pkg.tar.zst a756a84a5ecdd9a1d545d32c304d60304eb98f49cf9775ae7b788dccf0e5612a
extra boost-1.89.0-3-aarch64.pkg.tar.zst 3cf4f9945a3127d0afa5441944040e20af6832c8de4c983834885a102569f5da
extra boost-libs-1.89.0-3-aarch64.pkg.tar.zst ede5f59d40ec7aa4a7c9625261ba6ae32b4222c47ab9920668506aa1ec9ac180
extra libepoxy-1.5.10-3-aarch64.pkg.tar.zst e31b35835f495a2cfbc3ffeb001793a56e2cca3b8e2051d00654fb0376916461
extra mesa-1:25.2.7-1-aarch64.pkg.tar.zst f30cf864cf9c76a160fbbf1ea0344f3207fcfae5422daa12501c2818fee9e9de usr/include
extra opencv-4.12.0-13-aarch64.pkg.tar.zst 94e775dfb72fa050cb237389ea46a7f7a06ed77bb5b3d304b00ab177a54e8cda"

limited() {  # run a command in the shared C++ build budget
	flock /tmp/cxx-build.lock systemd-run --user --wait --pipe --quiet --same-dir --expand-environment=no \
		--slice=cxx-build.slice "$@"
}

fetch() {
	if [[ ! -d $SRC/.git ]]; then
		git clone "$BASALT_URL" "$SRC"
	fi
	if [[ $(git -C "$SRC" rev-parse HEAD) != "$BASALT_REV" ]]; then
		git -C "$SRC" fetch origin "$BASALT_REV" 2>/dev/null || git -C "$SRC" fetch origin
		git -C "$SRC" checkout -q "$BASALT_REV"
	fi
	git -C "$SRC" submodule update --init --recursive
}

host() {
	local out=$P/build/basalt-host
	mkdir -p "$out"
	# The basalt-monado package's build environment provides OpenCV, oneTBB, fmt, boost, lz4,
	# bzip2, GLEW, libepoxy and X11; the sources (with their submodules) are ours. The nix compiler
	# wrapper drops -march=native (NIX_ENFORCE_NO_NATIVE), so the binaries are generic x86-64.
	# build/basalt-host/nix-profile keeps the libraries (RUNPATH into /nix/store) from GC.
	limited "$(command -v nix)" develop --profile "$out/nix-profile" "$NIXPKGS#basalt-monado" --command bash -euc '
		cmake -S "$1" -B "$2/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX="$2/install" -DCMAKE_INSTALL_LIBDIR=lib \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DBASALT_INSTANTIATIONS_DOUBLE=OFF -DBASALT_BUILD_TESTS=OFF -DBUILD_TESTS=OFF \
			-DBASALT_ENABLE_ROSBAG2=OFF -DCXX_MARCH=native
		ninja -C "$2/build" -j"$3"
		ninja -C "$2/build" install
		# CMake treats the nix library dirs as implicit and leaves them out of the RUNPATH
		rp=$2/install/lib:$(dirname "$(c++ -print-file-name=libstdc++.so)")
		for f in $NIX_LDFLAGS; do
			if [[ $f == -L/nix/store/* && :$rp: != *:${f#-L}:* ]]; then rp+=:${f#-L}; fi
		done
		for f in "$2"/install/bin/* "$2"/install/lib/*.so*; do
			if [[ -f $f && ! -L $f ]] && head -c4 "$f" | grep -q ELF; then
				patchelf --set-rpath "$rp" "$f"
			fi
		done' _ "$SRC" "$out" "$JOBS"
	echo "Host Basalt in $out/install/bin"
}

aarch64() {
	local out=$P/build/basalt-aarch64 llvm bin lld deps tc
	[[ -x $R/usr/bin/pacman ]] || { echo "No Holo rootfs at $R" >&2; exit 1; }
	mkdir -p "$out"
	# Pinned clang/lld from nixpkgs (binary cache); the host clang 18 is older than the rootfs's
	# GCC 15 libstdc++. The unwrapped clang sets no ELF interpreter: pass it to the linker.
	llvm=$(nix build --no-link --print-out-paths "$NIXPKGS#llvmPackages_21.clang-unwrapped")
	bin=$(nix build --no-link --print-out-paths "$NIXPKGS#llvmPackages_21.llvm")
	lld=$(nix build --no-link --print-out-paths "$NIXPKGS#llvmPackages_21.lld")
	nix build --no-link "$NIXPKGS#llvmPackages_21.clang-unwrapped.lib"  # resource dir
	deps=$out/deps
	local D=$P/build/steamos/downloads/packages repo file sha sub
	mkdir -p "$D"
	rm -rf "$deps" && mkdir -p "$deps"
	while read -r repo file sha sub; do
		[[ -f $D/$file ]] || curl -fsSL -o "$D/$file" "$HOLO/$repo/os/aarch64/${file//:/%3A}"
		echo "$sha  $D/$file" | sha256sum -c --quiet
		tar -C "$deps" -xf "$D/$file" --exclude='.*' ${sub:-usr}
	done <<< "$CROSS_PKGS"
	tc=$out/toolchain.cmake
	cat > "$tc" <<-EOF
		set(CMAKE_SYSTEM_NAME Linux)
		set(CMAKE_SYSTEM_PROCESSOR aarch64)
		set(CMAKE_SYSROOT $R)
		set(CMAKE_C_COMPILER $llvm/bin/clang)
		set(CMAKE_CXX_COMPILER $llvm/bin/clang++)
		set(CMAKE_C_COMPILER_TARGET aarch64-unknown-linux-gnu)
		set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-gnu)
		set(CMAKE_AR $bin/bin/llvm-ar)
		set(CMAKE_RANLIB $bin/bin/llvm-ranlib)
		set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=$lld/bin/ld.lld -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 -Wl,-rpath-link,$deps/usr/lib -Wl,--allow-shlib-undefined -Wl,--as-needed")
		set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=$lld/bin/ld.lld -Wl,-rpath-link,$deps/usr/lib -Wl,--as-needed")
		set(CMAKE_FIND_ROOT_PATH $deps)
		set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
		set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
		set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
		set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
		# No pkg-config: it only finds Wayland for Pangolin, whose wayland-scanner step cannot
		# run here; Pangolin then has X11 and headless EGL windowing.
		set(ENV{PKG_CONFIG_LIBDIR} /nonexistent)
	EOF
	# Two builds into one prefix. "tools": basalt_vio and the other tools; they always include
	# Pangolin, run them with --show-gui 0 on the headset. "lib": the libbasalt.so that Monado's
	# SLAM tracker loads (VIT), without Pangolin, boost or X11; installed last, so it replaces the
	# tools build's library. Cortex-A73/A53: armv8-a. TBB_USE_GLIBCXX_VERSION: the rootfs's
	# libstdc++ (GCC 15.2), not clang's version. MOLD_PROGRAM: Basalt would replace the
	# toolchain's linker flags with -fuse-ld=mold if it finds the host's mold.
	rm -rf "$out/install"
	local kind
	for kind in tools lib; do
		local flags=(-DBASALT_BUILD_SHARED_LIBRARY_ONLY=OFF -DBASALT_BUILD_VISUALIZATION=ON)
		[[ $kind == lib ]] && flags=(-DBASALT_BUILD_SHARED_LIBRARY_ONLY=ON -DBASALT_BUILD_VISUALIZATION=OFF)
		limited cmake -S "$SRC" -B "$out/build-$kind" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" \
			-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/basalt \
			-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_RPATH=/opt/basalt/lib \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCXX_MARCH=armv8-a -DTBB_USE_GLIBCXX_VERSION=150201 \
			-DMOLD_PROGRAM=MOLD_PROGRAM-NOTFOUND \
			-DBASALT_INSTANTIATIONS_DOUBLE=OFF -DBASALT_BUILD_TESTS=OFF -DBUILD_TESTS=OFF \
			-DBASALT_ENABLE_ROSBAG2=OFF "${flags[@]}"
		limited ninja -C "$out/build-$kind" -j"$JOBS"
		DESTDIR=$out/install ninja -C "$out/build-$kind" install
	done
	echo "aarch64 Basalt in $out/install/opt/basalt (copy to /opt/basalt in the rootfs)"
}

case ${1:-} in
	host) fetch; host ;;
	aarch64) fetch; aarch64 ;;
	*) echo "Usage: $0 host|aarch64" >&2; exit 2 ;;
esac
