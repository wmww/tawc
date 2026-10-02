package me.phie.tawc.install.distro.voidlinux

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.BootstrapFlavor
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.MirrorRegion
import me.phie.tawc.install.distro.MirrorRegions
import me.phie.tawc.install.distro.DistroBootstrap
import me.phie.tawc.install.distro.TarballBootstrap

/**
 * Void Linux (glibc) — one base class, two singleton flavours
 * ([VoidLinuxX86_64], [VoidLinuxAarch64]). Bootstrap is the dated
 * `void-<arch>-ROOTFS-YYYYMMDD.tar.xz` published under
 * `repo-default.voidlinux.org/live/current/`. There's no stable "latest"
 * symlink, so [resolveBootstrap] looks up the current entry from
 * `sha256sum.txt` at install time, after verifying that manifest's
 * minisign signature against a GitHub-published release key — see
 * [VoidSha256Resolver].
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

    final override val bootstrap: TarballBootstrap = TarballBootstrap(
        url = "https://repo-default.voidlinux.org/live/current/",
        format = BootstrapFormat.XZ,
        stripPrefix = null,
        verification = BootstrapVerification.ResolvedAtInstallTime,
    )

    final override fun resolveBootstrap(
        log: (String) -> Unit,
        mirrorProxy: MirrorProxy?,
        flavor: BootstrapFlavor,
    ): DistroBootstrap {
        require(flavor == BootstrapFlavor.TARBALL) { "void has only the tarball flavor" }
        log("void: resolving latest $linuxArch ROOTFS via sha256sum.txt")
        val r = VoidSha256Resolver.resolveLatest(linuxArch, mirrorProxy, log)
        log("void: latest=${r.filename} sha256=${r.sha256Hex}")
        return TarballBootstrap(
            url = r.downloadUrl,
            format = BootstrapFormat.XZ,
            stripPrefix = null,
            verification = BootstrapVerification.Sha256(r.sha256Hex),
        )
    }

    final override val basePackages: List<String> = VoidCommon.DEFAULT_BASE_PACKAGES

    override val mirrorRegions: List<MirrorRegion> = MirrorRegions.voidLinux

    /** xbps repository URLs for [region]; `null` = the built-in Fastly CDN. */
    internal fun mirrorConfig(region: MirrorRegion?): List<String> {
        val bases = region?.servers ?: listOf(VoidCommon.DEFAULT_MIRROR_BASE)
        return bases.map { VoidCommon.repositoryUrl(it, linuxArch) }
    }

    final override fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
    ) = VoidCommon.configure(
        method,
        rootfs,
        linuxArch,
        mirrorProxy,
        log,
        mirrorBase = VoidCommon.DEFAULT_MIRROR_BASE,
    )

    final override fun configureMirrors(
        method: InstallationMethod,
        rootfs: String,
        mirrorRegion: String?,
        log: (String) -> Unit,
    ) = VoidCommon.configureMirrors(
        method,
        rootfs,
        mirrorConfig(resolveMirrorRegion(mirrorRegion)),
        log,
    )

    final override fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        VoidCommon.initPackageManager(method, rootfs, log)

    final override fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        VoidCommon.installBasePackages(method, rootfs, basePackages, log)
}

internal object VoidLinuxX86_64 : VoidLinux(
    displayName = "Void Linux",
    linuxArch = "x86_64",
    androidAbi = "x86_64",
)

internal object VoidLinuxAarch64 : VoidLinux(
    displayName = "Void Linux",
    linuxArch = "aarch64",
    androidAbi = "arm64-v8a",
)
