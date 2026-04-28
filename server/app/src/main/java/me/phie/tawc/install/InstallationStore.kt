package me.phie.tawc.install

import android.content.Context
import java.io.File
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

    /**
     * Path to the auto-generated `enter.sh` (mount + chroot wrapper).
     * Sibling of `rootfs/` so it lives in the app-uid-owned dir
     * (writable from in-app code without root) but is invoked via
     * `su -c '<path>'` so root drives the chroot exec.
     */
    fun enterScriptFile(id: String): File = File(installationDir(id), "enter.sh")

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
     * enter.sh). For chroot installs the rootfs is root-owned, so
     * `du` only sees the right size under `su`. For proot installs
     * everything is app-uid-owned and `du` runs without privileges.
     * Returns -1 on failure (e.g. chroot-method install on a device
     * where `su` was revoked between install and now).
     *
     * Blocks on the shell; call from a background dispatcher.
     */
    fun computeSizeBytes(id: String): Long {
        val dir = installationDir(id)
        if (!dir.exists()) return 0L
        val needsRoot = load(id)?.method != Installation.METHOD_PROOT
        val cmd = "du -sk '${dir.absolutePath}' 2>/dev/null | awk '{print \$1}'"
        val output: String = if (needsRoot) {
            val r = Su.run(cmd)
            if (!r.ok) return -1L
            r.output
        } else {
            // Drain stdout BEFORE waitFor — pipe-fills-and-blocks
            // would deadlock if the command output ever grows past
            // the single short line `du -sk … | awk` produces today.
            // Propagate cancellation so DistroInfoActivity's
            // runInterruptible can abort when the user backs out.
            val proc = ProcessBuilder("/system/bin/sh", "-c", cmd)
                .redirectErrorStream(true)
                .start()
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
