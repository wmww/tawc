package me.phie.tawc.install

import android.util.Log
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.NoSuchFileException
import java.nio.file.attribute.BasicFileAttributes

/**
 * Age-based sweep of `<rootfs>/tmp` for every install, run from
 * [me.phie.tawc.TawcApplication]'s startup thread.
 *
 * `/tmp` in every install method is a plain directory on flash, not
 * tmpfs — the rootfs has no init, so nothing ever clears it, and
 * [RootfsEnv] points both `TMPDIR` and `XDG_RUNTIME_DIR` at it, so
 * runtime sockets and gpg-agent state accumulate against app storage
 * across sessions and reboots. A real tmpfs is not reachable
 * rootlessly (`mount()` needs CAP_SYS_ADMIN, SELinux blocks user
 * namespaces for untrusted apps, no app-writable tmpfs to bind from),
 * so we sweep instead.
 *
 * Age-based rather than a full clear: `setsid`'d guests from a
 * previous app process can outlive it (Android's phantom-process
 * killer is neither prompt nor reliable), and yanking a live guest's
 * `XDG_RUNTIME_DIR` out from under it is the failure mode to avoid.
 * Aging is mtime-only, so it shrinks that hazard rather than
 * eliminating it: socket inodes get their mtime at `bind()` and
 * never again on traffic (atime/ctime don't move either, so wider
 * timestamps wouldn't help), meaning a guest session that lives past
 * the threshold loses its runtime sockets at the next app start.
 * Accepted residual risk — multi-day survivors are rare under the
 * phantom-process killer. Run at app start, not per spawn: spawns
 * happen while other sessions are live, and a "no session active"
 * signal doesn't exist.
 *
 * Best-effort: per-entry failures are counted, summarised in one log
 * line, and never abort startup. Known case: chroot guests run as
 * real root and can leave files the app uid can't delete —
 * acceptable for a debug-only method.
 */
object RootfsTmpSweeper {
    private const val TAG = "tawc-install"

    /** Entries untouched for longer than this are swept. */
    private const val MAX_AGE_MS = 3L * 24 * 60 * 60 * 1000

    /**
     * Skipped by name at the top level: tawcroot treats it as a bind
     * key, chroot has a real bind mount there, and the backing
     * `share/xtmp` dir is shared across spawn surfaces. Not worth
     * mount detection for one well-known path.
     */
    private const val X11_DIR = ".X11-unix"

    /** Sweep every install's `<rootfs>/tmp`; one summary log line. */
    fun sweepAll(store: InstallationStore) {
        val cutoff = System.currentTimeMillis() - MAX_AGE_MS
        var deleted = 0
        var failed = 0
        for (inst in store.list()) {
            // Same containment rule as RootfsCleaner: never compute a
            // delete target from an unvalidated id (metadata could be
            // corrupt).
            if (!Installation.isValidId(inst.id)) continue
            val tmp = File(store.rootfsDir(inst.id), "tmp")
            if (!tmp.isDirectory) continue
            val stats = sweep(tmp, cutoff)
            deleted += stats.deleted
            failed += stats.failed
        }
        if (deleted > 0 || failed > 0) {
            Log.i(TAG, "rootfs /tmp sweep: deleted=$deleted failed=$failed")
        }
    }

    data class Stats(var deleted: Int = 0, var failed: Int = 0)

    /**
     * Delete entries under [tmpDir] whose lstat mtime is before
     * [cutoffMs]: old non-directories first, then directories that
     * were already old *and* ended up empty. [tmpDir] itself is never
     * deleted. Symlinks are unlinked, not followed — modulo a
     * dir→symlink swap between lstat and the recursing `listFiles`,
     * a TOCTOU race only same-uid guest code (which can already
     * delete anything app-uid-writable) could stage; accepted.
     */
    fun sweep(tmpDir: File, cutoffMs: Long): Stats {
        val stats = Stats()
        for (child in tmpDir.listFiles().orEmpty()) {
            if (child.name == X11_DIR) continue
            sweepEntry(child, cutoffMs, stats)
        }
        return stats
    }

    private fun sweepEntry(f: File, cutoffMs: Long, stats: Stats) {
        // lstat — never follow symlinks, and treat a link-to-dir as a
        // plain entry (unlink the link, don't recurse into the target).
        val attrs = try {
            Files.readAttributes(
                f.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS
            )
        } catch (_: NoSuchFileException) {
            return // a live guest removed it first — not a failure
        } catch (_: Exception) {
            stats.failed++
            return
        }
        // Capture the age before recursing: unlinking a child bumps
        // the parent dir's mtime, so a dir's "old" verdict must come
        // from its pre-sweep state.
        val old = attrs.lastModifiedTime().toMillis() < cutoffMs
        if (attrs.isDirectory) {
            for (child in f.listFiles().orEmpty()) {
                sweepEntry(child, cutoffMs, stats)
            }
            if (old && f.listFiles().orEmpty().isEmpty()) {
                delete(f, stats)
            }
        } else if (old) {
            delete(f, stats)
        }
    }

    private fun delete(f: File, stats: Stats) {
        try {
            // deleteIfExists: a lost race with a live guest is
            // neither a delete nor a failure.
            if (Files.deleteIfExists(f.toPath())) stats.deleted++
        } catch (_: Exception) {
            stats.failed++
        }
    }
}
