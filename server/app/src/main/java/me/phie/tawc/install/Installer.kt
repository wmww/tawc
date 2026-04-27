package me.phie.tawc.install

import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.util.AppOwnership
import me.phie.tawc.install.util.HumanSize
import java.io.IOException

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
 *                             `distro.linuxArch` as the cache key.
 *   3. EXTRACTING           — [Archive.extractAsRoot] honouring the
 *                             optional `bootstrap.stripPrefix`.
 *   4. CONFIGURING          — [Distro.configure] writes /etc files,
 *                             then [writeEnterScript] renders
 *                             `enter.sh`.
 *   5. PKG_KEYRING          — [Distro.initPackageManager] (pacman-key
 *                             init / keyring populate / pacman -Syu
 *                             for Arch; apt-get update for Debian).
 *   6. PKG_INSTALL          — [Distro.installBasePackages] installs
 *                             the base package list.
 *   7. (state write)        — `setState(READY)`.
 *
 * The state-machine gate ([InstallationService]) only dispatches to
 * `install` against a `(no dir)` slot, so the rootfs is laid down on a
 * clean directory and never overlaid. `uninstall` delegates straight
 * to [RootfsCleaner.wipe]; mounts are torn down there, never here.
 */
class Installer(
    private val store: InstallationStore,
    private val cache: BootstrapCache,
    private val distro: Distro,
    private val id: String,
) {
    /** Throws on failure. Reports progress + log lines via the callbacks. */
    fun install(
        progress: (InstallProgress) -> Unit,
        log: (String) -> Unit,
    ) {
        val rootfsDir = store.rootfsDir(id)
        val rootfsPath = rootfsDir.absolutePath

        // Lay down the metadata first thing, in INSTALLING. The parent
        // dir is created with app uid (chown-fixed below) so this
        // writeText is a plain Java file write — no su needed.
        store.installationDir(id).mkdirs()
        AppOwnership.chownAppDirNonRecursive(store.installationDir(id))
        store.save(
            Installation(
                id = id,
                distro = distro.key,
                arch = distro.androidAbi,
                method = Installation.METHOD_CHROOT,
                installedAtMillis = System.currentTimeMillis(),
                sourceUrl = distro.bootstrap.url,
                state = Installation.State.INSTALLING,
            )
        )

        // Stage 1: download. BootstrapCache owns the cache dir
        // entirely — filename scheme, freshness mtime, TTL janitor —
        // so the installer just hands it (linuxArch, url, format).
        progress(InstallProgress(
            InstallStage.DOWNLOADING,
            "Downloading ${distro.linuxArch} bootstrap…",
        ))
        log("download: ${distro.bootstrap.url}")
        val cacheFile = cache.download(
            distro.linuxArch,
            distro.bootstrap.url,
            distro.bootstrap.format,
        ) { read, total ->
            val pct = total?.let { ((read * 100) / it).toInt().coerceIn(0, 100) }
            val totalLabel = total?.let { HumanSize.format(it) } ?: "?"
            progress(InstallProgress(
                InstallStage.DOWNLOADING,
                "Downloading bootstrap: ${HumanSize.format(read)} / $totalLabel",
                pct,
            ))
        }

        // Stage 2: extract. The rootfs dir does not exist yet — the
        // gate only invokes install on a `(no dir)` slot — so tar lays
        // everything onto a fresh tree. Archive.extractAsRoot does not
        // wipe; never has reason to. For zstd bootstraps we pass the
        // cache-owned transient path so all `cache/install/` files
        // have one owner.
        progress(InstallProgress(InstallStage.EXTRACTING, "Extracting rootfs…"))
        log("extract: ${cacheFile.name} -> $rootfsPath (strip=${distro.bootstrap.stripPrefix})")
        Archive.extractAsRoot(
            tarball = cacheFile,
            destDir = rootfsPath,
            tempPlainTar = cache.tempPlainTarFor(distro.linuxArch),
            stripPrefix = distro.bootstrap.stripPrefix,
        ) { line ->
            log("tar: $line")
        }

        // Stage 3: configure. /etc files via Distro.configure, then
        // the auto-generated enter.sh (mount + chroot wrapper) that
        // both ChrootRunner.run and host-side client/tawc-chroot-run
        // invoke. enter.sh is generic, so Installer writes it (not
        // Distro). Without enter.sh later pacman steps can't run.
        progress(InstallProgress(InstallStage.CONFIGURING, "Configuring chroot…"))
        distro.configure(rootfsPath, log)
        writeEnterScript(rootfsPath, log)

        // Stage 4: package-manager bootstrap. State stays INSTALLING
        // throughout — if either pacman invocation fails the service
        // wraps it as FAILED and the only recovery is uninstall +
        // install again.
        progress(InstallProgress(InstallStage.PKG_KEYRING, "Initializing package manager…"))
        distro.initPackageManager(rootfsPath, log)

        // Stage 5: install base packages.
        progress(InstallProgress(
            InstallStage.PKG_INSTALL,
            "Installing base packages (this takes a few minutes)…",
        ))
        distro.installBasePackages(rootfsPath, log)

        // All stages succeeded — flip to READY. From this point the
        // gate refuses install and only allows uninstall.
        store.setState(id, Installation.State.READY)
        progress(InstallProgress(InstallStage.DONE, "Installation complete."))
    }

    /**
     * Permanently remove [id]: state → UNINSTALLING, [RootfsCleaner.wipe],
     * then the directory (including metadata.json) is gone. On a
     * `(no dir)` slot this is a no-op. Throws on wipe failure; the
     * service wraps as `FAILED` so a subsequent uninstall can retry.
     *
     * No [Distro] is needed for uninstall — every chroot dir is wiped
     * the same way, by dev:inode of `/proc/<pid>/root` and `find -xdev
     * -depth -delete`.
     */
    fun uninstall(
        progress: (InstallProgress) -> Unit,
        log: (String) -> Unit,
    ) {
        val installDir = store.installationDir(id)
        if (!installDir.exists()) {
            progress(InstallProgress(InstallStage.DONE, "Nothing to uninstall."))
            return
        }
        store.setState(id, Installation.State.UNINSTALLING)

        progress(InstallProgress(InstallStage.UNMOUNTING, "Unmounting chroot…"))
        progress(InstallProgress(InstallStage.DELETING, "Deleting rootfs…"))
        RootfsCleaner.wipe(installDir, log)

        progress(InstallProgress(InstallStage.DONE, "Uninstalled."))
    }

    /**
     * Render `<installation-dir>/enter.sh` from
     * [ChrootMounter.enterScript]. Owned by app uid, so a plain file
     * write is enough — no `su` needed. Made +x so `su -c '<path>'`
     * can exec it directly. Both [ChrootRunner.run] and host tooling
     * call this single file, so the mount logic only lives in
     * [ChrootMounter].
     */
    private fun writeEnterScript(rootfsPath: String, log: (String) -> Unit) {
        val file = store.enterScriptFile(id)
        file.writeText(ChrootMounter.enterScript(rootfsPath))
        if (!file.setExecutable(true, false)) {
            log("warn: failed to chmod +x ${file.absolutePath}")
        } else {
            log("wrote ${file.absolutePath}")
        }
    }
}
