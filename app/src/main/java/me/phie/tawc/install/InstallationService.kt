package me.phie.tawc.install

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.runInterruptible
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ops.CancelConfirmation
import me.phie.tawc.ops.MutableOperation
import me.phie.tawc.ops.OperationProgress
import me.phie.tawc.ops.OperationStage
import me.phie.tawc.ops.OperationsNotificationCenter
import me.phie.tawc.ops.OperationsRegistry
import me.phie.tawc.tasks.ProcessScanner

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
 * Both the in-app trigger surfaces (the Install button on
 * [InstallActivity] and the Delete confirm on [DistroInfoActivity])
 * and the dev exec broker's `install` / `uninstall` actions are
 * inputs; this service decides whether to actually run.
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
 * across the install→uninstall transition: the install's `finally`
 * skips `stopForeground` while [pendingFollowupUninstallId] is set,
 * and just before launching the follow-up uninstall the cancel coroutine
 * re-anchors `startForeground` to a placeholder notification keyed on
 * the *uninstall* op id. The follow-up [startUninstall] then registers
 * its [MutableOperation] under the same id, and the registry-watcher's
 * [OperationsNotificationCenter] notify upgrades the placeholder in
 * place. The service is therefore continuously FGS-anchored across the
 * transition (no out-of-memory-kill window), and the visible
 * notification swaps content from "Cancelling install: cleaning up…"
 * to the live uninstall progress with no flicker.
 */
class InstallationService : Service() {

    enum class JobKind { INSTALL, UNINSTALL }

    private data class JobState(val job: Job, val id: String, val kind: JobKind, val op: MutableOperation)

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

    private val _log = MutableSharedFlow<String>(replay = 200, extraBufferCapacity = 1024)
    val log: SharedFlow<String> = _log.asSharedFlow()

    /**
     * Drop every replay-buffered log line so a freshly-bound panel
     * for the next operation doesn't inherit the previous run's tail.
     * Called at the start of every job; viewers don't need this.
     */
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    private fun resetLogReplay() {
        _log.resetReplayCache()
    }

    private var lastLoggedStage: InstallStage? = null

    inner class LocalBinder : Binder() {
        val service: InstallationService get() = this@InstallationService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Anchor the FGS to a placeholder notification keyed on the op
        // id we're about to register. Android requires a startForeground
        // call within 5 seconds of startForegroundService, before our
        // validation logic in start* below has a chance to reject.
        // [OperationsNotificationCenter.placeholderForegroundFor] uses
        // the same id derivation as the per-op notification, so once
        // the op is registered the registry-watcher's notify(...) call
        // seamlessly upgrades the placeholder to the real per-op
        // notification (with tap PendingIntent + Cancel action) without
        // a flash or a duplicate.
        val rawId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH
        val anchorOpId = when (intent?.action) {
            ACTION_INSTALL -> "install:$rawId"
            ACTION_UNINSTALL -> "uninstall:$rawId"
            else -> "tawc:installation"
        }
        val anchorTitle = when (intent?.action) {
            ACTION_INSTALL -> "Install $rawId"
            ACTION_UNINSTALL -> "Uninstall $rawId"
            else -> "tawc"
        }
        val (notifId, notif) = OperationsNotificationCenter.placeholderForegroundFor(
            applicationContext, anchorOpId, anchorTitle,
        )
        startForeground(notifId, notif)

        when (intent?.action) {
            ACTION_INSTALL -> startInstall(
                rawId,
                intent.getStringExtra(EXTRA_METHOD),
                intent.getStringExtra(EXTRA_DISTRO),
                intent.getStringExtra(EXTRA_LABEL),
                intent.getStringExtra(EXTRA_MIRROR_PROXY),
            )
            ACTION_UNINSTALL -> startUninstall(rawId)
            else -> {
                Log.w(TAG, "InstallationService started without a known action")
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
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
        mirrorProxyUrl: String? = null,
    ) {
        if (!Installation.isValidId(id)) {
            rejectInstall(id, "invalid id (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            return
        }
        if (currentJob?.job?.isActive == true) {
            rejectInstall(id, "another job is already running")
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
                rejectInstall(id, "id is in state $s; uninstall first")
                return
            }
        }
        // Resolve the requested distro (or fall back to the host
        // default) before any disk state is written so an unsupported
        // ABI / unknown distro key is a clean reject rather than a
        // half-installed FAILED slot.
        val distro = if (distroKey != null) {
            DistroRegistry.forKey(distroKey) ?: run {
                rejectInstall(
                    id,
                    "unknown or host-incompatible distro '$distroKey' " +
                        "(available: ${DistroRegistry.availableForHost().joinToString { it.key }})",
                )
                return
            }
        } else {
            DistroRegistry.defaultForHost()
        }
        if (distro == null) {
            rejectInstall(id, "no Distro supports ABI ${android.os.Build.SUPPORTED_ABIS.joinToString(",")}")
            return
        }
        // Resolve the install method. An explicit bad key (or one that
        // this build doesn't ship — see [EnabledMethods]) is a clean
        // reject; null falls back to the host default (tawcroot when
        // enabled — the default for new installs).
        val method = if (methodKey != null) {
            InstallationMethod.forKey(applicationContext, methodKey) ?: run {
                rejectInstall(
                    id,
                    "method '$methodKey' is not enabled in this build " +
                        "(enabled: ${EnabledMethods.keys.joinToString()})",
                )
                return
            }
        } else {
            InstallationMethod.defaultForHost(applicationContext)
        }
        if (!method.isAvailable(applicationContext)) {
            rejectInstall(id, "method '${method.key}' is not available on this device")
            return
        }
        // mirrorProxyUrl is gated to debug builds: a release APK with a
        // malformed install intent that nevertheless includes the extra
        // is treated as if the extra were absent. Belt-and-braces with
        // the InstallActivity-side gate that omits the form checkbox
        // outside debug builds.
        val mirrorProxy = mirrorProxyUrl?.takeIf { me.phie.tawc.BuildConfig.DEBUG }
            ?.let { MirrorProxy(it) }
        if (mirrorProxyUrl != null && mirrorProxy == null) {
            appendLog("[install] ignoring mirrorProxy '$mirrorProxyUrl' (release build)")
        } else if (mirrorProxy != null) {
            appendLog("[install] using mirror proxy ${mirrorProxy.base}")
        }
        val rootfsPath = store.rootfsDir(id).absolutePath
        val op = MutableOperation(
            id = "install:$id",
            title = "Install $id",
            log = _log,
            cancelConfirmation = CancelConfirmation(
                title = "Cancel install of '$id'?",
                message = "Cancelling will stop the install and remove the partially " +
                    "extracted rootfs at\n$rootfsPath.\n" +
                    "Nothing of yours has been written there yet, so no data will be lost.",
                confirmLabel = "Cancel install",
                keepLabel = "Keep installing",
            ),
            cancelHandler = { cancelInstallAndUninstall(id) },
        )
        OperationsRegistry.register(op)
        // The placeholder FGS anchor posted in onStartCommand uses the
        // same notification id as the per-op notification, so this
        // [fgsAnchorFor] update is for the case where startInstall was
        // called directly (companion-object helper from in-app code,
        // e.g. the Install button), not via onStartCommand. Either way
        // the registry-watcher's first progress emit immediately
        // updates the visible notification with proper PendingIntents.
        val (notifId, notif) = OperationsNotificationCenter.fgsAnchorFor(op.id)
        startForeground(notifId, notif)
        val job = scope.launch {
            val installer = Installer(
                applicationContext, store, BootstrapCache(applicationContext),
                distro, method, id, label, mirrorProxy,
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
        currentJob = JobState(job, id, JobKind.INSTALL, op)
    }

    /** Begin an uninstall for [id]. Refuses only if a job is already running. */
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    fun startUninstall(id: String) {
        if (!Installation.isValidId(id)) {
            rejectUninstall(id, "invalid id (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            return
        }
        if (currentJob?.job?.isActive == true) {
            rejectUninstall(id, "another job is already running")
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
        val op = MutableOperation(
            id = "uninstall:$id",
            title = "Uninstall $id",
            log = _log,
            // Uninstall has no confirm dialog at the cancel boundary —
            // the user might be tapping Cancel to abort a wipe that's
            // about to delete their work, and another dialog in the
            // way risks finishing the wipe before they can react.
            cancelConfirmation = null,
            cancelHandler = { cancelUninstall(id) },
        )
        OperationsRegistry.register(op)
        val (notifId, notif) = OperationsNotificationCenter.fgsAnchorFor(op.id)
        startForeground(notifId, notif)
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
        currentJob = JobState(job, id, JobKind.UNINSTALL, op)
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
            // Bridge: the install's finally just unregistered its
            // Operation, which cancelled the install's notification —
            // we're FGS-anchored to a no-longer-visible notification
            // until [startUninstall] re-anchors. Post a placeholder
            // keyed on the uninstall op id so the registry-watcher
            // upgrades it in place once the uninstall op registers,
            // exactly the same way [onStartCommand]'s placeholder is
            // upgraded for direct CLI starts.
            val (bridgeNotifId, bridgeNotif) = OperationsNotificationCenter.placeholderForegroundFor(
                applicationContext, "uninstall:$id", "Uninstall $id", "Cancelling install: cleaning up…",
            )
            startForeground(bridgeNotifId, bridgeNotif)
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
        val state = currentJob
        if (state?.id == id) {
            // Unregister the Operation adapter from the registry; the
            // OperationsNotificationCenter cancels its notification.
            // The op's StateFlow value remains accessible to any
            // already-bound LogScreenActivity (the panel keeps the
            // frozen final state), just no further updates.
            OperationsRegistry.unregister(state.op.id)
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
     * Kill any process attached to [id]'s install dir — guests inside
     * the rootfs by `cwd`/`exe`/`root`, plus out-of-rootfs helpers
     * (`tar`, `find`) by argv-mentions-install-path. Best-effort: the
     * post-cancel uninstall calls [ProcessScanner.killAllInRootfs]
     * again with the same parameters, which catches anything this
     * missed.
     *
     * `includeChroot` is `true` for chroot installs (need `su` to see
     * root-owned procs); `false` for proot/tawcroot (app-uid only,
     * no su prompt).
     */
    private fun runCancelKillScript(id: String) {
        val store = InstallationStore(applicationContext)
        val installDir = store.installationDir(id)
        val rootfsPath = store.rootfsDir(id).absolutePath
        val includeChroot = store.load(id)?.method == ChrootMethod.KEY
        try {
            ProcessScanner.killAllInRootfs(
                rootfsPath = rootfsPath,
                installId = id,
                includeChroot = includeChroot,
                extraCmdlinePath = installDir.absolutePath,
                log = { appendLog("cancel-kill: $it") },
            )
        } catch (t: Throwable) {
            Log.w(TAG, "cancel-kill failed (best-effort)", t)
        }
    }

    private fun rejectInstall(id: String, reason: String) =
        rejectAsTransientOp("install:$id", "Install $id", "install '$id'", reason)

    private fun rejectUninstall(id: String, reason: String) =
        rejectAsTransientOp("uninstall:$id", "Uninstall $id", "uninstall '$id'", reason)

    /**
     * Surface a refused-by-gate request through the [OperationsRegistry]
     * as a brief transient Operation so any bound viewer
     * ([LogScreenActivity], [InstallActivity]'s panel, …) renders the
     * FAILED state. Per the project's "unregister immediately on
     * terminal" choice the op vanishes after [TRANSIENT_REJECT_HOLD_MS];
     * any bound viewer keeps the frozen state on screen.
     *
     * The transient op briefly posts a tray notification (the registry-
     * watcher does this on every register), then the unregister cancels
     * it. Net effect: a notification flashes for [TRANSIENT_REJECT_HOLD_MS]
     * then disappears. Acceptable for a dev / power-user surface; the
     * primary feedback is on the in-app panel and the broker mirror's
     * stdout.
     *
     * The hold duration is **load-bearing for the broker mirror path**:
     * `InstallActions.mirrorOperation` waits up to 5s for an op named
     * `<verb>:<id>` to appear in the registry. As long as
     * [TRANSIENT_REJECT_HOLD_MS] is well under that window, the broker
     * action sees the transient before it disappears and reports FAILED.
     * If you shorten this, also check that contract.
     */
    private fun rejectAsTransientOp(
        opId: String,
        opTitle: String,
        what: String,
        reason: String,
    ) {
        val msg = "rejected $what: $reason"
        Log.w(TAG, msg)
        // Drop foreground state so we're not anchored to the placeholder
        // notification posted in onStartCommand. The transient op's own
        // notification gets posted by the registry-watcher and will be
        // cancelled when we unregister below.
        stopForeground(STOP_FOREGROUND_REMOVE)
        val transient = MutableOperation(
            id = opId, title = opTitle,
            log = _log,
            cancelConfirmation = null,
            cancelHandler = { /* no-op for a terminal-on-arrival op */ },
        )
        OperationsRegistry.register(transient)
        appendLog(msg)
        // Publish directly to the transient op (it's not currentJob, so
        // the worker-side publishProgress wouldn't see it).
        transient.publish(OperationProgress(OperationStage.FAILED, msg))
        scope.launch {
            kotlinx.coroutines.delay(TRANSIENT_REJECT_HOLD_MS)
            OperationsRegistry.unregister(opId)
        }
    }

    /** See [rejectAsTransientOp]; load-bearing for the broker mirror. */
    private val TRANSIENT_REJECT_HOLD_MS: Long = 2_000

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
        // Write directly to the current op's StateFlow. The
        // OperationsNotificationCenter watches op.progress and updates
        // the visible notification on every emit; the broker mirror
        // and any bound LogScreenActivity collect from the same flow.
        currentJob?.op?.publish(effective.toOperationProgress())
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

    companion object {
        private const val TAG = "tawc-install"

        const val ACTION_INSTALL = "me.phie.tawc.install.SERVICE_INSTALL"
        const val ACTION_UNINSTALL = "me.phie.tawc.install.SERVICE_UNINSTALL"
        const val EXTRA_ID = "id"
        const val EXTRA_METHOD = "method"
        const val EXTRA_DISTRO = "distro"
        const val EXTRA_LABEL = "label"
        const val EXTRA_MIRROR_PROXY = "mirrorProxy"

        fun startInstall(
            context: Context,
            id: String,
            methodKey: String? = null,
            distroKey: String? = null,
            label: String? = null,
            mirrorProxyUrl: String? = null,
        ) {
            val i = Intent(context, InstallationService::class.java)
                .setAction(ACTION_INSTALL)
                .putExtra(EXTRA_ID, id)
            if (methodKey != null) i.putExtra(EXTRA_METHOD, methodKey)
            if (distroKey != null) i.putExtra(EXTRA_DISTRO, distroKey)
            if (label != null) i.putExtra(EXTRA_LABEL, label)
            if (mirrorProxyUrl != null) i.putExtra(EXTRA_MIRROR_PROXY, mirrorProxyUrl)
            context.startForegroundService(i)
        }

        fun startUninstall(context: Context, id: String) {
            val i = Intent(context, InstallationService::class.java)
                .setAction(ACTION_UNINSTALL)
                .putExtra(EXTRA_ID, id)
            context.startForegroundService(i)
        }
    }
}
