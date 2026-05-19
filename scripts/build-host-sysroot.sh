#!/bin/bash
# Build a target Linux rootfs/sysroot on the host, without adb or an
# already-installed device rootfs. This is the canonical source for
# glibc-side cross-build inputs used by production assets and test apps.
#
# Output:
#   build/sysroots/<distro>-<abi>/
#   build/<abi>-sysroot -> sysroots/<distro>-<abi>   (compatibility link)
#
# Usage:
#   scripts/build-host-sysroot.sh --abi=aarch64 [--distro=arch]
#   scripts/build-host-sysroot.sh --abi=x86_64  [--distro=arch]
#   scripts/build-host-sysroot.sh --abi=both    [--distro=arch]
#   scripts/build-host-sysroot.sh --clean --abi=aarch64
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ABIS=""
DISTRO="${TAWC_SYSROOT_DISTRO:-arch}"
CLEAN=0
PROFILE="full"
for arg in "$@"; do
    case "$arg" in
        --abi=aarch64|--abi=arm64) ABIS="aarch64" ;;
        --abi=x86_64)              ABIS="x86_64" ;;
        --abi=both)                ABIS="aarch64 x86_64" ;;
        --distro=*)                DISTRO="${arg#--distro=}" ;;
        --profile=*)               PROFILE="${arg#--profile=}" ;;
        --clean)                   CLEAN=1 ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'
            exit 0
            ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 1 ;;
    esac
done
[ -n "$ABIS" ] || ABIS="aarch64"

MIRROR_PROXY="${TAWC_MIRROR_PROXY:-}"
if [ "$PROFILE" != "prod" ] && [ -z "$MIRROR_PROXY" ]; then
    MIRROR_PROXY="http://127.0.0.1:8080/proxy/"
fi

case "$DISTRO" in
    arch|manjaro|void) ;;
    *) echo "ERROR: unsupported distro '$DISTRO' (expected arch, manjaro, void)" >&2; exit 1 ;;
esac
case "$PROFILE" in
    prod|test|full) ;;
    *) echo "ERROR: unsupported profile '$PROFILE' (expected prod, test, full)" >&2; exit 1 ;;
esac

BUILD_ROOT="$REPO_DIR/build"
SYSROOT_ROOT="$BUILD_ROOT/sysroots"
CACHE_ROOT="$BUILD_ROOT/host-sysroot-cache"

need_tool() {
    command -v "$1" >/dev/null || {
        echo "ERROR: '$1' not on PATH. See notes/building.md." >&2
        exit 1
    }
}

fetch_url() {
    local url="$1" out="$2" fetch
    fetch="$url"
    if [ -n "$MIRROR_PROXY" ]; then
        if [[ "$url" =~ ^(https?)://([^/]+)/(.*)$ ]]; then
            fetch="${MIRROR_PROXY%/}/${BASH_REMATCH[1]}/${BASH_REMATCH[2]}/${BASH_REMATCH[3]}"
        else
            echo "ERROR: cannot proxy unsupported URL: $url" >&2
            return 1
        fi
    fi
    if ! curl -fsSL --retry 3 -o "$out" "$fetch"; then
        if [ -n "$MIRROR_PROXY" ]; then
            echo "ERROR: failed to fetch through mirror proxy: $MIRROR_PROXY" >&2
            echo "       upstream URL: $url" >&2
            echo "       proxy URL: $fetch" >&2
            echo "       If 127.0.0.1:8080 is refused, run: scripts/cache-proxy.sh run" >&2
        fi
        return 1
    fi
}

arch_packages_for_profile() {
    case "$PROFILE" in
        prod)
            echo "glibc linux-api-headers wayland wayland-protocols libdrm systemd-libs libffi gcc-libs vulkan-icd-loader libxkbcommon libglvnd libdisplay-info"
            ;;
        test|full)
            local pkgs="glibc linux-api-headers wayland wayland-protocols libdrm systemd-libs libffi gcc-libs vulkan-icd-loader libxkbcommon libglvnd libdisplay-info gtk4 cairo libx11 libxcb mesa"
            # ALARM's sync db omits %DEPENDS%, so list the test-app
            # pkg-config closure explicitly. x86_64 resolves these from
            # repo metadata, but adding them there is harmless too.
            pkgs+=" zlib libpng fontconfig freetype2 bzip2 brotli expat xorgproto libxext libxrender libxau libxdmcp pixman pango gdk-pixbuf2 graphene glib2 harfbuzz fribidi libepoxy"
            pkgs+=" sysprof pcre2 graphite libthai libxft glycin shared-mime-info util-linux-libs"
            echo "$pkgs"
            ;;
    esac
}

void_packages_for_profile() {
    case "$PROFILE" in
        prod)
            echo "wayland-devel wayland-protocols libdrm-devel eudev-libudev-devel libffi-devel gcc-libs vulkan-loader-devel libxkbcommon-devel libglvnd-devel libdisplay-info-devel"
            ;;
        test|full)
            echo "wayland-devel wayland-protocols libdrm-devel eudev-libudev-devel libffi-devel gcc-libs vulkan-loader-devel libxkbcommon-devel libglvnd-devel libdisplay-info-devel gtk4-devel cairo-devel libX11-devel libxcb-devel MesaLib-devel"
            ;;
    esac
}

arch_repo_specs() {
    local abi="$1"
    case "$DISTRO/$abi" in
        arch/x86_64)
            echo "core https://mirror.alwyzon.net/archlinux/core/os/x86_64"
            echo "extra https://mirror.alwyzon.net/archlinux/extra/os/x86_64"
            ;;
        arch/aarch64)
            echo "core https://fl.us.mirror.archlinuxarm.org/aarch64/core"
            echo "extra https://fl.us.mirror.archlinuxarm.org/aarch64/extra"
            echo "community https://fl.us.mirror.archlinuxarm.org/aarch64/community"
            echo "alarm https://fl.us.mirror.archlinuxarm.org/aarch64/alarm"
            echo "aur https://fl.us.mirror.archlinuxarm.org/aarch64/aur"
            ;;
        manjaro/aarch64)
            echo "core https://mirror.easyname.at/manjaro/arm-stable/core/aarch64"
            echo "extra https://mirror.easyname.at/manjaro/arm-stable/extra/aarch64"
            echo "community https://mirror.easyname.at/manjaro/arm-stable/community/aarch64"
            ;;
        *)
            echo "ERROR: unsupported pacman sysroot target $DISTRO/$abi" >&2
            exit 1
            ;;
    esac
}

strip_dep_name() {
    local dep="$1"
    dep="${dep%%[<>=]*}"
    dep="${dep%%:*}"
    echo "$dep"
}

is_ignored_virtual_dep() {
    case "$1" in
        sh|opengl-driver|libgl|libegl|vulkan-driver|libsystemd|libverto-module-base)
            return 0
            ;;
        *) return 1 ;;
    esac
}

build_arch_sysroot() {
    local abi="$1"
    need_tool curl
    need_tool bsdtar

    local dst="$SYSROOT_ROOT/$DISTRO-$abi"
    local work="$CACHE_ROOT/$DISTRO-$abi"
    local cache="$work/pkg"
    local sync="$work/sync"
    local db_extract="$work/db-extract"
    local packages
    packages="$(arch_packages_for_profile)"

    if [ "$CLEAN" = "1" ]; then
        rm -rf "$dst" "$work"
    fi
    mkdir -p "$dst" "$cache" "$sync" "$db_extract" "$work"

    declare -A pkg_repo pkg_base pkg_filename pkg_depends provides_to_pkg
    local repo base db_file db_dir
    echo "==> [$DISTRO/$abi] fetching package databases"
    while read -r repo base; do
        [ -n "$repo" ] || continue
        db_file="$sync/$repo.db"
        db_dir="$db_extract/$repo"
        mkdir -p "$db_dir"
        fetch_url "$base/$repo.db" "$db_file"
        rm -rf "$db_dir"
        mkdir -p "$db_dir"
        bsdtar -xf "$db_file" -C "$db_dir"
        local desc name filename section line dep provide
        while IFS= read -r -d '' desc; do
            name=""
            filename=""
            section=""
            local deps=()
            local provs=()
            while IFS= read -r line || [ -n "$line" ]; do
                case "$line" in
                    %NAME%|%FILENAME%|%DEPENDS%|%PROVIDES%) section="$line"; continue ;;
                    %*) section=""; continue ;;
                    "") continue ;;
                esac
                case "$section" in
                    %NAME%)     name="$line" ;;
                    %FILENAME%) filename="$line" ;;
                    %DEPENDS%)  deps+=("$(strip_dep_name "$line")") ;;
                    %PROVIDES%) provs+=("$(strip_dep_name "$line")") ;;
                esac
            done < "$desc"
            local depfile="${desc%/desc}/depends"
            if [ -f "$depfile" ]; then
                section=""
                while IFS= read -r line || [ -n "$line" ]; do
                    case "$line" in
                        %DEPENDS%|%PROVIDES%) section="$line"; continue ;;
                        %*) section=""; continue ;;
                        "") continue ;;
                    esac
                    case "$section" in
                        %DEPENDS%)  deps+=("$(strip_dep_name "$line")") ;;
                        %PROVIDES%) provs+=("$(strip_dep_name "$line")") ;;
                    esac
                done < "$depfile"
            fi
            [ -n "$name" ] || continue
            pkg_repo["$name"]="$repo"
            pkg_base["$name"]="$base"
            pkg_filename["$name"]="$filename"
            pkg_depends["$name"]="${deps[*]}"
            for provide in "${provs[@]}"; do
                [ -n "$provide" ] && provides_to_pkg["$provide"]="$name"
            done
        done < <(find "$db_dir" -mindepth 2 -maxdepth 2 -name desc -print0)
    done < <(arch_repo_specs "$abi")

    declare -A selected seen
    local queue=($packages)
    local i=0
    while [ "$i" -lt "${#queue[@]}" ]; do
        local pkg="${queue[$i]}"
        i=$((i + 1))
        pkg="$(strip_dep_name "$pkg")"
        [ -n "$pkg" ] || continue
        # Only resolve shared-library virtuals (libfoo.so=...). Generic
        # providers like vulkan-driver/libgl can pick enormous concrete
        # packages (for example nvidia-utils) that are runtime choices,
        # not sysroot build inputs.
        if [ -z "${pkg_filename[$pkg]+x}" ] && [[ "$pkg" == *.so* ]] && [ -n "${provides_to_pkg[$pkg]+x}" ]; then
            pkg="${provides_to_pkg[$pkg]}"
        fi
        if [ -z "${pkg_filename[$pkg]+x}" ]; then
            is_ignored_virtual_dep "$pkg" || \
                echo "WARN: [$DISTRO/$abi] dependency '$pkg' not found in package databases" >&2
            continue
        fi
        [ -n "${seen[$pkg]+x}" ] && continue
        seen["$pkg"]=1
        selected["$pkg"]=1
        for dep in ${pkg_depends[$pkg]:-}; do
            queue+=("$dep")
        done
    done

    echo "==> [$DISTRO/$abi] downloading ${#selected[@]} package archives"
    local pkg file url
    for pkg in "${!selected[@]}"; do
        file="${pkg_filename[$pkg]}"
        [ -n "$file" ] || continue
        if [ ! -f "$cache/$file" ]; then
            url="${pkg_base[$pkg]}/$file"
            fetch_url "$url" "$cache/$file"
        fi
    done

    echo "==> [$DISTRO/$abi] extracting sysroot"
    find "$cache" -maxdepth 1 -type f \( -name '*.pkg.tar.*' ! -name '*.sig' \) -print0 \
        | sort -z \
        | while IFS= read -r -d '' pkg; do
            bsdtar -xpf "$pkg" -C "$dst" --no-same-owner --no-xattrs \
                --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.PKGINFO'
        done
    finalize_sysroot "$abi" "$dst"
}

build_void_sysroot() {
    local abi="$1"
    need_tool xbps-install

    local dst="$SYSROOT_ROOT/$DISTRO-$abi"
    local repo
    case "$abi" in
        aarch64) repo="https://repo-default.voidlinux.org/current/aarch64" ;;
        x86_64)  repo="https://repo-default.voidlinux.org/current" ;;
        *) echo "ERROR: bad ABI '$abi'" >&2; exit 1 ;;
    esac
    local packages
    packages="$(void_packages_for_profile)"

    if [ "$CLEAN" = "1" ]; then
        rm -rf "$dst"
    fi
    mkdir -p "$dst"
    echo "==> [void/$abi] installing sysroot packages"
    XBPS_ARCH="$abi" xbps-install -Sy -r "$dst" -R "$repo" -y $packages
    finalize_sysroot "$abi" "$dst"
}

finalize_sysroot() {
    local abi="$1" dst="$2"
    mkdir -p "$dst/usr/lib" "$dst/usr/include"
    ln -sfn usr/lib "$dst/lib"
    if [ "$abi" = "x86_64" ]; then
        ln -sfn usr/lib "$dst/lib64"
    fi

    # Overlay the host-built libhybris development/link tree when present.
    # Runtime still gets libhybris through the APK asset installer; this copy
    # exists so test clients that link -lhybris-common never need a device pull.
    if [ "$abi" = "aarch64" ] && [ -d "$REPO_DIR/build/libhybris-aarch64/install/usr/lib/hybris" ]; then
        mkdir -p "$dst/usr/lib/hybris"
        cp -a "$REPO_DIR/build/libhybris-aarch64/install/usr/lib/hybris/." "$dst/usr/lib/hybris/"
    fi

    local compat="$BUILD_ROOT/$abi-sysroot"
    if [ -e "$compat" ] && [ ! -L "$compat" ]; then
        rm -rf "$compat"
    fi
    ln -sfn "sysroots/$DISTRO-$abi" "$compat"
    local record_profile="$PROFILE"
    if [ "$PROFILE" = "prod" ] && grep -q " full " "$dst/.tawc-sysroot" 2>/dev/null; then
        record_profile="full"
    fi
    echo "$DISTRO $abi $record_profile $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$dst/.tawc-sysroot"
    echo "==> [$DISTRO/$abi] sysroot ready: $dst"
    du -sh "$dst"
}

mkdir -p "$SYSROOT_ROOT" "$CACHE_ROOT"
for abi in $ABIS; do
    case "$DISTRO" in
        arch|manjaro) build_arch_sysroot "$abi" ;;
        void)         build_void_sysroot "$abi" ;;
    esac
done
