package me.phie.tawc.install.distro.arch

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.Installation
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap

/**
 * Arch Linux ARM (ALARM) for aarch64. The bootstrap tarball is gzip
 * (no zstd transient needed) and unwrapped (no `stripPrefix`).
 *
 * `os.archlinuxarm.org` doesn't redirect to HTTPS for `/os/`, so the
 * URL is plain HTTP. `res/xml/network_security_config.xml` carves out
 * cleartext for that host only.
 */
internal object ArchLinuxArm : Distro {
    override val key: String = Installation.DISTRO_ARCH
    override val displayName: String = "Arch Linux ARM"
    override val linuxArch: String = "aarch64"
    override val androidAbi: String = "arm64-v8a"

    override val bootstrap: DistroBootstrap = DistroBootstrap(
        url = "http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz",
        format = BootstrapFormat.GZIP,
        stripPrefix = null,
    )

    override val basePackages: List<String> = ArchPacmanCommon.DEFAULT_BASE_PACKAGES

    /**
     * ALARM ships a single-Server mirrorlist
     * (`http://mirror.archlinuxarm.org/$arch/$repo`, the geo-IP
     * redirector). With `ParallelDownloads` enabled it's possible (and
     * observed) for one parallel request to hit a regional mirror
     * mid-sync and 404 on a single `*.pkg.tar.xz`. With only one
     * Server entry pacman has no fallback and the whole transaction
     * aborts. Listing several specific mirrors (in addition to the
     * redirector) lets pacman skip past a stale mirror to the next on
     * 404. The redirector goes first so the common case still uses
     * the closest mirror.
     */
    private val MIRROR_LIST: String = listOf(
        "Server = http://mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://nj.us.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://fl.us.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://ca.us.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://de.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://fr.mirror.archlinuxarm.org/\$arch/\$repo",
    ).joinToString("\n")

    /** ALARM kernel package is `linux-aarch64`; firmware split as on x86. */
    private val IGNORED_PACKAGES = listOf(
        "linux-aarch64", "linux-firmware", "linux-firmware-*",
    )

    override fun configure(rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.configure(rootfs, MIRROR_LIST, IGNORED_PACKAGES, log)

    override fun initPackageManager(rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.initPackageManager(rootfs, keyring = "archlinuxarm", log = log)

    override fun installBasePackages(rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.installBasePackages(rootfs, basePackages, log)
}
