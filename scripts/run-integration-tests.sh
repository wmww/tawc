#!/bin/bash
# Run the integration tests against the selected adb target.
#
# Usage: scripts/run-integration-tests.sh [--no-build] [filter]
# Requires an installed distro; set TAWC_INSTALL_ID when more than one exists.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TAWC_EXEC="${TAWC_EXEC:-$ROOT_DIR/scripts/tawc-exec.sh}"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

DO_BUILD=1
TEST_FILTER=""
for arg in "$@"; do
    case "$arg" in
        --no-build|-n)
            DO_BUILD=0
            ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'
            exit 0
            ;;
        *)
            if [ -n "$TEST_FILTER" ]; then
                echo "ERROR: test filter already set to '$TEST_FILTER', got '$arg'" >&2
                exit 2
            fi
            TEST_FILTER="$arg"
            ;;
    esac
done

# shellcheck source=../scripts/lib/select-device.sh
source "$ROOT_DIR/scripts/lib/select-device.sh"
export TAWC_EXEC_BIN="$ROOT_DIR/build/tawc-exec/tawc-exec"

echo "=== Checking adb connection ($ANDROID_SERIAL) ==="
adb get-state >/dev/null 2>&1 || { echo "ERROR: No adb device connected"; exit 1; }

DEVICE_ABI="$(adb shell getprop ro.product.cpu.abi | tr -d '\r\n')"
if [ -z "$DEVICE_ABI" ]; then
    DEVICE_ABI="$(adb shell uname -m | tr -d '\r\n')"
fi
DEVICE_IS_EMULATOR=0
if [[ "$ANDROID_SERIAL" == emulator-* ]] || [ "$(adb shell getprop ro.kernel.qemu | tr -d '\r\n')" = "1" ]; then
    DEVICE_IS_EMULATOR=1
fi
echo "=== Detected device ABI: $DEVICE_ABI ==="
if [ "$DEVICE_IS_EMULATOR" -eq 1 ]; then
    echo "=== Detected emulator target ==="
fi

if [ "$DO_BUILD" -eq 1 ]; then
    # Build + install the APK. We skip the launch — this script does its
    # own force-stop + am start + readiness wait below, so the
    # compositor lifetime brackets the cargo run cleanly.
    "$ROOT_DIR/scripts/app-build-install.sh" --no-launch
fi

# tawc-install-id.sh may use the in-app broker to auto-detect the install
# id, so resolve it only after the APK install step has had a chance to
# put the debug app on the device.
# shellcheck source=../scripts/lib/tawc-install-id.sh
source "$ROOT_DIR/scripts/lib/tawc-install-id.sh"

# tawc-install-id.sh exported TAWC_INSTALL_ID (auto-detected when unset
# and exactly one install is present; errors if 0 or >1). The cargo
# test harness reads the same env var via tawc_integration::install_id.
INSTALL_ID="$TAWC_INSTALL_ID"
INSTALL_DIR="/data/data/me.phie.tawc/distros/$INSTALL_ID"
TAWC_DISTROS_DIR="$INSTALL_DIR"
ROOTFS_DIR="$INSTALL_DIR/rootfs"

echo "=== Using install id: $INSTALL_ID ==="

read_distro_key() {
    local distro_key
    distro_key=$("$TAWC_EXEC" /system/bin/cat "$TAWC_DISTROS_DIR/metadata.json" \
        | tr -d '\r' \
        | sed -n 's/.*"distro"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n1)
    if [ -z "$distro_key" ]; then
        echo "ERROR: could not read distro key from $TAWC_DISTROS_DIR/metadata.json" >&2
        exit 1
    fi
    echo "$distro_key"
}

detect_rootfs_abi() {
    local host_arch
    host_arch=$("$TAWC_EXEC" /system/bin/uname -m | tr -d '\r\n')
    case "$host_arch" in
        aarch64) echo aarch64 ;;
        x86_64)  echo x86_64 ;;
        *) echo "ERROR: unsupported rootfs arch '$host_arch'" >&2; exit 1 ;;
    esac
}

set_required_packages() {
    local distro_key="$1"
    case "$distro_key" in
        arch|manjaro)
            REQUIRED_PKGS=(
                gtk4 cairo wayland libx11 libxcb libglvnd
                gtk3 gtk3-demos gtk4-demos firefox supertuxkart
                mesa-utils weston vulkan-tools
                xorg-xclock
                mesa-demos
            )
            PACKAGE_CHECK_CMD="pacman -Q ${REQUIRED_PKGS[*]} >/dev/null 2>&1"
            INSTALL_CMD="pacman -Syu --noconfirm --needed ${REQUIRED_PKGS[*]}"
            ;;
        void)
            REQUIRED_PKGS=(
                gtk4 cairo wayland libX11 libxcb libglvnd
                gtk+3 gtk+3-demo gtk4-demo firefox supertuxkart
                glxinfo weston Vulkan-Tools
                xclock
                mesa-demos mesa-dri
                dejavu-fonts-ttf
            )
            PACKAGE_CHECK_CMD="for p in ${REQUIRED_PKGS[*]}; do xbps-query -p pkgver \"\$p\" >/dev/null 2>&1 || exit 1; done"
            INSTALL_CMD="xbps-install -Suy && xbps-install -y ${REQUIRED_PKGS[*]}"
            ;;
        debian-sid)
            REQUIRED_PKGS=(
                libgtk-4-1 libcairo2 libwayland-client0 libx11-6 libxcb1 libglvnd0
                libgtk-3-0 gtk-3-examples gtk-4-examples firefox supertuxkart
                mesa-utils mesa-utils-extra weston vulkan-tools gstreamer1.0-plugins-base
                x11-apps dbus-x11
                libgl1-mesa-dri mesa-vulkan-drivers fonts-dejavu-core
            )
            PACKAGE_CHECK_CMD="dpkg-query -W ${REQUIRED_PKGS[*]} >/dev/null 2>&1"
            INSTALL_CMD="apt-get update && DEBIAN_FRONTEND=noninteractive apt-get -y install --no-install-recommends ${REQUIRED_PKGS[*]}"
            ;;
        *)
            echo "ERROR: unsupported distro '$distro_key' (expected arch / manjaro / void / debian-sid)" >&2
            exit 1
            ;;
    esac
}

ensure_runtime_packages() {
    local distro_key="$1"
    set_required_packages "$distro_key"
    echo "=== Checking rootfs test packages ($distro_key) ==="
    if TAWC_OP_TITLE= "$ROOT_DIR/scripts/rootfs-run.sh" "$PACKAGE_CHECK_CMD" >/dev/null 2>&1; then
        echo "=== Rootfs test packages already installed ==="
        return
    fi
    echo "=== Installing rootfs test packages: ${REQUIRED_PKGS[*]} ==="
    TAWC_OP_TITLE="install integration test deps ($distro_key)" \
        "$ROOT_DIR/scripts/rootfs-run.sh" "$INSTALL_CMD"
}

host_file_sha() {
    sha256sum "$1" | awk '{print $1}'
}

device_file_sha() {
    local path="$1"
    "$TAWC_EXEC" /system/bin/sh -c "test -f $path && sha256sum $path 2>/dev/null | awk '{print \$1}'" \
        | tr -d '\r' \
        | head -n1
}

deploy_pidfile_helper() {
    # shellcheck source=../scripts/lib/tawc-scratch.sh
    source "$ROOT_DIR/scripts/lib/tawc-scratch.sh"
    local src="$ROOT_DIR/tests/apps/tawc-pidfile-exec.sh"
    local dst="$ROOTFS_DIR/tmp/tawc-pidfile-exec.sh"
    if [ "$(host_file_sha "$src")" = "$(device_file_sha "$dst" || true)" ]; then
        echo "=== pidfile helper already deployed ==="
        return
    fi
    echo "=== Deploying pidfile helper ==="
    adb push "$src" "$TAWC_SCRATCH/tawc-pidfile-exec.sh" >/dev/null
    "$TAWC_EXEC" /system/bin/sh -c "cp $TAWC_SCRATCH/tawc-pidfile-exec.sh $dst && chmod +x $dst"
}

copy_test_app() {
    local name="$1"
    local out_dir="$ROOT_DIR/build/test-apps/$BUILD_DISTRO-$BUILD_ABI/$name"
    local staging="$TAWC_SCRATCH/$name-out"
    local bin_dir="$ROOTFS_DIR/usr/local/bin"
    local lib_dir="$ROOTFS_DIR/usr/local/lib"
    local srcs=("$out_dir/$name")
    local dsts=("$bin_dir/$name")

    if [ "$name" = "libhybris-tls-repro" ]; then
        srcs+=("$out_dir/tls_lib.so" "$out_dir/weak_lib.so")
        dsts+=("$lib_dir/tls_lib.so" "$lib_dir/weak_lib.so")
    fi

    local changed=0
    local i
    for i in "${!srcs[@]}"; do
        [ -f "${srcs[$i]}" ] || { echo "ERROR: missing ${srcs[$i]}" >&2; exit 1; }
        if [ "$(host_file_sha "${srcs[$i]}")" != "$(device_file_sha "${dsts[$i]}" || true)" ]; then
            changed=1
        fi
    done

    if [ "$changed" = "0" ]; then
        echo "=== $name already deployed ==="
        return
    fi

    echo "=== Deploying $name ==="
    "$TAWC_EXEC" /system/bin/sh -c "mkdir -p $TAWC_SCRATCH"
    adb shell rm -rf "$staging" >/dev/null
    adb push "$out_dir" "$staging" >/dev/null
    if [ "$name" = "libhybris-tls-repro" ]; then
        "$TAWC_EXEC" /system/bin/sh -c "\
            mkdir -p $bin_dir $lib_dir && \
            cp $staging/$name $bin_dir/$name && \
            cp $staging/tls_lib.so $staging/weak_lib.so $lib_dir/ && \
            chmod a+rx $bin_dir/$name $lib_dir/tls_lib.so $lib_dir/weak_lib.so"
    else
        "$TAWC_EXEC" /system/bin/sh -c "\
            mkdir -p $bin_dir && \
            cp $staging/$name $bin_dir/$name && \
            chmod a+rx $bin_dir/$name"
    fi
}

build_and_deploy_test_apps() {
    local distro_key="$1"
    BUILD_ABI="$(detect_rootfs_abi)"
    if [ -n "${TAWC_SYSROOT_DISTRO:-}" ]; then
        BUILD_DISTRO="$TAWC_SYSROOT_DISTRO"
    else
        BUILD_DISTRO="$distro_key"
        case "$BUILD_DISTRO" in
            debian-sid)
                # build-host-sysroot.sh has pacman/xbps resolvers today. The
                # test clients only need a glibc sysroot with the same core
                # Wayland/X11/GL ABI, so reuse the existing Arch sysroot.
                BUILD_DISTRO=arch
                ;;
        esac
    fi

    echo "=== Building test apps if needed ($BUILD_DISTRO/$BUILD_ABI) ==="
    make -C "$ROOT_DIR/tests/apps" -j"$(nproc)" "DISTRO=$BUILD_DISTRO" "ABI=$BUILD_ABI" all

    # shellcheck source=../scripts/lib/tawc-scratch.sh
    source "$ROOT_DIR/scripts/lib/tawc-scratch.sh"
    APPS=(wayland-debug-app x11-debug-app eglx11-test)
    if [ "$BUILD_ABI" = "aarch64" ]; then
        APPS+=(tawc-dri-test libhybris-tls-repro)
    else
        echo "=== Skipping tawc-dri-test, libhybris-tls-repro on $BUILD_ABI (need libhybris, aarch64-only) ==="
    fi
    for app in "${APPS[@]}"; do
        copy_test_app "$app"
    done
}

ensure_tawcroot_device_tests() {
    case "$ANDROID_SERIAL" in
        emulator-*) TAWCROOT_ABI=x86_64 ;;
        *)          TAWCROOT_ABI=aarch64 ;;
    esac
    local out_dir="$ROOT_DIR/build/tawcroot-$TAWCROOT_ABI"
    local stamp="$out_dir/.device-tests.stamp"
    local required=(
        "$out_dir/libtawcroot-testhost.so"
        "$out_dir/libtawcroot.so"
        "$out_dir/tests"
        "$out_dir/programs"
    )
    local need=0
    local f
    for f in "${required[@]}"; do
        [ -e "$f" ] || need=1
    done
    if [ "$need" = "0" ] && [ -f "$stamp" ]; then
        if find "$ROOT_DIR/tawcroot/src" \
                "$ROOT_DIR/tawcroot/include" \
                "$ROOT_DIR/tawcroot/tests" \
                "$ROOT_DIR/tawcroot/build.sh" \
                "$ROOT_DIR/tawcroot/build-fixtures.sh" \
                "$ROOT_DIR/deps/deps.list" \
                "$ROOT_DIR/scripts/lib/deps.sh" \
                -newer "$stamp" -print -quit | grep -q .; then
            need=1
        fi
    else
        need=1
    fi

    if [ "$need" = "0" ]; then
        echo "=== tawcroot device tests already built ($TAWCROOT_ABI) ==="
        return
    fi

    echo "=== Building tawcroot device tests ($TAWCROOT_ABI) ==="
    "$ROOT_DIR/tawcroot/build.sh" "--abi=$TAWCROOT_ABI" --testhost --tests
    "$ROOT_DIR/tawcroot/build-fixtures.sh" "$TAWCROOT_ABI"
    touch "$stamp"
}

if [ "$DO_BUILD" -eq 1 ]; then
    echo "=== Verifying in-app install is present at $INSTALL_DIR ==="
    if ! "$TAWC_EXEC" /system/bin/sh -c "test -d $ROOTFS_DIR" >/dev/null 2>&1; then
        cat >&2 <<EOF
ERROR: in-app install not found at $INSTALL_DIR/.

Install it from the host:
  scripts/tawc-exec.sh --foreground-app --action install \\
      --arg id=$INSTALL_ID --arg mirrorProxy=http://127.0.0.1:8080/proxy/

Progress streams to your TTY; the in-app log screen also opens.
EOF
        exit 1
    fi

    DISTRO_KEY="$(read_distro_key)"
    echo "=== Detected distro: $DISTRO_KEY ==="
    ensure_runtime_packages "$DISTRO_KEY"
    deploy_pidfile_helper
    build_and_deploy_test_apps "$DISTRO_KEY"
    ensure_tawcroot_device_tests
fi

# Launch the compositor once for the whole suite. Tests assert it is
# running rather than starting it themselves, so the suite gets a single
# clean compositor lifetime instead of N partial ones. Force-stop first
# so a previous app process/compositor is gone before the new one starts.
# MainActivity intentionally does not start the compositor; RUNINSIDE is
# the shared user/rootfs launch path that starts it lazily.
echo "=== Starting compositor ==="
adb shell "am force-stop me.phie.tawc"
sleep 0.3
adb logcat -c >/dev/null 2>&1 || true
"$TAWC_EXEC" --in-rootfs "$INSTALL_ID" -- true >/dev/null

# Wait until the tawc process is alive, the wayland socket exists, AND
# the calloop event loop is dispatching. The compositor binds the
# socket as its last setup step (see compositor/src/event_loop.rs::run)
# so a fresh "Entering calloop event loop" log line confirms both — but
# `am force-stop` leaves the previous run's socket file behind, so the
# stat alone would falsely match a stale socket while the new
# compositor is still in early init. The logcat probe disambiguates.
COMPOSITOR_READY=0
for _ in $(seq 1 150); do
    # Wayland socket lives in the app's private data dir; probe via
    # the broker (runs as the app uid).
    if adb shell 'pidof me.phie.tawc >/dev/null' 2>/dev/null && \
       "$TAWC_EXEC" /system/bin/sh -c "test -e /data/data/me.phie.tawc/share/wayland-0" 2>/dev/null && \
       adb logcat -d -s tawc-native 2>/dev/null | grep -q "Entering calloop event loop"; then
        COMPOSITOR_READY=1
        break
    fi
    sleep 0.1
done
if [ "$COMPOSITOR_READY" -ne 1 ]; then
    echo "ERROR: compositor did not become ready within 15s" >&2
    adb shell am force-stop me.phie.tawc || true
    exit 1
fi

LIBTEST_ARGS=(--nocapture --test-threads=1)
if [ -n "$TEST_FILTER" ]; then
    LIBTEST_ARGS+=("$TEST_FILTER")
    echo "=== Running integration tests matching: $TEST_FILTER ==="
else
    echo "=== Running integration tests ==="
fi
EXTRA_RUSTFLAGS=()
if [ "$DEVICE_IS_EMULATOR" -eq 1 ]; then
    echo "=== Marking gfxstream:: tests ignored on emulator ==="
    EXTRA_RUSTFLAGS+=(--cfg tawc_skip_gfxstream_on_target)
fi
case "$DEVICE_ABI" in
    x86*|i386|i686)
        echo "=== Marking libhybris-backed tests ignored on x86 device ($DEVICE_ABI) ==="
        EXTRA_RUSTFLAGS+=(--cfg tawc_skip_libhybris_on_target)
        ;;
esac
if [ "${#EXTRA_RUSTFLAGS[@]}" -gt 0 ]; then
    export RUSTFLAGS="${RUSTFLAGS:-} ${EXTRA_RUSTFLAGS[*]}"
fi
cd "$ROOT_DIR/tests/integration"
set +e
cargo test -- "${LIBTEST_ARGS[@]}"
TEST_EXIT=$?
set -euo pipefail

echo "=== Stopping compositor ==="
adb shell am force-stop me.phie.tawc

exit $TEST_EXIT
