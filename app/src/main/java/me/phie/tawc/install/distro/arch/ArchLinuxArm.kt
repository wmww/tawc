package me.phie.tawc.install.distro.arch

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.MirrorRegion
import me.phie.tawc.install.distro.MirrorRegions
import me.phie.tawc.install.distro.TarballBootstrap

/**
 * Arch Linux ARM (ALARM) for aarch64. The bootstrap tarball is gzip
 * (no zstd transient needed) and unwrapped (no `stripPrefix`).
 *
 * Integrity story: upstream publishes a detached OpenPGP signature at
 * `<tarball>.sig`, signed by the ALARM build system key
 * (`68B3537F39A313B3E574D06777193F152BDBE6A6`, rsa4096) — the same key
 * `pacman-key --populate archlinuxarm` trusts for packages once the
 * rootfs is up. We ship it at `res/raw/archlinuxarm_signing_key.asc`,
 * so the bootstrap's trust root is a key in the APK rather than mirror
 * infrastructure. Key rotation upstream breaks installs until we ship
 * a new APK; that is the intended failure mode (see
 * notes/installation.md "Bootstrap integrity").
 */
internal object ArchLinuxArm : Distro {
    override val key: String = Installation.DISTRO_ARCH
    override val displayName: String = "Arch Linux ARM"
    override val supported: Boolean = true
    override val defaultLabel: String = "Arch"
    override val linuxArch: String = "aarch64"
    override val androidAbi: String = "arm64-v8a"

    private const val PRIMARY_MIRROR = "fl.us.mirror.archlinuxarm.org"

    // Fetch goes over HTTPS to fl.us — the geo-redirector at
    // os.archlinuxarm.org would 301 to plain HTTP, and most regional
    // mirrors only have a cert for archlinuxarm.org and fail TLS
    // hostname validation. fl.us serves a proper cert covering its own
    // hostname. TLS is belt-and-braces here: the PGP signature below is
    // what actually decides whether the bytes are trusted.
    private const val BOOTSTRAP_URL =
        "https://$PRIMARY_MIRROR/os/ArchLinuxARM-aarch64-latest.tar.gz"

    override val bootstrap: TarballBootstrap = TarballBootstrap(
        url = BOOTSTRAP_URL,
        format = BootstrapFormat.GZIP,
        stripPrefix = null,
        // Detached PGP signature by the Arch Linux ARM Build System key
        // (68B3 537F 39A3 13B3 E574  D067 7719 3F15 2BDB E6A6), the
        // same key archlinuxarm-keyring pins as trusted. Public key
        // shipped at res/raw/archlinuxarm_signing_key.asc.
        verification = BootstrapVerification.Pgp(
            signatureUrl = "$BOOTSTRAP_URL.sig",
            keyResource = "archlinuxarm_signing_key",
        ),
    )

    override val basePackages: List<String> = ArchPacmanCommon.DEFAULT_BASE_PACKAGES

    /**
     * Default mirror bases in priority order — what a user who never
     * picks a region in settings gets.
     *
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
     *
     * HTTPS-first: fl.us and ca.us are the two ALARM mirrors with
     * certs covering their own hostnames (the geo-redirector
     * mirror.archlinuxarm.org and most regional ones only have a
     * cert for archlinuxarm.org and fail TLS hostname validation).
     * Pacman package signatures are verified anyway via
     * archlinuxarm-keyring (SigLevel=Required-DatabaseOptional),
     * so the HTTP fallbacks are belt-and-braces, not a security
     * hole — but TLS first reduces the attack surface.
     */
    private val DEFAULT_MIRROR_BASES: List<String> = listOf(
        "https://fl.us.mirror.archlinuxarm.org",
        "https://ca.us.mirror.archlinuxarm.org",
        "http://mirror.archlinuxarm.org",
        "http://nj.us.mirror.archlinuxarm.org",
        "http://de.mirror.archlinuxarm.org",
        "http://fr.mirror.archlinuxarm.org",
    )

    /** ALARM's repo path after a mirror base. */
    private const val MIRROR_SUFFIX = "/\$arch/\$repo"

    /**
     * ALARM kernel package is `linux-aarch64`; firmware split as on
     * x86. `IgnorePkg` is defence in depth — these are removed via
     * `pacman -Rdd` after the bootstrap extract (see
     * [ArchPacmanCommon.initPackageManager]); the IgnorePkg line
     * keeps a future `pacman -Syu` from pulling them back if some
     * package marks them as an optional dep.
     */
    private val IGNORED_PACKAGES = listOf(
        "linux-aarch64", "linux-firmware", "linux-firmware-*",
    )

    /** See `ArchPacmanCommon.initPackageManager` — kernel package name. */
    private val ARCH_SPECIFIC_CRUFT = listOf("linux-aarch64")

    override val mirrorRegions: List<MirrorRegion> = MirrorRegions.archLinuxArm

    /** pacman mirrorlist contents for [region]; `null` = the default list. */
    internal fun mirrorConfig(region: MirrorRegion?): String =
        ArchPacmanCommon.serverLines(region?.servers ?: DEFAULT_MIRROR_BASES, MIRROR_SUFFIX)

    override fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
    ) = ArchPacmanCommon.configure(method, rootfs, mirrorConfig(null), IGNORED_PACKAGES, mirrorProxy, log)

    override fun configureMirrors(
        method: InstallationMethod,
        rootfs: String,
        mirrorRegion: String?,
        log: (String) -> Unit,
    ) = ArchPacmanCommon.configureMirrors(
        method,
        rootfs,
        mirrorConfig(resolveMirrorRegion(mirrorRegion)),
        log,
    )

    override fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.initPackageManager(
            method,
            rootfs,
            keyring = "archlinuxarm",
            archSpecificCruft = ARCH_SPECIFIC_CRUFT,
            log = log,
        )

    override fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.installBasePackages(method, rootfs, basePackages, log)
}
