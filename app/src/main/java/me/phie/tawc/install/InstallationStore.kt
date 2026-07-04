package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.AppPaths
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * On-disk layout for per-distro installations under
 * `<app data>/distros/<id>/`. The bootstrap-tarball cache at
 * `<cacheDir>/install/` is owned by [BootstrapCache], not this class —
 * see notes/installation.md for the full layout.
 *
 * The default install id is `arch`. Future multi-install support just
 * varies the id; nothing else here cares.
 */
class InstallationStore(context: Context) {
    val baseDir: File = AppPaths.from(context).distrosDir

    fun installationDir(id: String): File = File(baseDir, id)
    fun rootfsDir(id: String): File = File(installationDir(id), "rootfs")
    fun metadataFile(id: String): File = File(installationDir(id), "metadata.json")

    /**
     * Per-distro ando broker dir — sibling of `rootfs/`, so uninstall
     * removes it, and (crucially) OUTSIDE the wholesale-bound share dir
     * so no other distro's guest can enumerate it. Holds the listening
     * socket [andoSocket]; bound into the guest at
     * [TawcrootMethod.GUEST_ANDO_DIR] only when ando is enabled.
     * See notes/ando.md.
     */
    fun andoDir(id: String): File = File(installationDir(id), "ando")
    fun andoSocket(id: String): File = File(andoDir(id), "ando.sock")

    /**
     * The install id owning [rootfs] (`<distros>/<id>/rootfs`), or null
     * if [rootfs] isn't a known install's rootfs dir. Lets spawn paths
     * derive per-distro settings from the rootfs alone without threading
     * [Installation] through their call chains (same trick
     * [TawcrootMethod.externalBindsFor] uses).
     */
    fun idForRootfs(rootfs: String): String? {
        val id = File(rootfs).absoluteFile.parentFile?.name ?: return null
        return if (rootfsDir(id).absolutePath == File(rootfs).absolutePath) id else null
    }

    /**
     * Whether [id] may use ando, honoring the test-mode override
     * ([setAndoOverride]) over the persisted [Installation.andoEnabled].
     * Read per-spawn (bind emission) and by [me.phie.tawc.AndoBrokers].
     * Default `false`: fail-closed, opt-in.
     */
    fun andoEnabled(id: String): Boolean =
        andoOverrides[id] ?: (load(id)?.andoEnabled ?: false)

    /** As [andoEnabled], but for an already-loaded record — skips the
     *  metadata re-read when the caller already holds the [Installation]. */
    fun andoEnabled(inst: Installation): Boolean =
        andoOverrides[inst.id] ?: inst.andoEnabled

    /**
     * The host ando dir for the install owning [rootfs] when ando is
     * enabled for it, else null. Creates the dir (so a bind src / broker
     * bind can open it) and honors the test override. Single resolution
     * point for the tawcroot/proot/chroot bind builders. See notes/ando.md.
     */
    fun andoHostDir(rootfs: String): String? {
        val id = idForRootfs(rootfs) ?: return null
        if (!andoEnabled(id)) return null
        return andoDir(id).apply { mkdirs() }.absolutePath
    }

    /** Discover installations on disk by scanning [baseDir]. */
    fun list(): List<Installation> {
        val dir = baseDir
        if (!dir.exists()) return emptyList()
        return dir.listFiles { f -> f.isDirectory }
            ?.mapNotNull { d ->
                val meta = File(d, "metadata.json")
                if (!meta.exists()) return@mapNotNull null
                runCatching { Installation.fromJson(meta.readText()) }.getOrNull()
            }
            ?.sortedBy { it.id }
            ?: emptyList()
    }

    fun load(id: String): Installation? {
        val f = metadataFile(id)
        if (!f.exists()) return null
        return runCatching { Installation.fromJson(f.readText()) }.getOrNull()
    }

    /**
     * Persist [installation], creating its slot if absent. The write is
     * atomic: we stage the JSON in a sibling `metadata.json.tmp` and
     * `rename(2)` it into place, so a crash mid-write leaves either the
     * old contents or the new ones — never a half-written file that
     * fromJson can't parse.
     *
     * This is the *create* entry point ([Installer] lays down the
     * initial INSTALLING record here). Every read-modify-write of an
     * existing record must go through [update] instead, which re-reads
     * the record under the same per-id lock this method takes — a bare
     * `save(load(id).copy(...))` from two writers would still let the
     * loser silently revert the winner's fields. The lock is reentrant
     * ([update] calls this while holding it), so nesting is safe.
     */
    fun save(installation: Installation) {
        synchronized(lockFor(installation.id)) {
            installationDir(installation.id).mkdirs()
            val finalFile = metadataFile(installation.id)
            val tmpFile = File(finalFile.parentFile, finalFile.name + ".tmp")
            tmpFile.writeText(installation.toJson())
            Files.move(tmpFile.toPath(), finalFile.toPath(), StandardCopyOption.ATOMIC_MOVE)
        }
    }

    /**
     * Read-modify-write [id]'s metadata under a per-id in-process lock —
     * the one safe way to change a field of an existing record.
     *
     * [mutate] receives the record re-loaded *inside* the lock (so it
     * already reflects any concurrent writer that committed first) and
     * returns the record to persist, or `null` to abort the write (e.g.
     * a state gate that no longer holds). Returns the persisted record,
     * or `null` when the metadata is gone or [mutate] aborted.
     *
     * A missing file is never recreated: a writer racing an uninstall
     * (whose [RootfsCleaner.wipe] deletes `metadata.json`) must not
     * resurrect a wiped slot as a ghost with no rootfs. Callers that
     * gate on READY/FAILED get this for free — a slot mid-uninstall
     * reads UNINSTALLING inside the lock, and their [mutate] returns
     * null. All in-process writers serialise on the same lock, so the
     * load→save window that used to drop the loser's edit is closed.
     *
     * Note the lock is in-process only; it does not guard against the
     * out-of-lock unlink `wipe` performs, but the re-read-then-refuse
     * above covers the case that unlink beat us to it.
     */
    fun update(id: String, mutate: (Installation) -> Installation?): Installation? {
        synchronized(lockFor(id)) {
            val current = load(id) ?: return null
            val next = mutate(current) ?: return null
            save(next)
            return next
        }
    }

    /**
     * Move [id]'s [Installation.state] (and optional [failure] detail),
     * leaving every other field unchanged. The single entry point
     * through which the state machine moves; [InstallationService]/
     * [Installer] are the only callers.
     *
     * A no-op if no metadata exists yet — install transitions call
     * [save] first to lay down the initial record, and uninstall never
     * moves a `(no dir)` slot.
     */
    fun setState(id: String, state: Installation.State, failure: String? = null) {
        update(id) { it.copy(state = state, failure = failure) }
    }

    /**
     * Total bytes used by [id]'s installation dir (rootfs + metadata).
     * Only the `chroot` method puts root-owned files on disk (the
     * rootfs's own uids); `proot` and `tawcroot` are app-uid-owned
     * end-to-end and `du` runs unprivileged. Returns -1 on failure
     * (e.g. chroot-method install on a device where `su` was revoked
     * between install and now, or no `su` on PATH at all —
     * DistroInfoActivity then renders "?" instead of crashing).
     *
     * Blocks on the shell; call from a background dispatcher.
     */
    fun computeSizeBytes(id: String): Long {
        val dir = installationDir(id)
        if (!dir.exists()) return 0L
        val needsRoot = load(id)?.method == Installation.METHOD_CHROOT
        val cmd = "du -sk '${dir.absolutePath}' 2>/dev/null | awk '{print \$1}'"
        val output: String = if (needsRoot) {
            // ProcessBuilder("su").start() throws IOException with
            // "Permission denied" or "No such file" on devices where su
            // isn't reachable from the app uid (rootless, or Magisk
            // policy hasn't granted us yet). Without this catch the
            // exception bubbles out of runInterruptible and crashes
            // the activity.
            val r = try {
                Su.run(cmd)
            } catch (_: IOException) {
                return -1L
            }
            if (!r.ok) return -1L
            r.output
        } else {
            // Drain stdout BEFORE waitFor — pipe-fills-and-blocks
            // would deadlock if the command output ever grows past
            // the single short line `du -sk … | awk` produces today.
            // Propagate cancellation so DistroInfoActivity's
            // runInterruptible can abort when the user backs out.
            val proc = try {
                ProcessBuilder("/system/bin/sh", "-c", cmd)
                    .redirectErrorStream(true)
                    .start()
            } catch (_: IOException) {
                return -1L
            }
            val captured = proc.inputStream.bufferedReader().use { it.readText() }
            try {
                proc.waitFor()
            } catch (e: InterruptedException) {
                proc.destroyForcibly()
                Thread.currentThread().interrupt()
                throw e
            }
            if (proc.exitValue() != 0) return -1L
            captured
        }
        val kb = output.trim().toLongOrNull() ?: return -1L
        return kb * 1024L
    }

    companion object {
        // Per-id write locks. InstallationStore is constructed ad-hoc
        // wherever metadata is touched, so the locks must be process-
        // global (here) rather than instance state, or two `new
        // InstallationStore(ctx)` for the same id wouldn't exclude each
        // other. Keyed by install id; entries are never removed (ids are
        // few and short — an uninstalled-then-reinstalled id just reuses
        // its monitor).
        private val locks = java.util.concurrent.ConcurrentHashMap<String, Any>()

        private fun lockFor(id: String): Any = locks.computeIfAbsent(id) { Any() }

        // Test-only in-memory per-id ando override (notes/ando.md).
        // Mirrors Settings.enterTestMode: the `set-ando` broker action
        // writes here so integration tests can flip ando without a
        // durable metadata write; [andoEnabled] and
        // [me.phie.tawc.AndoBrokers] read through it, and app-process
        // death discards it. Empty (no override) in production.
        private val andoOverrides = java.util.concurrent.ConcurrentHashMap<String, Boolean>()

        /** Test hook: force [andoEnabled] for [id] regardless of metadata. */
        fun setAndoOverride(id: String, enabled: Boolean) {
            andoOverrides[id] = enabled
        }

        /** Drop [id]'s override (uninstall path — a stale override must
         *  not resurrect ando on a later reinstall of the same id). */
        fun clearAndoOverride(id: String) {
            andoOverrides.remove(id)
        }

        /** Test hook: drop all ando overrides, restoring metadata-backed state. */
        fun clearAndoOverrides() {
            andoOverrides.clear()
        }
    }
}
