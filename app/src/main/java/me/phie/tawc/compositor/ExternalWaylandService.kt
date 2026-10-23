package me.phie.tawc.compositor

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.os.Parcel
import android.os.ParcelFileDescriptor
import android.os.Process
import android.util.Log
import me.phie.tawc.ExternalAccessMode
import me.phie.tawc.Settings

/**
 * Exported fd source for Wayland clients outside TAWC's uid.
 *
 * A Termux process cannot connect to `share/wayland-0`: `/data/data/<pkg>`
 * is `0700` (DAC) and a cross-app `connect()` between two `untrusted_app`
 * domains is denied by SELinux. So the client asks here for one end of a
 * socketpair; the other end goes into the compositor's display through
 * [NativeBridge.nativeAdoptClientFd]. No `connect()` ever happens, so
 * neither check applies.
 *
 * Two entry points, with different caller-identity stories:
 *
 *  - [ExternalBinder.openClient] over Binder, for a real app that can
 *    `bindService`. `Binder.getCallingUid()` is the caller.
 *  - [onStartCommand], for the Termux `app_process` launcher, which AMS
 *    refuses to bind. The start is delivered by AMS, so
 *    `Binder.getCallingUid()` is TAWC itself; the caller is recovered
 *    from `SO_PEERCRED` on the socketpair the launcher created.
 *
 * Both funnel into [ExternalWaylandAccess], which applies the user's
 * [me.phie.tawc.ExternalAccessMode] and allowlist.
 */
class ExternalWaylandService : Service() {

    private val binder = ExternalBinder()

    inner class ExternalBinder : IExternalWayland.Stub() {
        override fun openClient(): ParcelFileDescriptor {
            val uid = Binder.getCallingUid()
            val decision = ExternalWaylandAccess.decide(
                callerUid = uid,
                callerPackages = packagesFor(uid),
                ownUid = Process.myUid(),
            )
            if (!decision.allowed) {
                Log.w(TAG, "external Wayland: refused uid $uid: ${decision.reason}")
                throw SecurityException("external Wayland: ${decision.reason}")
            }
            return newClient()
        }
    }

    /** One fresh socketpair; the near end is handed to native. */
    private fun newClient(): ParcelFileDescriptor {
        val pair = ParcelFileDescriptor.createSocketPair()

        // Native owns the near end from here. With no compositor thread
        // it queues the fd and asks Kotlin to start one.
        if (!NativeBridge.nativeAdoptClientFd(pair[1].detachFd())) {
            pair[0].close()
            throw IllegalStateException("compositor refused the client fd")
        }
        return pair[0]
    }

    override fun onCreate() {
        super.onCreate()
        // Nothing below is needed for a caller that will be refused, and a
        // disabled policy means there can be none.
        if (Settings.externalAccessMode == ExternalAccessMode.DISABLED) return

        // Sets the TAWC_* env, caches the app context for the reverse-JNI
        // onActivationRequested callback, and starts the socket holder.
        // Idempotent; must precede the first openClient().
        CompositorService.ensureActivation(this)

        // Start the compositor now rather than on the first
        // nativeAdoptClientFd queue miss.
        CompositorService.ensureRunning(this)
    }

    /**
     * Termux's `wayland-run` cannot bindService: it is an `app_process`
     * process, and AMS rejects binds from callers without a process
     * record. It starts this service with a Binder token instead, then
     * calls the existing `IExternalWayland.openClient()` on it. That
     * synchronous Binder call is what carries the fd; an Intent cannot
     * (AMS rejects `startService` intents with file descriptors).
     *
     * `wayland-run` then execve()s itself into the client, which destroys
     * its Binder threads. Before it does that it waits for
     * [TRANSACTION_CLIENT_ADOPTED]; sending that only after the fd is
     * handled guarantees the reply was already delivered.
     *
     * The caller identity AMS reports here is TAWC itself, so the access
     * check runs on the socketpair's peer uid instead — the launcher
     * creates the pair in its own process, so `SO_PEERCRED` names it.
     */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val host = intent?.getBundleExtra(EXTRA_HOST)?.getBinder(KEY_HOST)
        if (host == null) {
            Log.w(TAG, "external Wayland: startService without host binder")
            stopSelf(startId)
            return START_NOT_STICKY
        }

        try {
            val iface = IExternalWayland.Stub.asInterface(host)
            val pfd = iface.openClient()
            if (pfd == null) {
                Log.w(TAG, "external Wayland: host returned no client fd")
            } else {
                // Authenticate before adopting. `openClient()` above went
                // to the launcher's own binder, so it created the
                // socketpair in the launcher's process and the peer
                // credential names the launcher, not AMS.
                val peerUid = NativeBridge.nativePeerUid(pfd.fd)
                val decision = ExternalWaylandAccess.decide(
                    callerUid = peerUid,
                    callerPackages = packagesFor(peerUid),
                    ownUid = Process.myUid(),
                )
                if (!decision.allowed) {
                    Log.w(TAG, "external Wayland: refused peer uid $peerUid: ${decision.reason}")
                    pfd.close()
                    // Tell the launcher why, so it can report a clear error
                    // instead of exec'ing a client that would just fail to
                    // connect. An older launcher ignores the transaction
                    // (it is one-way) and falls back to exec'ing anyway.
                    notifyRefused(host, decision.reason)
                } else if (!NativeBridge.nativeAdoptClientFd(pfd.detachFd())) {
                    Log.w(TAG, "external Wayland: compositor refused the client fd")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "external Wayland: host callback failed", e)
        } finally {
            notifyAdopted(host)
        }

        stopSelf(startId)
        return START_NOT_STICKY
    }

    /**
     * Lets the launcher exec the client now that the fd cannot be lost.
     *
     * Sent even after a refusal (see [notifyRefused]): an older launcher
     * has no refusal handler and would otherwise wait out its whole
     * timeout, and a launcher that did read the refusal has already
     * exited.
     */
    private fun notifyAdopted(host: IBinder) {
        val data = Parcel.obtain()
        try {
            host.transact(TRANSACTION_CLIENT_ADOPTED, data, null, IBinder.FLAG_ONEWAY)
        } catch (e: Exception) {
            Log.w(TAG, "external Wayland: adopted signal failed", e)
        } finally {
            data.recycle()
        }
    }

    /** Explains a denial, ahead of [notifyAdopted]. */
    private fun notifyRefused(host: IBinder, reason: String) {
        val data = Parcel.obtain()
        try {
            data.writeString(reason)
            host.transact(TRANSACTION_CLIENT_REFUSED, data, null, IBinder.FLAG_ONEWAY)
        } catch (e: Exception) {
            Log.w(TAG, "external Wayland: refused signal failed", e)
        } finally {
            data.recycle()
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    /** Packages owning [uid]; empty when unknown or hidden. */
    private fun packagesFor(uid: Int): List<String> {
        if (uid < 0) return emptyList()
        return packageManager.getPackagesForUid(uid)?.toList().orEmpty()
    }

    companion object {
        private const val TAG = "tawc"

        /** Matches WaylandRun's EXTRA_HOST / KEY_HOST. */
        const val EXTRA_HOST = "me.phie.tawc.extra.HOST"
        const val KEY_HOST = "host"

        /**
         * Matches WaylandRun's TRANSACTION_CLIENT_ADOPTED. The generated
         * stub uses FIRST_CALL_TRANSACTION for openClient(), so this is 2.
         */
        const val TRANSACTION_CLIENT_ADOPTED = 2

        /**
         * Matches WaylandRun's TRANSACTION_CLIENT_REFUSED: a one-way call
         * carrying the denial reason, sent ahead of [TRANSACTION_CLIENT_ADOPTED].
         */
        const val TRANSACTION_CLIENT_REFUSED = 3
    }
}
