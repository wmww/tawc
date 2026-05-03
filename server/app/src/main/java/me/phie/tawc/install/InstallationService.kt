package me.phie.tawc.install

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.runInterruptible
import me.phie.tawc.MainActivity
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroRegistry

/**
 * Foreground service that runs install / uninstall jobs in a coroutine
 * **and** enforces the installation state machine — the single gate
 * through which `<distros>/<id>/` is mutated.
 *
 * The transition table lives in `notes/installation.md`; the short
 * version is:
 *
 *   install:    only allowed from `(no dir)`
 *   uninstall:  allowed from every state but `(no dir)` (which is a no-op)
 *
 * Both UI ([InstallActivity] / [UninstallActivity]) and `am start`
 * autoStart are inputs; this service decides whether to actually run.
 * On a refused request we emit a [InstallStage.FAILED] progress event
 * so the bound UI can surface the rejection — disk state is unchanged.
 *
 * On a running operation success the wrapper writes
 * [Installation.State.READY] / removes the dir; on a throw we write
 * [Installation.State.FAILED] with the message so the user can see
 * what went wrong and decide whether to uninstall.
 *
 * ### Cancellation
 *
 * The job body runs inside `runInterruptible(Dispatchers.IO)` so a
 * coroutine cancel translates into a thread interrupt that the
 * blocking IO callers ([Su.run], [ProotMethod.runShell],
 * [Downloader.download]) all honour by destroying their subprocesses
 * / re-throwing `InterruptedIOException`. The state-machine
 * consequences:
 *   - **Cancel install**: the job throws `CancellationException`,
 *     the outer catch parks the slot in `FAILED`, and we then start
 *     an uninstall (`INSTALLING → FAILED → UNINSTALLING → (no dir)`).
 *     No data loss because the install gate guarantees an empty slot
 *     on entry — the only files in the rootfs are bootstrap + freshly
 *     installed packages, none of which the user has put their work
 *     into yet.
 *   - **Cancel uninstall**: the job throws, the outer catch parks
 *     the slot in `FAILED` (since the dir may still exist), and we
 *     stop. The user can re-trigger uninstall from the home screen
 *     to finish, or pull leftover files manually before doing so.
 *
 * Subprocess descendants spawned via `su` are not always reliably
 * killed by `proc.destroyForcibly()` of the immediate `su` client
 * (Magisk's su forwards through magiskd), so [runCancelKillScript]
 * supplements the thread interrupt with an explicit /proc sweep that
 * SIGKILLs anything chrooted into the rootfs *or* whose argv mentions
 * the install path (catches the host-side `tar` / `find` helpers).
 *
 * The cancel-install-then-uninstall flow stays in the foreground
 * across the install→uninstall transition: install's `finally` skips
 * `stopForeground` while [pendingFollowupUninstallId] is set, and the
 * cancel-launch refreshes the notification before kicking off the
 * follow-up uninstall. This keeps the service safe from
 * out-of-memory kill during the gap.
 */
class InstallationService : Service() {

    enum class JobKind { INSTALL, UNINSTALL }

    private data class JobState(val job: Job, val id: String, val kind: JobKind)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /**
     * Single source of truth for "is a job in flight". Volatile so
     * the cancel methods (called from the UI thread) and the cancel-
     * launch coroutine see a coherent value without surprise from JIT
     * reordering. Cleared in the job's `finally`.
     */
    @Volatile private var currentJob: JobState? = null

    /**
     * Set by [cancelInstallAndUninstall] before cancelling, cleared
     * after the follow-up uninstall has been kicked off. Two roles:
     *   1. Cancel idempotency — a second tap during the
     *      install→uninstall transition is a no-op.
     *   2. FGS continuity — the install's `finally` skips
     *      `stopForeground` while this is set so we don't drop out of
     *      foreground between the two operations.
     */
    @Volatile private var pendingFollowupUninstallId: String? = null

    /**
     * Set by [cancelInstallAndUninstall] / [cancelUninstall] before
     * cancelling the job, so the catch handler can write a
     * "Cancelled by user" failure message instead of dumping the
     * `CancellationException` stack trace as the failure detail.
     */
    @Volatile private var userCancelledId: String? = null

    /**
     * Non-null when the uninstall in flight is the follow-up triggered
     * by [cancelInstallAndUninstall], holding that uninstall's id.
     * Used in [publishProgress] to relabel the terminal `DONE` status
     * from "Deleted" to "Install cancelled" — the user didn't ask to
     * delete anything, they asked to abort an install.
     *
     * Keyed by id rather than a boolean so a refused [startUninstall]
     * (invalid id, or a parallel job racing in) leaves a stale value
     * naturally desynced from the next *real* uninstall's id, instead
     * of silently relabeling some unrelated future delete as a cancel.
     */
    @Volatile private var installCancelTailUninstallId: String? = null

    private val binder = LocalBinder()

    private val _progress = MutableStateFlow(
        InstallProgress(InstallStage.IDLE, "Idle")
    )
    val progress: StateFlow<InstallProgress> = _progress.asStateFlow()

    private val _log = MutableSharedFlow<String>(replay = 200, extraBufferCapacity = 1024)
    val log: SharedFlow<String> = _log.asSharedFlow()

    /**
     * Drop every replay-buffered log line so a freshly-binding panel
     * can't see a previous operation's tail. Called by
     * [OperationLogPanel.clearLog] alongside its local TextView wipe
     * to close the bind→collect→wipe race; also called internally at
     * the start of every job for the new-subscriber case.
     */
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    fun resetLogReplay() {
        _log.resetReplayCache()
    }

    private var lastLoggedStage: InstallStage? = null

    inner class LocalBinder : Binder() {
        val service: InstallationService get() = this@InstallationService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ensureChannel()
        startForeground(NOTIFICATION_ID, buildNotification("tawc"))
        when (intent?.action) {
            ACTION_INSTALL -> startInstall(
                intent.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH,
                intent.getStringExtra(EXTRA_METHOD),
                intent.getStringExtra(EXTRA_DISTRO),
                intent.getStringExtra(EXTRA_LABEL),
            )
            ACTION_UNINSTALL -> startUninstall(intent.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH)
            else -> Log.w(TAG, "InstallationService started without a known action")
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    /** The kind of the in-flight job, or `null` if idle. UI uses this to dispatch cancel taps. */
    val currentKind: JobKind? get() = currentJob?.kind

    /**
     * Begin an install for [id] using [methodKey] (or auto-pick if
     * null) and [distroKey] (or the host default if null). [label] is
     * the user-set display name persisted in metadata; null means "fall
     * back to the registry displayName". Refuses if the gate forbids it.
     */
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    fun startInstall(
        id: String,
        methodKey: String? = null,
        distroKey: String? = null,
        label: String? = null,
    ) {
        if (!Installation.isValidId(id)) {
            reject("install '$id'", "invalid id (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            return
        }
        if (currentJob?.job?.isActive == true) {
            reject("install '$id'", "another job is already running")
            return
        }
        // Reset the log replay buffer at the start of each operation so
        // a freshly-bound panel sees only this run's lines, not stale
        // history from the previous install/uninstall (replay=200 would
        // otherwise leak across operations).
        _log.resetReplayCache()
        lastLoggedStage = null
        val store = InstallationStore(applicationContext)
        when (val s = store.load(id)?.state) {
            null -> Unit  // (no dir) — proceed
            Installation.State.READY,
            Installation.State.INSTALLING,
            Installation.State.UNINSTALLING,
            Installation.State.FAILED -> {
                reject("install '$id'", "id is in state $s; uninstall first")
                return
            }
        }
        // Resolve the requested distro (or fall back to the host
        // default) before any disk state is written so an unsupported
        // ABI / unknown distro key is a clean reject rather than a
        // half-installed FAILED slot.
        val distro = if (distroKey != null) {
            DistroRegistry.forKey(distroKey) ?: run {
                reject(
                    "install '$id'",
                    "unknown or host-incompatible distro '$distroKey' " +
                        "(available: ${DistroRegistry.availableForHost().joinToString { it.key }})",
                )
                return
            }
        } else {
            DistroRegistry.defaultForHost()
        }
        if (distro == null) {
            reject("install '$id'", "no Distro supports ABI ${android.os.Build.SUPPORTED_ABIS.joinToString(",")}")
            return
        }
        // Resolve the install method (chroot vs proot). An explicit
        // bad key is a clean reject; null falls back to the host
        // default (chroot if `su` works, proot otherwise — see
        // [InstallationMethod.defaultForHost]).
        val method = if (methodKey != null) {
            InstallationMethod.forKey(applicationContext, methodKey) ?: run {
                reject("install '$id'", "unknown method '$methodKey' (try chroot or proot)")
                return
            }
        } else {
            InstallationMethod.defaultForHost(applicationContext)
        }
        if (!method.isAvailable(applicationContext)) {
            reject("install '$id'", "method '${method.key}' is not available on this device")
            return
        }
        val job = scope.launch {
            val installer = Installer(
                applicationContext, store, BootstrapCache(applicationContext),
                distro, method, id, label,
            )
            try {
                // runInterruptible maps coroutine cancellation onto a
                // thread interrupt so the blocking calls inside
                // installer.install (Su.run, Downloader, tar/pacman
                // shells) actually abort instead of running to
                // completion ignoring the cancel flag.
                runInterruptible(Dispatchers.IO) {
                    installer.install(::publishProgress, ::appendLog)
                }
            } catch (t: Throwable) {
                handleInstallThrow(store, id, t)
            } finally {
                // Skip stopForeground when a follow-up uninstall is
                // queued — we need to stay FGS through the
                // transition or Android can reap the service before
                // the uninstall lands.
                if (pendingFollowupUninstallId != id) {
                    stopForeground(STOP_FOREGROUND_REMOVE)
                }
                clearCurrentJob(id)
            }
        }
        currentJob = JobState(job, id, JobKind.INSTALL)
    }

    /** Begin an uninstall for [id]. Refuses only if a job is already running. */
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    fun startUninstall(id: String) {
        if (!Installation.isValidId(id)) {
            reject("uninstall '$id'", "invalid id (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            return
        }
        if (currentJob?.job?.isActive == true) {
            reject("uninstall '$id'", "another job is already running")
            return
        }
        // Same fresh-log policy as install: each operation starts the
        // log from empty so the bound panel never inherits the previous
        // run's lines.
        _log.resetReplayCache()
        lastLoggedStage = null
        val store = InstallationStore(applicationContext)
        // Uninstall doesn't need a Distro — `method.wipe(...)` is
        // distro-agnostic. Resolve one for symmetry / future logging,
        // falling back to the host default and ultimately to the
        // first registered distro if no metadata exists. The Installer
        // never invokes Distro.* on the uninstall path.
        val distro: Distro = store.load(id)?.let { DistroRegistry.forInstallation(it) }
            ?: DistroRegistry.defaultForHost()
            ?: DistroRegistry.all.first()
        // Method is recorded in metadata at install time; honour what's
        // there, with the host default as last-resort fallback for
        // (rare) records missing the field.
        val method: InstallationMethod = store.load(id)?.let {
            InstallationMethod.forKey(applicationContext, it.method)
        } ?: InstallationMethod.defaultForHost(applicationContext)
        val job = scope.launch {
            val installer = Installer(
                applicationContext, store, BootstrapCache(applicationContext),
                distro, method, id,
            )
            try {
                runInterruptible(Dispatchers.IO) {
                    installer.uninstall(::publishProgress, ::appendLog)
                }
            } catch (t: Throwable) {
                handleUninstallThrow(store, id, t)
            } finally {
                stopForeground(STOP_FOREGROUND_REMOVE)
                clearCurrentJob(id)
            }
        }
        currentJob = JobState(job, id, JobKind.UNINSTALL)
    }

    /**
     * Dispatch a cancel based on the current job kind. UI panel
     * doesn't need to know whether install or uninstall is in flight
     * — it just calls this.
     *
     * Returns the kind that was cancelled (so the caller knows
     * whether to render a follow-up dialog), or `null` if there was
     * nothing to cancel.
     */
    fun cancelCurrent(id: String): JobKind? {
        val state = currentJob ?: return null
        if (state.id != id || !state.job.isActive) return null
        return when (state.kind) {
            JobKind.INSTALL -> { cancelInstallAndUninstall(id); JobKind.INSTALL }
            JobKind.UNINSTALL -> { cancelUninstall(id); JobKind.UNINSTALL }
        }
    }

    /**
     * Cancel the in-flight install for [id] and immediately start an
     * uninstall once it's finished. Per the state machine,
     * `INSTALLING → uninstall` is a valid edge (and `INSTALLING →
     * FAILED → uninstall` is too) — the gate accepts the second op
     * once the first job is no longer active.
     *
     * Idempotent: a second call while [pendingFollowupUninstallId]
     * is set is a no-op (the user double-tapped Cancel; we already
     * have the right thing in flight).
     */
    fun cancelInstallAndUninstall(id: String) {
        val state = currentJob
        if (state == null || !state.job.isActive || state.id != id || state.kind != JobKind.INSTALL) {
            // Don't fall through to startUninstall — a confirm-and-
            // cancel tap that arrives after the install has already
            // completed shouldn't accidentally wipe a freshly READY
            // slot.
            appendLog("[cancel] no install for '$id' to cancel")
            return
        }
        if (pendingFollowupUninstallId == id) {
            appendLog("[cancel] cancel-and-uninstall already pending for '$id'")
            return
        }
        pendingFollowupUninstallId = id
        appendLog("[cancel] cancelling install of '$id'; will uninstall after job finishes")
        userCancelledId = id
        // Fire defensive process-tree kill in parallel: cancellation
        // propagates through runInterruptible / Su.run, but Magisk's
        // su client doesn't reliably forward signals to the remote
        // shell's descendants (tar / pacman / find) — those land as
        // orphans. The /proc sweep below SIGKILLs them by chroot
        // dev:inode and by argv-mentions-install-path so we can't
        // race the follow-up uninstall extracting more bytes into the
        // rootfs after we've decided to wipe it.
        scope.launch { runCancelKillScript(id) }
        state.job.cancel(CancellationException("cancelled by user"))
        // Wait for the install's catch handler + finally to run
        // (writes FAILED, releases currentJob), then start uninstall
        // — the gate refuses while currentJob is active so we can't
        // skip the join. Re-throw any CancellationException reaching
        // us (i.e. the service scope itself was cancelled, e.g.
        // onDestroy) so we don't queue an uninstall against a dead
        // scope.
        scope.launch {
            try {
                state.job.join()
            } catch (e: CancellationException) {
                throw e
            } catch (_: Throwable) {
                /* job already finished one way or another */
            }
            // Refresh the FGS notification text before launching the
            // follow-up. The install's finally skipped its own
            // stopForeground because pendingFollowupUninstallId is
            // still set, so we're already in the foreground state —
            // this just updates the visible message.
            startForeground(NOTIFICATION_ID, buildNotification("Cancelling install: cleaning up…"))
            pendingFollowupUninstallId = null
            installCancelTailUninstallId = id
            startUninstall(id)
        }
    }

    /**
     * Cancel the in-flight uninstall for [id]. Per the state machine
     * (`UNINSTALLING → FAILED` on a wipe failure), the catch handler
     * already writes FAILED if the dir survives — and with the two-
     * pass wipe in [RootfsCleaner.wipe] / [ProotMethod.wipe] the
     * `metadata.json` survives a cancel mid-pass-1, so the slot stays
     * recognisable on the home screen for a manual recovery.
     *
     * No confirm dialog at the activity layer (a quick double-tap of
     * Cancel might be the user trying to save a chroot they didn't
     * mean to delete; we don't want a second dialog in the way).
     */
    fun cancelUninstall(id: String) {
        val state = currentJob
        if (state == null || !state.job.isActive || state.id != id || state.kind != JobKind.UNINSTALL) {
            appendLog("[cancel] no uninstall for '$id' to cancel")
            return
        }
        if (userCancelledId == id) {
            appendLog("[cancel] uninstall cancel already in flight for '$id'")
            return
        }
        appendLog("[cancel] cancelling uninstall of '$id'")
        userCancelledId = id
        scope.launch { runCancelKillScript(id) }
        state.job.cancel(CancellationException("cancelled by user"))
    }

    private fun handleInstallThrow(store: InstallationStore, id: String, t: Throwable) {
        // Only treat as user-cancelled when we explicitly set the
        // flag; a bare CancellationException with no flag means the
        // service scope itself died (onDestroy), in which case
        // "Cancelled by user" would be a lie.
        val cancelled = userCancelledId == id
        if (cancelled) {
            Log.w(TAG, "install cancelled by user")
            store.setState(id, Installation.State.FAILED, "Cancelled by user")
            appendLog("[cancel] install of '$id' cancelled")
            publishProgress(InstallProgress(
                InstallStage.FAILED,
                "Install cancelled by user",
                errorMessage = "cancelled",
            ))
        } else {
            Log.e(TAG, "install failed", t)
            store.setState(id, Installation.State.FAILED, t.message ?: "(no detail)")
            appendLog("FAILED: ${t.message}")
            publishProgress(InstallProgress(
                InstallStage.FAILED,
                "Install failed: ${firstLine(t.message)}",
                errorMessage = t.message,
            ))
        }
    }

    private fun handleUninstallThrow(store: InstallationStore, id: String, t: Throwable) {
        val cancelled = userCancelledId == id
        // Only park as FAILED if the dir actually survived; a wipe
        // that succeeded then threw on cleanup is logically gone and
        // shouldn't show up as a parking-state install.
        if (store.installationDir(id).exists()) {
            store.setState(
                id,
                Installation.State.FAILED,
                if (cancelled) "Cancelled by user" else (t.message ?: "(no detail)"),
            )
        }
        if (cancelled) {
            Log.w(TAG, "uninstall cancelled by user")
            appendLog("[cancel] uninstall of '$id' cancelled (rootfs may be partially deleted)")
            publishProgress(InstallProgress(
                InstallStage.FAILED,
                "Uninstall cancelled by user",
                errorMessage = "cancelled",
            ))
        } else {
            Log.e(TAG, "uninstall failed", t)
            appendLog("FAILED: ${t.message}")
            publishProgress(InstallProgress(
                InstallStage.FAILED,
                "Uninstall failed: ${firstLine(t.message)}",
                errorMessage = t.message,
            ))
        }
    }

    private fun clearCurrentJob(id: String) {
        // The gate (`currentJob?.isActive == true`) blocks any new
        // start while our finally runs (the job is still active until
        // the body, including this finally, returns). So no concurrent
        // startInstall/startUninstall can have replaced our slot
        // before now; the id-equality check is just defence in depth
        // for future refactors.
        if (currentJob?.id == id) {
            currentJob = null
        }
        if (userCancelledId == id) userCancelledId = null
        // Drop the cancel-tail id once *this* operation finishes. A
        // refused-uninstall path that left the id set will reach this
        // via its eventual real uninstall, but the id-keyed match in
        // [publishProgress] also protects unrelated jobs in the gap.
        if (installCancelTailUninstallId == id) installCancelTailUninstallId = null
    }

    /**
     * Kill any process attached to [id]'s install dir — chrooted
     * descendants (pacman, gpg-agent) by `/proc/<pid>/root` dev:ino,
     * out-of-chroot helpers (tar, find) by argv-mentions-install-path
     * via `/proc/<pid>/cmdline`. Runs via su when available (so we
     * can see and signal root processes), falls back to the app uid
     * shell otherwise (proot installs are app-uid-owned end-to-end).
     *
     * `installPath` is `<dataDir>/distros/<id>` and `rootfsPath` is
     * `<installPath>/rootfs`, so the substring grep on installPath
     * catches both rootfs-internal and installDir-relative paths.
     *
     * Best-effort: errors are logged and swallowed. The post-cancel
     * uninstall path runs its own
     * [RootfsCleaner.killChrootProcessesScript] later, which catches
     * anything this missed.
     */
    private fun runCancelKillScript(id: String) {
        val store = InstallationStore(applicationContext)
        val installDir = store.installationDir(id)
        val installPath = installDir.absolutePath
        val rootfsPath = store.rootfsDir(id).absolutePath
        val needsRoot = store.load(id)?.method != Installation.METHOD_PROOT
        // Single-quoted shell literals — paths come from
        // [InstallationStore] under app dataDir / a regex-validated
        // id, so they don't carry single quotes. The grep is plain
        // -F (fixed-string) on the install path, which handles
        // anything else.
        val script = buildString {
            appendLine("ROOT_DI=${'$'}(stat -c '%d:%i' '$rootfsPath' 2>/dev/null || true)")
            appendLine("killed=0; scanned=0")
            appendLine("for entry in /proc/[0-9]*; do")
            appendLine("    [ -e \"\$entry\" ] || continue")
            appendLine("    pid=${'$'}{entry##*/}")
            // Skip self and direct parent (the wrapping shell / su)
            // so the script can't accidentally signal-kill its own
            // process tree mid-sweep.
            appendLine("    [ \"\$pid\" = \"\$\$\" ] && continue")
            appendLine("    [ \"\$pid\" = \"\$PPID\" ] && continue")
            appendLine("    scanned=${'$'}((scanned + 1))")
            appendLine("    hit=")
            appendLine("    rdi=${'$'}(stat -L -c '%d:%i' \"\$entry/root\" 2>/dev/null || true)")
            appendLine("    if [ -n \"\$ROOT_DI\" ] && [ \"\$rdi\" = \"\$ROOT_DI\" ]; then hit=1; fi")
            appendLine("    if [ -z \"\$hit\" ]; then")
            appendLine("        if tr '\\0' ' ' < \"\$entry/cmdline\" 2>/dev/null | grep -F -q '$installPath'; then")
            appendLine("            hit=1")
            appendLine("        fi")
            appendLine("    fi")
            appendLine("    if [ -n \"\$hit\" ]; then")
            appendLine("        cmd=${'$'}(cat \"\$entry/comm\" 2>/dev/null || true)")
            appendLine("        echo \"cancel-kill pid=${'$'}pid cmd=${'$'}cmd\"")
            appendLine("        kill -KILL \"\$pid\" 2>/dev/null || true")
            appendLine("        killed=${'$'}((killed + 1))")
            appendLine("    fi")
            appendLine("done")
            appendLine("echo \"cancel-kill: swept \$scanned procs, killed \$killed\"")
        }
        try {
            if (needsRoot && Su.rootAvailable()) {
                Su.run(script) { appendLog("cancel-kill: $it") }
            } else {
                // App-uid only: limited to our own processes (Android
                // hidepid policy) but proot installs run as us anyway.
                val pb = ProcessBuilder("/system/bin/sh").redirectErrorStream(true)
                val proc = pb.start()
                proc.outputStream.bufferedWriter().use { w ->
                    w.write(script); w.write("\n")
                }
                proc.inputStream.bufferedReader().use { r ->
                    r.forEachLine { appendLog("cancel-kill: $it") }
                }
                proc.waitFor()
            }
        } catch (t: Throwable) {
            Log.w(TAG, "cancel-kill script failed (best-effort)", t)
        }
    }

    /**
     * Surface a refused-by-gate request to the bound UI without
     * mutating disk state. Uses the FAILED progress stage because
     * that's the one the panel renders as a terminal error; the
     * message starts with `rejected:` so it's distinguishable from a
     * mid-operation throw.
     */
    private fun reject(what: String, reason: String) {
        val msg = "rejected $what: $reason"
        Log.w(TAG, msg)
        appendLog(msg)
        publishProgress(InstallProgress(InstallStage.FAILED, msg, errorMessage = reason))
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    private fun firstLine(s: String?): String =
        s?.lineSequence()?.firstOrNull { it.isNotBlank() } ?: "(no detail)"

    private fun publishProgress(p: InstallProgress) {
        // Relabel the terminal DONE / FAILED of the install-cancel
        // follow-up uninstall: from the user's POV they cancelled an
        // install, not deleted anything. Mid-stages keep their literal
        // names ("Deleting rootfs…") because the work being done IS a
        // delete; only the summary line changes. We also flip the
        // terminal stage from DONE to FAILED so the panel renders the
        // status in danger-red — a cancelled install isn't a "success"
        // outcome the user should see in green. The id-keyed match
        // (against currentJob's id) keeps a stale flag from an earlier
        // refused-uninstall path from accidentally relabelling some
        // unrelated future delete.
        val cancelTailMatch = installCancelTailUninstallId != null &&
            installCancelTailUninstallId == currentJob?.id
        val effective = if (cancelTailMatch && p.stage == InstallStage.DONE) {
            p.copy(stage = InstallStage.FAILED, message = "Install cancelled")
        } else {
            p
        }
        _progress.value = effective
        // Re-issue the foreground notification so its text stays current.
        val nm = getSystemService(NotificationManager::class.java)
        nm?.notify(NOTIFICATION_ID, buildNotification(effective.message))
        // Only log on stage transitions; downloading streams progress
        // every 256 KiB and we don't want each chunk in logcat.
        if (effective.stage != lastLoggedStage) {
            appendLog("[stage:${effective.stage}] ${effective.message}")
            lastLoggedStage = effective.stage
        }
    }

    private fun appendLog(line: String) {
        Log.d(TAG, line)
        _log.tryEmit(line)
    }

    private fun ensureChannel() {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        if (nm.getNotificationChannel(CHANNEL_ID) == null) {
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    "Installation",
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = "Long-running install / uninstall jobs"
                }
            )
        }
    }

    private fun buildNotification(text: String): Notification {
        val tap = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        // android.R.drawable.stat_sys_download is always present and matches
        // the "background data" feel of an install operation.
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("tawc installation")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(tap)
            .build()
    }

    companion object {
        private const val TAG = "tawc-install"
        private const val CHANNEL_ID = "tawc-install"
        private const val NOTIFICATION_ID = 0xA001

        const val ACTION_INSTALL = "me.phie.tawc.install.SERVICE_INSTALL"
        const val ACTION_UNINSTALL = "me.phie.tawc.install.SERVICE_UNINSTALL"
        const val EXTRA_ID = "id"
        const val EXTRA_METHOD = "method"
        const val EXTRA_DISTRO = "distro"
        const val EXTRA_LABEL = "label"

        fun startInstall(
            context: Context,
            id: String,
            methodKey: String? = null,
            distroKey: String? = null,
            label: String? = null,
        ) {
            val i = Intent(context, InstallationService::class.java)
                .setAction(ACTION_INSTALL)
                .putExtra(EXTRA_ID, id)
            if (methodKey != null) i.putExtra(EXTRA_METHOD, methodKey)
            if (distroKey != null) i.putExtra(EXTRA_DISTRO, distroKey)
            if (label != null) i.putExtra(EXTRA_LABEL, label)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(i)
            } else {
                context.startService(i)
            }
        }

        fun startUninstall(context: Context, id: String) {
            val i = Intent(context, InstallationService::class.java)
                .setAction(ACTION_UNINSTALL)
                .putExtra(EXTRA_ID, id)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(i)
            } else {
                context.startService(i)
            }
        }
    }
}
