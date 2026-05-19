#!/bin/bash
# Cross-compile Mesa's chroot-side bits for aarch64 or x86_64 glibc:
#   - libvulkan_gfxstream.so + libvirtgpu_kumquat_ffi.a + ICD JSON
#     (gfxstream-bridge backend's chroot-side guest Vulkan driver)
#   - libEGL_mesa.so.0 + libgallium-*.so + zink_dri.so + libGLESv2.so
#     (LIBHYBRIS_ZINK backend's desktop-GL → SPIR-V → libhybris-vulkan
#     translator — see notes/libhybris-zink.md)
#
# Two separate meson configs against the same Mesa source tree:
#   - build-<abi>-gfxstream/  → -Dvulkan-drivers=gfxstream -Dgallium-drivers=
#                              installed to install/usr/lib/gfxstream/
#   - build-<abi>-mesa-zink/  → -Dgallium-drivers=zink -Degl=enabled
#                              installed to install/usr/lib/mesa-zink/
# The shared setup work (host-built sysroot, cargo virtgpu_kumquat_ffi,
# .so stubs, pkg-config files, cross.txt) is done once per ABI.
#
# Bridge architecture:
#   chroot: vulkan app -> libvulkan.so.1 -> libvulkan_gfxstream.so (this)
#                      -> kumquat protocol over Unix socket
#                      -> Android-side bridge (in-process compositor thread)
#                      -> system libvulkan.so + Adreno/AVD-gfxstream
#
# Why a separate libvulkan_gfxstream.so vs distro Mesa: distro Mesa
# packages already ship gfxstream-vk (Arch ARM aarch64, Debian, Arch
# x86_64) but WITHOUT `-Dvirtgpu_kumquat=true`. Without kumquat the
# driver only does DRM virtio-gpu transport, which we have no kernel
# for. We need the kumquat backend (Unix-socket transport, no kernel).
#
# Why we cargo-build the Rust pieces ourselves: Mesa's
# subprojects/packagefiles/*/meson.build hard-code `native: true` on
# every Rust crate's static_library. With `-Dvirtgpu_kumquat=true`
# meson tries to use the same proc-macro chain (cfg-if/syn/quote/
# proc-macro2/unicode-ident) for both build-machine targets (proc-
# macros) and host-machine targets (mesa3d_util etc.) and errors with
# "Tried to mix a build machine library with a host machine target".
# Plain cargo handles cross-builds + proc-macros transparently — so
# we patch Mesa with a `virtgpu_kumquat_external_ffi` option that
# resolves `dep_virtgpu_kumquat_ffi` via pkg-config from a separate
# cargo build instead. See deps/mesa-patches/mesa/.
#
# Output (per --abi):
#   build/mesa-<arch>/install/usr/lib/gfxstream/
#     libvulkan_gfxstream.so
#     gfxstream_vk_icd.<arch>.json
#   build/mesa-<arch>/install/usr/lib/mesa-zink/
#     libEGL_mesa.so.0
#     libgallium-<version>.so
#     libGLESv2_mesa.so.0
#     dri/zink_dri.so   (= libdril_dri.so + symlinks)
#     plus supporting Mesa-Zink runtime libs
#
# Usage:
#   scripts/build-mesa-gfxstream.sh                 # default --abi=aarch64
#   scripts/build-mesa-gfxstream.sh --abi=x86_64
#   scripts/build-mesa-gfxstream.sh --abi=both
#   scripts/build-mesa-gfxstream.sh --clean

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/deps.sh
source "$SCRIPT_DIR/lib/deps.sh"
MESA_DIR="$(dep_dir mesa)"
PATCH_DIR="$REPO_DIR/deps/mesa-patches/mesa"

CLEAN=0
ABIS=""
WITH_ZINK=1
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --no-zink) WITH_ZINK=0 ;;
        --abi=aarch64) ABIS="aarch64" ;;
        --abi=x86_64)  ABIS="x86_64" ;;
        --abi=both)    ABIS="aarch64 x86_64" ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done
[ -n "$ABIS" ] || ABIS="aarch64"

for tool in meson ninja pkg-config cargo rustc wayland-scanner; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: '$tool' not on PATH. See notes/building.md." >&2
        exit 1
    }
done

# ── Vendored Mesa + patches ──
dep_apply_patches mesa "$PATCH_DIR"

build_one() {
    local abi="$1"
    local HOST_TRIPLE CC_BIN CXX_BIN AR_BIN STRIP_BIN RUST_TARGET MESON_CPU
    case "$abi" in
        aarch64)
            HOST_TRIPLE="aarch64-linux-gnu"
            CC_BIN="${HOST_TRIPLE}-gcc"
            CXX_BIN="${HOST_TRIPLE}-g++"
            AR_BIN="${HOST_TRIPLE}-ar"
            STRIP_BIN="${HOST_TRIPLE}-strip"
            RUST_TARGET="aarch64-unknown-linux-gnu"
            MESON_CPU="aarch64"
            ;;
        x86_64)
            # Build host is also x86_64-glibc, so the system tools ARE
            # the right tools — no cross-toolchain to install. Resolve
            # each binary independently: Arch ships gcc as a triple-
            # prefixed symlink (`x86_64-linux-gnu-gcc -> gcc`) but
            # doesn't symlink ar/strip the same way, so a "use the
            # prefix if present" check on gcc alone falsely signs us
            # up for the prefixed names on tools that aren't there.
            HOST_TRIPLE="x86_64-linux-gnu"
            resolve_tool() {
                local name="$1"
                if command -v "${HOST_TRIPLE}-$name" >/dev/null; then
                    echo "${HOST_TRIPLE}-$name"
                else
                    echo "$name"
                fi
            }
            CC_BIN="$(resolve_tool gcc)"
            CXX_BIN="$(resolve_tool g++)"
            AR_BIN="$(resolve_tool ar)"
            STRIP_BIN="$(resolve_tool strip)"
            RUST_TARGET="x86_64-unknown-linux-gnu"
            MESON_CPU="x86_64"
            ;;
        *) echo "ERROR: bad abi: $abi" >&2; return 1 ;;
    esac
    command -v "$CC_BIN" >/dev/null || {
        echo "ERROR: $CC_BIN not on PATH (install $abi cross-toolchain — see notes/building.md)" >&2
        return 1
    }
    rustup target list --installed 2>/dev/null | grep -q "^${RUST_TARGET}\$" || {
        echo "ERROR: rustup target ${RUST_TARGET} missing." >&2
        echo "       Install with: rustup target add ${RUST_TARGET}" >&2
        return 1
    }

    local OUT_DIR="$REPO_DIR/build/mesa-$abi"
    local PREFIX="$OUT_DIR/install"
    local PC_DIR="$OUT_DIR/pkgconfig"
    local STUB_DIR="$OUT_DIR/stubs"
    local CARGO_DIR="$OUT_DIR/cargo"
    local SYSROOT_DISTRO="${TAWC_SYSROOT_DISTRO:-arch}"
    local SYSROOT="$REPO_DIR/build/sysroots/$SYSROOT_DISTRO-$abi"
    local SYSROOT_FIFO_XML="$SYSROOT/usr/share/wayland-protocols/staging/fifo/fifo-v1.xml"

    if [ ! -d "$SYSROOT/usr" ] || [ ! -f "$SYSROOT/.tawc-sysroot" ] || [ ! -f "$SYSROOT_FIFO_XML" ]; then
        "$SCRIPT_DIR/build-host-sysroot.sh" "--abi=$abi" "--distro=$SYSROOT_DISTRO" --profile=prod
    fi

    if [ "$CLEAN" = "1" ]; then
        echo "==> [$abi] wiping $OUT_DIR and Mesa build-$abi-* trees"
        rm -rf "$OUT_DIR" "$MESA_DIR/build-$abi" \
               "$MESA_DIR/build-$abi-gfxstream" "$MESA_DIR/build-$abi-mesa-zink"
    fi

    mkdir -p "$OUT_DIR" "$PREFIX" "$PC_DIR" "$STUB_DIR" "$CARGO_DIR"

    # ── Cargo build of virtgpu_kumquat_ffi ──
    # This sidesteps the meson Rust-subproject mess; we hand the resulting
    # static lib + Mesa's existing virtgpu_kumquat_ffi.h to meson via
    # pkg-config and the patched `-Dvirtgpu_kumquat_external_ffi=true`.
    mkdir -p "$CARGO_DIR/.cargo"
    cat >"$CARGO_DIR/.cargo/config.toml" <<EOF
[target.${RUST_TARGET}]
linker = "$CC_BIN"
EOF
    echo "==> [$abi] cargo build virtgpu_kumquat_ffi (target=${RUST_TARGET}, release)"
    ( cd "$MESA_DIR" && \
      CARGO_TARGET_DIR="$CARGO_DIR/target" \
      CARGO_HOME="$CARGO_DIR" \
      cargo build -p virtgpu_kumquat_ffi --target="$RUST_TARGET" --release )

    local KUMQUAT_FFI_A="$CARGO_DIR/target/${RUST_TARGET}/release/libvirtgpu_kumquat_ffi.a"
    local KUMQUAT_FFI_INC="$MESA_DIR/src/virtio/virtgpu_kumquat_ffi/include"
    [ -f "$KUMQUAT_FFI_A" ] || { echo "ERROR: cargo did not produce $KUMQUAT_FFI_A" >&2; return 1; }
    [ -f "$KUMQUAT_FFI_INC/virtgpu_kumquat_ffi.h" ] || { echo "ERROR: header missing at $KUMQUAT_FFI_INC" >&2; return 1; }

    # ── Stub .so files for runtime deps ──
    # Same trick as build-libhybris.sh: the chroot supplies the real
    # implementations at load time, we just need stubs to record DT_NEEDED.
    # Mesa references some interface symbols (wl_*_interface) at link time
    # from wayland-scanner-generated protocol files, so we copy real .so
    # files for libwayland-{client,server} from the host-built sysroot.
    # Pure stubs (libudev, libffi) are fine.
    local SYSROOT_LIB="$SYSROOT/usr/lib"
    gen_stub() {
        local soname="$1"
        if [ ! -f "$STUB_DIR/$soname" ]; then
            "$CC_BIN" -shared -nostdlib -Wl,-soname,"$soname" \
                -x c /dev/null -o "$STUB_DIR/$soname"
        fi
        local base="${soname%.so.*}.so"
        ln -sf "$soname" "$STUB_DIR/$base"
    }
    copy_or_stub() {
        local soname="$1"
        if [ -f "$SYSROOT_LIB/$soname" ]; then
            cp -L "$SYSROOT_LIB/$soname" "$STUB_DIR/$soname"
            local base="${soname%.so.*}.so"
            ln -sf "$soname" "$STUB_DIR/$base"
        else
            gen_stub "$soname"
        fi
    }
    copy_or_stub libwayland-client.so.0
    copy_or_stub libwayland-server.so.0
    copy_or_stub libdrm.so.2
    copy_or_stub libudev.so.1
    copy_or_stub libffi.so.8

    # ── Synthetic pkg-config files ──
    # Headers and target-side .pc metadata come from build-host-sysroot.sh,
    # not from a live device rootfs. Library .so paths point to our stub
    # dir for runtime DT_NEEDED entries. virtgpu_kumquat_ffi points at the
    # cargo static lib + Mesa's own header dir (in-tree, untouched).
    local SYSROOT_WAYLAND_INC="$SYSROOT/usr/include"
    local SYSROOT_WAYLAND_PROT_DATADIR="$SYSROOT/usr/share/wayland-protocols"
    local HOST_WAYLAND_SCANNER
    HOST_WAYLAND_SCANNER="$(command -v wayland-scanner)"

    write_pc() {
        local name="$1" cflags="$2" libs="$3" version="${4:-1.0.0}"
        cat >"$PC_DIR/$name.pc" <<EOF
Name: $name
Description: $name (cross stub)
Version: $version
Cflags: $cflags
Libs: $libs
EOF
    }

    write_pc wayland-client "-I$SYSROOT_WAYLAND_INC" "-L$STUB_DIR -lwayland-client" "1.25.0"
    # libglvnd is headers-only at Mesa build time (Mesa-Zink's libEGL_mesa.so.0
    # is a glvnd vendor lib, registered via runtime libEGL.so.1 from the
    # distro). The host's headers in /usr/include/glvnd/ are ABI-portable.
    write_pc libglvnd "-I$SYSROOT/usr/include" "" "$(PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" pkg-config --modversion libglvnd 2>/dev/null || echo 1.7.0)"
    # wayland-egl-backend: headers-only API used by Mesa to plug its libEGL
    # into wl_egl_window. Host pkg-config wayland-egl-backend is the
    # natural source; no runtime lib needed at build time.
    write_pc wayland-egl-backend "-I$SYSROOT_WAYLAND_INC" "" \
        "$(PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" pkg-config --modversion wayland-egl-backend 2>/dev/null || echo 3)"
    write_pc wayland-server "-I$SYSROOT_WAYLAND_INC" "-L$STUB_DIR -lwayland-server" "1.25.0"
    write_pc libdrm "-I$SYSROOT/usr/include -I$SYSROOT/usr/include/libdrm" "-L$STUB_DIR -ldrm" "$(PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" pkg-config --modversion libdrm)"
    write_pc libudev "-I$SYSROOT/usr/include" "-L$STUB_DIR -ludev" "$(PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" pkg-config --modversion libudev)"
    write_pc libffi "-I$SYSROOT/usr/include" "-L$STUB_DIR -lffi" "$(PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" pkg-config --modversion libffi)"

    # virtgpu_kumquat_ffi: static lib from cargo + header from Mesa source.
    # Static archive built by cargo pulls in libstd's stubs for libgcc_s,
    # pthread, dl, util, m, c, rt — those are fine because libvulkan_gfxstream.so
    # already DT_NEEDED's libstdc++ / libm / libc and the chroot has them.
    write_pc virtgpu_kumquat_ffi \
        "-I$KUMQUAT_FFI_INC" \
        "$KUMQUAT_FFI_A -lpthread -ldl -lutil -lrt -lm" \
        "0.1.76"

    cat >"$PC_DIR/wayland-scanner.pc" <<EOF
wayland_scanner=$HOST_WAYLAND_SCANNER
Name: wayland-scanner
Description: host wayland-scanner
Version: 1.25.0
Cflags:
Libs:
EOF

    cat >"$PC_DIR/wayland-protocols.pc" <<EOF
pkgdatadir=$SYSROOT_WAYLAND_PROT_DATADIR
Name: wayland-protocols
Description: host wayland-protocols
Version: 1.45
Cflags:
Libs:
EOF

    # ── meson cross file ──
    # Target headers come from the host-built sysroot's .pc files above.
    # Keep the compiler's libc/sysroot selection with the toolchain:
    # the app/rootfs runtime supplies distro libs, but Mesa itself only
    # needs ABI-stable C headers and explicit DT_NEEDED stubs here.
    cat >"$OUT_DIR/cross.txt" <<EOF
[binaries]
c = '$CC_BIN'
cpp = '$CXX_BIN'
ar = '$AR_BIN'
strip = '$STRIP_BIN'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PC_DIR'

[host_machine]
system = 'linux'
cpu_family = '$MESON_CPU'
cpu = '$MESON_CPU'
endian = 'little'
EOF

    # ── Configure + build: gfxstream-vk ──
    # `-Dvirtgpu_kumquat=true -Dvirtgpu_kumquat_external_ffi=true` enables
    # kumquat without pulling Mesa's Rust subprojects into the meson graph.
    # Required by patch deps/mesa-patches/mesa/02-meson-external-kumquat-ffi.patch.
    local BUILD_DIR_GFXSTREAM="$MESA_DIR/build-$abi-gfxstream"
    if [ -f "$BUILD_DIR_GFXSTREAM/build.ninja" ] && grep -q -- "-idirafter.*usr/include" "$BUILD_DIR_GFXSTREAM/build.ninja"; then
        echo "==> [$abi] old host-header Mesa builddir detected; reconfiguring"
        rm -rf "$BUILD_DIR_GFXSTREAM"
    fi
    if [ ! -f "$BUILD_DIR_GFXSTREAM/build.ninja" ]; then
        PKG_CONFIG_LIBDIR="$PC_DIR" meson setup "$BUILD_DIR_GFXSTREAM" "$MESA_DIR" \
            --cross-file "$OUT_DIR/cross.txt" \
            -Dvulkan-drivers=gfxstream \
            -Dgallium-drivers= \
            -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Degl=disabled \
            -Dglx=disabled -Dvideo-codecs= \
            -Dvirtgpu_kumquat=true \
            -Dvirtgpu_kumquat_external_ffi=true \
            -Dplatforms=wayland \
            -Dlmsensors=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
            -Dshared-glapi=disabled -Dllvm=disabled -Dxmlconfig=disabled \
            -Ddisplay-info=disabled \
            --buildtype release -Ddefault_library=shared \
            --prefix /usr --libdir lib/gfxstream
    fi
    ninja -C "$BUILD_DIR_GFXSTREAM"

    # ── Stage gfxstream-vk outputs ──
    # Mesa names the ICD JSON `gfxstream_vk_icd.<cpu>.json` based on
    # host_machine.cpu in the cross file — so `aarch64` → `.aarch64.json`,
    # `x86_64` → `.x86_64.json`. Stage with the same name; the Gradle
    # packMesaGfxstream task picks it back up by glob.
    # `--prefix=/usr --libdir=lib/gfxstream` → Mesa bakes the ICD JSON's
    # `library_path` as `/usr/lib/gfxstream/libvulkan_gfxstream.so`,
    # which is where [BridgeInstallProvider] copies it into the rootfs.
    # We co-locate the JSON in the same dir (the runtime picks it up via
    # an explicit `VK_ICD_FILENAMES=…`, not via /usr/share scan, so the
    # FHS-y `/usr/share/vulkan/icd.d/` location buys us nothing here).
    mkdir -p "$PREFIX/usr/lib/gfxstream"
    cp "$BUILD_DIR_GFXSTREAM/src/gfxstream/guest/vulkan/libvulkan_gfxstream.so" \
       "$PREFIX/usr/lib/gfxstream/"
    cp "$BUILD_DIR_GFXSTREAM/src/gfxstream/guest/vulkan/gfxstream_vk_icd.${MESON_CPU}.json" \
       "$PREFIX/usr/lib/gfxstream/"

    echo "==> [$abi] built libvulkan_gfxstream.so:"
    ls -la "$PREFIX/usr/lib/gfxstream/libvulkan_gfxstream.so"
    echo "==> [$abi] gfxstream ICD JSON:"
    cat "$PREFIX/usr/lib/gfxstream/gfxstream_vk_icd.${MESON_CPU}.json"

    # ── Configure + build: Mesa-Zink (LIBHYBRIS_ZINK backend) ──
    # Gated by --no-zink so APK builds that disable the libhybris-zink
    # backend (via `-PtawcGraphics=libhybris,gfxstream,cpu`) skip this
    # configure entirely — it's the bulk of the Mesa cross-build. The
    # gfxstream-vk pieces above still build either way.
    if [ "$WITH_ZINK" = "0" ]; then
        echo "==> [$abi] Mesa-Zink skipped (--no-zink)"
        # Sweep any stale outputs from a previous WITH_ZINK=1 build so
        # `outputs.files(mesaZinkTar)` doesn't see a leftover tar.
        rm -f "$PREFIX/usr/lib/mesa-zink-$MESON_CPU.tar"
        rm -rf "$PREFIX/usr/lib/mesa-zink"
        return 0
    fi
    # Desktop GL/EGL stack with Zink as the only Gallium driver and Vulkan
    # disabled (libhybris's libvulkan is what Zink dlopens at runtime, via
    # the `LD_LIBRARY_PATH=/usr/lib/hybris-vulkan-only:...` shadow set by
    # `RootfsEnv`). No llvmpipe → no LLVM dep at build time; no DRI/GLX
    # → no libxcb/libX11 deps. Installs to `/usr/lib/mesa-zink/`; the
    # libEGL_mesa.so.0 in there carries our `06-tawc-zink-nokms.patch`
    # to fall through to Zink+Kopper when DRM isn't available.
    #
    # `-Dgles2=enabled -Dopengl=true -Degl=enabled` produces libEGL_mesa.so.0
    # + libGLESv2_mesa.so.0 + libgallium-<ver>.so. `-Dshared-glapi=enabled`
    # is required by libEGL_mesa. `-Dglvnd=true` makes the EGL/GLES libs
    # build as glvnd vendor libs (libfoo_mesa.so.0) which is what the distro
    # libglvnd dispatch expects.
    local BUILD_DIR_ZINK="$MESA_DIR/build-$abi-mesa-zink"
    if [ -f "$BUILD_DIR_ZINK/build.ninja" ] && grep -q -- "-idirafter.*usr/include" "$BUILD_DIR_ZINK/build.ninja"; then
        echo "==> [$abi] old host-header Mesa-Zink builddir detected; reconfiguring"
        rm -rf "$BUILD_DIR_ZINK"
    fi
    if [ ! -f "$BUILD_DIR_ZINK/build.ninja" ]; then
        PKG_CONFIG_LIBDIR="$PC_DIR" meson setup "$BUILD_DIR_ZINK" "$MESA_DIR" \
            --cross-file "$OUT_DIR/cross.txt" \
            -Dvulkan-drivers= \
            -Dgallium-drivers=zink \
            -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Degl=enabled \
            -Dglx=disabled -Dvideo-codecs= \
            -Dvirtgpu_kumquat=false \
            -Dplatforms=wayland \
            -Dlmsensors=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
            -Dshared-glapi=enabled -Dllvm=disabled -Dxmlconfig=disabled \
            -Ddisplay-info=disabled \
            -Dglvnd=true \
            --buildtype release -Ddefault_library=shared \
            --prefix /usr --libdir lib/mesa-zink
    fi
    ninja -C "$BUILD_DIR_ZINK"
    # Run Mesa's own install logic: it handles soname symlinks + dri/
    # subdir + glvnd egl_vendor.d JSON layout for us. DESTDIR isolates
    # to our stage dir so /usr/lib/mesa-zink/ is the only thing populated.
    DESTDIR="$PREFIX" ninja -C "$BUILD_DIR_ZINK" install >/dev/null

    # Trim subproject leftovers (zlib, expat) and pkgconfig dirs we don't
    # ship — distro Mesa's libz.so.1 / libexpat.so.1 are what libgallium
    # NEEDs at runtime, and our copies would only shadow those if anyone
    # put /usr/lib/mesa-zink/ on the load path before /usr/lib/. The dri
    # GBM backend (`gbm/dri_gbm.so`) doesn't get exercised on this path
    # either — Kopper-Zink doesn't go through GBM. Keeping only the
    # files actually needed at runtime keeps the APK + install footprint
    # down to ~20 MB instead of ~25 MB.
    local ZINK_INSTALL="$PREFIX/usr/lib/mesa-zink"
    rm -rf "$ZINK_INSTALL"/libexpat.so* \
           "$ZINK_INSTALL"/libz.so* \
           "$ZINK_INSTALL"/pkgconfig \
           "$ZINK_INSTALL"/gbm
    # Tar the dir so symlinks survive the APK asset roundtrip. Matches
    # the libhybris asset pattern; CompositorService extracts at runtime.
    # `-C` so entries are relative (no leading `mesa-zink/`), so the
    # runtime extractor lands them straight at <filesDir>/mesa-zink/.
    local ZINK_TAR="$PREFIX/usr/lib/mesa-zink-$MESON_CPU.tar"
    tar -C "$ZINK_INSTALL" -cf "$ZINK_TAR" .
    echo "==> [$abi] Mesa-Zink staged:"
    tar tf "$ZINK_TAR" | sort
}

for abi in $ABIS; do
    build_one "$abi"
done
