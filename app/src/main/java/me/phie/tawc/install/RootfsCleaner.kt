package me.phie.tawc.install

import me.phie.tawc.tasks.ProcessScanner
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException

/**
 * The one and only path that deletes anything under `<distros>/<id>/`
 * — every install method, every wipe. Each guard below exists exactly
 * once; `RootfsCleanerTripwireTest` enforces the "only deleter" claim
 * by scanning the sources for stray recursive deletes.
 *
 * The two capability facts the sequence branches on (kernel mounts to
 * tear down, root-owned guests/files — both chroot-only today) are
 * derived from the method key recorded in `metadata.json`, NOT from a
 * live [InstallationMethod]: a slot recorded against a method that's
 * disabled in this build must still wipe with the right guards.
 * Missing/corrupt metadata degrades to the rootless path, whose `su`
 * retry still clears a root-owned tree.
 *
 * The install pipeline never wipes (the state-machine gate guarantees
 * `install` only runs against a `(no dir)` slot). The sequence is:
 *
 *   1. **Containment.** The engine computes the target dir itself from
 *      the store and a validated id, so a caller-side path bug can
 *      never hand it `/data/data/me.phie.tawc` or worse.
 *   2. **Kill guests** via [ProcessScanner.killAllInRootfs]. The
 *      canonical leak is the `gpg-agent --daemon` that `pacman-key
 *      --init` detaches; left alive it holds FDs into the rootfs and
 *      races the delete, which on Android 14 spins vold's FUSE
 *      accounting into a `vdc volume abort_fuse` storm bad enough to
 *      ANR system_server. That hazard is method-independent — a
 *      `setsid`'d tawcroot guest from a previous app process is just
 *      as live as a chroot one. `extraCmdlinePath` additionally
 *      catches supervisors (proot tracer) and out-of-rootfs helpers
 *      (`tar`, `find`) whose argv mentions the install dir.
 *   3. **Unmount** via [ChrootMounter.unmount] (mount-master mode),
 *      chroot only.
 *   4. **Mount gate, uniform.** Read `/proc/self/mounts` and refuse to
 *      delete if any mount entry sits under the install dir — every
 *      method, re-checked before every `su` escalation (pass 1 over a
 *      multi-GB tree takes minutes; a mount leaked meanwhile must not
 *      meet a root `find`). For rootless methods a hit means something
 *      external leaked a mount, and a refused wipe is the correct,
 *      loud outcome. This gate — not `find -xdev` — is the
 *      load-bearing protection: `-xdev` only refuses to cross
 *      *filesystems*, and a bind mount with a same-filesystem source
 *      (anything under `/data`, e.g. the app share dir) has the same
 *      `st_dev` and gets walked straight through.
 *   5. **Delete, two passes.** `find -xdev -depth -delete` over the
 *      rootfs subtree (never `rm -rf`: toybox `rm` has no
 *      `--one-file-system`, and `-xdev` is a free extra fence even if
 *      step 4 is the guard), then explicit `metadata.json.tmp` →
 *      `metadata.json` → `rmdir` so a cancel mid-wipe can never strand
 *      the slot without `metadata.json` — the home screen still lists
 *      the slot and a second uninstall picks up cleanly because pass 1
 *      is idempotent. Root-owned trees (chroot) delete via `su`
 *      directly; app-uid trees get a `chmod -R u+rwX` first
 *      (mode-0500 bootstrap dirs, e.g. ca-certificates' `cadir`) and
 *      one `su` retry for root-owned droppings left by interleaved
 *      debug use — root is necessarily available on any device that
 *      could have created them.
 */
object RootfsCleaner {

    /**
     * Wipe `<store.baseDir>/<id>` and everything inside it. Idempotent
     * — a missing dir is a successful wipe, a leftover-from-cancelled-
     * pass-1 dir (rootfs gone, metadata still there) re-runs cleanly.
     * Throws [IOException] on the first sub-step failure; the caller
     * is expected to record `FAILED` state if the dir survives.
     */
    fun wipe(
        store: InstallationStore,
        id: String,
        log: (String) -> Unit = {},
    ) {
        if (!Installation.isValidId(id)) {
            throw IOException("wipe refused: invalid install id '$id'")
        }
        val installDir = store.installationDir(id)
        if (!installDir.exists()) return
        val rootfsPath = File(installDir, "rootfs").absolutePath
        val installPath = installDir.absolutePath
        // Both facts are chroot-only today; a future method that
        // mounts or runs guests as root must be added here.
        val chroot = store.load(id)?.method == ChrootMethod.KEY

        log("kill: guest processes (root=$rootfsPath)")
        try {
            ProcessScanner.killAllInRootfs(
                rootfsPath = rootfsPath,
                installId = id,
                includeChroot = chroot,
                extraCmdlinePath = installPath,
                log = { log("kill: $it") },
            )
        } catch (t: Throwable) {
            // Don't abort — we're about to nuke the rootfs anyway and
            // the mount gate below catches any actual blockers.
            log("kill: warning, ${t.javaClass.simpleName}: ${t.message}")
        }

        if (chroot) {
            log("unmount: $rootfsPath")
            val ur = ChrootMounter.unmount(rootfsPath)
            if (!ur.ok) {
                throw IOException("Unmount refused (active mounts):\n${ur.output}")
            }
        }

        assertNoMountsUnder(installDir)

        if (!chroot) {
            // App-uid unlink needs write permission on the parent dir;
            // `-f` semantics via `; exit 0` since a partial previous
            // attempt may have removed some of the tree already.
            log("chmod: making $installPath writable")
            val cr = Sh.run("chmod -R u+rwX ${Sh.quote(installPath)} 2>/dev/null; exit 0")
            if (!cr.ok) log("chmod: warning, exit=${cr.exitCode}")
        }

        // Pass 1: rootfs only. The bulk of the deletion lives here. If
        // a cancel arrives we want to tip over before metadata.json
        // gets touched. `>/dev/null` because toybox `find -delete`
        // prints every deleted path on stdout — thousands of lines on
        // a cancel path; stderr (real errors) still reaches the
        // captured output shown on failure. No per-line `onLine` for
        // the same reason.
        if (File(rootfsPath).exists()) {
            log("rm: rootfs subtree at $rootfsPath")
            deletePass(
                script = "find ${Sh.quote(rootfsPath)} -xdev -depth -delete >/dev/null",
                chroot = chroot,
                installDir = installDir,
                gone = { !File(rootfsPath).exists() },
                what = "rootfs delete",
                log = log,
            )
        }

        // Pass 2: explicit ordering — any half-written
        // `metadata.json.tmp` first, then `metadata.json`, then
        // `rmdir`. `find -depth` leaves readdir-order between siblings
        // undefined, which means a cancel between two arbitrary
        // unlinks could orphan the slot (installDir present,
        // metadata.json gone — invisible on the home screen). Explicit
        // order makes metadata.json the second-to-last visible
        // artefact, with only the empty dir remaining and `rmdir`
        // finishing the job near-atomically.
        log("rm: container at $installPath (metadata.json, rmdir)")
        deletePass(
            script = buildString {
                appendLine("rm -f ${Sh.quote("$installPath/metadata.json.tmp")}")
                appendLine("rm -f ${Sh.quote("$installPath/metadata.json")}")
                appendLine("rmdir ${Sh.quote(installPath)}")
            },
            chroot = chroot,
            installDir = installDir,
            gone = { !installDir.exists() },
            what = "metadata delete",
            log = log,
        )
    }

    /**
     * Refuse to delete while any mount entry sits at or under [dir].
     * Reads the app's own mounts view; on rooted devices a
     * global-namespace mount could in principle be invisible here, but
     * the only method that can create such mounts is chroot, whose
     * [ChrootMounter.unmount] verifies via `su -mm` before this gate
     * runs. Do not add more namespace checks than that.
     */
    private fun assertNoMountsUnder(dir: File) {
        val canon = try { dir.canonicalPath } catch (_: IOException) { dir.absolutePath }
        val hits = File("/proc/self/mounts").readLines().mapNotNull { line ->
            val mountPoint = line.split(' ').getOrNull(1) ?: return@mapNotNull null
            unescapeMountField(mountPoint)
                .takeIf { it == canon || it.startsWith("$canon/") }
        }
        if (hits.isNotEmpty()) {
            throw IOException(
                "wipe refused: active mount(s) under $canon:\n" + hits.joinToString("\n")
            )
        }
    }

    /** `/proc/self/mounts` octal-escapes space/tab/newline/backslash. */
    private fun unescapeMountField(s: String): String {
        if ('\\' !in s) return s
        val sb = StringBuilder(s.length)
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (c == '\\' && i + 3 < s.length) {
                val v = s.substring(i + 1, i + 4).toIntOrNull(8)
                if (v != null) {
                    sb.append(v.toChar()); i += 4; continue
                }
            }
            sb.append(c); i++
        }
        return sb.toString()
    }

    /**
     * Run one delete pass. Root-owned trees (chroot) go straight to
     * `su` — an app-uid attempt over a root-owned rootfs is doomed and
     * noisy. App-uid trees try as the app uid first (no Magisk prompt
     * on a properly rootless install) with one `su` retry for
     * root-owned droppings; on a device with no root the app-uid
     * failure is final.
     *
     * Before every `su` escalation the mount gate is re-checked (a
     * root `find` ignores DAC and pass 1 takes minutes — a mount
     * leaked since the wipe-level check must refuse, not delete
     * through) and a pending cancel is honoured (the cancel kill
     * sweep can fell the app-uid `find` before the thread interrupt
     * lands, which would otherwise read as a failure and escalate the
     * very delete the user cancelled).
     */
    private fun deletePass(
        script: String,
        chroot: Boolean,
        installDir: File,
        gone: () -> Boolean,
        what: String,
        log: (String) -> Unit,
    ) {
        fun preSuCheck() {
            if (Thread.interrupted()) {
                throw InterruptedIOException("$what interrupted before su escalation")
            }
            assertNoMountsUnder(installDir)
        }
        if (chroot) {
            preSuCheck()
            val r = Su.run(script)
            if (!r.ok || !gone()) {
                throw IOException("$what failed (exit=${r.exitCode}):\n${r.output}")
            }
            return
        }
        val r = Sh.run(script)
        if (r.ok && gone()) return
        if (Su.rootAvailable()) {
            preSuCheck()
            log("rm: app-uid $what failed, retrying via su")
            val sr = Su.run(script)
            if (sr.ok && gone()) return
            throw IOException("$what failed (su retry exit=${sr.exitCode}):\n${sr.output}")
        }
        throw IOException("$what failed (exit=${r.exitCode}):\n${r.output}")
    }
}
