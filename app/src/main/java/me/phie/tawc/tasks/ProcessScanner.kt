package me.phie.tawc.tasks

import android.content.Context
import android.os.Process
import android.system.Os
import android.system.OsConstants
import me.phie.tawc.install.ChrootMethod
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore

/**
 * Single source of truth for "what guest processes are running, and
 * which install do they belong to?". Backs both the task manager UI
 * and (via [killAllInRootfs]) the uninstall-time process sweep in
 * [me.phie.tawc.install.RootfsCleaner].
 *
 * One [scan] call covers every install in one pass — the Kotlin
 * walker over `/proc` runs once for the whole system, and the
 * (chroot-only) `su` walker runs at most once per call regardless of
 * how many chroot installs are in the list. No N×install latency.
 *
 * The su path is guarded behind a single `hasChrootInstalls && Su.rootAvailable()`
 * check; everything else is plain Kotlin against the app uid's view of
 * `/proc`. Tawcroot- or proot-only systems never call into [SuProcfsScanner]
 * and never trigger a Magisk prompt. See that file's header for the
 * "build-time disablable" note.
 */
object ProcessScanner {

    /**
     * Snapshot of running guest processes, scoped to [installs] +
     * orphans. Empty list ⇒ no guests running (or no installs to
     * scan).
     */
    data class ScanResult(val processes: List<ProcessInfo>) {
        /** `installId -> processes`; orphans are *not* in this map. */
        fun groupedByInstall(): Map<String, List<ProcessInfo>> =
            processes.filter { it.ownerInstallId != null }
                .groupBy { it.ownerInstallId!! }

        /** Processes belonging to a now-uninstalled rootfs slot. */
        fun orphans(): List<ProcessInfo> =
            processes.filter { it.ownerInstallId == null }
    }

    /**
     * Scan `/proc` and classify every guest process found against the
     * given [installs]. The [context] is used to derive the
     * `<distros>` parent dir for orphan detection.
     */
    fun scan(context: Context, installs: List<Installation>): ScanResult {
        val store = InstallationStore(context)
        val pairs: List<Pair<String, String>> = installs.map { inst ->
            canonicalize(store.rootfsDir(inst.id).absolutePath) to inst.id
        }
        val knownIds = installs.map { it.id }.toSet()
        val orphan = AppUidProcfsScanner.OrphanPattern(
            parentPath = canonicalize(store.baseDir.absolutePath),
            knownIds = knownIds,
        )

        val ownPid = Process.myPid()
        val appProcs = AppUidProcfsScanner.scan(pairs, orphan, ownPid)

        // Only invoke su if there's at least one chroot install. This
        // is the gate that keeps tawcroot-/proot-only systems off the
        // su path.
        val chrootInstalls = installs.filter { it.method == ChrootMethod.KEY }
        val suProcs = if (chrootInstalls.isNotEmpty()) {
            val chrootPairs = chrootInstalls.map {
                canonicalize(store.rootfsDir(it.id).absolutePath) to it.id
            }
            SuProcfsScanner.scan(chrootPairs)
        } else {
            emptyList()
        }

        // Defensive dedupe; prefer app-uid records (killable without su).
        val seen = HashSet<Int>(appProcs.size + suProcs.size)
        val merged = ArrayList<ProcessInfo>(appProcs.size + suProcs.size)
        for (p in appProcs) if (seen.add(p.pid)) merged += p
        for (p in suProcs) if (seen.add(p.pid)) merged += p
        merged.sortBy { it.pid }
        return ScanResult(merged)
    }

    /**
     * Stop one process. Returns true if it's gone by the end of the
     * call (or was already gone).
     *
     * Sends SIGTERM, waits [graceMs], then sends SIGKILL if the pid is
     * still alive. App-uid processes use [Os.kill] directly; root-owned
     * chroot processes send the same signals via `su`.
     */
    fun stop(proc: ProcessInfo, graceMs: Long = 1000): Boolean {
        sendSignal(proc, OsConstants.SIGTERM)
        if (waitForExit(proc.pid, graceMs)) return true
        sendSignal(proc, OsConstants.SIGKILL)
        return waitForExit(proc.pid, graceMs)
    }

    private fun sendSignal(proc: ProcessInfo, signal: Int) {
        if (proc.requiresSu) {
            SuProcfsScanner.kill(proc.pid, signal)
        } else {
            sendSignal(proc.pid, signal)
        }
    }

    private fun sendSignal(pid: Int, signal: Int) {
        try { Os.kill(pid, signal) } catch (_: Throwable) {}
    }

    /**
     * `/proc/<pid>/cwd` reports the canonical path; `Context.dataDir`
     * on modern Android returns `/data/user/0/<pkg>` (a symlink to
     * `/data/data/<pkg>`). Without this normalisation prefix matches
     * miss every guest on every device.
     */
    private fun canonicalize(path: String): String = try {
        java.io.File(path).canonicalPath
    } catch (_: Throwable) {
        path
    }

    /**
     * Spaced 50ms so a fast-exiting process is reported gone within
     * one tick rather than holding the UI for the full grace period.
     */
    private fun waitForExit(pid: Int, budgetMs: Long): Boolean {
        val deadline = System.currentTimeMillis() + budgetMs
        while (true) {
            val alive = try {
                Os.kill(pid, 0)
                true
            } catch (e: android.system.ErrnoException) {
                // ESRCH means gone. EPERM still means a live pid, just
                // one this process cannot signal or probe directly.
                e.errno == OsConstants.EPERM
            } catch (_: Throwable) {
                false
            }
            if (!alive) return true
            if (System.currentTimeMillis() >= deadline) return false
            try { Thread.sleep(50) } catch (_: InterruptedException) {
                Thread.currentThread().interrupt(); return false
            }
        }
    }

    /**
     * Scan + hard-kill every match for [installId] until a scan comes
     * back empty or [maxSweeps] is reached. Repeated sweeps catch
     * fork-on-signal respawns like `pacman-key`'s detached `gpg-agent`
     * and ordinary fork races between scan and kill.
     *
     * Context-free so [me.phie.tawc.install.RootfsCleaner] (which has
     * no [Context]) can call in directly.
     *
     * **Rootfs matches catch guests only, not supervisors.**
     * `cwd`/`exe`/`root` or an executable mapped file inside
     * [rootfsPath] matches the actual programs the user launched
     * inside the chroot/proot/tawcroot; the host-side supervisor
     * process (proot tracer, tawcroot leader pre-`exec`) is not — its
     * `exe` is the supervisor binary in `nativeLibraryDir`, not in
     * the rootfs. Wipe/cancel callers close that gap by passing
     * [extraCmdlinePath]: the supervisor's argv names the rootfs
     * path, so the cmdline match catches it.
     *
     * [includeChroot] gates the `su` branch — pass `true` for chroot
     * installs, `false` for proot / tawcroot (which can't have
     * root-owned guest pids and shouldn't pay the magisk-prompt
     * latency).
     *
     * [extraCmdlinePath] adds an OR-match on `/proc/<pid>/cmdline`
     * substring — [me.phie.tawc.install.RootfsCleaner] and the
     * install-cancel sweep pass the install dir, which also catches
     * out-of-rootfs helpers (`tar`, `find`) launched against it from
     * outside.
     */
    fun killAllInRootfs(
        rootfsPath: String,
        installId: String,
        includeChroot: Boolean,
        extraCmdlinePath: String? = null,
        maxSweeps: Int = 8,
        sweepDelayMs: Long = 250,
        log: (String) -> Unit,
    ): Int {
        val pair = listOf(canonicalize(rootfsPath) to installId)
        var killedCount = 0

        fun killOne(p: ProcessInfo) {
            // Skip the polite SIGTERM — uninstall is forced teardown,
            // the rootfs is about to disappear from under these procs.
            killedCount += 1
            if (p.requiresSu) {
                SuProcfsScanner.kill(p.pid, OsConstants.SIGKILL)
            } else {
                sendSignal(p.pid, OsConstants.SIGKILL)
            }
        }

        fun sweep(): List<ProcessInfo> {
            val app = AppUidProcfsScanner.scan(
                installs = pair,
                orphanPattern = null,
                ownPid = Process.myPid(),
                extraCmdlinePath = extraCmdlinePath,
                extraCmdlineId = installId,
            )
            val su = if (includeChroot) {
                SuProcfsScanner.scan(
                    installs = pair,
                    extraCmdlinePath = extraCmdlinePath,
                    extraCmdlineId = installId,
                )
            } else emptyList()
            val seen = HashSet<Int>(app.size + su.size)
            return (app + su).filter { seen.add(it.pid) }
        }

        val limit = maxSweeps.coerceAtLeast(1)
        var killedAny = false
        for (pass in 1..limit) {
            val procs = sweep()
            if (procs.isEmpty()) {
                if (!killedAny) log("no guest processes to clean up")
                return killedCount
            }
            killedAny = true
            for (p in procs) {
                val prefix = if (pass == 1) "pid" else "rescan#$pass pid"
                log("$prefix=${p.pid} (${p.comm})")
                killOne(p)
            }
            try { Thread.sleep(sweepDelayMs) } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                return killedCount
            }
        }
        val survivors = sweep()
        if (survivors.isNotEmpty()) {
            log("warning: guest processes still present after $limit sweeps")
            for (p in survivors) log("survivor pid=${p.pid} (${p.comm})")
        }
        return killedCount
    }

    /**
     * Exit-time sweep across every installed rootfs plus orphan slots
     * still visible under `<distros>`. Delegates each target to
     * [killAllInRootfs], so this follows the same repeated scan/KILL
     * semantics as task-manager "Stop all" and uninstall cleanup.
     */
    fun killAllKnownRootfs(context: Context, log: (String) -> Unit) {
        val store = InstallationStore(context)
        val installs = store.list()
        val targets = LinkedHashMap<String, RootfsSweepTarget>()

        fun addTarget(id: String, includeChroot: Boolean) {
            if (!Installation.isValidId(id)) {
                log("skip invalid rootfs id '$id'")
                return
            }
            val rootfsPath = store.rootfsDir(id).absolutePath
            val key = canonicalize(rootfsPath)
            val existing = targets[key]
            targets[key] = RootfsSweepTarget(
                rootfsPath = rootfsPath,
                installId = id,
                includeChroot = includeChroot || existing?.includeChroot == true,
            )
        }

        for (inst in installs) {
            addTarget(inst.id, inst.method == ChrootMethod.KEY)
        }
        val hasChrootInstalls = installs.any { it.method == ChrootMethod.KEY }

        // Catch slots with no readable metadata: failed/cancelled
        // deletes can leave a rootfs directory that task-manager still
        // reports as an orphan.
        val knownIds = installs.mapTo(HashSet()) { it.id }
        store.baseDir.listFiles { f -> f.isDirectory }
            ?.map { it.name }
            ?.forEach { id ->
                if (id in knownIds) return@forEach
                if (store.rootfsDir(id).exists()) addTarget(id, includeChroot = hasChrootInstalls)
            }

        // Also include orphan ids currently referenced by running
        // app-uid processes, even if the rootfs directory itself has
        // already been deleted.
        val scan = runCatching { scan(context, installs) }.getOrElse { t ->
            log("warning: process scan failed before exit sweep: ${t.message}")
            ScanResult(emptyList())
        }
        for (proc in scan.orphans()) {
            proc.orphanRootfsId?.let { addTarget(it, includeChroot = hasChrootInstalls) }
        }

        if (targets.isEmpty()) {
            log("no rootfs targets to sweep")
            return
        }
        for (target in targets.values) {
            log("sweep ${target.installId}: ${target.rootfsPath}")
            killAllInRootfs(
                rootfsPath = target.rootfsPath,
                installId = target.installId,
                includeChroot = target.includeChroot,
                log = { log("${target.installId}: $it") },
            )
        }
    }

    private data class RootfsSweepTarget(
        val rootfsPath: String,
        val installId: String,
        val includeChroot: Boolean,
    )
}
