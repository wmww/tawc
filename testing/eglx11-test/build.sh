#!/bin/bash
# Build eglx11-test inside the chroot.
# Requires: gcc, libX11, EGL/GLES headers (provided by the chroot's
# mesa-utils + libglvnd / installed by testing/install-test-deps.sh).
set -e
cd "$(dirname "$0")"
gcc -o eglx11-test eglx11-test.c \
    $(pkg-config --cflags --libs x11) \
    -lEGL -lGLESv2 \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -ldl -Wall -Wextra
echo "Built: eglx11-test"
