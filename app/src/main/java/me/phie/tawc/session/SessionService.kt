package me.phie.tawc.session

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import androidx.core.app.ServiceCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import me.phie.tawc.MainActivity
import me.phie.tawc.R
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.tasks.ProcessScanner
import me.phie.tawc.terminal.TerminalSessions
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The one foreground service: up exactly while [SessionHolds] has a
 * reason (or stray guest processes remain), so the process is never
 * cached while something in a rootfs could be lost. See
 * notes/session-service.md.
 *
 * `onCreate` must stay trivial — `startForegroundService` gives ~5 s to
 * reach `startForeground`.
 */
class SessionService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var strays = 0
    private var strayJob: Job? = null
    private var stopped = false
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onCreate() {
        super.onCreate()
        SessionHolds.serviceStarted(this)
        SessionWake.serviceStarted(this)
        ensureChannel()
        goForeground()
        scope.launch {
            SessionWake.held.collect { on ->
                if (stopped) return@collect
                if (on) acquireLocks() else releaseLocks()
                notifyNow()
            }
        }
        scope.launch {
            SessionHolds.reasons.collect { reasons ->
                if (stopped) return@collect
                if (reasons.isEmpty()) {
                    startStrayWatch()
                } else {
                    strayJob?.cancel()
                    strayJob = null
                    strays = 0
                    notifyNow()
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Every startForegroundService must be answered, even when
        // already in the foreground.
        goForeground()
        when (intent?.action) {
            ACTION_EXIT -> {
                SessionWake.set(false)
                SessionExit.killEverything(applicationContext)
            }
            ACTION_WAKE -> SessionWake.set(!SessionWake.held.value)
        }
        // Not sticky: after a process kill every guest is dead, and a
        // restart would call startForeground from the background.
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    /**
     * Only an explicit removal (recents swipe) lands here, never a
     * system kill. Swiping the home screen closes its terminals like
     * closing desktop terminal windows; compositor window tasks don't.
     */
    override fun onTaskRemoved(rootIntent: Intent?) {
        super.onTaskRemoved(rootIntent)
        if (!isHomeTask(rootIntent)) return
        if (TerminalSessions.selfRemoving) {
            TerminalSessions.selfRemoving = false
        } else {
            TerminalSessions.hangUpAll()
        }
    }

    /** The default-affinity task, where MainActivity lives (compositor
     *  and launch trampolines use `taskAffinity=""`). */
    private fun isHomeTask(rootIntent: Intent?): Boolean {
        val component = rootIntent?.component ?: return false
        return try {
            packageManager.getActivityInfo(component, 0).taskAffinity == packageName
        } catch (_: PackageManager.NameNotFoundException) {
            false
        }
    }

    override fun onDestroy() {
        SessionHolds.serviceStopped(this)
        SessionWake.serviceStopped(this)
        releaseLocks()
        scope.cancel()
        super.onDestroy()
    }

    /**
     * Tail state: no explicit hold left. Stay up while guest processes
     * (`nohup`, `setsid`, a daemon a GUI app left) remain, re-scanning
     * slowly; this is the only state that polls.
     */
    private fun startStrayWatch() {
        if (strayJob != null) return
        strayJob = scope.launch {
            while (true) {
                val count = withContext(Dispatchers.IO) { countGuests() }
                // An acquire may have landed during the scan.
                if (SessionHolds.reasons.value.isNotEmpty()) return@launch
                // No suspension between the check and stopSelf: starts are
                // posted to this thread (see install), so none can land
                // in between and be torn down unanswered.
                if (count == 0 && SessionHolds.serviceStopIfIdle(this@SessionService)) {
                    stopped = true
                    SessionWake.serviceStopped(this@SessionService)
                    releaseLocks()
                    ServiceCompat.stopForeground(this@SessionService, ServiceCompat.STOP_FOREGROUND_REMOVE)
                    stopSelf()
                    return@launch
                }
                strays = count
                notifyNow()
                withTimeoutOrNull(STRAY_POLL_MS) { rescan.receive() }
            }
        }
    }

    private fun countGuests(): Int = try {
        ProcessScanner.scan(this, InstallationStore(this).list()).processes.size
    } catch (t: Throwable) {
        Log.w(TAG, "stray scan failed", t)
        0
    }

    /** Non-reference-counted: toggling twice never stacks. */
    private fun acquireLocks() {
        val wake = wakeLock ?: getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG)
            .apply { setReferenceCounted(false) }
            .also { wakeLock = it }
        // No timeout: the user asked for it, and it goes with the service.
        @Suppress("WakelockTimeout")
        wake.acquire()
        val wifi = wifiLock ?: (getSystemService(WifiManager::class.java)
            ?.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, WAKE_LOCK_TAG)
            ?.apply { setReferenceCounted(false) }
            ?.also { wifiLock = it })
        wifi?.acquire()
    }

    private fun releaseLocks() {
        wakeLock?.takeIf { it.isHeld }?.release()
        wifiLock?.takeIf { it.isHeld }?.release()
    }

    private fun currentReasons(): List<Reason> {
        val reasons = SessionHolds.reasons.value
        return if (reasons.isEmpty() && strays > 0) listOf(Reason.Stray(strays)) else reasons
    }

    private fun goForeground() {
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
        } else {
            0
        }
        ServiceCompat.startForeground(this, NOTIFICATION_ID, buildNotification(), type)
    }

    private fun notifyNow() {
        getSystemService(NotificationManager::class.java)
            ?.notify(NOTIFICATION_ID, buildNotification())
    }

    private fun buildNotification(): Notification =
        Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.session_notification_title))
            .setContentText(describe(SessionSummary.of(currentReasons())))
            .setSmallIcon(R.drawable.ic_terminal)
            .setContentIntent(homePendingIntent())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(
                Notification.Action.Builder(
                    Icon.createWithResource(this, R.drawable.ic_close),
                    getString(R.string.session_notification_exit),
                    exitPendingIntent(),
                ).build(),
            )
            .addAction(
                Notification.Action.Builder(
                    Icon.createWithResource(this, R.drawable.ic_terminal),
                    getString(
                        if (SessionWake.held.value) R.string.session_notification_release_wake
                        else R.string.session_notification_keep_awake,
                    ),
                    servicePendingIntent(2, ACTION_WAKE),
                ).build(),
            )
            .build()

    private fun describe(s: SessionSummary): String {
        val parts = ArrayList<String>()
        if (s.terminals > 0) {
            parts += resources.getQuantityString(R.plurals.session_terminals, s.terminals, s.terminals)
        }
        if (s.windows > 0) {
            parts += resources.getQuantityString(R.plurals.session_windows, s.windows, s.windows)
        }
        when (s.commands.size) {
            0 -> Unit
            1 -> parts += getString(R.string.session_running_command, s.commands[0])
            else -> parts += resources.getQuantityString(
                R.plurals.session_commands, s.commands.size, s.commands.size,
            )
        }
        if (s.strays > 0) {
            parts += resources.getQuantityString(R.plurals.session_strays, s.strays, s.strays)
        }
        if (s.remote) {
            parts += getString(R.string.session_remote)
            if (s.remoteClients > 0) {
                parts += resources.getQuantityString(R.plurals.session_remote_clients, s.remoteClients, s.remoteClients)
            }
        }
        // Only a windowless compositor (e.g. serving a clipboard client).
        if (parts.isEmpty()) parts += getString(R.string.session_display_server)
        if (SessionWake.held.value) parts += getString(R.string.session_awake)
        return parts.joinToString(" · ")
    }

    private fun homePendingIntent(): PendingIntent {
        val intent = Intent(this, MainActivity::class.java).apply {
            action = Intent.ACTION_MAIN
            addCategory(Intent.CATEGORY_LAUNCHER)
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                Intent.FLAG_ACTIVITY_CLEAR_TOP or
                Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
        return PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    private fun exitPendingIntent(): PendingIntent = servicePendingIntent(1, ACTION_EXIT)

    private fun servicePendingIntent(requestCode: Int, action: String): PendingIntent = PendingIntent.getService(
        this, requestCode,
        Intent(this, SessionService::class.java).setAction(action),
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )

    private fun ensureChannel() {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        if (nm.getNotificationChannel(CHANNEL_ID) == null) {
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.session_channel_name),
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = getString(R.string.session_channel_description)
                    setShowBadge(false)
                },
            )
        }
        try { nm.deleteNotificationChannel(LEGACY_CHANNEL_ID) } catch (_: Throwable) {}
    }

    companion object {
        private const val TAG = "tawc"
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "tawc_session"
        private const val LEGACY_CHANNEL_ID = "tawc_compositor"
        private const val ACTION_EXIT = "me.phie.tawc.session.EXIT"
        private const val ACTION_WAKE = "me.phie.tawc.session.WAKE"
        private const val WAKE_LOCK_TAG = "tawc:session"
        private const val STRAY_POLL_MS = 15_000L

        private val loggedStartFailure = AtomicBoolean(false)

        private val rescan = Channel<Unit>(Channel.CONFLATED)

        /** Re-scan for strays now instead of at the next poll (after
         *  Exit has killed them). Any thread. */
        fun rescanStrays() {
            rescan.trySend(Unit)
        }

        /**
         * Hook [SessionHolds] up to this service. `Application.onCreate`.
         *
         * Starts always run on the main thread, the same thread that
         * decides to stop. A `startForegroundService` from another thread
         * could otherwise land between "nothing is held" and `stopSelf`;
         * the service then dies with that start unanswered and Android
         * kills the whole process
         * (`ForegroundServiceDidNotStartInTimeException`) — guests included.
         */
        fun install(context: Context) {
            val app = context.applicationContext
            val main = Handler(Looper.getMainLooper())
            SessionHolds.starter = {
                if (Looper.myLooper() == Looper.getMainLooper()) start(app) else main.post { start(app) }
            }
        }

        private fun start(app: Context) {
            // Released again before we got here: nothing to protect.
            if (SessionHolds.reasons.value.isEmpty()) return
            run {
                try {
                    app.startForegroundService(Intent(app, SessionService::class.java))
                } catch (e: IllegalStateException) {
                    // Android 12+: ForegroundServiceStartNotAllowedException
                    // when nothing of ours is visible (debug broker without
                    // --foreground-app). Run unprotected rather than fail
                    // the spawn; the next acquire retries.
                    if (loggedStartFailure.compareAndSet(false, true)) {
                        Log.w(TAG, "session service start refused; running unprotected: ${e.message}")
                    }
                }
            }
        }
    }
}
