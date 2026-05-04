package me.phie.tawc.install.distro.arch

import me.phie.tawc.install.InstallationMethod
import java.io.IOException

/**
 * Shared helpers for the two Arch flavours
 * ([ArchLinuxX86_64], [ArchLinuxArm]). Both use pacman; the only real
 * differences between them are the bootstrap URL/format, the keyring
 * name (`archlinux` vs `archlinuxarm`), the mirrorlist contents, and
 * the cruft package set (kernel / firmware split package names).
 *
 * The chroot is a Wayland-userland-only environment — no kernel runs
 * inside, no init system manages services, no user logs in
 * interactively. Anything in the bootstrap that exists to boot a real
 * Linux install (kernel, firmware, mkinitcpio, console keymaps) or to
 * make a package manageable from a local console (man pages, info
 * pages, doc trees, translation .mo files) is dead weight on
 * Android's NAND. Both Arch flavours strip the same shape of files,
 * so the policy lives here.
 */
internal object ArchPacmanCommon {

    /**
     * Paths under the rootfs to delete after the bootstrap tarball is
     * extracted. These are big standalone trees (kernel/firmware/locale
     * data, package docs) that will be re-created by future `pacman -S`
     * operations unless we also tell pacman not to extract them — see
     * [NO_EXTRACT_PATTERNS]. Leading slash relative to rootfs.
     *
     * Sizes from a fresh ALARM aarch64 bootstrap (2026-04):
     *   /usr/lib/firmware       920 MB
     *   /boot                   344 MB
     *   /usr/lib/modules        185 MB
     *   /usr/share/locale       111 MB
     *   /usr/share/man           36 MB
     *   /usr/share/doc           13 MB
     *   /usr/share/info           8 MB
     *   /var/cache/pacman/pkg   197 MB (bootstrap-bundled .pkg.tar.xz)
     *
     * That's ~1.8 GB of immediate reclaim before we even run pacman.
     */
    private val POST_EXTRACT_PURGE_PATHS: List<String> = listOf(
        // kernel & boot artefacts — no kernel runs in the chroot
        "/boot",
        "/usr/lib/firmware",
        "/usr/lib/modules",
        // package manager pre-cached tarballs from the bootstrap
        "/var/cache/pacman/pkg",
        // human-readable docs — no interactive shell, no man reader
        "/usr/share/man",
        "/usr/share/info",
        "/usr/share/doc",
        "/usr/share/gtk-doc",
        "/usr/share/help",
        // translation .mo files and locale-gen inputs (we don't run
        // locale-gen). C/POSIX locale is built into glibc and works
        // without any of this. Apps that look up missing translations
        // fall back to English source strings.
        "/usr/share/locale",
        "/usr/share/i18n",
        // GObject introspection XML sources. Only `g-ir-compiler` and
        // doc generators (`g-ir-doc-tool`) read these. Runtime GI
        // consumers (vala, pygobject, gjs) read the compiled
        // `.typelib` files in `/usr/lib/girepository-1.0/` instead. We
        // never recompile typelibs in the chroot, so the XML is
        // ~56 MB of pure dead weight.
        "/usr/share/gir-1.0",
    )

    /**
     * File-glob paths to delete after extract. Kept separate from
     * [POST_EXTRACT_PURGE_PATHS] because the shell expansion has to
     * happen *outside* the `"$ROOTFS$path"` quoting that the
     * directory list relies on.
     *
     * Targets large per-file artefacts that aren't worth removing the
     * containing dir for. Paired with matching entries in
     * [NO_EXTRACT_PATTERNS] so future `pacman -S` doesn't put them
     * back. Sizes from a fresh manjaro-arm bootstrap + base packages
     * (2026-05):
     *   /usr/lib/lib(go|gphobos|...).so  ~120 MB (multi-language gcc runtimes)
     *   /usr/lib/lib(go|gphobos|stdc++|c|isl|...).a  ~50 MB (static archives;
     *     keeps libgcc*.a / libssp*.a — gcc's default link spec
     *     references them)
     */
    private val POST_EXTRACT_PURGE_GLOBS: List<String> = listOf(
        // gcc-libs ships runtime .so files for every language frontend
        // gcc supports — Go, D, Fortran, Objective-C, Ada — plus the
        // sanitizer runtimes (asan/tsan/ubsan/lsan/hwasan). We only
        // ever build C in the chroot, none of our binaries link any
        // sanitizer, and nothing else in the rootfs imports these.
        // Keep libgcc_s, libstdc++, libatomic, libgomp, libssp — those
        // are pulled in dynamically by ordinary C/C++ binaries.
        "/usr/lib/libgo.so*",
        "/usr/lib/libgphobos.so*",
        "/usr/lib/libgdruntime.so*",
        "/usr/lib/libgfortran.so*",
        "/usr/lib/libobjc.so*",
        "/usr/lib/libgnat-*.so*",
        "/usr/lib/libgnarl-*.so*",
        "/usr/lib/libasan.so*",
        "/usr/lib/libtsan.so*",
        "/usr/lib/libubsan.so*",
        "/usr/lib/liblsan.so*",
        "/usr/lib/libhwasan.so*",
        // Static archives for the same multi-language runtimes and
        // sanitizers, plus a few large standalone static libs we never
        // statically link against. We build gtk4-debug-app dynamically
        // (see `testing/gtk4-debug-app/build.sh`), so the ld(1) `-l`
        // search of these is dead weight on disk.
        //
        // Listed by exact path on purpose. A blanket `usr/lib/*.a`
        // glob would *also* match `usr/lib/gcc/<triple>/<ver>/libgcc.a`
        // because pacman's NoExtract uses fnmatch(3) without
        // FNM_PATHNAME, so `*` matches `/`. Once libgcc.a is missing,
        // every gcc invocation aborts with `cannot find -lgcc` from
        // the default link spec. Stick to specific filenames.
        "/usr/lib/libgo.a",
        "/usr/lib/libgphobos.a",
        "/usr/lib/libgdruntime.a",
        "/usr/lib/libgfortran.a",
        "/usr/lib/libobjc.a",
        "/usr/lib/libgnat-*.a",
        "/usr/lib/libgnarl-*.a",
        "/usr/lib/libasan.a",
        "/usr/lib/libtsan.a",
        "/usr/lib/libubsan.a",
        "/usr/lib/liblsan.a",
        "/usr/lib/libhwasan.a",
        "/usr/lib/libstdc++.a",
        "/usr/lib/libsupc++.a",
        "/usr/lib/libisl.a",
        "/usr/lib/libgio-2.0.a",
        "/usr/lib/libgprofng.a",
        "/usr/lib/libc.a",
    )

    /**
     * `NoExtract` glob patterns appended to `/etc/pacman.conf`. These
     * keep future `pacman -S` invocations from re-creating the trees
     * we just deleted (test-deps install, etc) and
     * also skip newly-pulled docs/locales/firmware so the rootfs
     * doesn't grow back to its bootstrap size.
     *
     * Patterns are applied to file paths *inside* the package tarball
     * (no leading slash). `pacman.conf(5)` accepts shell globs and
     * `!`-prefixed exceptions; we use plain globs throughout for
     * clarity. Multiple `NoExtract = ` lines accumulate.
     */
    private val NO_EXTRACT_PATTERNS: List<String> = listOf(
        // kernel / firmware paths (defence in depth — packages we
        // don't install shouldn't try to drop files here, but if they
        // do, skip them)
        "boot/*",
        "usr/lib/firmware/*",
        "usr/lib/modules/*",
        // docs
        "usr/share/man/*",
        "usr/share/info/*",
        "usr/share/doc/*",
        "usr/share/gtk-doc/*",
        "usr/share/help/*",
        // translations & locale inputs (see POST_EXTRACT_PURGE_PATHS)
        "usr/share/locale/*",
        "usr/share/i18n/locales/*",
        "usr/share/i18n/charmaps/*",
        // GIR XML sources — runtime GI consumers read .typelib in
        // /usr/lib/girepository-1.0/ instead.
        "usr/share/gir-1.0/*",
        // multi-language gcc runtimes (Go/D/Fortran/Ada/ObjC) and
        // sanitizers (asan/tsan/ubsan/lsan/hwasan). We only ever
        // compile C in the chroot. See POST_EXTRACT_PURGE_GLOBS for
        // the matching post-extract delete.
        "usr/lib/libgo.so*",
        "usr/lib/libgphobos.so*",
        "usr/lib/libgdruntime.so*",
        "usr/lib/libgfortran.so*",
        "usr/lib/libobjc.so*",
        "usr/lib/libgnat-*.so*",
        "usr/lib/libgnarl-*.so*",
        "usr/lib/libasan.so*",
        "usr/lib/libtsan.so*",
        "usr/lib/libubsan.so*",
        "usr/lib/liblsan.so*",
        "usr/lib/libhwasan.so*",
        // static archives for the multi-language runtimes and
        // sanitizers, plus the large unused standalones. **Not**
        // libgcc*.a / libssp*.a — gcc's default link spec needs them.
        "usr/lib/libgo.a",
        "usr/lib/libgphobos.a",
        "usr/lib/libgdruntime.a",
        "usr/lib/libgfortran.a",
        "usr/lib/libobjc.a",
        "usr/lib/libgnat-*.a",
        "usr/lib/libgnarl-*.a",
        "usr/lib/libasan.a",
        "usr/lib/libtsan.a",
        "usr/lib/libubsan.a",
        "usr/lib/liblsan.a",
        "usr/lib/libhwasan.a",
        "usr/lib/libstdc++.a",
        "usr/lib/libsupc++.a",
        "usr/lib/libisl.a",
        "usr/lib/libgio-2.0.a",
        "usr/lib/libgprofng.a",
        "usr/lib/libc.a",
    )

    /**
     * Bootstrap-installed packages that the chroot has no use for —
     * removed via `pacman -Rdd` (no dep check) right after
     * `pacman-key --populate`. The kernel package name varies by arch
     * (`linux` vs `linux-aarch64`), so the caller passes that in via
     * [archSpecificCruft]; everything else is shared.
     *
     * The on-disk files are mostly already gone (see
     * [POST_EXTRACT_PURGE_PATHS]); this just cleans up pacman's
     * database so future operations don't think the cruft is
     * installed and don't try to upgrade it.
     *
     * Anything `base` / `base-devel` / `systemd` / `gnupg` lists as a
     * `Depends` is intentionally **not** here, because the subsequent
     * `pacman -Syu --needed <base packages>` would just reinstall it
     * — that's why `audit`, `kbd`, `hwdata`, `pciutils`, `iputils`,
     * `iproute2`, `device-mapper`, `cryptsetup`, `e2fsprogs`,
     * `tpm2-tss`, … are absent. Adding such a package here costs
     * an extra `pacman -Rdd` round-trip during install for no
     * net-disk reduction.
     */
    private val SHARED_CRUFT_PACKAGES: List<String> = listOf(
        // firmware split packages — present in both ALARM and Arch x86
        "linux-firmware",
        "linux-firmware-amdgpu",
        "linux-firmware-atheros",
        "linux-firmware-broadcom",
        "linux-firmware-cirrus",
        "linux-firmware-intel",
        "linux-firmware-mediatek",
        "linux-firmware-nvidia",
        "linux-firmware-other",
        "linux-firmware-radeon",
        "linux-firmware-realtek",
        "linux-firmware-whence",
        // initramfs builder — only useful for booting a kernel
        "mkinitcpio",
        "mkinitcpio-busybox",
        // editors — apps that need an EDITOR can install one
        // explicitly. (gpm is a console-only soft-dep of vim/emacs;
        // once vim is gone nothing else here pulls it in.)
        "ex-vi-compat",
        "nano",
        "vim",
        "vim-runtime",
        "gpm",
        // networking userland — DNS resolution comes from
        // /etc/resolv.conf, no firewall, no SSH, no DHCP client.
        // (iputils/iproute2 are kept; they are hard deps of `base`.)
        "dhcpcd",
        "iptables",
        "nftables",
        "netctl",
        "openssh",
        "net-tools",
        // systemd-resolved client — we use plain /etc/resolv.conf and
        // don't run systemd-resolved, so the libnss-resolve glue is
        // dead weight.
        "systemd-resolvconf",
    )

    /**
     * Write `/etc/resolv.conf`, the pacman.conf tweaks, the mirrorlist
     * and `profile.d/00-path.sh`, then strip the bootstrap of the
     * trees in [POST_EXTRACT_PURGE_PATHS]. The Wayland env in
     * `profile.d/01-tawc.sh` is *not* written here — `ChrootMounter`
     * regenerates it on every chroot entry so env changes don't need
     * a reinstall (see notes/installation.md).
     *
     * @param rootfs absolute path to the chroot rootfs.
     * @param mirrorListBody contents of `/etc/pacman.d/mirrorlist`
     *   (one or more `Server = ...` lines).
     * @param ignoredPackages packages to put on the `IgnorePkg` line —
     *   defence in depth so a future `pacman -Syu` doesn't pull the
     *   kernel/firmware packages back if some package picks them up
     *   as an optional dep.
     * @param log per-line log sink.
     */
    fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorListBody: String,
        ignoredPackages: List<String>,
        log: (String) -> Unit,
    ) {
        val ignoreLine = "IgnorePkg = " + ignoredPackages.joinToString(" ")
        val noExtractLines = NO_EXTRACT_PATTERNS.joinToString("\n") { "NoExtract = $it" }
        val purgeList = POST_EXTRACT_PURGE_PATHS.joinToString(" ") { "\"\$ROOTFS$it\"" }
        // Globs are intentionally NOT quoted — the shell expands the
        // wildcards at rm time. The "$ROOTFS" prefix stays quoted so
        // the rootfs path itself is safe; the suffix is a fixed
        // literal followed by `*`, so the glob expansion is bounded.
        val purgeGlobs = POST_EXTRACT_PURGE_GLOBS.joinToString(" ") { "\"\$ROOTFS\"$it" }

        val script = buildString {
            appendLine("set -eu")
            appendLine("ROOTFS='$rootfs'")
            appendLine("TAWC_INSTALL_METHOD='${method.key}'")
            appendLine(
                """
                # DNS
                rm -f "${'$'}ROOTFS/etc/resolv.conf"
                echo nameserver 8.8.8.8 > "${'$'}ROOTFS/etc/resolv.conf"

                # pacman.conf: leave SigLevel at upstream defaults so
                # the keyring is honoured on -Syyu. DisableSandbox
                # (Magisk's sandbox propagation breaks pacman's sandbox
                # helper), comment out CheckSpace (statvfs returns 0
                # inside the chroot's bind mounts and pacman aborts),
                # and IgnorePkg for the kernel/firmware packages that
                # would try to install boot artefacts into the rootfs
                # and fail.
                grep -q '^DisableSandbox' "${'$'}ROOTFS/etc/pacman.conf" || \
                    sed -i '/^SigLevel/a DisableSandbox' "${'$'}ROOTFS/etc/pacman.conf"
                sed -i 's/^CheckSpace/#CheckSpace/' "${'$'}ROOTFS/etc/pacman.conf"
                grep -q '^IgnorePkg' "${'$'}ROOTFS/etc/pacman.conf" || \
                    sed -i "/#CheckSpace/a $ignoreLine" \
                        "${'$'}ROOTFS/etc/pacman.conf"

                # NoExtract: keep future pacman -S calls from putting
                # back the docs/firmware/locale trees we delete below.
                # The block has to land *inside* the [options] section
                # — appending to end-of-file drops it into the last
                # repo block (e.g. [aur]) and pacman silently warns
                # "directive 'NoExtract' in section 'aur' not
                # recognized" while skipping it. We splice it in just
                # before the first non-[options] section header. The
                # `# tawc-no-extract` marker makes re-runs idempotent.
                if ! grep -q '^# tawc-no-extract' "${'$'}ROOTFS/etc/pacman.conf"; then
                    section_line=${'$'}(awk '/^\[/ && !/^\[options\]/{print NR; exit}' \
                        "${'$'}ROOTFS/etc/pacman.conf")
                    if [ -z "${'$'}section_line" ] || [ "${'$'}section_line" -le 1 ]; then
                        echo "ERROR: pacman.conf has no [<repo>] section after [options]" >&2
                        exit 1
                    fi
                    {
                        head -n ${'$'}((section_line - 1)) "${'$'}ROOTFS/etc/pacman.conf"
                        cat <<'PACMAN_EOF'
# tawc-no-extract: drop docs/locales/firmware/modules/boot from every
# pacman -S transaction. Reduces install time and disk footprint.
$noExtractLines

PACMAN_EOF
                        tail -n +${'$'}section_line "${'$'}ROOTFS/etc/pacman.conf"
                    } > "${'$'}ROOTFS/etc/pacman.conf.new"
                    mv "${'$'}ROOTFS/etc/pacman.conf.new" "${'$'}ROOTFS/etc/pacman.conf"
                fi
                """.trimIndent()
            )

            appendLine("# mirrorlist")
            appendLine("cat > \"\$ROOTFS/etc/pacman.d/mirrorlist\" <<'MIRROR_EOF'")
            appendLine(mirrorListBody)
            appendLine("MIRROR_EOF")

            appendLine(
                """
                # profile.d/00-path.sh — the chroot bash inherits PATH
                # from the host (Android) which leaks /system/bin and
                # breaks everything; force a sane PATH/TMPDIR/HOME here.
                # 01-tawc.sh (Wayland env) is rewritten on every chroot
                # entry by ChrootMounter so env changes pick up without
                # a reinstall — see notes/installation.md.
                mkdir -p "${'$'}ROOTFS/etc/profile.d"
                cat > "${'$'}ROOTFS/etc/profile.d/00-path.sh" <<'PROF1_EOF'
                # tawc: fix Android-leaked environment for the chroot
                export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
                export TMPDIR=/tmp
                export HOME=/root
                PROF1_EOF
                chmod 644 "${'$'}ROOTFS/etc/profile.d/00-path.sh"

                # Bulk-delete bootstrap cruft. The matching pacman -Rdd
                # for the database side runs in initPackageManager.
                # `find -delete` (toybox) doesn't take patterns, so we
                # just rm -rf the lot — these are dirs we never want.
                rm -rf $purgeList
                # Glob-deletes (multi-language gcc runtimes, static
                # archives). Globs are unquoted on purpose so the shell
                # expands them; missing matches don't fail the build.
                rm -f $purgeGlobs 2>/dev/null || true

                echo OK
                """.trimIndent()
            )
        }
        val r = method.runOutside(script) { log("conf: $it") }
        if (!r.ok) {
            throw IOException("Configure failed:\n${r.output}")
        }
    }

    /**
     * `pacman-key --init && pacman-key --populate <keyring>`, then
     * `pacman -Rdd` on the bootstrap-cruft package set
     * ([SHARED_CRUFT_PACKAGES] plus [archSpecificCruft] for the
     * kernel package). Skipped on packages that aren't actually
     * installed — both arches share most of the list but ALARM ships
     * a slightly different set, and we want the helper to be
     * tolerant of either.
     *
     * `--noscriptlet` is passed because the kernel's pre-remove hook
     * tries to regenerate the initramfs (which pulls in mkinitcpio,
     * which we're also removing). We don't have a kernel to install
     * an initramfs for anyway. `-Rdd` skips dependency checks, so
     * removing e.g. `linux-aarch64` doesn't cascade into uninstalling
     * the `base` metapackage.
     *
     * The `pacman -Syu` step is intentionally *not* here — see
     * [installBasePackages]. Splitting it out introduces a window
     * where the in-chroot DB and the upstream mirror state can drift
     * and pacman fetches a `pkg.tar.xz` that's already been rolled
     * forward; merging the sync into the same transaction as the
     * base-package install closes that.
     */
    /**
     * @param archSpecificCruft per-arch additions to
     *   [SHARED_CRUFT_PACKAGES] — currently just the kernel package
     *   name, since `linux` (Arch x86) and `linux-aarch64` (ALARM) are
     *   the only difference between the two flavours' bootstrap
     *   contents. Caller's list is concatenated with the shared one
     *   and filtered through `pacman -Qq` before the `-Rdd` pass.
     */
    fun initPackageManager(
        method: InstallationMethod,
        rootfs: String,
        keyring: String,
        archSpecificCruft: List<String>,
        log: (String) -> Unit,
    ) {
        val cruft = (archSpecificCruft + SHARED_CRUFT_PACKAGES).joinToString(" ")
        val pacmanKeyBlock = "pacman-key --init\n            pacman-key --populate $keyring"
        val res = method.runInside(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            set -e
            $pacmanKeyBlock

            # Filter the cruft list to packages actually installed,
            # then -Rdd them in one go. -Q is fast (local DB only) and
            # not affected by mirror state.
            installed=""
            for pkg in $cruft; do
                if pacman -Qq "${'$'}pkg" >/dev/null 2>&1; then
                    installed="${'$'}installed ${'$'}pkg"
                fi
            done
            if [ -n "${'$'}installed" ]; then
                echo "removing bootstrap cruft:${'$'}installed"
                pacman -Rdd --noconfirm --noscriptlet ${'$'}installed
            fi
            """.trimIndent(),
            onLine = { log("pacman-key: $it") },
        )
        if (!res.ok) {
            throw IOException("pacman-key init / cruft removal failed (exit=${res.exitCode})")
        }
    }

    /**
     * `pacman -Syu --needed --noconfirm <packages>`, then clear the
     * package cache.
     *
     * Combining `-Syyu` with the explicit package list (instead of two
     * separate `pacman -Syu` then `pacman -S --needed` calls)
     * eliminates the version-skew window: a single transaction sees
     * one DB snapshot, so it can never pacman-Sy a stale DB and then
     * try to fetch a `pkg.tar.xz` that the mirror has already rolled
     * past (observed: `weston-15.0.0-1` 404 across every mirror right
     * after the upstream rolled 15.0.0 → 15.0.1).
     *
     * `-Syy` (double y) forces a DB refresh even if pacman thinks the
     * local copy is current. The bootstrap tarball ships with its own
     * pacman sync DB snapshot under `/var/lib/pacman/sync/`, and after
     * extract those files have mtime=now. pacman's normal `-Sy` uses
     * `If-Modified-Since: <mtime>` which the mirror answers with 304
     * because its actual `Last-Modified` is the snapshot date (older
     * than now). Result: pacman silently keeps the bootstrap's
     * possibly-stale DB and then 404s on packages the mirror has
     * since rolled past — the very thing we're trying to avoid.
     * `-Syy` short-circuits the conditional GET and downloads the
     * current DB unconditionally.
     *
     * Wiping `/var/cache/pacman/pkg/` afterwards drops every cached
     * `.pkg.tar.xz` (uninstalled and currently-installed alike). The
     * install-time cache holds the exact tarballs we just unpacked,
     * ~hundreds of MB on a fresh install. None of the typical follow-
     * up operations actually consult them: a system upgrade pulls
     * *new* versions (so the cached old versions aren't read), and
     * installing a new package fetches a fresh tarball. The only
     * operation that would hit the cache is a same-version reinstall,
     * which is rare and not worth the NAND.
     *
     * (pacman's own `-Scc --noconfirm` is intentionally a no-op for
     * safety — `--noconfirm` forces the "remove also currently-
     * installed versions?" prompt to default-no, leaving everything
     * cached. Plain `rm` is the only path that actually clears it.
     * `find -mindepth 1 -delete` rather than an `rm -rf` shell glob
     * over `pkg/`, because the wildcard expansion of hundreds of files
     * blows past ARG_MAX on shells that pre-expand.)
     */
    fun installBasePackages(
        method: InstallationMethod,
        rootfs: String,
        packages: List<String>,
        log: (String) -> Unit,
    ) {
        val res = method.runInside(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            set -e
            pacman -Syyu --needed --noconfirm ${packages.joinToString(" ")}
            # Clear the cache via find -delete (not `rm -rf .../pkg/*`)
            # because the glob expands to hundreds of package files and
            # blows past ARG_MAX on shells that pre-expand. -mindepth 1
            # keeps the dir itself for future pacman calls.
            find /var/cache/pacman/pkg -mindepth 1 -delete
            """.trimIndent(),
            onLine = { log("pacman: $it") },
        )
        if (!res.ok) {
            throw IOException("pacman -Syyu --needed install failed (exit=${res.exitCode})")
        }
    }

    /**
     * Common base package set for every Arch flavour. Kept minimal —
     * specific subsystems (debug app, integration tests) install their
     * own deps via `testing/install-test-deps.sh` etc.
     */
    val DEFAULT_BASE_PACKAGES: List<String> = listOf(
        "base-devel", "git", "wayland",
        "weston", "gtk3", "gtk3-demos",
    )
}
