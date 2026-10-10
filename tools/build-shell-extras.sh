#!/usr/bin/env bash
# Build static ARM64 extras for the RAM-only minimal shell: fbtest and dropbear.
# Runs the compile inside the local Holo rootfs (QEMU), within cxx-build.slice.
# Output: build/minimal-shell/extras/{fbtest,dropbearmulti}
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
S=$P/build/steamos/sources
OUT=$P/build/minimal-shell/extras
DB=dropbear-2026.94
DB_SHA=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81

if [[ ${1:-} != --inside ]]; then
    systemctl --user cat cxx-build.slice >/dev/null
    echo "$DB_SHA  $S/dropbear-dl/$DB.tar.bz2" | sha256sum -c --quiet
    W=$S/shell-extras
    rm -rf "$W" && mkdir -p "$W"
    cp "$P/initrd/fbtest.c" "$W/"
    tar -xjf "$S/dropbear-dl/$DB.tar.bz2" -C "$W"
    cp "$0" "$W/build.sh"
    exec 9>/tmp/cxx-build.lock
    flock 9
    systemd-run --user --scope --quiet --slice=cxx-build.slice -- \
        "$P/tools/run-holo-local.sh" /bin/bash /build-sources/shell-extras/build.sh --inside
    mkdir -p "$OUT"
    install -m 755 "$W/fbtest" "$W/$DB/dropbearmulti" "$OUT/"
    (cd "$OUT" && sha256sum fbtest dropbearmulti > SHA256SUMS)
    file "$OUT/fbtest" "$OUT/dropbearmulti"
    exit 0
fi

# --- inside the ARM64 rootfs ---
unset CFLAGS CXXFLAGS LDFLAGS CPPFLAGS
cd /build-sources/shell-extras
gcc -static -O2 -Wall -o fbtest fbtest.c
cd $DB
cat > localoptions.h <<'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_X11FWD 0
#define DEFAULT_PATH "/bin"
#define DEFAULT_ROOT_PATH "/bin"
EOF
ac_cv_lib_crypt_crypt=no ./configure --disable-shadow --enable-static --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx \
    --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline
make -j8 PROGRAMS="dropbear dropbearkey" MULTI=1
