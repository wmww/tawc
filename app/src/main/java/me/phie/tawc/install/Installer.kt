package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.install.distro.Distro
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
 *                             detached signature for Arch x86_64;
 *                             [BootstrapVerification.None] with a
 *                             loud warning for ALARM aarch64 since
 *                             upstream publishes no signature). On
 *                             mismatch the install fails before any
 *                             byte hits the rootfs.
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
        val bootstrap = distro.resolveBootstrap(log, mirrorProxy)

        store.save(
            Installation(
                id = id,
                distro = distro.key,
                arch = distro.androidAbi,
                method = method.key,
                installedAtMillis = System.currentTimeMillis(),
                sourceUrl = bootstrap.url,
                state = Installation.State.INSTALLING,
                installedAtAppVersionCode = appVersionCode,
                label = label,
            )
        )

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
        // source of "tarball drifted out of sync with the live .md5".
        var attempt = 0
        var verified: File? = null
        while (verified == null) {
            checkCancel()
            // Stage 1: download. BootstrapCache owns the cache dir
            // entirely — filename scheme, freshness mtime, TTL janitor —
            // so the installer just hands it (cacheKey, url, format).
            progress(InstallProgress(
                InstallStage.DOWNLOADING,
                "Downloading ${distro.linuxArch} bootstrap…",
            ))
            log("download: $effectiveBootstrapUrl" + if (attempt > 0) " (retry $attempt)" else "")
            val cf = cache.download(
                distro.cacheKey,
                effectiveBootstrapUrl,
                bootstrap.format,
            ) { read, total ->
                val pct = total?.let { ((read * 100) / it).toInt().coerceIn(0, 100) }
                val totalLabel = total?.let { HumanSize.format(it) } ?: "?"
                progress(InstallProgress(
                    InstallStage.DOWNLOADING,
                    "Downloading bootstrap: ${HumanSize.format(read)} / $totalLabel",
                    pct,
                ))
            }

            checkCancel()
            // Stage 2: integrity check. PGP-verify the just-downloaded
            // tarball against the distro's [BootstrapVerification] before
            // any byte hits the rootfs. Throws on mismatch / missing
            // signature key / forged blob — and parks the install in
            // FAILED upstream so the user can uninstall + retry from a
            // clean tree. Distros that opt in to
            // [BootstrapVerification.None] (e.g. ALARM, where upstream
            // publishes no signature) get a loud warning and proceed —
            // see notes/installation.md "Bootstrap integrity".
            progress(InstallProgress(
                InstallStage.VERIFYING,
                "Verifying bootstrap signature…",
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
        progress(InstallProgress(InstallStage.EXTRACTING, "Extracting rootfs…"))
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

        checkCancel()
        // Stage 3: configure. /etc files via Distro.configure. The
        // chroot-entry mechanics (mounts, bind-table, setsid, exec)
        // live entirely in [InstallationMethod.startInside] now —
        // there's nothing to materialise on disk between calls.
        progress(InstallProgress(InstallStage.CONFIGURING, "Configuring chroot…"))
        distro.configure(method, rootfsPath, mirrorProxy, log)
        // Symlink the APK-bundled libhybris tree into /usr/local/lib.
        // Must follow distro.configure (which may create /usr/local
        // structure) and precede package-manager bootstrap (so any
        // package that touches /usr/local/lib sees a coherent state).
        // Both methods get libhybris: chroot bind-mounts /apex +
        // /vendor + /system + /linkerconfig in [ChrootMounter];
        // [ProotMethod] adds the same paths via `-b` flags and
        // pre-creates the in-rootfs targets. The libhybris asset itself
        // is ABI-gated (no x86_64 emulator build), so [link] returns
        // false on the emulator and the binds are harmless leftovers.
        LibhybrisLinker.link(context, method, rootfsPath, log)

        checkCancel()
        // Stage 4: package-manager bootstrap. State stays INSTALLING
        // throughout — if either pacman invocation fails the service
        // wraps it as FAILED and the only recovery is uninstall +
        // install again.
        progress(InstallProgress(InstallStage.PKG_KEYRING, "Initializing package manager…"))
        distro.initPackageManager(method, rootfsPath, log)

        checkCancel()
        // Stage 5: install base packages.
        progress(InstallProgress(
            InstallStage.PKG_INSTALL,
            "Installing base packages…",
        ))
        distro.installBasePackages(method, rootfsPath, log)

        // All stages succeeded — flip to READY. From this point the
        // gate refuses install and only allows uninstall.
        store.setState(id, Installation.State.READY)
        progress(InstallProgress(InstallStage.DONE, "Installed"))
    }

    /**
     * Permanently remove [id]: state → UNINSTALLING, [InstallationMethod.wipe],
     * then the directory (including metadata.json) is gone. On a
     * `(no dir)` slot this is a no-op. Throws on wipe failure; the
     * service wraps as `FAILED` so a subsequent uninstall can retry.
     *
     * No [Distro] is needed — wipe is method-dispatched (chroot kills
     * tracked-by-`/proc/<pid>/root` processes + unmounts + `find
     * -xdev -delete`; proot just kills any in-flight tracees and
     * recursive-deletes).
     */
    fun uninstall(
        progress: (InstallProgress) -> Unit,
        log: (String) -> Unit,
    ) {
        val installDir = store.installationDir(id)
        if (!installDir.exists()) {
            progress(InstallProgress(InstallStage.DONE, "Nothing to delete"))
            return
        }
        store.setState(id, Installation.State.UNINSTALLING)

        // The UNMOUNTING stage is meaningful for chroot (real bind
        // mounts to tear down). Proot has no global mounts, just an
        // app-uid recursive delete — but the stage rolls past quickly,
        // and the install pipeline / UI is structured around these
        // labels, so we keep both for symmetry.
        progress(InstallProgress(InstallStage.UNMOUNTING, "Unmounting chroot…"))
        progress(InstallProgress(InstallStage.DELETING, "Deleting rootfs…"))
        method.wipe(installDir, log)

        progress(InstallProgress(InstallStage.DONE, "Deleted"))
    }

}
