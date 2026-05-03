package me.phie.tawc.install.distro.arch

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap

/**
 * Arch Linux x86_64. Bootstrap is the geo-routed `pkgbuild.com` mirror
 * (zstd-compressed, wrapped in a `root.x86_64/` toplevel that needs
 * flattening). Mirrorlist points at the same geo-redirector since it's
 * the official recommended one and works well in practice.
 */
internal object ArchLinuxX86_64 : Distro {
    override val key: String = Installation.DISTRO_ARCH
    // Arch Linux ARM and Arch Linux x86 share the upstream "Arch Linux"
    // brand but have different bootstrap sources, kernel package names,
    // and (in this app) different Android ABIs. The display name has to
    // disambiguate at a glance — ALARM uses the official "ARM" suffix,
    // and we mirror that with a "(x86)" suffix here.
    override val displayName: String = "Arch Linux (x86)"
    override val defaultLabel: String = "Arch"
    override val linuxArch: String = "x86_64"
    override val androidAbi: String = "x86_64"

    private const val BOOTSTRAP_URL =
        "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"

    override val bootstrap: DistroBootstrap = DistroBootstrap(
        url = BOOTSTRAP_URL,
        format = BootstrapFormat.ZSTD,
        stripPrefix = "root.x86_64",
        // Detached PGP signature signed by Pierre Schmitz
        // (3E80 CA1A 8B89 F69C BA57  D98A 76A5 EF90 5444 9A5C),
        // listed at https://archlinux.org/people/developers/.
        // Public key shipped at res/raw/arch_signing_key.asc.
        verification = BootstrapVerification.Pgp(
            signatureUrl = "$BOOTSTRAP_URL.sig",
            keyResource = "arch_signing_key",
        ),
    )

    override val basePackages: List<String> = ArchPacmanCommon.DEFAULT_BASE_PACKAGES

    private const val MIRROR_LIST =
        "Server = https://geo.mirror.pkgbuild.com/\$repo/os/\$arch"

    /**
     * x86_64 Arch's stock kernel package is `linux`; firmware is in
     * `linux-firmware` and a few `linux-firmware-*` split packages.
     * Listing them on `IgnorePkg` is defence in depth — they're
     * already removed via `pacman -Rdd` after the bootstrap extract
     * (see [ArchPacmanCommon.initPackageManager]), but `IgnorePkg`
     * keeps a future `pacman -Syu` from picking them back up if some
     * package marks them as an optional dep.
     */
    private val IGNORED_PACKAGES = listOf(
        "linux", "linux-firmware", "linux-firmware-*",
    )

    /** See `ArchPacmanCommon.initPackageManager` — kernel package name. */
    private val ARCH_SPECIFIC_CRUFT = listOf("linux")

    override fun configure(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.configure(method, rootfs, MIRROR_LIST, IGNORED_PACKAGES, log)

    override fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.initPackageManager(
            method,
            rootfs,
            keyring = "archlinux",
            archSpecificCruft = ARCH_SPECIFIC_CRUFT,
            log = log,
        )

    override fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.installBasePackages(method, rootfs, basePackages, log)
}
