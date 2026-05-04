#!/bin/bash
# Cross-compile proot (and its libtalloc dependency) for Android via the
# NDK and stage the resulting binaries at
# `server/app/src/main/jniLibs/<abi>/libproot.so` so they ride along with
# the APK and are reachable at runtime via `applicationInfo.nativeLibraryDir`.
#
# proot is the rootless install method: a ptrace-based chroot lookalike
# that doesn't need CAP_SYS_CHROOT or CAP_SYS_ADMIN. See
# notes/installation.md for how it slots into the install pipeline.
#
# Layout (everything under the project dir, gitignored):
#   ./proot/                 -- proot-me/proot checkout, pinned tag below
#   ./proot-deps/talloc/     -- libtalloc tarball extract, pinned version
#
# Output:
#   server/app/src/main/jniLibs/arm64-v8a/libproot.so
#   server/app/src/main/jniLibs/x86_64/libproot.so
#
# Android requires native binaries shipped under jniLibs to be named
# lib*.so so the package installer extracts them; the actual binary is
# arch-specific (one ELF per ABI dir). At runtime we exec it directly
# from `nativeLibraryDir` — see install/method/ProotMethod.kt.
#
# Usage:
#   bash client/build-proot                  # build current host's primary ABI
#   bash client/build-proot --abi=aarch64    # explicit
#   bash client/build-proot --abi=x86_64     # emulator
#   bash client/build-proot --abi=both
#   bash client/build-proot --clean          # wipe build dirs first

set -euo pipefail

# ---------------------------------------------------------------------------
# We use the Termux fork rather than upstream proot-me/proot. Termux
# carries a SIGSYS-from-stacked-seccomp handler in src/tracee/seccomp.c
# that translates deprecated syscalls (`access` → `faccessat`, `open` →
# `openat`, `chmod` → `fchmodat`, etc.) when Android's app seccomp
# filter raises SIGSYS via SECCOMP_RET_TRAP. Without that handler our
# tracees die with SIGSYS the moment glibc's ld-linux on x86_64 calls
# `access(2)` — Android's untrusted_app filter only allowlists access
# for lp32 (32-bit). See `notes/proot.md` ("Why upstream proot doesn't
# work on Android x86_64") and the upstream issue trail in termux/proot
# for the full story.
#
# Termux's fork also carries Android-specific fixes for things like
# SYSCALL_AVOIDER values, loader ELF base addresses, and the `ioctl`
# filter rule. The pin (sha + repo URL) lives in client/deps.list.
#
# talloc is a tarball, not a git checkout — version is in the URL, so
# bumping TALLOC_VERSION below triggers a fresh download by construction
# (no separate pin file needed).
TALLOC_VERSION="2.4.4"
TALLOC_TARBALL_URL="https://download.samba.org/pub/talloc/talloc-${TALLOC_VERSION}.tar.gz"

# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=deps-lib.sh
source "$SCRIPT_DIR/deps-lib.sh"
PROOT_DIR="$(dep_dir proot)"
DEPS_DIR="$REPO_DIR/proot-deps"
TALLOC_DIR="$DEPS_DIR/talloc-${TALLOC_VERSION}"
JNILIBS_DIR="$REPO_DIR/server/app/src/main/jniLibs"

# NDK lookup mirrors client/build-libxkbcommon — see that script for rationale.
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    DEFAULT_SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
    if [ -d "$DEFAULT_SDK/ndk" ]; then
        ANDROID_NDK_HOME="$DEFAULT_SDK/ndk/$(ls -1 "$DEFAULT_SDK/ndk" | sort -V | tail -1)"
    fi
fi
if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "ERROR: ANDROID_NDK_HOME not set and no NDK found under \$ANDROID_HOME/ndk." >&2
    exit 1
fi
NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -x "$NDK_BIN/llvm-ar" ] || { echo "ERROR: NDK toolchain missing at $NDK_BIN" >&2; exit 1; }

# ---------------------------------------------------------------------------
ABI=""
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --abi=aarch64|--abi=arm64) ABI="aarch64" ;;
        --abi=x86_64)              ABI="x86_64" ;;
        --abi=both)                ABI="both" ;;
        --clean)                   CLEAN=1 ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done
if [ -z "$ABI" ]; then
    # Default to the host ABI if detectable, else aarch64.
    case "$(uname -m)" in
        x86_64|amd64) ABI="x86_64" ;;
        *)            ABI="aarch64" ;;
    esac
fi

# ---------------------------------------------------------------------------
# Source fetch. dep_ensure clones if missing and verifies HEAD against the
# pin in client/deps.list. Submodules and our small inline patch run after.
fetch_proot() {
    dep_ensure proot
    git -C "$PROOT_DIR" submodule update --init --depth 1 --quiet
    apply_local_patches
}

# `proot/src/extension/ashmem_memfd/ashmem_memfd.c` uses memcpy/strerror
# without including <string.h>. Compiles fine under glibc (transitive
# include) but errors under NDK clang. Idempotent via the grep guard.
apply_local_patches() {
    local f="$PROOT_DIR/src/extension/ashmem_memfd/ashmem_memfd.c"
    if [ -f "$f" ] && ! grep -q '#include <string.h>' "$f"; then
        echo "==> patching ashmem_memfd.c (add #include <string.h>)"
        sed -i '0,/^#include /{s|^#include |#include <string.h>\n#include |}' "$f"
    fi
}

fetch_talloc() {
    if [ -d "$TALLOC_DIR" ]; then return 0; fi
    mkdir -p "$DEPS_DIR"
    local tarball="$DEPS_DIR/talloc-${TALLOC_VERSION}.tar.gz"
    if [ ! -f "$tarball" ]; then
        echo "==> downloading $TALLOC_TARBALL_URL"
        curl -fsSL -o "$tarball" "$TALLOC_TARBALL_URL"
    fi
    echo "==> extracting talloc-${TALLOC_VERSION}"
    tar -xzf "$tarball" -C "$DEPS_DIR"
}

# ---------------------------------------------------------------------------
# Hand-rolled config.h for talloc's build. talloc's upstream build relies
# on waf running probe programs to fill this out, which doesn't survive
# cross-compilation. Since we know the target (Bionic on API 29+ via the
# NDK) at script-write time, we emit the same shape directly and skip
# waf entirely.
#
# Two Bionic-specific gotchas baked in:
#   - memset_s is C11 Annex K and Bionic doesn't ship it. We define a
#     macro shim to plain memset, which is enough for talloc's only
#     uses (zero-fill on free / realloc).
#   - HAVE___THREAD must be set explicitly. Without it talloc errors
#     "Configure failed to detect pthread library with missing TLS
#     support" in the absence of waf-driven probing.
write_talloc_config_h() {
    local out="$1"
    cat >"$out" <<'EOF'
#ifndef _TALLOC_BUILD_CONFIG_H
#define _TALLOC_BUILD_CONFIG_H 1
#define _GNU_SOURCE 1
#define __STDC_WANT_LIB_EXT1__ 1
#define HAVE_INTPTR_T 1
#define HAVE_UINTPTR_T 1
#define HAVE_PTRDIFF_T 1
#define HAVE_USECONDS_T 1
#define HAVE_USLEEP 1
#define HAVE_BLKCNT_T 1
#define HAVE_BLKSIZE_T 1
#define HAVE_INO_T 1
#define HAVE_LOFF_T 1
#define HAVE_OFF64_T 1
#define HAVE_DEV_T 1
#define HAVE_BOOL 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_CTYPE_H 1
#define HAVE_STDARG_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_FCNTL_H 1
#define HAVE_DLFCN_H 1
#define HAVE_GETPID 1
#define HAVE_PROGRAM_INVOCATION_SHORT_NAME 1
#define HAVE_VA_COPY 1
#define HAVE_GCC_ATOMIC_BUILTINS 1
#define HAVE_VISIBILITY_ATTR 1
#define HAVE___ATTRIBUTE__ 1
#define HAVE_CONSTRUCTOR_ATTRIBUTE 1
#define HAVE_DESTRUCTOR_ATTRIBUTE 1
#define HAVE_ALLOCA 1
#define HAVE_MMAP 1
#define HAVE_GETPAGESIZE 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMMEM 1
#define HAVE_MEMCHR 1
#define HAVE_STRNLEN 1
#define HAVE_STRNDUP 1
#define HAVE_STRTOK_R 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOLL 1
#define HAVE_GETLINE 1
#define HAVE_GETDELIM 1
#define HAVE_VSYSLOG 1
#define HAVE_VASPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_C99_VSNPRINTF 1
#define HAVE_VDPRINTF 1
#define HAVE_DPRINTF 1
#define HAVE_ASPRINTF 1
#define HAVE_SNPRINTF 1
#define HAVE_PIPE 1
#define HAVE_DUP2 1
#define HAVE_FTRUNCATE 1
#define HAVE_RANDOM 1
#define HAVE_RANDOM_R 1
#define HAVE_SRANDOM 1
#define HAVE_SRANDOM_R 1
#define HAVE_INITSTATE 1
#define HAVE_INITSTATE_R 1
#define HAVE_SETSTATE 1
#define HAVE_SETSTATE_R 1
#define HAVE_DECL_ENVIRON 1
#define HAVE_ENVIRON 1
#define HAVE_GETENV 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_PUTENV 1
#define HAVE_CLEARENV 1
#define HAVE_MKSTEMP 1
#define HAVE_MKDTEMP 1
#define HAVE_PREAD 1
#define HAVE_PWRITE 1
#define HAVE_CHOWN 1
#define HAVE_LCHOWN 1
#define HAVE_CHROOT 1
#define HAVE_BZERO 1
#define HAVE_STRLCAT 1
#define HAVE_STRLCPY 1
#define HAVE_GETIFADDRS 1
#define HAVE_FREEIFADDRS 1
#define HAVE_INET_NTOP 1
#define HAVE_INET_PTON 1
#define HAVE_INET_ATON 1
#define HAVE_INET_NTOA 1
#define HAVE_GETADDRINFO 1
#define HAVE_FREEADDRINFO 1
#define HAVE_GAI_STRERROR 1
#define HAVE_GETNAMEINFO 1
#define HAVE_POLL 1
#define HAVE_PPOLL 1
#define HAVE_SOCKETPAIR 1
#define HAVE_DLOPEN 1
#define HAVE_DLSYM 1
#define HAVE_DLERROR 1
#define HAVE_DLCLOSE 1
#define HAVE_FAULT_TOLERANT 1
#define HAVE_TIMEGM 1
#define HAVE_STRPTIME 1
#define HAVE_CLOSEFROM 1
#define HAVE_CLOSE_RANGE 1
#define HAVE_MEMSET_S 1
#define HAVE_EXPLICIT_BZERO 1
#define HAVE_LIBPTHREAD 1
#define HAVE_PTHREAD 1
#define HAVE___THREAD 1
#define HAVE_GETPROGNAME 1
#define HAVE_SETPROGNAME 1
#define HAVE_SETPROCTITLE 1
#define _LARGEFILE64_SOURCE 1
#define _FILE_OFFSET_BITS 64
#include <string.h>
#include <dlfcn.h>
/* Bionic doesn't ship memset_s (C11 Annex K). talloc only uses it for
 * zero-fills on free/realloc, so a thin macro shim over memset is
 * sufficient — the C11 contract that requires the side effect to
 * survive optimizer dead-store elimination doesn't matter to us. */
#define memset_s(dest, destsz, ch, count) ((memset((dest), (ch), (count))), 0)
#endif
EOF
}

# Build static libtalloc.a for one ABI.
build_talloc() {
    local abi="$1"
    local triple
    case "$abi" in
        aarch64) triple="aarch64-linux-android29" ;;
        x86_64)  triple="x86_64-linux-android29" ;;
        *) echo "internal error: unknown abi $abi" >&2; exit 1 ;;
    esac
    local build_dir="$TALLOC_DIR/build-$abi"
    if [ "$CLEAN" = "1" ] && [ -d "$build_dir" ]; then
        rm -rf "$build_dir"
    fi
    mkdir -p "$build_dir"
    write_talloc_config_h "$build_dir/config.h"
    if [ -f "$build_dir/libtalloc.a" ] && [ "$build_dir/libtalloc.a" -nt "$TALLOC_DIR/talloc.c" ]; then
        echo "    talloc-$abi: cached"
        return 0
    fi
    echo "==> compiling talloc-$abi"
    "$NDK_BIN/$triple-clang" \
        -fPIC -O2 -Wall -Wno-unused-parameter \
        -I"$TALLOC_DIR" -I"$build_dir" -I"$TALLOC_DIR/lib/replace" \
        -DHAVE_CONFIG_H=1 \
        -DTALLOC_BUILD_VERSION_MAJOR=2 \
        -DTALLOC_BUILD_VERSION_MINOR=4 \
        -DTALLOC_BUILD_VERSION_RELEASE=4 \
        -c "$TALLOC_DIR/talloc.c" -o "$build_dir/talloc.o"
    "$NDK_BIN/llvm-ar" rcs "$build_dir/libtalloc.a" "$build_dir/talloc.o"
}

# Build proot for one ABI, writing the binary to jniLibs/<abi>/libproot.so.
build_proot() {
    local abi="$1"
    local triple jnilib_abi
    case "$abi" in
        aarch64) triple="aarch64-linux-android29"; jnilib_abi="arm64-v8a" ;;
        x86_64)  triple="x86_64-linux-android29";  jnilib_abi="x86_64"   ;;
        *) echo "internal error: unknown abi $abi" >&2; exit 1 ;;
    esac
    local talloc_build="$TALLOC_DIR/build-$abi"
    [ -f "$talloc_build/libtalloc.a" ] || { echo "ERROR: talloc-$abi not built" >&2; exit 1; }

    # The Termux Makefile uses one in-tree build dir per checkout, so
    # consecutive cross-ABI invocations (e.g. Gradle's per-ABI tasks)
    # would link aarch64 against leftover x86_64 .o's. Stamp the dir
    # with the current ABI and `make clean` on mismatch.
    local abi_stamp="$PROOT_DIR/src/.tawc-built-abi"
    if [ "$CLEAN" = "1" ] || { [ -f "$abi_stamp" ] && [ "$(cat "$abi_stamp")" != "$abi" ]; }; then
        ( cd "$PROOT_DIR/src" && make clean >/dev/null 2>&1 || true )
    fi
    echo "$abi" > "$abi_stamp"

    # proot's Makefile builds .check_*.o programs and inspects whether
    # linking succeeded to decide whether process_vm_readv / seccomp
    # are available. Cross-compiled they always link (NDK API 29 has
    # both) but the auto-generated build.h is only filled in if the
    # check programs run, so we hand-write it. VERSION here is what
    # proot reports via --version.
    # HAVE_SECCOMP_FILTER is intentionally NOT defined: when proot
    # tries to install its own seccomp BPF on the tracee, the tracee
    # already has Android's untrusted_app filter inherited from
    # zygote, and bash's first execve trips a SIGSYS that kills the
    # whole tree (verified via `proot -v 2`: "vpid 1: terminated with
    # signal 31"). Compiling the seccomp-filter codepaths out forces
    # proot into pure-ptrace mode — every syscall is a trap-and-resume
    # rather than a seccomp RET_TRACE shortcut. Slower, but functional
    # under app uid. See `notes/proot.md` "Performance cost" for the
    # ~5-10× syscall overhead this implies.
    cat >"$PROOT_DIR/src/build.h" <<EOF
#ifndef BUILD_H
#define BUILD_H
#define VERSION "5.4.0-tawc"
#define HAVE_PROCESS_VM
#endif
EOF

    echo "==> building proot-$abi"
    (
        cd "$PROOT_DIR/src"
        # The Makefile uses `CFLAGS += $(shell pkg-config --cflags talloc)`
        # to discover talloc. There's no Android pkg-config to consult, so
        # we point at our build dir directly via env CFLAGS/LDFLAGS. Env
        # vars accumulate via `+=` in the makefile, whereas make-line
        # overrides (`make CFLAGS=...`) replace any later `+=` — so this
        # has to be `export`, not a make argument.
        #
        # -Wno-deprecated-declarations: talloc 2.4.4 deprecated
        # talloc_autofree_context, which proot still calls. The deprecation
        # is non-fatal and we don't gain anything from breaking the build
        # over a future-removed API call.
        export CFLAGS="-I$TALLOC_DIR -I$talloc_build -fPIC -O2 -Wno-unused-parameter -Wno-strict-aliasing -Wno-unknown-warning-option -Wno-deprecated-declarations"
        export LDFLAGS="-L$talloc_build -ltalloc"
        make \
            CC="$NDK_BIN/$triple-clang" \
            LD="$NDK_BIN/$triple-clang" \
            AR="$NDK_BIN/llvm-ar" \
            STRIP="$NDK_BIN/llvm-strip" \
            OBJCOPY="$NDK_BIN/llvm-objcopy" \
            OBJDUMP="$NDK_BIN/llvm-objdump" \
            -j"$(nproc 2>/dev/null || echo 4)" \
            proot
    )

    "$NDK_BIN/llvm-strip" "$PROOT_DIR/src/proot"
    [ -f "$PROOT_DIR/src/loader/loader" ] || \
        { echo "ERROR: proot loader not built at $PROOT_DIR/src/loader/loader" >&2; exit 1; }
    "$NDK_BIN/llvm-strip" "$PROOT_DIR/src/loader/loader"

    local out_dir="$JNILIBS_DIR/$jnilib_abi"
    mkdir -p "$out_dir"
    cp "$PROOT_DIR/src/proot" "$out_dir/libproot.so"
    chmod 0755 "$out_dir/libproot.so"
    # Vendor the per-invocation loader as a sibling jniLib. proot
    # would otherwise extract it to PROOT_TMP_DIR at runtime and
    # try to exec it from there — which Android 10+ blocks (apps
    # targeting API 29+ can't execve files under their own home
    # dir). With PROOT_LOADER pointing at this pre-extracted file in
    # nativeLibraryDir (apk_data_file context, exec allowed), proot
    # skips the extract+exec dance entirely. See ProotMethod.kt.
    cp "$PROOT_DIR/src/loader/loader" "$out_dir/libproot-loader.so"
    chmod 0755 "$out_dir/libproot-loader.so"
    echo "    -> $out_dir/libproot.so + libproot-loader.so"
}

# ---------------------------------------------------------------------------
fetch_proot
fetch_talloc

case "$ABI" in
    aarch64|x86_64) build_talloc "$ABI"; build_proot "$ABI" ;;
    both)
        build_talloc aarch64; build_proot aarch64
        build_talloc x86_64;  build_proot x86_64
        ;;
esac

echo "==> done"
