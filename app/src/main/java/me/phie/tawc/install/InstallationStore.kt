package me.phie.tawc.install

import android.content.Context
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
    val baseDir: File = File(context.dataDir, "distros")

    fun installationDir(id: String): File = File(baseDir, id)
    fun rootfsDir(id: String): File = File(installationDir(id), "rootfs")
    fun metadataFile(id: String): File = File(installationDir(id), "metadata.json")

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
     * Persist [installation] to its `metadata.json`. The write is atomic:
     * we stage the JSON in a sibling `metadata.json.tmp` and `rename(2)`
     * it into place, so a crash mid-write leaves either the old contents
     * or the new ones — never a half-written file that fromJson can't
     * parse. All writes go through this one method (including
     * [setState]'s read-modify-write), and [InstallationService]
     * serialises jobs via `currentJob`, so concurrent writers don't
     * exist.
     */
    fun save(installation: Installation) {
        installationDir(installation.id).mkdirs()
        val finalFile = metadataFile(installation.id)
        val tmpFile = File(finalFile.parentFile, finalFile.name + ".tmp")
        tmpFile.writeText(installation.toJson())
        Files.move(tmpFile.toPath(), finalFile.toPath(), StandardCopyOption.ATOMIC_MOVE)
    }

    /**
     * Update the [Installation.state] field (and optional [failure]
     * detail) for [id], leaving every other field unchanged. The single
     * entry point through which the state machine moves; [InstallationService]
     * is the only caller.
     *
     * If no metadata exists yet the call is a no-op — install transitions
     * call [save] first to lay down the initial record, and uninstall
     * never moves a `(no dir)` slot.
     */
    fun setState(id: String, state: Installation.State, failure: String? = null) {
        val current = load(id) ?: return
        save(current.copy(state = state, failure = failure))
    }

    /**
     * Total bytes used by [id]'s installation dir (rootfs + metadata +
     * enter.sh). Only the `chroot` method puts root-owned files on
     * disk (the rootfs's own uids); `proot` and `tawcroot` are
     * app-uid-owned end-to-end and `du` runs unprivileged. Returns -1
     * on failure (e.g. chroot-method install on a device where `su`
     * was revoked between install and now, or no `su` on PATH at
     * all — DistroInfoActivity then renders "?" instead of crashing).
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
}
