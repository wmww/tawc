package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.AndoBrokers
import me.phie.tawc.R
import me.phie.tawc.install.distro.BootstrapFlavor
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.PackageBootstrap
import me.phie.tawc.install.distro.TarballBootstrap
import me.phie.tawc.install.pkgbootstrap.PackageBootstrapInstaller
import me.phie.tawc.install.util.AppOwnership
import me.phie.tawc.install.util.HumanSize
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException

/**
 * Generic install/uninstall pipeline. The shape is the same regardless
 * of distro family (Arch today, Ubuntu/Fedora later); per-distro policy
 * lives in the [Distro] passed in.
 *
 * Stages 2-4 below are the tarball flavor's; a [PackageBootstrap]
 * dispatches them to [PackageBootstrapInstaller] instead (see
 * notes/installation.md "Bootstrap flavors") and rejoins at stage 5.
 *
 * Stages, mirroring [InstallStage]:
 *
 *   1. (state write)        — `setState(INSTALLING)` after `mkdir` of
 *                             `<distros>/<id>/`. From here the slot
 *                             exists on disk; any failure parks it in
 *                             FAILED for the user to uninstall + retry.
 *   2. DOWNLOADING          — [BootstrapCache.download] using
 *                             `distro.cacheKey` (e.g. `arch-aarch64`,
 *                             `manjaro-aarch64`) as the cache key.
 *   3. VERIFYING            — [SignatureVerifier.verify] checks the
 *                             tarball against the distro's
 *                             [BootstrapVerification] policy (PGP
 *                             detached signature for both Arch
 *                             flavours, resolved SHA-256 digest for
 *                             Manjaro/Void/Debian). On mismatch the
 *                             install fails before any byte hits the
 *                             rootfs.
 *   4. EXTRACTING           — [InstallationMethod.extractBootstrap]
 *                             (chroot → toybox tar via su; proot →
 *                             pure-Kotlin [ProotArchiveExtractor]),
 *                             honouring `bootstrap.stripPrefix`.
 *   5. CONFIGURING          — [Distro.configure] writes /etc files
 *                             (mirrorlist, pacman.conf tweaks, etc.).
 *   6. PKG_KEYRING          — [Distro.initPackageManager] (pacman-key
 *                             init / keyring populate / pacman -Syu
 *                             for Arch; apt-get update for Debian).
 *   7. PKG_INSTALL          — [Distro.installBasePackages] installs
 *                             the base package list.
 *   8. (state write)        — `setState(READY)`.
 *
 * The state-machine gate ([InstallationService]) only dispatches to
 * `install` against a `(no dir)` slot, so the rootfs is laid down on a
 * clean directory and never overlaid. `uninstall` delegates straight
 * to [RootfsCleaner.wipe]; mounts are torn down there, never here.
 */
class Installer(
    private val context: Context,
    private val store: InstallationStore,
    private val cache: BootstrapCache,
    private val distro: Distro,
    private val method: InstallationMethod,
    private val id: String,
    private val label: String? = null,
    /**
     * Dev-time caching reverse proxy. When non-null, bootstrap fetches
     * and the rootfs's package-mirror config get rewritten through it.
     * Always null for production installs — set only via the
     * `--es mirrorProxy` install intent extra (debug builds) or the
     * "Use local proxy mirror" form checkbox. See
     * `notes/cache-proxy.md`.
     */
    private val mirrorProxy: MirrorProxy? = null,
    /**
     * External-storage binds persisted into the initial metadata, so
     * they're already live for every in-rootfs step of the install
     * itself (first boot included). Resolved by [InstallationService]
     * — defaults or an explicit caller-provided list.
     */
    private val externalBinds: List<ExternalBind> = emptyList(),
    /**
     * Whether ando (notes/ando.md) is enabled for this install. Persisted
     * into the initial metadata so the broker listener + per-distro bind
     * are live for the install's own in-rootfs steps (first boot
     * included). Default false — opt-in, fail-closed.
     */
    private val andoEnabled: Boolean = false,
    /**
     * Which bootstrap flavor to install. `null` means the distro's
     * supported flavor — the only value reachable in release builds
     * ([InstallationService] rejects anything else there). The
     * uninstall path never reads this.
     */
    private val bootstrapFlavor: BootstrapFlavor? = null,
    /**
     * Package-mirror region id to configure the rootfs with
     * (`Distro.mirrorRegions`); `null` = the distro's built-in default.
     * Persisted into the initial metadata so the settings screen shows
     * what this install actually used. Only the bootstrap tarball is
     * mirror-independent (see notes/installation.md).
     */
    private val mirrorRegion: String? = null,
) {
    /** Throws on failure. Reports progress + log lines via the callbacks. */
    fun install(
        progress: (InstallProgress) -> Unit,
        log: (String) -> Unit,
    ) {
        // Stage-boundary cancel gate. `runInterruptible` translates a
        // coroutine cancel into a thread interrupt, but inner blocking
        // calls (PGP digest, tar extract, pacman) are uninterruptible
        // for chunks of seconds-to-minutes and only honour the flag
        // when they reach a poll point. This guard ensures that even
        // if a slow stage runs to completion ignoring the interrupt,
        // we tip over at the next stage boundary instead of plowing
        // through the whole pipeline.
        fun checkCancel() {
            if (Thread.interrupted()) {
                throw InterruptedIOException("install cancelled by user")
            }
        }

        val rootfsDir = store.rootfsDir(id)
        val rootfsPath = rootfsDir.absolutePath

        // Lay down the metadata first thing, in INSTALLING. The parent
        // dir is created with app uid (chown-fixed below for chroot)
        // so this writeText is a plain Java file write — no su needed.
        store.installationDir(id).mkdirs()
        // The chown only matters for the chroot path: a previous `su`
        // invocation could have left `<distros>/<id>/` root-owned, and
        // we then can't write `metadata.json` from app uid. Proot
        // installs are app-uid-owned end-to-end, and on a non-rooted
        // device `Su.run` would throw IOException on `ProcessBuilder
        // .start("su")` and tank the install before stage 0.
        if (method.requiresRoot) {
            AppOwnership.chownAppDirNonRecursive(store.installationDir(id))
        }
        // Stamp the app version that performed this install. The rootfs
        // is treated as immutable across app updates (see
        // notes/installation.md "Upgrade policy"), so this is the
        // version whose `Distro.configure` output the rootfs carries —
        // useful later for "if installed before vN, do X" gating.
        val appVersionCode = try {
            context.packageManager.getPackageInfo(context.packageName, 0).longVersionCode
        } catch (_: android.content.pm.PackageManager.NameNotFoundException) { 0L }
        // Resolve the bootstrap descriptor before writing metadata so
        // the persisted `sourceUrl` reflects the actual URL the install
        // ran against (matters for distros with dynamic resolution like
        // ManjaroArm, where the latest GitHub release tag changes
        // weekly). Failure here aborts before disk state is laid down.
        val flavor = bootstrapFlavor ?: distro.supportedFlavor
        val bootstrap = distro.resolveBootstrap(log, mirrorProxy, flavor)
        val sourceUrl = when (bootstrap) {
            is TarballBootstrap -> bootstrap.url
            is PackageBootstrap -> bootstrap.archiveRoot
        }

        store.save(
            Installation(
                id = id,
                distro = distro.key,
                arch = distro.androidAbi,
                method = method.key,
                installedAtMillis = System.currentTimeMillis(),
                sourceUrl = sourceUrl,
                state = Installation.State.INSTALLING,
                installedAtAppVersionCode = appVersionCode,
                label = label,
                externalBinds = externalBinds,
                andoEnabled = andoEnabled,
                bootstrapFlavor = flavor.id,
                mirrorRegion = mirrorRegion,
            )
        )
        // Bring the ando broker listener up (or down) to match the
        // freshly-written metadata, so the install's own in-rootfs
        // steps can use ando when enabled. See notes/ando.md.
        AndoBrokers.refresh(context)

        // Stages 1-3 diverge by flavor: the tarball path downloads/
        // verifies/extracts one archive; the packages path assembles
        // the rootfs from signed repo metadata + debootstrap. Both
        // rejoin at the flavor-agnostic configure step below.
        when (bootstrap) {
            is TarballBootstrap ->
                installTarballBootstrap(bootstrap, rootfsPath, ::checkCancel, progress, log)
            is PackageBootstrap ->
                PackageBootstrapInstaller(
                    context, store, cache, distro, method, id, bootstrap, mirrorProxy,
                ).install(::checkCancel, progress, log)
        }

        checkCancel()
        // Stage 3: configure. /etc files via Distro.configure. The
        // chroot-entry mechanics (mounts, bind-table, setsid, exec)
        // live entirely in [InstallationMethod.startInside] now —
        // there's nothing to materialise on disk between calls.
        progress(InstallProgress(InstallStage.CONFIGURING, context.getString(R.string.install_progress_configuring_chroot)))
        distro.configure(method, rootfsPath, mirrorProxy, log)
        // A picked region replaces the mirror config configure just
        // wrote — same call the settings row makes on a live rootfs, so
        // both paths cannot drift apart.
        if (mirrorRegion != null) {
            distro.configureMirrors(method, rootfsPath, mirrorRegion, log)
        }
        // Lay down everything the app ships per-rootfs (libhybris into
        // /usr/lib/hybris, the glvnd vendor JSON, …) as real files via
        // [TawcInstaller]. Must follow distro.configure (which may
        // create the /usr tree we're writing into) and precede the
        // package-manager bootstrap so any pacman scriptlet that
        // touches our paths sees a coherent state. Idempotent: the
        // (id, app-stamp) pair gets recorded so the same call from
        // [me.phie.tawc.TawcApplication.onCreate] no-ops on subsequent
        // app starts until an APK upgrade bumps the stamp.
        TawcInstaller.installInto(context, store, id, log)

        checkCancel()
        // Stage 4: package-manager bootstrap. State stays INSTALLING
        // throughout — if either pacman invocation fails the service
        // wraps it as FAILED and the only recovery is uninstall +
        // install again.
        progress(InstallProgress(InstallStage.PKG_KEYRING, context.getString(R.string.install_progress_initializing_package_manager)))
        distro.initPackageManager(method, rootfsPath, log)

        checkCancel()
        // Stage 5: install base packages.
        progress(InstallProgress(
            InstallStage.PKG_INSTALL,
            context.getString(R.string.install_progress_installing_base_packages),
        ))
        distro.installBasePackages(method, rootfsPath, log)

        // All stages succeeded — flip to READY. From this point the
        // gate refuses install and only allows uninstall.
        store.setState(id, Installation.State.READY)
        progress(InstallProgress(InstallStage.DONE, context.getString(R.string.install_progress_installed)))
    }

    /**
     * Tarball flavor, stages 1-3: download the bootstrap tarball into
     * the cache, integrity-check it against the distro's
     * [BootstrapVerification] policy, extract onto the fresh rootfs.
     */
    private fun installTarballBootstrap(
        bootstrap: TarballBootstrap,
        rootfsPath: String,
        checkCancel: () -> Unit,
        progress: (InstallProgress) -> Unit,
        log: (String) -> Unit,
    ) {
        // Funnel the bootstrap fetch through the dev-time mirror cache
        // when set. The proxy URL is only what the wire request goes to;
        // metadata.json's [Installation.sourceUrl] still records the
        // canonical upstream URL above so the install record reads
        // sensibly across runs with/without the proxy.
        val effectiveBootstrapUrl = mirrorProxy?.wrap(bootstrap.url) ?: bootstrap.url

        // Stages 1+2: download and integrity-check, with one retry on
        // verify failure. The cached tarball at
        // `<cacheDir>/install/bootstrap-<arch>.tar.<ext>` survives across
        // uninstall+reinstall cycles, and Downloader's "skip if size
        // matches Content-Length" check happily reuses a corrupt blob if
        // the on-wire size happens to match — without this loop a single
        // bad download (or a stale entry served by some upstream cache)
        // sticks forever. On the second failure we surface a hint
        // pointing at the dev cache proxy, since that's the most common
        // source of "tarball drifted out of sync with the live .sig".
        var attempt = 0
        var verified: File? = null
        while (verified == null) {
            checkCancel()
            // Stage 1: download. BootstrapCache owns the cache dir
            // entirely — filename scheme, freshness mtime, TTL janitor —
            // so the installer just hands it (cacheKey, url, format).
            progress(InstallProgress(
                InstallStage.DOWNLOADING,
                context.getString(R.string.install_progress_downloading_arch_bootstrap, distro.linuxArch),
            ))
            log("download: $effectiveBootstrapUrl" + if (attempt > 0) " (retry $attempt)" else "")
            val cf = cache.download(
                distro.cacheKey,
                effectiveBootstrapUrl,
                bootstrap.format,
            ) { read, total ->
                val pct = total?.let { ((read * 100) / it).toInt().coerceIn(0, 100) }
                val totalLabel = total?.let { HumanSize.format(it) }
                    ?: context.getString(R.string.distro_info_unknown)
                progress(InstallProgress(
                    InstallStage.DOWNLOADING,
                    context.getString(
                        R.string.install_progress_downloading_bootstrap,
                        HumanSize.format(read),
                        totalLabel,
                    ),
                    pct,
                ))
            }

            checkCancel()
            // Stage 2: integrity check. Verify the just-downloaded
            // tarball against the distro's [BootstrapVerification] before
            // any byte hits the rootfs. Throws on mismatch / missing
            // signature key / forged blob / a leftover
            // ResolvedAtInstallTime placeholder — and parks the install
            // in FAILED upstream so the user can uninstall + retry from
            // a clean tree. See notes/installation.md "Bootstrap
            // integrity" for what each distro declares.
            progress(InstallProgress(
                InstallStage.VERIFYING,
                context.getString(R.string.install_progress_verifying_bootstrap),
            ))
            log("verify: ${bootstrap.verification::class.simpleName}")
            try {
                SignatureVerifier.verify(context, cf, bootstrap.verification, mirrorProxy)
                verified = cf
            } catch (e: IOException) {
                if (attempt >= 1) {
                    if (mirrorProxy != null) {
                        log(
                            "verify: failed twice through the dev cache proxy at " +
                                "${mirrorProxy.base} — its cached entries (tarball + " +
                                "digests) appear out of sync with each other. " +
                                "Ask the user to clear build/cache-proxy/cache/ and retry.",
                        )
                    }
                    throw e
                }
                log("verify: failed (${e.message}); evicting local cache and retrying once")
                cache.evict(distro.cacheKey, bootstrap.format)
                attempt++
            }
        }
        val cacheFile: File = verified

        checkCancel()
        // Stage 3: extract. The rootfs dir does not exist yet — the
        // gate only invokes install on a `(no dir)` slot — so the
        // method's extractor lays everything onto a fresh tree.
        // Neither extractor wipes; never has reason to. For zstd
        // bootstraps we pass the cache-owned FIFO path (used by the
        // chroot path; proot ignores it and decompresses via
        // zstd-jni) so all `cache/install/` files have one owner.
        progress(InstallProgress(InstallStage.EXTRACTING, context.getString(R.string.install_progress_extracting_rootfs)))
        log("extract: ${cacheFile.name} -> $rootfsPath (strip=${bootstrap.stripPrefix}, method=${method.key})")
        method.extractBootstrap(
            tarball = cacheFile,
            rootfs = rootfsPath,
            format = bootstrap.format,
            stripPrefix = bootstrap.stripPrefix,
            tempFifo = cache.tempFifoFor(distro.cacheKey),
        ) { line ->
            log("tar: $line")
        }
    }

    /**
     * Permanently remove [id]: state → UNINSTALLING, [RootfsCleaner.wipe],
     * then the directory (including metadata.json) is gone. On a
     * `(no dir)` slot this is a no-op. Throws on wipe failure; the
     * service wraps as `FAILED` so a subsequent uninstall can retry.
     *
     * No [Distro] is needed — the wipe engine is distro-agnostic,
     * parameterised only by the method's capability flags.
     */
    fun uninstall(
        progress: (InstallProgress) -> Unit,
        log: (String) -> Unit,
    ) {
        if (!store.installationDir(id).exists()) {
            progress(InstallProgress(InstallStage.DONE, context.getString(R.string.install_progress_nothing_to_delete)))
            return
        }
        store.setState(id, Installation.State.UNINSTALLING)

        // The UNMOUNTING stage is meaningful for chroot (real bind
        // mounts to tear down). Proot has no global mounts, just an
        // app-uid recursive delete — but the stage rolls past quickly,
        // and the install pipeline / UI is structured around these
        // labels, so we keep both for symmetry.
        progress(InstallProgress(InstallStage.UNMOUNTING, context.getString(R.string.install_progress_unmounting_chroot)))
        progress(InstallProgress(InstallStage.DELETING, context.getString(R.string.install_progress_deleting_rootfs)))
        RootfsCleaner.wipe(store, id, log)

        // The wipe removed the ando dir; drop this install's broker
        // listener and any test-mode override now that its metadata is
        // gone, so neither survives into a reinstall of the same id.
        // See notes/ando.md.
        InstallationStore.clearAndoOverride(id)
        AndoBrokers.refresh(context)

        progress(InstallProgress(InstallStage.DONE, context.getString(R.string.install_progress_deleted)))
    }

}
