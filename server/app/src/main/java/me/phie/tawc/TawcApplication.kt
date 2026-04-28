package me.phie.tawc

import android.app.Application
import android.util.Log
import me.phie.tawc.install.BootstrapCache
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import java.io.File
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
 *    keeps host-side `client/tawc-chroot-run` working.
 */
class TawcApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        thread(name = "tawc-startup", isDaemon = true) {
            try {
                val n = BootstrapCache(this).sweepStale()
                if (n > 0) Log.i(TAG, "Bootstrap cache: evicted $n stale entries")
            } catch (t: Throwable) {
                Log.w(TAG, "Bootstrap cache sweep failed", t)
            }
            try {
                refreshEnterScripts()
            } catch (t: Throwable) {
                Log.w(TAG, "enter.sh refresh failed", t)
            }
        }
    }

    /**
     * Re-render `<distros>/<id>/enter.sh` for every install in
     * [Installation.State.READY] so the bake-in `nativeLibraryDir`
     * path is fresh after an APK upgrade. Cheap (<1 KB write per
     * install) and idempotent.
     */
    private fun refreshEnterScripts() {
        val store = InstallationStore(this)
        for (inst in store.list()) {
            if (inst.state != Installation.State.READY) continue
            val method = InstallationMethod.forKey(this, inst.method) ?: continue
            val rootfs = store.rootfsDir(inst.id).absolutePath
            val script = method.enterScript(this, rootfs)
            val file = store.enterScriptFile(inst.id)
            try {
                file.writeText(script)
                file.setExecutable(true, false)
                Log.d(TAG, "refreshed ${file.absolutePath} (method=${inst.method})")
            } catch (t: Throwable) {
                Log.w(TAG, "couldn't refresh ${file.absolutePath}", t)
            }
        }
    }

    companion object {
        private const val TAG = "tawc-install"
    }
}
