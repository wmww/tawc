#!/bin/bash
set -e
cd "$(dirname "$0")"

XDG_PROTO=$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
[ -f "$XDG_PROTO" ] || { echo "missing xdg-shell.xml at $XDG_PROTO" >&2; exit 1; }

wayland-scanner client-header "$XDG_PROTO" xdg-shell-client-protocol.h
wayland-scanner private-code  "$XDG_PROTO" xdg-shell-protocol.c

gcc -Wl,--no-as-needed -o adreno-struct-varying \
    adreno-struct-varying.c xdg-shell-protocol.c \
    -I. \
    $(pkg-config --cflags wayland-client wayland-egl) \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -lwayland-egl -lwayland-client \
    -lEGL -lGLESv2 -ldl -Wall -Wextra
echo "Built: adreno-struct-varying"
