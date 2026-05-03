package me.phie.tawc.install.distro.arch

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap

/**
 * Arch Linux ARM (ALARM) for aarch64. The bootstrap tarball is gzip
 * (no zstd transient needed) and unwrapped (no `stripPrefix`).
 *
 * Integrity story: ALARM doesn't sign the bootstrap (only an `.md5`
 * sidecar), so we can't do PGP. Instead we fetch the tarball over
 * HTTPS from a regional mirror with a valid TLS cert, then
 * cross-check the `.md5` against a second independent HTTPS mirror —
 * any single compromise (server, CDN, Let's Encrypt issuance for one
 * subdomain) needs to coincide with the same forgery on the other
 * subdomain to lie. This is weaker than PGP but materially better
 * than the previous plaintext-HTTP-no-check setup.
 *
 * Tracked in `issues/install-alarm-bootstrap-no-pgp.md`. Upgrade to
 * PGP when ALARM upstream starts signing.
 */
internal object ArchLinuxArm : Distro {
    override val key: String = Installation.DISTRO_ARCH
    override val displayName: String = "Arch Linux ARM"
    override val defaultLabel: String = "Arch"
    override val linuxArch: String = "aarch64"
    override val androidAbi: String = "arm64-v8a"

    private const val PRIMARY_MIRROR = "fl.us.mirror.archlinuxarm.org"
    private const val SECONDARY_MIRROR = "ca.us.mirror.archlinuxarm.org"

    override val bootstrap: DistroBootstrap = DistroBootstrap(
        // Primary fetch goes over HTTPS to fl.us — the geo-redirector
        // at os.archlinuxarm.org would 301 to plain HTTP, and most
        // regional mirrors only have a cert for archlinuxarm.org and
        // fail TLS hostname validation. fl.us and ca.us both serve a
        // proper cert covering their own hostname.
        url = "https://$PRIMARY_MIRROR/os/ArchLinuxARM-aarch64-latest.tar.gz",
        format = BootstrapFormat.GZIP,
        stripPrefix = null,
        // Cross-mirror MD5: each .md5 is fetched over its own HTTPS
        // session against an independently-operated mirror, then we
        // require byte-for-byte agreement before trusting either.
        verification = BootstrapVerification.CrossMirrorMd5(
            checksumUrls = listOf(
                "https://$PRIMARY_MIRROR/os/ArchLinuxARM-aarch64-latest.tar.gz.md5",
                "https://$SECONDARY_MIRROR/os/ArchLinuxARM-aarch64-latest.tar.gz.md5",
            ),
        ),
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
        // HTTPS-first: fl.us and ca.us are the two ALARM mirrors with
        // certs covering their own hostnames (the geo-redirector
        // mirror.archlinuxarm.org and most regional ones only have a
        // cert for archlinuxarm.org and fail TLS hostname validation).
        // Pacman package signatures are verified anyway via
        // archlinuxarm-keyring (SigLevel=Required-DatabaseOptional),
        // so the HTTP fallbacks are belt-and-braces, not a security
        // hole — but TLS first reduces the attack surface.
        "Server = https://fl.us.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = https://ca.us.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://nj.us.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://de.mirror.archlinuxarm.org/\$arch/\$repo",
        "Server = http://fr.mirror.archlinuxarm.org/\$arch/\$repo",
    ).joinToString("\n")

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

    override fun configure(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        ArchPacmanCommon.configure(method, rootfs, MIRROR_LIST, IGNORED_PACKAGES, log)

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
