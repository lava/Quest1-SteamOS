#!/bin/sh
# Build q1record in the Holo rootfs (tools/run-holo-local.sh), output next to the source:
#   tools/run-holo-local.sh /bin/sh /build-sources/q1record/build.sh
# The source dir and quest1_cameras_shm.h must be visible there (copied to build/steamos/sources/q1record).
set -e
cd "$(dirname "$0")"
gcc -std=gnu11 -O2 -Wall -Wextra -I. -o q1record q1record.c
