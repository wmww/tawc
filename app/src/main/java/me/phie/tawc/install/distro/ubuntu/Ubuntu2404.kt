package me.phie.tawc.install.distro.ubuntu

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.BootstrapFlavor
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroBootstrap
import me.phie.tawc.install.distro.TarballBootstrap
import me.phie.tawc.install.distro.apt.AptCommon

/**
 * Ubuntu 24.04 LTS (noble) — one base class, two singleton flavours
 * ([Ubuntu2404X86_64], [Ubuntu2404Aarch64]).
 *
 * Bootstrap is the `ubuntu-base` tarball published under
 * `cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/`. Ubuntu
 * publishes one `SHA256SUMS` per release directory covering every point
 * release and architecture, so [resolveBootstrap] fetches that manifest,
 * verifies its detached PGP signature ([UbuntuSha256Resolver] +
 * [UbuntuReleaseKeys]) and takes the newest entry for this
 * architecture — the same shape as Void, with PGP instead of minisign.
 *
 * Two things differ from the Debian flavours and are worth knowing
 * before editing this file:
 *
 *  - **arm64 lives on `ports`, not `archive`.** `archive.ubuntu.com` has
 *    no arm64 tree; the ports archive is `ports.ubuntu.com/ubuntu-ports`.
 *    The two flavours therefore carry different repository roots, and
 *    the two are not interchangeable.
 *  - **The base image ships its own `ubuntu.sources`** pointing at
 *    `archive.ubuntu.com`. `AptCommon.configure` deletes it along with
 *    `sources.list`; leaving it would let apt fetch from a source TAWC
 *    did not configure (and, on arm64, from a host with no arm64 tree).
 *
 * `supported = true` since 2026-10-01: an install was verified end to end
 * on the physical tablet (install → `apt-get update` → base packages →
 * GUI apps through the in-app launcher), so it sits in the
 * install form's main list rather than under "Other distros"
 * (see notes/installation.md "Distro abstraction").
 */
internal sealed class Ubuntu2404(
    override val displayName: String,
    override val linuxArch: String,
    override val androidAbi: String,
    /** Ubuntu architecture name in the tarball filename (`amd64`/`arm64`). */
    private val ubuntuArch: String,
    /** Archive root; `ports.ubuntu.com/ubuntu-ports` on arm64. */
    private val repoUrl: String,
) : Distro {
    final override val key: String = Installation.DISTRO_UBUNTU
    final override val defaultLabel: String = "Noble"
    final override val cacheKey: String = "$key-$linuxArch"
    final override val supported: Boolean = true

    final override val bootstrap: TarballBootstrap = TarballBootstrap(
        url = "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/",
        format = BootstrapFormat.GZIP,
        // The tarball is rooted at `./` (bin/, etc/, …), verified against
        // ubuntu-base-24.04.5-base-arm64.tar.gz.
        stripPrefix = null,
        verification = BootstrapVerification.ResolvedAtInstallTime,
    )

    final override fun resolveBootstrap(
        log: (String) -> Unit,
        mirrorProxy: MirrorProxy?,
        flavor: BootstrapFlavor,
    ): DistroBootstrap {
        require(flavor == BootstrapFlavor.TARBALL) { "ubuntu has only the tarball flavor" }
        log("ubuntu: resolving latest $ubuntuArch base tarball via SHA256SUMS")
        val r = UbuntuSha256Resolver.resolveLatest(ubuntuArch, mirrorProxy, log)
        log("ubuntu: latest=${r.filename} sha256=${r.sha256Hex}")
        return TarballBootstrap(
            url = r.downloadUrl,
            format = BootstrapFormat.GZIP,
            stripPrefix = null,
            verification = BootstrapVerification.Sha256(r.sha256Hex),
        )
    }

    /**
     * The shared apt-family set. Every entry resolves in noble, but
     * `systemd-standalone-sysusers` / `systemd-standalone-tmpfiles` are
     * **universe**-only, which is why [COMPONENTS] is not just `main` —
     * checked against `dists/noble/{main,universe}/binary-arm64/Packages.gz`.
     */
    final override val basePackages: List<String> = AptCommon.DEFAULT_BASE_PACKAGES

    /**
     * Packages apt must refuse to install here (see
     * [AptCommon.blockedPackagesBody]).
     *
     * `snapd` is the one: its postinst runs `setcap` on `snap-confine`,
     * which needs `CAP_SETFCAP` — unavailable under tawcroot (fake root,
     * `CapEff: 0`). The result is not a self-contained failure: dpkg
     * leaves `snapd` half-configured and **every later apt transaction
     * exits non-zero**, so a user who follows Ubuntu's own "install the
     * Firefox snap" advice ends up with a broken package manager. Snaps
     * cannot work in this rootfs anyway (they need systemd, loop mounts
     * and apparmor — see notes/distro-options.md), so refusing up front
     * is the honest behaviour.
     *
     * The transitional `firefox` .deb uses `PreDepends: snapd (>= 2.54)`,
     * so with the pin in place apt refuses it up front
     * (`Package 'snapd' has no installation candidate` / "held broken
     * packages") instead of installing a stub. That is the honest
     * outcome: the stub gives no browser anyway (use the Mozilla PPA or
     * the upstream tarball).
     */
    internal open val blockedPackages: List<String> = listOf("snapd")

    final override fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
    ) = AptCommon.configure(
        method = method,
        rootfs = rootfs,
        suites = SUITES,
        repoUrl = repoUrl,
        signedBy = ARCHIVE_KEYRING,
        mirrorProxy = mirrorProxy,
        log = log,
        components = COMPONENTS,
        blockedPackages = blockedPackages,
    )

    final override fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        AptCommon.initPackageManager(method, rootfs, log)

    final override fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit) =
        AptCommon.installBasePackages(method, rootfs, basePackages, log)

    companion object {
        /**
         * Ubuntu's suite names: the release, its updates, and its
         * security pocket. Backports are deliberately excluded — TAWC
         * wants a stable userland, and the Debian flavour is equally
         * minimal.
         */
        private val SUITES = listOf("noble", "noble-updates", "noble-security")

        /** `universe` for the `systemd-standalone-*` pair; see [basePackages]. */
        private const val COMPONENTS = "main universe"

        /**
         * Keyring the ubuntu-base tarball ships (from `ubuntu-keyring`)
         * and that Ubuntu's own `ubuntu.sources` names. apt verifies
         * package signatures against it, so a different apt source
         * cannot weaken package integrity.
         */
        private const val ARCHIVE_KEYRING = "/usr/share/keyrings/ubuntu-archive-keyring.gpg"
    }
}

internal object Ubuntu2404X86_64 : Ubuntu2404(
    displayName = "Ubuntu 24.04 (x86_64)",
    linuxArch = "x86_64",
    androidAbi = "x86_64",
    ubuntuArch = "amd64",
    repoUrl = "http://archive.ubuntu.com/ubuntu",
)

internal object Ubuntu2404Aarch64 : Ubuntu2404(
    displayName = "Ubuntu 24.04",
    linuxArch = "aarch64",
    androidAbi = "arm64-v8a",
    ubuntuArch = "arm64",
    // arm64 is a ports architecture: it has no tree on archive.ubuntu.com.
    repoUrl = "http://ports.ubuntu.com/ubuntu-ports",
)
