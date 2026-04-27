package me.phie.tawc.install.distro.arch

import me.phie.tawc.install.ChrootRunner
import me.phie.tawc.install.Su
import java.io.IOException

/**
 * Shared helpers for the two Arch flavours
 * ([ArchLinuxX86_64], [ArchLinuxArm]). Both use pacman; the only real
 * differences between them are the bootstrap URL/format, the keyring
 * name (`archlinux` vs `archlinuxarm`), the mirrorlist contents, and
 * the kernel package name to put on `IgnorePkg`.
 */
internal object ArchPacmanCommon {

    /**
     * Write `/etc/resolv.conf`, the pacman.conf tweaks, the mirrorlist
     * and `profile.d/00-path.sh`. The Wayland env in
     * `profile.d/01-tawc.sh` is *not* written here — `ChrootMounter`
     * regenerates it on every chroot entry so env changes don't need a
     * reinstall (see notes/installation.md).
     *
     * @param rootfs absolute path to the chroot rootfs.
     * @param mirrorListBody contents of `/etc/pacman.d/mirrorlist`
     *   (one or more `Server = ...` lines).
     * @param ignoredPackages packages to put on the `IgnorePkg` line —
     *   the kernel package name varies across arches (`linux` for
     *   x86_64, `linux-aarch64` for ALARM) and `IgnorePkg` only
     *   accepts package names that exist in the chroot's repos.
     * @param log per-line log sink.
     */
    fun configure(
        rootfs: String,
        mirrorListBody: String,
        ignoredPackages: List<String>,
        log: (String) -> Unit,
    ) {
        val ignoreLine = "IgnorePkg = " + ignoredPackages.joinToString(" ")

        val script = buildString {
            appendLine("set -eu")
            appendLine("ROOTFS='$rootfs'")
            appendLine(
                """
                # DNS
                rm -f "${'$'}ROOTFS/etc/resolv.conf"
                echo nameserver 8.8.8.8 > "${'$'}ROOTFS/etc/resolv.conf"

                # pacman.conf: SigLevel=Never (we don't ship the keyring's
                # web-of-trust into the chroot), DisableSandbox (Magisk's
                # sandbox propagation breaks pacman's sandbox helper),
                # comment out CheckSpace (statvfs returns 0 inside the
                # chroot's bind mounts and pacman aborts), and IgnorePkg
                # for the kernel/firmware packages that would try to
                # install boot artefacts into the rootfs and fail.
                sed -i 's/^SigLevel.*/SigLevel = Never/' "${'$'}ROOTFS/etc/pacman.conf"
                grep -q '^DisableSandbox' "${'$'}ROOTFS/etc/pacman.conf" || \
                    sed -i '/^SigLevel/a DisableSandbox' "${'$'}ROOTFS/etc/pacman.conf"
                sed -i 's/^CheckSpace/#CheckSpace/' "${'$'}ROOTFS/etc/pacman.conf"
                grep -q '^IgnorePkg' "${'$'}ROOTFS/etc/pacman.conf" || \
                    sed -i "/#CheckSpace/a $ignoreLine" \
                        "${'$'}ROOTFS/etc/pacman.conf"
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
                echo OK
                """.trimIndent()
            )
        }
        val r = Su.run(script) { log("conf: $it") }
        if (!r.ok) {
            throw IOException("Configure failed:\n${r.output}")
        }
    }

    /**
     * `pacman-key --init && pacman-key --populate <keyring> && pacman
     * -Syu`. Although we set `SigLevel=Never` so the keyring isn't
     * strictly required, populating matches what the legacy create
     * scripts did and keeps `pacman -Syu` quiet.
     */
    fun initPackageManager(
        rootfs: String,
        keyring: String,
        log: (String) -> Unit,
    ) {
        val res = ChrootRunner.run(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            pacman-key --init
            pacman-key --populate $keyring 2>/dev/null || true
            pacman -Syu --noconfirm
            """.trimIndent(),
            onLine = { log("pacman-key: $it") },
        )
        if (!res.ok) {
            throw IOException("pacman-key init / -Syu failed (exit=${res.exitCode})")
        }
    }

    /** `pacman -S --noconfirm --needed <packages>`. */
    fun installBasePackages(
        rootfs: String,
        packages: List<String>,
        log: (String) -> Unit,
    ) {
        val res = ChrootRunner.run(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            pacman -S --noconfirm --needed ${packages.joinToString(" ")}
            """.trimIndent(),
            onLine = { log("pacman: $it") },
        )
        if (!res.ok) {
            throw IOException("pacman --needed install failed (exit=${res.exitCode})")
        }
    }

    /**
     * Common base package set for every Arch flavour. Kept minimal —
     * specific subsystems (debug app, libhybris build) install their
     * own deps via `testing/install-test-deps.sh` etc.
     */
    val DEFAULT_BASE_PACKAGES: List<String> = listOf(
        "base-devel", "git", "libtool", "wayland", "wayland-protocols",
        "pkg-config", "autoconf", "automake", "patchelf",
        "weston", "gtk3", "gtk3-demos",
    )
}
