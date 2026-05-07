package me.phie.tawc.dev

import android.content.Context
import android.net.LocalServerSocket
import android.util.Log
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Dev-only socket-based exec broker. Lets the host run commands as the
 * app uid + SELinux domain. See [notes/exec-broker.md] for the protocol
 * and security model.
 *
 * Auth boundary: every accepted connection's `SO_PEERCRED.uid` must be
 * in [ALLOWED_UIDS] (root or shell). Random apps connecting from
 * `untrusted_app` get rejected.
 *
 * Lifecycle: bound from [TawcApplication.onCreate] when
 * `BuildConfig.DEBUG`. Release builds never bind the socket. We use a
 * plain background thread (no Service) because:
 * - Foreground services can't be started from
 *   `Application.onCreate()` on Android 12+ in some cold-start paths.
 * - The broker has no UI / notification needs of its own.
 * - The thread keeps the process alive only as long as it's a user
 *   thread, which it isn't (daemon=true) — so the app is free to die
 *   normally if nothing else holds it open.
 */
object ExecBroker {
    const val TAG = "tawc-exec"
    const val SOCKET_NAME = "me.phie.tawc.exec"

    // shell (2000) covers `adb shell` and adbd-forwarded connections;
    // root (0) covers `su -c` and userdebug adbd. Other apps run as
    // 10xxx and get filtered out — SO_PEERCRED is kernel-populated
    // and unspoofable.
    val ALLOWED_UIDS = setOf(0, 2000)

    private val running = AtomicBoolean(false)

    /**
     * Application context, captured on [start]. Per-connection sessions
     * read this so action handlers can reach Android system services
     * without each handler having to thread the context through.
     */
    @Volatile internal lateinit var appContext: Context
        private set

    fun start(context: Context) {
        appContext = context.applicationContext
        if (!running.compareAndSet(false, true)) return
        thread(name = "tawc-exec-broker", isDaemon = true) { acceptLoop() }
    }

    private fun acceptLoop() {
        val server = try {
            LocalServerSocket(SOCKET_NAME)
        } catch (t: IOException) {
            Log.e(TAG, "bind LocalServerSocket($SOCKET_NAME) failed", t)
            return
        }
        Log.i(TAG, "Listening on @$SOCKET_NAME (allowed uids: $ALLOWED_UIDS)")
        try {
            while (true) {
                val client = try {
                    server.accept()
                } catch (t: IOException) {
                    Log.w(TAG, "accept failed; broker shutting down", t)
                    break
                }
                val cred = client.peerCredentials
                if (cred.uid !in ALLOWED_UIDS) {
                    Log.w(TAG, "rejecting connection from uid=${cred.uid} pid=${cred.pid}")
                    client.close()
                    continue
                }
                thread(name = "tawc-exec-session-${cred.pid}", isDaemon = true) {
                    try {
                        ExecBrokerSession(client).run()
                    } catch (t: Throwable) {
                        Log.w(TAG, "session error", t)
                    } finally {
                        try { client.close() } catch (_: IOException) {}
                    }
                }
            }
        } finally {
            try { server.close() } catch (_: IOException) {}
        }
    }
}
