package me.phie.tawc.ops

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import me.phie.tawc.R

/**
 * Application-singleton that keeps Android notifications in sync with
 * [OperationsRegistry]. Started from [me.phie.tawc.TawcApplication.onCreate]
 * via [start].
 *
 * Per registered op:
 *   - Posts an ongoing notification on the `tawc-operations` channel.
 *   - Updates its content text from [Operation.progress.value.message]
 *     on every emit.
 *   - Tap PendingIntent → [LogScreenActivity] for that op (the only
 *     side-effect of opening that activity is rendering the panel —
 *     no operation triggered).
 *   - Cancel action button → [CancelOperationReceiver], which calls
 *     [Operation.cancel] without a confirm dialog (the notification
 *     tap is itself the deliberate decision).
 *   - When the op leaves the registry (terminal-then-unregister), the
 *     notification is cancelled. There is no "completed" toast — per
 *     the project's "unregister immediately on terminal" choice the
 *     surface goes dark on completion.
 *
 * [fgsAnchorFor] is for services that want to use one of their
 * registered ops' notifications as their `startForeground` anchor —
 * see [me.phie.tawc.install.InstallationService.startInstall]. The
 * notification id is stable per op, so the FGS anchor and the
 * standalone notification are the same notification.
 */
object OperationsNotificationCenter {

    private const val TAG = "tawc-ops"
    const val CHANNEL_ID = "tawc-operations"

    /**
     * Notification IDs are derived from the op id's hash with a fixed
     * high-bit prefix to keep them out of the way of any other
     * NotificationManager.notify caller in the app. Stable per op id
     * so the same notification gets updated rather than spawning
     * duplicates.
     */
    private const val NOTIFICATION_ID_PREFIX = 0x6F00_0000.toInt()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** Per-op collector job, so we can cancel one when its op is unregistered. */
    private val collectors = mutableMapOf<String, Job>()

    @Volatile private var started = false
    @Volatile private var appContext: Context? = null

    fun start(context: Context) {
        if (started) return
        started = true
        appContext = context.applicationContext
        ensureChannel(context)
        scope.launch {
            OperationsRegistry.ops.collect { ops -> reconcile(ops) }
        }
    }

    /**
     * Build a notification for the registered op named [opId] and return
     * `(notificationId, notification)` so a caller can pass them to
     * `Service.startForeground`. The standalone notification posted by
     * the registry-watcher uses the same id, so the FGS anchor and the
     * tray notification are the same notification (Android merges them
     * by id).
     */
    fun fgsAnchorFor(opId: String): Pair<Int, Notification> {
        val ctx = appContext ?: error("OperationsNotificationCenter not started")
        val op = OperationsRegistry.get(opId)
            ?: error("op '$opId' is not registered")
        return notificationIdFor(opId) to buildNotification(ctx, op)
    }

    /**
     * Build a placeholder notification for an op that's about to be
     * registered, so a service can satisfy `startForeground`'s 5-second
     * deadline before its validation / coroutine-launch code runs. The
     * id matches what [fgsAnchorFor] will return for the same [opId];
     * once the op is registered, the registry-watcher's `nm.notify`
     * with the same id seamlessly replaces this placeholder content.
     *
     * No PendingIntents — the placeholder's lifetime is sub-second;
     * the per-op build replaces those once the op is real.
     */
    fun placeholderForegroundFor(
        ctx: Context,
        opId: String,
        title: String,
        message: String = ctx.getString(R.string.operation_starting),
    ): Pair<Int, Notification> {
        ensureChannel(ctx)
        return notificationIdFor(opId) to buildNotificationContent(
            ctx = ctx,
            opId = opId,
            title = title,
            message = message,
            withActions = false,
        )
    }

    private fun reconcile(ops: Map<String, Operation>) {
        // Spawn collectors for newly-registered ops.
        for ((id, op) in ops) {
            if (id !in collectors) {
                collectors[id] = scope.launch { collectFor(op) }
            }
        }
        // Cancel collectors and notifications for departed ops.
        val gone = collectors.keys - ops.keys
        for (id in gone) {
            collectors.remove(id)?.cancel()
            val ctx = appContext ?: continue
            ctx.getSystemService(NotificationManager::class.java)
                ?.cancel(notificationIdFor(id))
        }
    }

    private suspend fun collectFor(op: Operation) {
        val ctx = appContext ?: return
        val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
        // Throttle: a fast-cached download or pacman line stream can
        // emit progress at hundreds of hertz; Android's notifier rate-
        // limits and silently drops everything past ~5 Hz, so most of
        // the work was wasted. distinctUntilChanged drops repeat-content
        // emits (eg. the same "Installing base packages…" message
        // re-published with a new InstallStage internal field), conflate
        // collapses bursts to the latest, and the post-emit delay keeps
        // us under the system's drop rate. Terminal stages skip the
        // delay so the final "Installed"/"Failed"/"Deleted" message
        // never sits behind a 250 ms timer.
        op.progress
            .map { p -> NotificationSnapshot(p.stage.isTerminal, p.message) }
            .distinctUntilChanged()
            .conflate()
            .collect { snap ->
                nm.notify(notificationIdFor(op.id), buildNotification(ctx, op))
                if (!snap.terminal) delay(NOTIFICATION_THROTTLE_MS)
            }
    }

    private data class NotificationSnapshot(val terminal: Boolean, val message: String)
    private const val NOTIFICATION_THROTTLE_MS: Long = 250

    private fun buildNotification(ctx: Context, op: Operation): Notification =
        buildNotificationContent(
            ctx = ctx,
            opId = op.id,
            title = op.title,
            message = op.progress.value.message,
            withActions = true,
        )

    /**
     * Single notification builder used both by the registry-watcher
     * (with tap + Cancel actions) and by the placeholder path used by
     * services to satisfy the FGS-within-5-seconds rule (no actions —
     * the op doesn't exist yet, so the PendingIntents would target
     * nothing).
     */
    private fun buildNotificationContent(
        ctx: Context,
        opId: String,
        title: String,
        message: String,
        withActions: Boolean,
    ): Notification {
        val builder = Notification.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle(title)
            .setContentText(message)
            .setOngoing(true)
        if (withActions) {
            // Per-op data URI on both intents: notification ids (and so
            // the request codes) are a 24-bit hash of the op id, and on
            // a hash collision between two live ops FLAG_UPDATE_CURRENT
            // would rewrite the colliding PendingIntent's extras in
            // place (Intent.filterEquals ignores extras) — one
            // notification's Cancel then cancels the *other* op. A
            // distinct URI makes the intents non-equal, so each op
            // keeps its own PendingIntent regardless of id collisions.
            val opUri = android.net.Uri.parse("tawc://operation/" + android.net.Uri.encode(opId))
            val tap = PendingIntent.getActivity(
                ctx, notificationIdFor(opId),
                LogScreenActivity.intentFor(ctx, opId)
                    .setData(opUri)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            val cancel = PendingIntent.getBroadcast(
                ctx, notificationIdFor(opId) xor 0x1,
                Intent(ctx, CancelOperationReceiver::class.java)
                    .setAction(CancelOperationReceiver.ACTION)
                    .setData(opUri)
                    .putExtra(CancelOperationReceiver.EXTRA_OPERATION_ID, opId),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            builder
                .setContentIntent(tap)
                .addAction(Notification.Action.Builder(null, ctx.getString(R.string.action_cancel), cancel).build())
        }
        return builder.build()
    }

    private fun ensureChannel(ctx: Context) {
        val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
        if (nm.getNotificationChannel(CHANNEL_ID) == null) {
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    ctx.getString(R.string.operation_channel_name),
                    NotificationManager.IMPORTANCE_LOW,
                ).apply { description = ctx.getString(R.string.operation_channel_description) }
            )
        }
        // Clean up the legacy install-only channel from before the ops
        // refactor. Safe to call repeatedly; no-op if it never existed.
        try { nm.deleteNotificationChannel(LEGACY_CHANNEL_ID) } catch (_: Throwable) {}
    }

    private fun notificationIdFor(opId: String): Int =
        NOTIFICATION_ID_PREFIX or (opId.hashCode() and 0x00FF_FFFF)

    private const val LEGACY_CHANNEL_ID = "tawc-install"
}
