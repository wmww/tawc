#!/bin/bash
# Build libhybris + GL shims inside the Arch Linux chroot.
# Called by client/build-libhybris after it extracts the source bundle into /tmp.
#
# Expects in /tmp (from tar extraction):
#   libhybris/          -- source tree
#   libgl-shim.c        -- GL shim source
#   libglesv2-shim.c    -- GLESv2 shim source
#   fix-android-headers  -- optional header fix script
#
# Usage: bash /tmp/build-libhybris-chroot.sh [0|1]
#   0 = incremental (default), 1 = clean

set -eo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export TMPDIR=/tmp HOME=/root PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

CLEAN=${1:-0}
SRC=/tmp/libhybris
DST=/root/libhybris

# ── Sync source to build tree ──
# For incremental builds, only overwrite files whose content changed so that
# make's timestamp-based dependency tracking stays valid.
if [ "$CLEAN" = "1" ] || [ ! -d "$DST" ]; then
    echo '==> Fresh source copy...'
    rm -rf "$DST"
    cp -a "$SRC" "$DST"
else
    echo '==> Syncing changed files...'
    cd "$SRC"
    find . -type f | while IFS= read -r f; do
        if ! cmp -s "$f" "$DST/$f" 2>/dev/null; then
            mkdir -p "$(dirname "$DST/$f")"
            cp "$f" "$DST/$f"
        fi
    done
fi
rm -rf "$SRC"

# ── Build dependencies ──
pacman -S --noconfirm --needed \
    libtool wayland wayland-protocols pkg-config autoconf automake \
    patchelf vulkan-headers vulkan-tools 2>&1 | tail -3

if ! pkg-config --exists android-headers 2>/dev/null; then
    echo '==> Installing android-headers...'
    cd /root && rm -rf android-headers
    git clone --branch halium-11.0 --depth 1 https://github.com/Halium/android-headers.git
    cd android-headers
    sed -i 's/ANDROID_VERSION_MAJOR 11/ANDROID_VERSION_MAJOR 16/' android-version.h
    sed -i 's/Version: 11.0.0/Version: 16.0.0/' android-headers.pc.in
    make PREFIX=/usr/local install
fi

[ -x /tmp/fix-android-headers ] && /tmp/fix-android-headers || true

# ── Source fixups ──
if grep -q 'ANDROID_VERSION_MAJOR < 12' "$DST/hybris/libsync/sync.c" 2>/dev/null; then
    echo '==> Fixing libsync version guard...'
    sed -i 's/#if (ANDROID_VERSION_MAJOR >= 10) && (ANDROID_VERSION_MAJOR < 12)/#if (ANDROID_VERSION_MAJOR >= 10)/' \
        "$DST/hybris/libsync/sync.c"
fi

# ── Configure ──
cd "$DST/hybris"
NEED_CONFIGURE=0
if [ ! -f Makefile ] || [ "$CLEAN" = "1" ]; then
    NEED_CONFIGURE=1
elif [ -n "$(find "$DST" \( -name 'configure.ac' -o -name 'Makefile.am' \) -newer Makefile 2>/dev/null)" ]; then
    echo '==> Build system changed, reconfiguring...'
    NEED_CONFIGURE=1
fi

if [ "$NEED_CONFIGURE" = "1" ]; then
    echo '==> Running autogen.sh + configure...'
    ./autogen.sh 2>&1 | tail -5
    ./configure \
        --enable-arch=arm64 \
        --enable-wayland \
        --disable-wayland_serverside_buffers \
        --enable-adreno-quirks \
        --enable-property-cache \
        --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64 \
        --prefix=/usr/local 2>&1 | grep -E 'Android version|arch |prefix|Features|wayland'
else
    echo '==> Incremental build (use --clean to reconfigure)'
fi

# ── Build & install ──
echo '==> Building...'
DIRS="include common properties libsync platforms hardware ui gralloc egl glesv1 glesv2 hwc2 vulkan utils"
for dir in $DIRS; do
    if [ -d "$dir" ] && [ -f "$dir/Makefile" ]; then
        make -C "$dir" -j4 || echo "WARNING: $dir build failed"
    fi
done

echo '==> Installing...'
for dir in $DIRS; do
    if [ -d "$dir" ] && [ -f "$dir/Makefile" ]; then
        make -C "$dir" install 2>/dev/null || true
    fi
done

patchelf --clear-execstack /usr/local/lib/libhybris-common.so.1.0.0 2>/dev/null || true

# Clean stale glvnd libraries from older builds
rm -f /usr/local/lib/libEGL_libhybris* /usr/local/lib/libGLESv1_CM_libhybris* /usr/local/lib/libGLESv2_libhybris*
rm -f /usr/local/share/glvnd/egl_vendor.d/10_libhybris.json /usr/share/glvnd/egl_vendor.d/10_libhybris.json

ldconfig

# ── GL shims ──
# Thin wrappers that sit ahead of distro libraries on LD_LIBRARY_PATH.
# Export NULL-returning glX* stubs so Mesa GLX is never probed (it can't
# init in this chroot), while DT_NEEDED-linking the real libhybris GLES.
echo '==> Building GL shims...'
SHIM_DIR=/tmp/gl-shims
rm -rf "$SHIM_DIR"
mkdir -p "$SHIM_DIR"
cd "$SHIM_DIR"

cp /usr/local/lib/libGLESv2.so.2 libGLESv2_real.so
patchelf --set-soname libGLESv2_real.so libGLESv2_real.so

gcc -shared -fPIC -o libGLESv2.so.2 /tmp/libglesv2-shim.c \
    -L. -l:libGLESv2_real.so \
    -Wl,-rpath,"$SHIM_DIR" -Wl,--no-as-needed -Wl,-soname,libGLESv2.so.2
ln -sf libGLESv2.so.2 libGLESv2.so

ln -sf "$SHIM_DIR/libGLESv2.so.2" libGL.so.1
gcc -shared -fPIC -o libGL.so /tmp/libgl-shim.c \
    -L. -l:libGL.so.1 \
    -Wl,-rpath,"$SHIM_DIR" -Wl,--no-as-needed -Wl,-soname,libGL.so.1

# ── Verify ──
echo '==> Verifying...'
MISSING=""
for lib in /usr/local/lib/libhybris-common.so /usr/local/lib/libEGL.so /usr/local/lib/libGLESv2.so \
           /usr/local/lib/libhybris/eglplatform_wayland.so \
           "$SHIM_DIR/libGL.so" "$SHIM_DIR/libGLESv2.so.2"; do
    [ -f "$lib" ] || MISSING="$MISSING $lib"
done
if [ -n "$MISSING" ]; then
    echo "ERROR: missing libraries:$MISSING"
    exit 1
fi
echo '==> Done'
