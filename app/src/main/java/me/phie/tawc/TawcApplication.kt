package me.phie.tawc

import android.app.Application
import android.util.Log
import me.phie.tawc.dev.ExecBroker
import me.phie.tawc.install.BootstrapCache
import me.phie.tawc.ops.OperationsNotificationCenter
import kotlin.concurrent.thread

/**
 * Process-wide entry point. Used for cheap startup chores that have no
 * UI and shouldn't block onCreate of the launcher / compositor:
 *
 *  - Sweep stale bootstrap-tarball cache entries — the OS only evicts
 *    `cacheDir` under storage pressure, so a 200 MB tarball can squat
 *    on disk for months without our own TTL ([BootstrapCache.sweepStale]).
 *  - Refresh on-disk `enter.sh` for every installed distro. The
 *    script bakes in `applicationInfo.nativeLibraryDir`, which is a
 *    `/data/app/~~<hash>/...` path that changes on every APK
 *    re-install. If the user/test loop runs `adb install -r` (or
 *    Play Store auto-update), the path stamped at install time
 *    points at the *previous* APK's lib dir, which the OS deletes
 *    when the new APK is in place. Re-rendering on every cold start
 *    keeps host-side `scripts/tawc-chroot-run.sh` working.
 */
class TawcApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        // Start the per-op notification center before any service can
        // register an Operation. The center is the single owner of the
        // `tawc-operations` notification channel and the
        // OperationsRegistry → notification fan-out — see
        // me.phie.tawc.ops package KDoc.
        OperationsNotificationCenter.start(this)
        thread(name = "tawc-startup", isDaemon = true) {
            try {
                val n = BootstrapCache(this).sweepStale()
                if (n > 0) Log.i(TAG, "Bootstrap cache: evicted $n stale entries")
            } catch (t: Throwable) {
                Log.w(TAG, "Bootstrap cache sweep failed", t)
            }
        }
        // Dev-only exec broker. Started here (not from MainActivity)
        // so it's available no matter which Activity / Service the
        // cold-start went through — the install + integration test
        // flows often start at InstallActivity. Release builds skip
        // this entirely. See notes/exec-broker.md.
        if (BuildConfig.DEBUG) {
            ExecBroker.start(this)
            // Action handlers must register before any host connection;
            // the broker thread spawned by start() above accepts asynchronously
            // but won't dispatch ACTION headers to a missing handler.
            me.phie.tawc.install.InstallActions.registerAll()
        }
    }

    companion object {
        private const val TAG = "tawc-install"
    }
}
