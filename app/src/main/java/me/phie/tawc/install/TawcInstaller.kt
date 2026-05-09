package me.phie.tawc.install

import android.content.Context
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import me.phie.tawc.compositor.CompositorService
import java.io.File
import java.io.IOException

/**
 * Generic "copy app-shipped files into a rootfs" engine.
 *
 * Replaces the per-feature one-offs (the old [LibhybrisLinker]'s
 * symlink dance, etc.) with a single uniform mechanism that:
 *
 *  - Asks every registered [TawcInstallProvider] for a manifest of
 *    files it wants in the rootfs.
 *  - Copies / symlinks each entry into the rootfs's actual on-disk
 *    tree (NOT a bind mount — entries become per-rootfs-owned files,
 *    so a misbehaving rootfs can't corrupt the source).
 *  - Records the manifest in [Installation.tawcInstalls] alongside
 *    a [Installation.tawcStamp] tag.
 *
 * The stamp is whatever [CompositorService.currentExtractStamp]
 * returns — a `versionCode + lastUpdateTime` pair that bumps on every
 * `adb install -r` (so dev iteration triggers re-installs cleanly,
 * not just version-code bumps).
 *
 * Two trigger sites — see [installInto] kdoc:
 *   1. [Installer.install] at the end of CONFIGURING (fresh install).
 *   2. [me.phie.tawc.TawcApplication.onCreate] (APK upgrade detection
 *      via per-rootfs stamp comparison; idempotent no-op when the
 *      rootfs is already up to date).
 *
 * Not called from [InstallationMethod.startInside] — entries are a
 * hot path and this work shouldn't run on every shell invocation.
 */
internal object TawcInstaller {
    private const val TAG = "tawc-installer"

    /** All providers consulted by [installInto]. Order doesn't matter
     *  for correctness — but keep it stable so logs read consistently. */
    private val providers: List<TawcInstallProvider> = listOf(
        LibhybrisInstallProvider,
    )

    /**
     * Make the rootfs at [Installation.id] match the current app
     * version's tawc-installed file set. No-op when
     * `installation.tawcStamp == CompositorService.currentExtractStamp`.
     * Otherwise:
     *
     *   1. Walk the recorded [Installation.tawcInstalls] manifest and
     *      delete each `dest` from the rootfs (symlinks are unlinked,
     *      regular files are unlinked, dirs are recursively deleted).
     *      Skips entries whose dest is missing — a manual `rm` inside
     *      the rootfs is fine.
     *   2. Ask every [TawcInstallProvider] for its current set of
     *      [TawcInstall] entries.
     *   3. For each entry: ensure the parent dir exists, then copy
     *      (`COPY`) or symlink (`LINK`) `src` to `dest` inside the
     *      rootfs. Existing files at `dest` are unlinked first so we
     *      don't follow a leftover symlink and write through it.
     *   4. Persist the new manifest + stamp via [InstallationStore.save].
     *
     * Throws [IOException] on filesystem errors (caller wraps as a
     * stage failure during install; logs and skips the rootfs during
     * the [TawcApplication]-driven walk so one corrupted slot can't
     * keep the app from launching).
     */
    fun installInto(
        context: Context,
        store: InstallationStore,
        id: String,
        log: (String) -> Unit,
    ) {
        val installation = store.load(id) ?: run {
            log("tawc-installer: $id has no metadata.json — skipping")
            return
        }
        val rootfs = store.rootfsDir(id)
        if (!rootfs.isDirectory) {
            log("tawc-installer: $id rootfs missing at $rootfs — skipping")
            return
        }
        val currentStamp = CompositorService.currentExtractStamp(context)
        if (installation.tawcStamp == currentStamp) {
            // Up to date — no work, no log spam (this fires on every
            // app start in the steady state).
            return
        }
        log(
            "tawc-installer: $id stamp=${installation.tawcStamp ?: "(none)"} → " +
                "$currentStamp; refreshing"
        )

        // 1. Wipe old. We do this before gathering the new manifest so
        //    a provider that's been removed between app versions still
        //    has its entries cleaned up — the manifest in metadata is
        //    the source of truth for what to delete, not the live
        //    provider list.
        for (entry in installation.tawcInstalls) {
            removeFromRootfs(rootfs, entry, log)
        }

        // 2. Gather new manifest. Provider order is preserved in the
        //    output so logs are deterministic. Empty result is valid:
        //    e.g. x86_64 emulator with no libhybris asset has nothing
        //    to install — we still record the (empty) manifest + stamp
        //    so the next app start hits the no-op fast path.
        val newManifest = mutableListOf<TawcInstall>()
        for (provider in providers) {
            val entries = try {
                provider.entries(context)
            } catch (e: Exception) {
                throw IOException(
                    "tawc-installer: provider ${provider.name} failed to enumerate: $e",
                    e,
                )
            }
            log("tawc-installer: ${provider.name}: ${entries.size} entries")
            newManifest += entries
        }

        // 3. Apply. The order matches the manifest order; the same
        //    order will be used at wipe time on the next refresh.
        for (entry in newManifest) {
            applyToRootfs(rootfs, entry)
        }

        // 4. Persist. Stamp is updated in the same write as the
        //    manifest, so a crash between wipe and save leaves us with
        //    a stale stamp + missing files — which the next run treats
        //    as "needs refresh" (the old manifest's dests have already
        //    been removed; the new ones simply get re-laid). Idempotent
        //    by construction.
        store.save(installation.copy(
            tawcStamp = currentStamp,
            tawcInstalls = newManifest,
        ))
        log("tawc-installer: $id refreshed (${newManifest.size} entries)")
    }

    /**
     * Walk every installation in [store] and call [installInto] on
     * each. Used by [me.phie.tawc.TawcApplication.onCreate] so an APK
     * upgrade picks up immediately rather than waiting for the user
     * to run something inside each rootfs.
     *
     * Per-rootfs failures are logged and swallowed — one corrupt slot
     * shouldn't keep the app from launching the others.
     */
    fun installAll(context: Context, store: InstallationStore) {
        for (installation in store.list()) {
            try {
                installInto(context, store, installation.id) { line ->
                    Log.i(TAG, line)
                }
            } catch (e: Exception) {
                Log.e(TAG, "installAll: ${installation.id} failed: $e", e)
            }
        }
    }

    // --- internals ---------------------------------------------------

    /**
     * Delete one manifest entry's `dest` from the rootfs. Robust to:
     *   - missing dest (someone already removed it; fine)
     *   - dest being a symlink (unlink without following)
     *   - dest being a directory (recursive delete; rare — we
     *     currently don't ship dir-typed entries, but a future
     *     provider might, and uniform handling keeps the manifest
     *     contract simple).
     *
     * Failures are logged but don't throw — partial cleanup followed
     * by a fresh apply is preferable to refusing to upgrade because
     * one stale file couldn't be unlinked.
     */
    private fun removeFromRootfs(
        rootfs: File,
        entry: TawcInstall,
        log: (String) -> Unit,
    ) {
        val target = File(rootfs, entry.dest.removePrefix("/"))
        try {
            // Os.lstat to detect a symlink without following it.
            val stat = try {
                Os.lstat(target.absolutePath)
            } catch (e: ErrnoException) {
                if (e.errno == OsConstants.ENOENT) return
                log("tawc-installer: lstat $target: ${e.message}")
                return
            }
            if (OsConstants.S_ISDIR(stat.st_mode)) {
                if (!target.deleteRecursively()) {
                    log("tawc-installer: deleteRecursively $target: failed")
                }
            } else {
                Os.remove(target.absolutePath)
            }
        } catch (e: ErrnoException) {
            log("tawc-installer: unlink $target: ${e.message}")
        }
    }

    /**
     * Materialise one manifest entry into the rootfs. Creates parent
     * dirs as needed; clears any pre-existing file/symlink at `dest`
     * so a stale entry doesn't get written-through-symlink to its old
     * source.
     */
    private fun applyToRootfs(rootfs: File, entry: TawcInstall) {
        val target = File(rootfs, entry.dest.removePrefix("/"))
        target.parentFile?.mkdirs()
        // Pre-clear so we don't follow a stale symlink when copying.
        try {
            Os.remove(target.absolutePath)
        } catch (e: ErrnoException) {
            if (e.errno != OsConstants.ENOENT) {
                // Best-effort — directory at the path, or a truly
                // unkillable inode. Let the next operation fail with
                // a more specific error if it can't proceed.
            }
        }
        when (entry.type) {
            TawcInstall.Type.LINK -> {
                Os.symlink(entry.src, target.absolutePath)
            }
            TawcInstall.Type.COPY -> {
                File(entry.src).inputStream().use { input ->
                    target.outputStream().use { output -> input.copyTo(output) }
                }
                // Preserve executable bit from the source: libhybris
                // .so files come out of the asset extract with mode
                // 0711 and we want that to follow into the rootfs so
                // dlopen works. Other modes don't matter for our
                // current providers.
                if (File(entry.src).canExecute()) {
                    target.setExecutable(true, false)
                }
            }
        }
    }
}

/**
 * Source of [TawcInstall] entries for [TawcInstaller]. Implementations
 * are stateless objects registered in [TawcInstaller.providers].
 */
internal interface TawcInstallProvider {
    /** Stable identifier used in log lines. */
    val name: String

    /**
     * Compute the entries this provider wants installed in the rootfs
     * for the current app version. Empty list = nothing to install
     * (e.g. an ABI-gated provider on a device whose ABI it doesn't
     * support). Called fresh each time [TawcInstaller.installInto]
     * runs; results aren't cached.
     */
    fun entries(context: Context): List<TawcInstall>
}
