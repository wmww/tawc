package me.phie.tawc.install.distro.voidlinux

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap

/**
 * Void Linux (glibc) — one base class, two singleton flavours
 * ([VoidLinuxX86_64], [VoidLinuxAarch64]). Bootstrap is the dated
 * `void-<arch>-ROOTFS-YYYYMMDD.tar.xz` published under
 * `repo-default.voidlinux.org/live/current/`. There's no stable "latest"
 * symlink, so [resolveBootstrap] looks up the current entry from
 * `sha256sum.txt` at install time — see [VoidSha256Resolver].
 *
 * Unlike the Arch flavours (which diverge in mirrorlists, IgnorePkg sets,
 * and kernel-package cruft), the two Void flavours differ only in
 * `linuxArch`/`androidAbi`/`displayName`, so the body lives here.
 */
internal sealed class VoidLinux(
    override val displayName: String,
    override val linuxArch: String,
    override val androidAbi: String,
) : Distro {
    final override val key: String = Installation.DISTRO_VOID
    final override val defaultLabel: String = "Void"

    final override val bootstrap: DistroBootstrap = DistroBootstrap(
        url = "https://repo-default.voidlinux.org/live/current/",
        format = BootstrapFormat.XZ,
        stripPrefix = null,
        verification = BootstrapVerification.None,
    )

    final override fun resolveBootstrap(log: (String) -> Unit): DistroBootstrap {
        log("void: resolving latest $linuxArch ROOTFS via sha256sum.txt")
        val r = VoidSha256Resolver.resolveLatest(linuxArch)
        log("void: latest=${r.filename} sha256=${r.sha256Hex}")
        return DistroBootstrap(
            url = r.downloadUrl,
            format = BootstrapFormat.XZ,
            stripPrefix = null,
            verification = BootstrapVerification.Sha256(r.sha256Hex),
        )
    }

    final override val basePackages: List<String> = VoidCommon.DEFAULT_BASE_PACKAGES

    final override fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
    ) = VoidCommon.configure(method, rootfs, linuxArch, mirrorProxy, log)

    final override fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        VoidCommon.initPackageManager(method, rootfs, log)

    final override fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        VoidCommon.installBasePackages(method, rootfs, basePackages, log)
}

internal object VoidLinuxX86_64 : VoidLinux(
    displayName = "Void Linux (x86)",
    linuxArch = "x86_64",
    androidAbi = "x86_64",
)

internal object VoidLinuxAarch64 : VoidLinux(
    displayName = "Void Linux ARM",
    linuxArch = "aarch64",
    androidAbi = "arm64-v8a",
)
