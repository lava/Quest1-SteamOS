#!/bin/sh
# Build q1passthrough in the Holo rootfs (tools/run-holo-local.sh), output next to the source:
#   tools/run-holo-local.sh /bin/sh /build-sources/q1passthrough/build.sh
# The source dir must be visible there (copied to build/steamos/sources/q1passthrough).
set -e
cd "$(dirname "$0")"
gcc -std=gnu11 -O2 -Wall -Wextra -I. -o q1passthrough q1passthrough.c -lopenxr_loader -lvulkan -lm
