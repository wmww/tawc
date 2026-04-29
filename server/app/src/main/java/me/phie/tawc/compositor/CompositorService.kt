package me.phie.tawc.compositor

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.system.Os
import android.util.Log
import android.view.WindowManager
import java.io.File
import java.lang.ref.WeakReference
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream

/**
 * Foreground service that owns the Rust Wayland compositor thread for the
 * lifetime of the tawc process. The compositor outlives any single
 * [CompositorActivity], which is the prerequisite for the multi-window
 * design (see notes/multi-activity.md).
 *
 * Activities bind to this service to register themselves; the service
 * tracks them by `activityId` so reverse-JNI calls (keyboard show/hide,
 * future per-host operations) can find the right Activity's view.
 *
 * The service is `START_STICKY`: if Android kills it under memory
 * pressure, the OS recreates it (without the original Intent), and our
 * `onCreate` re-spawns the compositor thread. Wayland clients connected
 * over the chroot socket will see a brief disconnect and reconnect.
 */
class CompositorService : Service() {

    private val binder = LocalBinder()
    private val activities = mutableMapOf<String, WeakReference<CompositorActivity>>()

    /** State-query broadcast lives on the service (always alive) rather
     *  than on a CompositorActivity (only exists when there's a window).
     *  Tests poll this before any chroot client has connected. */
    private val queryStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            NativeBridge.nativeQueryState()
        }
    }

    inner class LocalBinder : Binder() {
        fun getService(): CompositorService = this@CompositorService
    }

    override fun onCreate() {
        super.onCreate()
        Log.d(TAG, "CompositorService onCreate")

        ensureNotificationChannel()
        // Foreground type "specialUse" is the correct fit on Android 14+ —
        // none of the standard types (mediaPlayback, dataSync, etc.) match
        // a desktop compositor. The app declares the corresponding
        // PROPERTY_SPECIAL_USE_FGS_SUBTYPE in the manifest.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                buildNotification(),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            )
        } else {
            startForeground(NOTIFICATION_ID, buildNotification())
        }

        // xkbcommon's keymap lookup happens during compositor startup and
        // dereferences a NULL keymap if XKB_CONFIG_ROOT is missing — extract
        // bundled xkb data before starting the compositor thread. Idempotent
        // (skips when files/xkb/.version matches the package version), so
        // it's a no-op on subsequent service restarts.
        ensureXkbDataExtracted()

        // libhybris ships in the APK as a tarball asset (one tree per ABI)
        // and is extracted into the app data dir. The chroot installer
        // symlinks individual files in /usr/local/lib/ at install time —
        // extracting before the chroot install means the symlinks always
        // land on a complete tree. Idempotent on the same versionCode.
        ensureLibhybrisExtracted(this)

        // Xwayland is bionic-built and packed in the APK as
        // `assets/xwayland/<abi>.tar`. Extracted into
        // `<filesDir>/xwayland/{bin,lib,share}/` so the compositor can
        // exec it as a child process; PATH + LD_LIBRARY_PATH are set
        // by the Rust side in `compositor::xwayland::start_xwayland`.
        ensureXwaylandExtracted(this)

        // Hand the application context + service to NativeBridge so its
        // reverse-JNI spawnActivity/finishActivity entry points work even
        // when no Activity is currently in the foreground.
        NativeBridge.attachService(this)
        // Pass the display size so the compositor can advertise a real
        // initial output_logical_size before any CompositorActivity has
        // registered. Otherwise a client (notably vkcube) that connects
        // before the first Activity boots receives configure(0,0), creates
        // a default-sized swapchain, then receives a real size mid-flight
        // when the Activity finally registers — Vulkan WSI doesn't recover
        // from that and the cube hangs after committing two buffers.
        val (w, h) = currentDisplaySize()
        NativeBridge.nativeStartCompositor(w, h)

        @Suppress("UnspecifiedRegisterReceiverFlag")
        registerReceiver(
            queryStateReceiver,
            IntentFilter("me.phie.tawc.QUERY_STATE"),
            RECEIVER_EXPORTED,
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // START_STICKY: the system recreates the service after a kill so the
        // compositor comes back even if every Activity has been destroyed.
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        Log.d(TAG, "CompositorService onDestroy — stopping compositor")
        try { unregisterReceiver(queryStateReceiver) } catch (_: IllegalArgumentException) {}
        NativeBridge.nativeStopCompositor()
        NativeBridge.detachService()
        activities.clear()
        super.onDestroy()
    }

    fun registerActivity(activityId: String, activity: CompositorActivity) {
        activities[activityId] = WeakReference(activity)
        Log.d(TAG, "Registered activity $activityId (count=${activities.size})")
    }

    fun unregisterActivity(activityId: String) {
        activities.remove(activityId)
        Log.d(TAG, "Unregistered activity $activityId (count=${activities.size})")
    }

    /** Look up an alive Activity by id. Returns null if it was GC'd. */
    fun getActivity(activityId: String): CompositorActivity? {
        val ref = activities[activityId] ?: return null
        val activity = ref.get()
        if (activity == null) {
            // Weak ref expired — clean up the dead entry.
            activities.remove(activityId)
        }
        return activity
    }

    /** Read the current display size (physical pixels) without needing an
     *  Activity. WindowManager.maximumWindowMetrics returns the full
     *  display bounds — close enough to the per-Activity SurfaceView size
     *  (CompositorActivity uses immersive fullscreen) to seed the initial
     *  configure correctly; refined on the first nativeRegister. */
    private fun currentDisplaySize(): Pair<Int, Int> {
        val wm = getSystemService(WindowManager::class.java) ?: return 0 to 0
        val bounds = wm.maximumWindowMetrics.bounds
        return bounds.width() to bounds.height()
    }

    private fun ensureXkbDataExtracted() {
        val destDir = File(filesDir, "xkb")
        val versionFile = File(destDir, ".version")
        val currentVersion = try {
            packageManager.getPackageInfo(packageName, 0).longVersionCode
        } catch (_: PackageManager.NameNotFoundException) { 0L }

        if (versionFile.exists() && versionFile.readText().trim() == currentVersion.toString()) return

        destDir.deleteRecursively()
        fun extractDir(assetPath: String, destPath: File) {
            val children = assets.list(assetPath) ?: return
            if (children.isEmpty()) {
                assets.open(assetPath).use { input ->
                    destPath.outputStream().use { output -> input.copyTo(output) }
                }
            } else {
                destPath.mkdirs()
                for (child in children) {
                    extractDir("$assetPath/$child", File(destPath, child))
                }
            }
        }
        extractDir("xkb", destDir)
        versionFile.writeText(currentVersion.toString())
        Log.d(TAG, "Extracted xkb data to $destDir")
    }

    private fun ensureNotificationChannel() {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID, "Compositor", NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Background notification for the running Wayland compositor."
            setShowBadge(false)
        }
        nm.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("tawc compositor")
            .setContentText("Wayland compositor is running")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
    }

    companion object {
        private const val TAG = "tawc"
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "tawc_compositor"

        /** Lock for [ensureLibhybrisExtracted] — see method KDoc. */
        private val LIBHYBRIS_EXTRACT_LOCK = Any()
        /** Lock for [ensureXwaylandExtracted] — see method KDoc. */
        private val XWAYLAND_EXTRACT_LOCK = Any()

        /**
         * Extract `assets/libhybris/<abi>.tar` into `filesDir/libhybris/`,
         * preserving symlinks. Idempotent — versioned by `longVersionCode`
         * via a `.version` stamp written last, so a partial extract is
         * indistinguishable from "never extracted" and gets retried.
         *
         * Called both from this Service's `onCreate` (compositor boot)
         * and from [me.phie.tawc.install.LibhybrisLinker.link] during
         * chroot install. On a fresh device with both happening at the
         * same time we'd have two extractors racing the same staging
         * dir — synchronize on a process-level lock so only one runs.
         *
         * Returns true if extracted (or was already up-to-date), false
         * if no asset is shipped for this ABI (e.g. emulator build).
         */
        fun ensureLibhybrisExtracted(context: Context): Boolean = synchronized(LIBHYBRIS_EXTRACT_LOCK) {
            val abi = Build.SUPPORTED_ABIS.firstOrNull()
                ?: return false
            val assetPath = "libhybris/$abi.tar"
            val available = try {
                context.assets.open(assetPath).close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
                Log.i(TAG, "No libhybris asset shipped for ABI $abi; skipping extract")
                return false
            }

            val destDir = File(context.filesDir, "libhybris")
            val versionFile = File(destDir, ".version")
            val currentVersion = try {
                context.packageManager
                    .getPackageInfo(context.packageName, 0).longVersionCode.toString()
            } catch (_: PackageManager.NameNotFoundException) { "0" }

            if (versionFile.exists() && versionFile.readText().trim() == currentVersion) {
                return true
            }

            // Stage into `.new` and swap, so a partial extract from a
            // prior interrupted run gets cleaned up rather than read.
            val stagingDir = File(context.filesDir, "libhybris.new")
            stagingDir.deleteRecursively()
            stagingDir.mkdirs()
            // Containment guard: defence-in-depth against a libhybris
            // build that ever produces a tar entry with `..` in its
            // path. Asset is built from our own cross-compile so this
            // shouldn't fire, but the extractor in
            // [me.phie.tawc.install.ProotArchiveExtractor] does the
            // same check; keep both consistent.
            val stagingReal = stagingDir.canonicalFile
            val stagingPrefix = stagingReal.absolutePath + File.separator

            context.assets.open(assetPath).use { raw ->
                TarArchiveInputStream(raw).use { tar ->
                    while (true) {
                        val entry = tar.nextEntry ?: break
                        val outFile = File(stagingDir, entry.name).canonicalFile
                        val abs = outFile.absolutePath
                        if (abs != stagingReal.absolutePath && !abs.startsWith(stagingPrefix)) {
                            throw java.io.IOException("libhybris tar entry escapes staging: ${entry.name}")
                        }
                        when {
                            entry.isDirectory -> outFile.mkdirs()
                            entry.isSymbolicLink -> {
                                outFile.parentFile?.mkdirs()
                                // Os.symlink errors if target exists; in
                                // a fresh staging dir it never does.
                                Os.symlink(entry.linkName, outFile.absolutePath)
                            }
                            else -> {
                                outFile.parentFile?.mkdirs()
                                outFile.outputStream().use { out -> tar.copyTo(out) }
                                if ((entry.mode and 0b001_001_001) != 0) {
                                    outFile.setExecutable(true, false)
                                }
                            }
                        }
                    }
                }
            }

            destDir.deleteRecursively()
            // No copy fallback for the rename: `copyRecursively` follows
            // symlinks instead of preserving them, which would silently
            // turn libhybris's symlink topology into duplicate file
            // copies. rename(2) within the same parent dir works on
            // every Android filesystem we ship on; if it ever fails we
            // want to fail loudly and investigate, not paper over it.
            if (!stagingDir.renameTo(destDir)) {
                throw java.io.IOException(
                    "rename $stagingDir -> $destDir failed; refusing to fall back to a " +
                        "symlink-flattening copy. Try clearing app storage and reinstalling."
                )
            }
            File(destDir, ".version").writeText(currentVersion)
            Log.i(TAG, "Extracted libhybris ($abi) to $destDir")
            return true
        }

        /**
         * Extract `assets/xwayland/<abi>.tar` into `filesDir/xwayland/`,
         * preserving symlinks. Same versioned-stamp + staging-dir-then-
         * rename pattern as [ensureLibhybrisExtracted].
         *
         * The tar contains `bin/Xwayland`, `bin/xkbcomp`, `lib/` (the
         * bionic-built shared libs Xwayland's DT_NEEDED references),
         * `share/X11/xkb` (a relative symlink to `share/xkeyboard-config-2`
         * — the build script rewrites the install-time absolute symlink
         * for portability), and `share/xkeyboard-config-2/` (the actual
         * XKB rules / symbols / geometry data files).
         *
         * Returns true on success or if no asset is shipped for this
         * ABI. The compositor handles the latter gracefully — Xwayland
         * just won't spawn (X11 clients that try `:0` get
         * connection-refused), and Wayland clients keep working.
         */
        fun ensureXwaylandExtracted(context: Context): Boolean = synchronized(XWAYLAND_EXTRACT_LOCK) {
            val abi = Build.SUPPORTED_ABIS.firstOrNull()
                ?: return false
            val assetPath = "xwayland/$abi.tar"
            val available = try {
                context.assets.open(assetPath).close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
                Log.i(TAG, "No Xwayland asset shipped for ABI $abi; X11 clients will fail to connect")
                return false
            }

            val destDir = File(context.filesDir, "xwayland")
            val versionFile = File(destDir, ".version")
            val currentVersion = try {
                context.packageManager
                    .getPackageInfo(context.packageName, 0).longVersionCode.toString()
            } catch (_: PackageManager.NameNotFoundException) { "0" }

            if (versionFile.exists() && versionFile.readText().trim() == currentVersion) {
                return true
            }

            val stagingDir = File(context.filesDir, "xwayland.new")
            stagingDir.deleteRecursively()
            stagingDir.mkdirs()
            val stagingReal = stagingDir.canonicalFile
            val stagingPrefix = stagingReal.absolutePath + File.separator

            context.assets.open(assetPath).use { raw ->
                TarArchiveInputStream(raw).use { tar ->
                    while (true) {
                        val entry = tar.nextEntry ?: break
                        val outFile = File(stagingDir, entry.name).canonicalFile
                        val abs = outFile.absolutePath
                        if (abs != stagingReal.absolutePath && !abs.startsWith(stagingPrefix)) {
                            throw java.io.IOException("Xwayland tar entry escapes staging: ${entry.name}")
                        }
                        when {
                            entry.isDirectory -> outFile.mkdirs()
                            entry.isSymbolicLink -> {
                                outFile.parentFile?.mkdirs()
                                Os.symlink(entry.linkName, outFile.absolutePath)
                            }
                            else -> {
                                outFile.parentFile?.mkdirs()
                                outFile.outputStream().use { out -> tar.copyTo(out) }
                                if ((entry.mode and 0b001_001_001) != 0) {
                                    outFile.setExecutable(true, false)
                                }
                            }
                        }
                    }
                }
            }

            destDir.deleteRecursively()
            if (!stagingDir.renameTo(destDir)) {
                throw java.io.IOException(
                    "rename $stagingDir -> $destDir failed; refusing to fall back to a " +
                        "symlink-flattening copy. Try clearing app storage and reinstalling."
                )
            }
            File(destDir, ".version").writeText(currentVersion)
            Log.i(TAG, "Extracted Xwayland ($abi) to $destDir")
            return true
        }
    }
}
