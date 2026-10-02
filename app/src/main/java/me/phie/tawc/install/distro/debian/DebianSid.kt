package me.phie.tawc.install.distro.debian

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.BootstrapFlavor
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap
import me.phie.tawc.install.distro.MirrorRegion
import me.phie.tawc.install.distro.MirrorRegions
import me.phie.tawc.install.distro.PackageBootstrap
import me.phie.tawc.install.distro.TarballBootstrap
import me.phie.tawc.install.distro.apt.AptCommon

internal sealed class DebianSid(
    override val linuxArch: String,
    override val androidAbi: String,
    private val bashbrewArch: String,
    /** dpkg architecture (`arm64` / `amd64`) — names the archive's
     *  `binary-<arch>` index for the packages flavor. */
    private val dpkgArch: String,
) : Distro {
    final override val key: String = Installation.DISTRO_DEBIAN_SID
    final override val displayName: String = "Debian Sid"
    final override val supported: Boolean = true
    final override val defaultLabel: String = "Sid"
    final override val cacheKey: String = "$key-$linuxArch"

    final override val bootstrap: TarballBootstrap = TarballBootstrap(
        url = "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/dist-$bashbrewArch/sid/oci/blobs/rootfs.tar.gz",
        format = BootstrapFormat.GZIP,
        stripPrefix = null,
        verification = BootstrapVerification.ResolvedAtInstallTime,
    )

    /** Real debootstrap on-device against the signed sid archive,
     *  trust-rooted in the shipped
     *  `res/raw/debian_archive_keyring.asc`. Debug builds only — see
     *  [me.phie.tawc.install.EnabledBootstrapFlavors]. */
    private val packageBootstrap = PackageBootstrap(
        archiveRoot = REPO_URL,
        suite = SUITE,
        packagesArch = dpkgArch,
        keyResource = "debian_archive_keyring",
    )

    /**
     * Two flavors: the debuerreotype tarball (supported, unchanged,
     * the default everywhere) and `packages`, which stays debug-only
     * until it has earned supported status — release builds filter it
     * out of `bootstrapFlavors` entirely.
     */
    final override val declaredBootstrapFlavors: Map<BootstrapFlavor, DistroBootstrap> = mapOf(
        BootstrapFlavor.TARBALL to bootstrap,
        BootstrapFlavor.PACKAGES to packageBootstrap,
    )

    final override fun resolveBootstrap(
        log: (String) -> Unit,
        mirrorProxy: MirrorProxy?,
        flavor: BootstrapFlavor,
    ): DistroBootstrap = when (flavor) {
        BootstrapFlavor.TARBALL -> {
            log("debian sid: resolving latest $linuxArch rootfs via OCI manifest")
            val b = DebianDockerResolver.resolve(SUITE, bashbrewArch, mirrorProxy)
            val v = b.verification as BootstrapVerification.Sha256
            log("debian sid: rootfs=${b.url} sha256=${v.expectedHex}")
            b
        }
        // Static descriptor is complete: all live data (InRelease,
        // index) is fetched and verified inside the packages installer
        // itself, behind the shipped-keyring trust boundary.
        BootstrapFlavor.PACKAGES -> packageBootstrap
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

    override val mirrorRegions: List<MirrorRegion> = MirrorRegions.debian

    /** Archive root for [region]; `null` = the geo-routed CDN. */
    internal fun mirrorConfig(region: MirrorRegion?): String =
        region?.servers?.first() ?: REPO_URL

    final override fun configureMirrors(
        method: InstallationMethod,
        rootfs: String,
        mirrorRegion: String?,
        log: (String) -> Unit,
    ) = AptCommon.configureMirrors(
        method = method,
        rootfs = rootfs,
        suite = SUITE,
        repoUrl = mirrorConfig(resolveMirrorRegion(mirrorRegion)),
        signedBy = DEBIAN_ARCHIVE_KEYRING,
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
    dpkgArch = "amd64",
)

internal object DebianSidAarch64 : DebianSid(
    linuxArch = "aarch64",
    androidAbi = "arm64-v8a",
    bashbrewArch = "arm64v8",
    dpkgArch = "arm64",
)
