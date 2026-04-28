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
 */
class InstallationService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var currentJob: Job? = null
    private val binder = LocalBinder()

    private val _progress = MutableStateFlow(
        InstallProgress(InstallStage.IDLE, "Idle")
    )
    val progress: StateFlow<InstallProgress> = _progress.asStateFlow()

    private val _log = MutableSharedFlow<String>(replay = 200, extraBufferCapacity = 1024)
    val log: SharedFlow<String> = _log.asSharedFlow()

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

    /**
     * Begin an install for [id] using [methodKey] (or auto-pick if
     * null). Refuses if the gate forbids it.
     */
    fun startInstall(id: String, methodKey: String? = null) {
        if (!Installation.isValidId(id)) {
            reject("install '$id'", "invalid id (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            return
        }
        if (currentJob?.isActive == true) {
            reject("install '$id'", "another job is already running")
            return
        }
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
        // Resolve the host's default distro before any disk state is
        // written so an unsupported ABI (e.g. 32-bit-only or armeabi)
        // is a clean reject rather than a half-installed FAILED slot.
        val distro = DistroRegistry.defaultForHost()
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
        currentJob = scope.launch {
            val installer = Installer(
                applicationContext, store, BootstrapCache(applicationContext),
                distro, method, id,
            )
            try {
                installer.install(::publishProgress, ::appendLog)
            } catch (t: Throwable) {
                Log.e(TAG, "install failed", t)
                store.setState(id, Installation.State.FAILED, t.message ?: "(no detail)")
                appendLog("FAILED: ${t.message}")
                publishProgress(
                    InstallProgress(
                        InstallStage.FAILED,
                        "Install failed: ${firstLine(t.message)}",
                        errorMessage = t.message,
                    )
                )
            } finally {
                stopForeground(STOP_FOREGROUND_REMOVE)
            }
        }
    }

    /** Begin an uninstall for [id]. Refuses only if a job is already running. */
    fun startUninstall(id: String) {
        if (!Installation.isValidId(id)) {
            reject("uninstall '$id'", "invalid id (allowed: ^[a-z0-9][a-z0-9_-]{0,31}$)")
            return
        }
        if (currentJob?.isActive == true) {
            reject("uninstall '$id'", "another job is already running")
            return
        }
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
        currentJob = scope.launch {
            val installer = Installer(
                applicationContext, store, BootstrapCache(applicationContext),
                distro, method, id,
            )
            try {
                installer.uninstall(::publishProgress, ::appendLog)
            } catch (t: Throwable) {
                Log.e(TAG, "uninstall failed", t)
                // Only park as FAILED if the dir actually survived; a
                // wipe that succeeded then threw on cleanup is logically
                // gone and shouldn't show up as a parking-state install.
                if (store.installationDir(id).exists()) {
                    store.setState(id, Installation.State.FAILED, t.message ?: "(no detail)")
                }
                appendLog("FAILED: ${t.message}")
                publishProgress(
                    InstallProgress(
                        InstallStage.FAILED,
                        "Uninstall failed: ${firstLine(t.message)}",
                        errorMessage = t.message,
                    )
                )
            } finally {
                stopForeground(STOP_FOREGROUND_REMOVE)
            }
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
        _progress.value = p
        // Re-issue the foreground notification so its text stays current.
        val nm = getSystemService(NotificationManager::class.java)
        nm?.notify(NOTIFICATION_ID, buildNotification(p.message))
        // Only log on stage transitions; downloading streams progress
        // every 256 KiB and we don't want each chunk in logcat.
        if (p.stage != lastLoggedStage) {
            appendLog("[stage:${p.stage}] ${p.message}")
            lastLoggedStage = p.stage
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

        fun startInstall(context: Context, id: String, methodKey: String? = null) {
            val i = Intent(context, InstallationService::class.java)
                .setAction(ACTION_INSTALL)
                .putExtra(EXTRA_ID, id)
            if (methodKey != null) i.putExtra(EXTRA_METHOD, methodKey)
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
