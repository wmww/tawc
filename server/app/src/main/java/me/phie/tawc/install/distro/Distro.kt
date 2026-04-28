package me.phie.tawc.install.distro

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.InstallationMethod

/**
 * Per-distro policy: bootstrap tarball, `/etc` configuration,
 * package-manager init, base package install. The generic
 * `Installer` orchestrates these in a fixed order
 * (`download → extract → configure → init pkgmgr → install pkgs`)
 * and one `Distro` per (distro family × Linux arch) plugs in here.
 *
 * Today's set: [me.phie.tawc.install.distro.arch.ArchLinuxX86_64],
 * [me.phie.tawc.install.distro.arch.ArchLinuxArm]. Adding e.g. Ubuntu
 * is a fresh file in `distro/ubuntu/`; nothing in `Installer` /
 * `InstallationService` cares.
 */
interface Distro {
    /**
     * Stable identifier written to `metadata.json`. Used together with
     * [androidAbi] by [DistroRegistry] to resolve a record back to the
     * implementation that produced it. Existing on-disk records use
     * `"arch"` for both Arch Linux and Arch Linux ARM, so both
     * implementations share that value and are disambiguated by arch.
     */
    val key: String

    /** Human-readable name for UI titles, e.g. `"Arch Linux ARM"`. */
    val displayName: String

    /**
     * Linux `uname -m` name (`"x86_64"`, `"aarch64"`). Used for
     * tarball URLs, the [BootstrapCache] filename, and the UI
     * "Architecture:" row. Distinct from [androidAbi] because pacman
     * et al. do not speak Android ABI names.
     */
    val linuxArch: String

    /**
     * `Build.SUPPORTED_ABIS` value matching this distro
     * (`"x86_64"`, `"arm64-v8a"`). Used for host detection
     * ([DistroRegistry.defaultForHost]) and stored in
     * `Installation.arch` for back-compat with the metadata schema
     * that predates this abstraction.
     */
    val androidAbi: String

    /** Bootstrap tarball metadata. */
    val bootstrap: DistroBootstrap

    /** Base packages to `pacman -S --needed` (or equivalent) at install time. */
    val basePackages: List<String>

    /**
     * Write `/etc` configuration into the freshly-extracted [rootfs]:
     * DNS, package-manager config, mirrorlist, profile.d. Runs via
     * [method].runOutside (which is `su` for chroot installs and a
     * plain app-uid shell for proot installs — the latter works
     * because the rootfs is app-uid-owned in proot mode). The
     * `enter.sh` wrapper is *not* written here — `Installer` does
     * that after `configure` because it's generic to every distro.
     */
    fun configure(method: InstallationMethod, rootfs: String, log: (String) -> Unit)

    /**
     * Bootstrap the package manager inside the chroot at [rootfs]
     * (e.g. `pacman-key --init && pacman-key --populate <keyring> &&
     * pacman -Syu`). Runs via [method].runInside.
     */
    fun initPackageManager(method: InstallationMethod, rootfs: String, log: (String) -> Unit)

    /**
     * Install [basePackages] inside the chroot at [rootfs]. Runs via
     * [method].runInside.
     */
    fun installBasePackages(method: InstallationMethod, rootfs: String, log: (String) -> Unit)
}

/**
 * Bootstrap-tarball descriptor.
 *
 * @property url HTTP(S) URL of the tarball.
 * @property format compression format ([BootstrapCache] uses this for
 *   the cache filename; [me.phie.tawc.install.Archive] dispatches on
 *   the file extension to either stream zstd through a FIFO or hand
 *   gzip / plain `.tar` straight to toybox tar).
 * @property stripPrefix single top-level directory inside the tarball
 *   to flatten into the rootfs (`"root.x86_64"` for the Arch x86_64
 *   bootstrap; `null` for tarballs that are already flat). Toybox tar
 *   has no `--strip-components`, so `Archive.extractAsRoot` flattens
 *   with `mv` after extraction when this is non-null.
 * @property verification integrity-check policy (PGP detached
 *   signature, etc.) consumed by [me.phie.tawc.install.SignatureVerifier]
 *   between download and extract. Distros must opt out explicitly via
 *   [BootstrapVerification.None] so the security stance of every
 *   bootstrap source is visible at the call site.
 */
data class DistroBootstrap(
    val url: String,
    val format: BootstrapFormat,
    val stripPrefix: String?,
    val verification: BootstrapVerification,
)
