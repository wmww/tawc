package me.phie.tawc.install.distro

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.EnabledBootstrapFlavors
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import java.io.IOException

/**
 * Per-distro policy: bootstrap tarball, `/etc` configuration,
 * package-manager init, base package install. The generic
 * `Installer` orchestrates these in a fixed order
 * (`download → extract → configure → init pkgmgr → install pkgs`)
 * and one `Distro` per (distro family × Linux arch) plugs in here.
 *
 * Today's set includes Arch, Manjaro ARM, Void glibc, Debian sid, and
 * Ubuntu 24.04. Adding another is a fresh directory under `distro/`
 * plus whatever shared helpers its package manager already has (see
 * `distro/ubuntu/` for the smallest example); nothing in `Installer` /
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
     * Whether this distro is one we actually support for users.
     * Supported distros are Arch Linux ARM and Debian sid (plus Arch
     * Linux x86_64, the emulator-only stand-in for ALARM); everything
     * else ships but is dev/experimental and hides behind the install
     * form's "Other distros" expander. Purely a UI/policy label — the
     * install pipeline treats every distro identically. See
     * notes/distro-options.md.
     */
    val supported: Boolean get() = false

    /**
     * Short label used as the install-form Label default and as the
     * basis of the on-disk id slug (e.g. `"Arch"`, `"Manjaro"`).
     * Must be slugifiable via [Installation.slugifyLabel] so the
     * derived id matches [Installation.isValidId]. Distinct from
     * [displayName] because the full name typically has spaces and
     * arch suffixes that produce a long, ugly directory name.
     */
    val defaultLabel: String

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

    /**
     * Cache filename component for the bootstrap tarball. Two distros
     * sharing one [linuxArch] (Arch Linux ARM and Manjaro ARM both at
     * `aarch64`) can't share a cache slot — the [BootstrapCache]
     * filename is `bootstrap-<cacheKey>.tar.<ext>`, so they need
     * distinct keys. Default is `"$key-$linuxArch"` which is
     * already-unique without per-distro overrides.
     */
    val cacheKey: String get() = "$key-$linuxArch"

    /**
     * Static bootstrap tarball metadata. For most distros this is the
     * single source of truth — [resolveBootstrap] just returns it. For
     * distros where the URL or expected digest is only known at install
     * time (e.g. GitHub Releases "latest" with a server-side
     * SHA-256 in the API response), this is a placeholder carrying
     * [BootstrapVerification.ResolvedAtInstallTime] and
     * [resolveBootstrap] does the runtime lookup. The placeholder
     * fails closed: if it reaches the verify stage the install throws.
     */
    val bootstrap: TarballBootstrap

    /**
     * Every bootstrap flavor this distro implements, whether or not
     * this APK ships it. Default: just the tarball path, wrapping
     * [bootstrap] — distros with a single flavor change nothing. A
     * distro adding a `packages` flavor overrides this with both
     * entries; the map's [DistroBootstrap] values are static
     * descriptors ([resolveBootstrap] may substitute live data at
     * install time, e.g. a resolved tarball digest).
     *
     * Callers outside the build gate and its tests want
     * [bootstrapFlavors] instead.
     */
    val declaredBootstrapFlavors: Map<BootstrapFlavor, DistroBootstrap>
        get() = mapOf(BootstrapFlavor.TARBALL to bootstrap)

    /**
     * The flavors of [declaredBootstrapFlavors] this build actually
     * ships ([me.phie.tawc.install.EnabledBootstrapFlavors]): the
     * install form, the service gate, and [resolveBootstrap] all go
     * through here, so a release APK sees tarball only. Not meant to
     * be overridden — override [declaredBootstrapFlavors].
     */
    val bootstrapFlavors: Map<BootstrapFlavor, DistroBootstrap>
        get() = declaredBootstrapFlavors.filterKeys { EnabledBootstrapFlavors.isEnabled(it) }

    /**
     * The one release-supported flavor, always shipped. Anything else
     * is a debug-only experiment, excluded from release builds at
     * build time by [me.phie.tawc.install.EnabledBootstrapFlavors] and
     * rejected by [me.phie.tawc.install.InstallationService] if some
     * caller names it anyway.
     */
    val supportedFlavor: BootstrapFlavor get() = BootstrapFlavor.TARBALL

    /**
     * Resolve the bootstrap descriptor at install time. Default impl
     * returns the static [bootstrapFlavors] entry. Override for
     * distros whose URL or [BootstrapVerification] digest must be
     * looked up live — e.g. ManjaroArm hits the GitHub Releases API
     * for the latest tag's `Manjaro-ARM-aarch64-latest.tar.gz` asset
     * and reads its server-computed SHA-256 from the `digest` field.
     * Runs before download; failures throw so we never attempt a
     * download whose verification can't be set up.
     *
     * @param mirrorProxy debug-builds-only knob: when non-null,
     *   implementations that fetch over HTTP for resolution (GitHub
     *   Releases API, Void's `sha256sum.txt`) should route through it
     *   so the dev cache stays coherent with the proxied tarball
     *   download. See `notes/cache-proxy.md`.
     * @param flavor which of [bootstrapFlavors] to resolve; the
     *   service has already validated it against the map and the
     *   release gate by the time an install reaches this.
     */
    fun resolveBootstrap(
        log: (String) -> Unit,
        mirrorProxy: MirrorProxy? = null,
        flavor: BootstrapFlavor = supportedFlavor,
    ): DistroBootstrap = bootstrapFlavors[flavor]
        ?: throw IOException("distro $key has no ${flavor.id} bootstrap flavor")

    /** Base packages to `pacman -S --needed` (or equivalent) at install time. */
    val basePackages: List<String>

    /**
     * Write `/etc` configuration into the freshly-extracted [rootfs]:
     * DNS, package-manager config, mirrorlist, profile.d. Runs via
     * [method].runOutside (which is `su` for chroot installs and a
     * plain app-uid shell for proot installs — the latter works
     * because the rootfs is app-uid-owned in proot mode).
     *
     * @param mirrorProxy when non-null, every package-mirror URL the
     *   implementation writes into the rootfs (pacman mirrorlist,
     *   xbps repository conf, apt sources.list) must be rewritten
     *   through it via [MirrorProxy.wrap]. Verification endpoints
     *   (`.sig` and friends) are **not** proxied here — see
     *   `notes/cache-proxy.md`.
     */
    fun configure(
        method: InstallationMethod,
        rootfs: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
    )

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
 * How a rootfs is assembled. One of the sealed shapes below; each
 * carries its trust root in the type — there is deliberately no
 * verification-free variant to accidentally reach (see
 * notes/installation.md "Bootstrap integrity").
 */
sealed interface DistroBootstrap

/**
 * Identifier for one entry of [Distro.bootstrapFlavors]. [id] is the
 * wire/persist form: the broker `--arg bootstrap=` value and the
 * `metadata.json` `bootstrapFlavor` field.
 */
enum class BootstrapFlavor(val id: String) {
    TARBALL("tarball"),
    PACKAGES("packages");

    companion object {
        fun fromId(id: String): BootstrapFlavor? = entries.firstOrNull { it.id == id }
    }
}

/**
 * Bootstrap-tarball descriptor — download one archive, verify, extract.
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
 *   between download and extract. Every descriptor that reaches the
 *   verify stage must carry a concrete policy; static placeholders use
 *   [BootstrapVerification.ResolvedAtInstallTime], which throws there,
 *   so a distro cannot end up unverified by omission.
 */
data class TarballBootstrap(
    val url: String,
    val format: BootstrapFormat,
    val stripPrefix: String?,
    val verification: BootstrapVerification,
) : DistroBootstrap

/**
 * Assemble the rootfs from signed repo metadata + individual packages
 * (Debian family today, via vendored debootstrap — see
 * [me.phie.tawc.install.pkgbootstrap.PackageBootstrapInstaller]).
 *
 * The trust root is [keyResource]: a `res/raw` PGP keyring shipped in
 * the APK, against which the suite's clearsigned `InRelease` is
 * verified before any downloaded byte is trusted. Deliberately
 * non-optional — this type has no "skip verification" shape at all.
 *
 * @property archiveRoot repo root, e.g. `http://deb.debian.org/debian`.
 * @property suite e.g. `"sid"`.
 * @property packagesArch dpkg architecture (`"arm64"`, `"amd64"`) —
 *   names the `binary-<arch>` index; distinct from [Distro.linuxArch].
 * @property keyResource `res/raw` keyring name (no extension),
 *   registered in [me.phie.tawc.install.SignatureVerifier]'s key map.
 */
data class PackageBootstrap(
    val archiveRoot: String,
    val suite: String,
    val packagesArch: String,
    val keyResource: String,
) : DistroBootstrap
