#!/bin/bash
# Build the glibc-side repro binary inside the rootfs.
#
# tls_lib.so (the bionic-built TLS library that repro loads) is
# cross-compiled with the Android NDK on the host by
# scripts/install-test-deps.sh and copied into this directory before
# build.sh runs — see the libhybris-tls-repro hook there.
set -e
cd "$(dirname "$0")"
gcc -O0 -g repro.c -o libhybris-tls-repro \
    -L/usr/lib/hybris -lhybris-common -Wl,-rpath,/usr/lib/hybris \
    -Wall -Wextra -pthread
echo "Built: libhybris-tls-repro"
