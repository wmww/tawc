package me.phie.tawc.compositor

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
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import android.system.Os
import android.util.Log
import androidx.core.app.ServiceCompat
import me.phie.tawc.AppPaths
import me.phie.tawc.BuildConfig
import me.phie.tawc.MainActivity
import me.phie.tawc.tasks.ProcessScanner
import java.io.File
import java.lang.ref.WeakReference
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
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
    private val windowRegistry = WindowRegistry()
    private val toplevelCount = MutableStateFlow(0)
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var lifecycle = Lifecycle.STOPPED
    private var restartAfterStop = false

    val openWindows: StateFlow<List<OpenWindow>> = windowRegistry.windows

    inner class LocalBinder : Binder() {
        fun getService(): CompositorService = this@CompositorService
    }

    override fun onCreate() {
        super.onCreate()
        serviceScope.launch {
            toplevelCount.collect { count ->
                if (!lifecycle.postsNotification) return@collect
                val nm = getSystemService(NotificationManager::class.java) ?: return@collect
                nm.notify(NOTIFICATION_ID, buildNotification(count))
            }
        }
        ensureCompositorRunning()
    }

    private fun ensureCompositorRunning() {
        when (lifecycle) {
            Lifecycle.STARTING, Lifecycle.RUNNING -> return
            Lifecycle.STOPPING -> {
                restartAfterStop = true
                return
            }
            Lifecycle.STOPPED -> Unit
        }
        lifecycle = Lifecycle.STARTING
        val appPaths = AppPaths.from(this)

        ensureNotificationChannel()
        // Foreground type "specialUse" is the correct fit on Android 14+ —
        // none of the standard types (mediaPlayback, dataSync, etc.) match
        // a desktop compositor. The app declares the corresponding
        // PROPERTY_SPECIAL_USE_FGS_SUBTYPE in the manifest.
        val foregroundType = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
        } else {
            0
        }
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(windowCount = 0),
            foregroundType,
        )

        // xkbcommon's keymap lookup happens during compositor startup and
        // dereferences a NULL keymap if XKB_CONFIG_ROOT is missing — extract
        // bundled xkb data before starting the compositor thread. Idempotent
        // (skips when files/xkb/.version matches the package version), so
        // it's a no-op on subsequent service restarts.
        ensureXkbDataExtracted()

        // libhybris ships in the APK as a tarball asset (one tree per
        // ABI) and is extracted into the app data dir.
        // [me.phie.tawc.install.TawcInstaller] / LibhybrisInstallProvider
        // copies it into each rootfs at /usr/lib/hybris/ — extracting
        // here before any rootfs entry means the copy always sees a
        // complete tree. Idempotent on the same versionCode.
        ensureLibhybrisExtracted(this)

        // Xwayland's exec'ables and runtime libs ride in the APK as
        // `jniLibs/<abi>/lib{xwayland,xkbcomp,*}.so`, where the OS
        // extracts them into `applicationInfo.nativeLibraryDir`. That
        // dir has the `apk_data_file` SELinux type, which
        // `untrusted_app` is allowed to exec — unlike `app_data_file`
        // (the type assigned to anything we extract into `filesDir`),
        // where `execute_no_trans` is denied on Android 10+ and used
        // to force us to ship a `magiskpolicy --live` rule via su.
        // The compositor PATH-resolves `Xwayland` against
        // `<filesDir>/xwayland/bin/`, where this call lays down
        // symlinks pointing at the real binaries in nativeLibraryDir.
        // Only the XKB share tree (read by fopen via baked-in absolute
        // path) still needs runtime extraction.
        val xwaylandAvailable = ensureXwaylandExtracted(this)

        // The Rust side adds nativeLibraryDir to LD_LIBRARY_PATH so
        // Xwayland's bionic linker finds its DT_NEEDED libs (libX11.so,
        // libxcb.so, …) alongside the binary in apk_data_file context.
        Os.setenv("TAWC_XWAYLAND_ENABLED", if (xwaylandAvailable) "1" else "0", true)
        Os.setenv("TAWC_NATIVE_LIB_DIR", applicationInfo.nativeLibraryDir, true)
        Os.setenv("TAWC_APP_DATA_DIR", appPaths.dataDir.absolutePath, true)
        Os.setenv("TAWC_APP_FILES_DIR", appPaths.filesDir.absolutePath, true)
        Os.setenv("TAWC_APP_SHARE_DIR", appPaths.shareDir.absolutePath, true)
        Os.setenv("TAWC_DISTROS_DIR", appPaths.distrosDir.absolutePath, true)
        Os.setenv("TAWC_XWAYLAND_DIR", appPaths.xwaylandDir.absolutePath, true)
        Os.setenv("TAWC_XWAYLAND_RUNTIME_DIR", appPaths.xwaylandRuntimeDir.absolutePath, true)
        Os.setenv("TAWC_XKB_CONFIG_ROOT", appPaths.xkbDir.absolutePath, true)

        // Hand the application context + service to NativeBridge so its
        // reverse-JNI spawnActivity/finishActivity entry points work even
        // when no Activity is currently in the foreground.
        NativeBridge.attachService(this)
        NativeBridge.nativeStartCompositor(
            me.phie.tawc.Settings.outputScale,
            me.phie.tawc.Settings.xwayland,
            me.phie.tawc.Settings.gtk3BrokenMenusWorkaround,
        )
        ClipboardBridge.syncCurrentTextToNative()
        // Push the saved render-time settings into the compositor. The
        // Rust side defaults match the Settings defaults, so this is
        // only strictly needed when the user has flipped a toggle, but
        // pushing unconditionally keeps the two sides in sync after a
        // process restart with no extra control flow.
        NativeBridge.nativeSetTintBuffersByType(me.phie.tawc.Settings.tintBuffersByType)
        NativeBridge.nativeSetOutputScale(me.phie.tawc.Settings.outputScale)
        NativeBridge.nativeSetXwaylandEnabled(me.phie.tawc.Settings.xwayland)
        NativeBridge.nativeSetGtk3BrokenMenusWorkaround(me.phie.tawc.Settings.gtk3BrokenMenusWorkaround)
        lifecycle = Lifecycle.RUNNING
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_EXIT) {
            exitFromNotification(startId)
            return START_NOT_STICKY
        }
        ensureCompositorRunning()
        // START_STICKY: the system recreates the service after a kill so the
        // compositor comes back even if every Activity has been destroyed.
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        serviceScope.cancel()
        NativeBridge.nativeStopCompositor()
        NativeBridge.detachService()
        activities.clear()
        windowRegistry.clear()
        lifecycle = Lifecycle.STOPPED
        super.onDestroy()
    }

    fun registerActivity(activityId: String, activity: CompositorActivity) {
        if (NativeBridge.consumePendingFinishActivity(activityId)) {
            activity.finishAndRemoveTask()
            return
        }
        activities[activityId] = WeakReference(activity)
        activity.setFullscreenFromCompositor(NativeBridge.fullscreenForActivity(activityId))
        windowRegistry.get(activityId)?.let { activity.setTaskMetadata(it) }
        NativeBridge.replayPendingKeyboardForActivity(activityId, activity)
    }

    fun unregisterActivity(activityId: String) {
        activities.remove(activityId)
    }

    fun removeWindow(activityId: String) {
        windowRegistry.remove(activityId)
    }

    fun updateWindowMetadata(
        activityId: String,
        title: String,
        appId: String,
        desktopId: String,
        desktopName: String,
        iconPath: String,
    ) {
        val window = windowRegistry.updateMetadata(
            activityId = activityId,
            title = title,
            appId = appId,
            desktopId = desktopId,
            desktopName = desktopName,
            iconPath = iconPath,
        )
        getActivity(activityId)?.setTaskMetadata(window)
    }

    fun setWindowFocused(activityId: String, focused: Boolean) {
        windowRegistry.setFocused(activityId, focused, SystemClock.uptimeMillis())
    }

    fun setWindowFullscreen(activityId: String, fullscreen: Boolean) {
        windowRegistry.setFullscreen(activityId, fullscreen)
    }

    fun updateToplevelCount(count: Int) {
        toplevelCount.value = count.coerceAtLeast(0)
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

    /**
     * Return the foreground [CompositorActivity] (the one whose window
     * currently has focus), or null if none. Used by the dev input broker
     * actions to dispatch test events to "the activity tests are driving"
     * — same model as the previous `testInputReceiver.hasWindowFocus()`
     * gate, just looked up centrally here.
     *
     * Walks the (small) `activities` map; expired weak refs are cleaned
     * up as a side-effect of the iteration.
     */
    fun focusedActivity(): CompositorActivity? {
        val it = activities.entries.iterator()
        while (it.hasNext()) {
            val entry = it.next()
            val activity = entry.value.get()
            if (activity == null) {
                it.remove()
            } else if (activity.hasWindowFocus()) {
                return activity
            }
        }
        return null
    }

    private fun exitFromNotification(startId: Int) {
        if (lifecycle == Lifecycle.STOPPING) return
        lifecycle = Lifecycle.STOPPING
        restartAfterStop = false
        serviceScope.launch {
            Log.i(TAG, "Notification exit requested")
            NativeBridge.nativeStopCompositor()
            toplevelCount.value = 0
            windowRegistry.clear()
            finishCompositorActivities()
            withContext(Dispatchers.IO) {
                ProcessScanner.killAllKnownRootfs(this@CompositorService) {
                }
            }
            if (restartAfterStop) {
                lifecycle = Lifecycle.STOPPED
                restartAfterStop = false
                ensureCompositorRunning()
            } else {
                lifecycle = Lifecycle.STOPPED
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelfResult(startId)
            }
        }
    }

    private fun finishCompositorActivities() {
        val liveActivities = activities.values.mapNotNull { it.get() }
        activities.clear()
        for (activity in liveActivities) {
            activity.finishAndRemoveTask()
        }
    }

    private fun ensureXkbDataExtracted() {
        val destDir = File(filesDir, "xkb")
        val currentStamp = currentExtractStamp(this)
        if (!isStampStale("xkb", destDir, currentStamp)) return

        val stagingDir = File(filesDir, "xkb.new")
        stagingDir.deleteRecursively()
        stagingDir.mkdirs()
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
        extractDir("xkb", stagingDir)
        atomicReplaceDir(stagingDir, destDir)
        File(destDir, ".version").writeText(currentStamp)
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

    private fun buildNotification(windowCount: Int): Notification {
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("TAWC running")
            .setContentText("$windowCount Linux windows open")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .setContentIntent(homePendingIntent())
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(
                Notification.Action.Builder(
                    Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel),
                    "Exit",
                    exitPendingIntent(),
                ).build(),
            )
            .build()
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
            this,
            0,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    private fun exitPendingIntent(): PendingIntent {
        val intent = Intent(this, CompositorService::class.java)
            .setAction(ACTION_EXIT)
        return PendingIntent.getService(
            this,
            1,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    private enum class Lifecycle {
        STOPPED,
        STARTING,
        RUNNING,
        STOPPING;

        val postsNotification: Boolean
            get() = this == STARTING || this == RUNNING
    }

    companion object {
        private const val TAG = "tawc"
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "tawc_compositor"
        private const val ACTION_EXIT = "me.phie.tawc.compositor.EXIT"

        fun ensureRunning(context: Context) {
            context.applicationContext.startForegroundService(
                Intent(context.applicationContext, CompositorService::class.java)
            )
        }

        /** Lock for [ensureLibhybrisExtracted] — see method KDoc. */
        private val LIBHYBRIS_EXTRACT_LOCK = Any()
        /** Lock for [ensureXwaylandExtracted] — see method KDoc. */
        private val XWAYLAND_EXTRACT_LOCK = Any()
        /** Lock for [ensureMesaGfxstreamExtracted] — see method KDoc. */
        private val MESA_GFXSTREAM_EXTRACT_LOCK = Any()
        /** Lock for [ensureMesaZinkExtracted] — see method KDoc. */
        private val MESA_ZINK_EXTRACT_LOCK = Any()

        /**
         * Stamp value written to `<destDir>/.version` to gate
         * re-extraction. Combines `longVersionCode` and `lastUpdateTime`
         * (epoch ms): the former gives a human-readable bump on real
         * version changes, the latter ensures `adb install -r` of the
         * same `versionCode` still triggers re-extraction (the system
         * bumps `lastUpdateTime` on every reinstall). Without the
         * `lastUpdateTime` component, devs iterating on Xwayland /
         * libhybris / xkb without bumping `versionCode` silently run
         * against the previous build's binary.
         */
        fun currentExtractStamp(context: Context): String {
            val info = try {
                context.packageManager.getPackageInfo(context.packageName, 0)
            } catch (_: PackageManager.NameNotFoundException) {
                return "v0@0"
            }
            return "v${info.longVersionCode}@${info.lastUpdateTime}"
        }

        /**
         * Returns true if `<destDir>/.version` is missing or doesn't
         * match [currentStamp]. Logs the comparison so a stale extract
         * is one logcat line away if this ever misfires again.
         */
        private fun isStampStale(name: String, destDir: File, currentStamp: String): Boolean {
            val versionFile = File(destDir, ".version")
            val onDisk = if (versionFile.exists()) versionFile.readText().trim() else "missing"
            val stale = onDisk != currentStamp
            Log.i(TAG, "$name extract: stamp=$currentStamp on-disk=$onDisk extracting=$stale")
            return stale
        }

        /**
         * Atomically swap [stagingDir] into place at [destDir]. If
         * [destDir] already has contents, move it aside via `rename`
         * (which works at the directory-entry level, regardless of
         * whether the tree has un-walkable / un-deletable entries) and
         * delete it as a best-effort cleanup afterwards.
         *
         * `File.deleteRecursively()` followed by `renameTo` was the
         * obvious encoding, but `deleteRecursively` returns Boolean
         * silently — when it can't traverse into a legacy symlink tree
         * (e.g. SELinux quirks, fs context oddities) it leaves the
         * destDir half-populated and the subsequent rename fails with
         * EEXIST / ENOTEMPTY. The rename-aside path doesn't depend on
         * walking the contents.
         */
        private fun atomicReplaceDir(stagingDir: File, destDir: File) {
            val asideDir = File(destDir.parentFile, "${destDir.name}.old")
            asideDir.deleteRecursively()
            if (destDir.exists() && !destDir.renameTo(asideDir)) {
                throw java.io.IOException(
                    "Could not move $destDir aside to $asideDir prior to swap " +
                        "(SELinux denial? cross-fs?). Try clearing app storage."
                )
            }
            if (!stagingDir.renameTo(destDir)) {
                // Best-effort restore so we don't leave an empty destDir
                // that the next start would treat as "extracted".
                asideDir.renameTo(destDir)
                throw java.io.IOException(
                    "rename $stagingDir -> $destDir failed; refusing to fall back to a " +
                        "symlink-flattening copy. Try clearing app storage and reinstalling."
                )
            }
            // The aside dir may still contain legacy entries we couldn't
            // delete via Java; that's fine — the rename has already
            // taken effect, and a stuck `.old` only costs disk.
            asideDir.deleteRecursively()
        }

        /**
         * Extract `assets/libhybris/<abi>.tar` into `filesDir/libhybris/`,
         * preserving symlinks. Idempotent — gated by [currentExtractStamp]
         * via a `.version` stamp written last, so a partial extract is
         * indistinguishable from "never extracted" and gets retried.
         *
         * The tar's contents are flat (libEGL.so, libhybris/, gl-shims/,
         * … at the tar root) — see `app/build.gradle.kts` `packLibhybris`
         * — so the extracted tree sits directly at `<filesDir>/libhybris/`,
         * which is where [me.phie.tawc.install.LibhybrisInstallProvider]
         * walks from to copy into each rootfs.
         *
         * Called both from this Service's `onCreate` (compositor boot)
         * and from [me.phie.tawc.install.TawcInstaller] during install /
         * APK upgrade. On a fresh device with both happening at the same
         * time we'd have two extractors racing the same staging dir —
         * synchronize on a process-level lock so only one runs.
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
            val currentStamp = currentExtractStamp(context)
            if (!isStampStale("libhybris", destDir, currentStamp)) {
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

            // Swap stagingDir into place. No copy fallback: `copyRecursively`
            // follows symlinks instead of preserving them, which would
            // silently turn libhybris's symlink topology into duplicate
            // file copies.
            atomicReplaceDir(stagingDir, destDir)
            File(destDir, ".version").writeText(currentStamp)
            Log.i(TAG, "Extracted libhybris ($abi) to $destDir")
            return true
        }

        /**
         * Names of the two raw assets shipped under
         * `assets/mesa-gfxstream/<abi>/` by Gradle's `packMesaGfxstream<Abi>`.
         * Listed here so [ensureMesaGfxstreamExtracted] and
         * [me.phie.tawc.install.BridgeInstallProvider] agree on what to
         * stage / install — change once, both sides follow.
         *
         * The Mesa source emits the ICD JSON with an arch-suffixed name
         * (`gfxstream_vk_icd.aarch64.json` / `.x86_64.json`); the Gradle
         * pack task renames to a single arch-free name so the runtime
         * doesn't need a per-ABI branch here.
         */
        const val MESA_GFXSTREAM_LIB_ASSET = "libvulkan_gfxstream.so"
        const val MESA_GFXSTREAM_ICD_ASSET = "gfxstream_vk_icd.json"

        /**
         * Extract `assets/mesa-gfxstream/<abi>/{libvulkan_gfxstream.so,
         * gfxstream_vk_icd.json}` into `<filesDir>/mesa-gfxstream/`. Picks
         * the asset subdir by [Build.SUPPORTED_ABIS][0]. Same versioned-
         * stamp + atomic-rename pattern as [ensureLibhybrisExtracted],
         * minus the tar walk — these are two raw asset files with no
         * symlink topology to preserve.
         *
         * Returns true if extracted (or already up to date), false if no
         * asset is shipped for this device's ABI.
         */
        fun ensureMesaGfxstreamExtracted(context: Context): Boolean = synchronized(MESA_GFXSTREAM_EXTRACT_LOCK) {
            val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: return false
            val assetDir = "mesa-gfxstream/$abi"
            val available = try {
                context.assets.open("$assetDir/$MESA_GFXSTREAM_LIB_ASSET").close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
                Log.i(TAG, "No mesa-gfxstream asset shipped for ABI $abi; gfxstream backend unavailable")
                return false
            }

            val destDir = File(context.filesDir, "mesa-gfxstream")
            val currentStamp = currentExtractStamp(context)
            if (!isStampStale("mesa-gfxstream", destDir, currentStamp)) {
                return true
            }

            val stagingDir = File(context.filesDir, "mesa-gfxstream.new")
            stagingDir.deleteRecursively()
            stagingDir.mkdirs()
            for (name in listOf(MESA_GFXSTREAM_LIB_ASSET, MESA_GFXSTREAM_ICD_ASSET)) {
                context.assets.open("$assetDir/$name").use { input ->
                    File(stagingDir, name).outputStream().use { out -> input.copyTo(out) }
                }
            }
            atomicReplaceDir(stagingDir, destDir)
            File(destDir, ".version").writeText(currentStamp)
            Log.i(TAG, "Extracted mesa-gfxstream ($abi) to $destDir")
            return true
        }

        /**
         * Extract `assets/mesa-zink/<abi>/mesa-zink.tar` into
         * `<filesDir>/mesa-zink/`, preserving symlinks. Same shape as
         * [ensureLibhybrisExtracted] — tar entries are flat (libEGL_mesa.so,
         * libgallium-*.so, libgbm.so.1, plus soname symlinks) so the
         * extracted tree lands directly at `<filesDir>/mesa-zink/`, which
         * is where [me.phie.tawc.install.MesaZinkInstallProvider] walks
         * from to ship the files into each rootfs.
         *
         * Consumed only by the [me.phie.tawc.GraphicsBackend.LIBHYBRIS_ZINK]
         * backend — see notes/libhybris-zink.md for the Mesa patch
         * (`06-tawc-zink-nokms.patch`) that makes Zink usable without
         * `/dev/dri/`. Other backends never load these libs.
         *
         * Returns true on success or if no asset is shipped for this ABI.
         */
        fun ensureMesaZinkExtracted(context: Context): Boolean = synchronized(MESA_ZINK_EXTRACT_LOCK) {
            val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: return false
            val assetPath = "mesa-zink/$abi/mesa-zink.tar"
            val available = try {
                context.assets.open(assetPath).close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
                Log.i(TAG, "No mesa-zink asset shipped for ABI $abi; LIBHYBRIS_ZINK backend unavailable")
                return false
            }

            val destDir = File(context.filesDir, "mesa-zink")
            val currentStamp = currentExtractStamp(context)
            if (!isStampStale("mesa-zink", destDir, currentStamp)) {
                return true
            }

            val stagingDir = File(context.filesDir, "mesa-zink.new")
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
                            throw java.io.IOException("mesa-zink tar entry escapes staging: ${entry.name}")
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
            atomicReplaceDir(stagingDir, destDir)
            File(destDir, ".version").writeText(currentStamp)
            Log.i(TAG, "Extracted mesa-zink ($abi) to $destDir")
            return true
        }

        /**
         * Lay down the runtime layout Xwayland expects under
         * `<filesDir>/xwayland/`:
         *  - `bin/Xwayland`, `bin/xkbcomp` — symlinks into
         *    `nativeLibraryDir/lib{xwayland,xkbcomp}.so`. The exec'ables
         *    live in `apk_data_file` context (where untrusted_app may
         *    exec), unlike anything we'd extract into filesDir.
         *  - `share/X11`, `share/xkeyboard-config-2` — extracted from
         *    `assets/xwayland/share.tar`. Xwayland reads these via
         *    fopen at the baked-in `-Dxkb_dir` path and the files
         *    cross-reference each other by relative path inside the
         *    tree, so we can't flatten them into jniLibs.
         *
         * Same versioned-stamp + staging-dir-then-rename pattern as
         * [ensureLibhybrisExtracted]. Returns true only when the build
         * config, native binaries, and asset tree are all present. Otherwise
         * Xwayland won't spawn (X11 clients see :0 connection-refused) and
         * Wayland clients keep working.
         */
        fun ensureXwaylandExtracted(context: Context): Boolean = synchronized(XWAYLAND_EXTRACT_LOCK) {
            if (!BuildConfig.XWAYLAND_ENABLED) {
                clearXwaylandExtraction(context)
                Log.i(TAG, "Xwayland disabled by build config; X11 clients will fail to connect")
                return false
            }

            val nativeLibDir = context.applicationInfo.nativeLibraryDir
            val nativeXwayland = File(nativeLibDir, "libxwayland.so")
            val nativeXkbcomp = File(nativeLibDir, "libxkbcomp.so")
            if (!nativeXwayland.exists() || !nativeXkbcomp.exists()) {
                clearXwaylandExtraction(context)
                Log.i(TAG, "No Xwayland native binaries shipped; X11 clients will fail to connect")
                return false
            }

            val assetPath = "xwayland/share.tar"
            val available = try {
                context.assets.open(assetPath).close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
                clearXwaylandExtraction(context)
                Log.i(TAG, "No Xwayland asset shipped; X11 clients will fail to connect")
                return false
            }

            val destDir = File(context.filesDir, "xwayland")
            val currentStamp = currentExtractStamp(context)
            if (!isStampStale("xwayland", destDir, currentStamp)) {
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
                            }
                        }
                    }
                }
            }

            // Symlink bin/{Xwayland,xkbcomp} → nativeLibraryDir/lib*.so.
            // PATH lookup in the compositor finds the symlink, execve(2)
            // follows to the real file, SELinux checks the *target's*
            // domain (apk_data_file) — allowed.
            val binDir = File(stagingDir, "bin").apply { mkdirs() }
            Os.symlink(nativeXwayland.absolutePath, File(binDir, "Xwayland").absolutePath)
            Os.symlink(nativeXkbcomp.absolutePath, File(binDir, "xkbcomp").absolutePath)

            atomicReplaceDir(stagingDir, destDir)
            File(destDir, ".version").writeText(currentStamp)
            Log.i(TAG, "Staged Xwayland under $destDir (binaries -> $nativeLibDir)")
            return true
        }

        private fun clearXwaylandExtraction(context: Context) {
            File(context.filesDir, "xwayland").deleteRecursively()
            File(context.filesDir, "xwayland.new").deleteRecursively()
        }
    }
}
