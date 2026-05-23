package me.phie.tawc.install.distro.debian

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap
import me.phie.tawc.install.distro.apt.AptCommon

internal sealed class DebianSid(
    override val linuxArch: String,
    override val androidAbi: String,
    private val bashbrewArch: String,
) : Distro {
    final override val key: String = Installation.DISTRO_DEBIAN_SID
    final override val displayName: String = "Debian Sid"
    final override val defaultLabel: String = "Sid"
    final override val cacheKey: String = "$key-$linuxArch"

    final override val bootstrap: DistroBootstrap = DistroBootstrap(
        url = "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/dist-$bashbrewArch/sid/oci/blobs/rootfs.tar.gz",
        format = BootstrapFormat.GZIP,
        stripPrefix = null,
        verification = BootstrapVerification.None,
    )

    final override fun resolveBootstrap(log: (String) -> Unit, mirrorProxy: MirrorProxy?): DistroBootstrap {
        log("debian sid: resolving latest $linuxArch rootfs via OCI manifest")
        val b = DebianDockerResolver.resolve(SUITE, bashbrewArch, mirrorProxy)
        val v = b.verification as BootstrapVerification.Sha256
        log("debian sid: rootfs=${b.url} sha256=${v.expectedHex}")
        return b
    }

    final override val basePackages: List<String> = AptCommon.DEFAULT_BASE_PACKAGES

    final override fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
    ) = AptCommon.configure(
        method = method,
        rootfs = rootfs,
        suite = SUITE,
        repoUrl = REPO_URL,
        signedBy = DEBIAN_ARCHIVE_KEYRING,
        mirrorProxy = mirrorProxy,
        log = log,
    )

    final override fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        AptCommon.initPackageManager(method, rootfs, log)

    final override fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        AptCommon.installBasePackages(method, rootfs, basePackages, log)

    companion object {
        private const val SUITE = "sid"
        private const val REPO_URL = "http://deb.debian.org/debian"
        private const val DEBIAN_ARCHIVE_KEYRING = "/usr/share/keyrings/debian-archive-keyring.pgp"
    }
}

internal object DebianSidX86_64 : DebianSid(
    linuxArch = "x86_64",
    androidAbi = "x86_64",
    bashbrewArch = "amd64",
)

internal object DebianSidAarch64 : DebianSid(
    linuxArch = "aarch64",
    androidAbi = "arm64-v8a",
    bashbrewArch = "arm64v8",
)
