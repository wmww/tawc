package me.phie.tawc.compositor

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
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
        ensureXwaylandExtracted(this)

        // The Rust side adds nativeLibraryDir to LD_LIBRARY_PATH so
        // Xwayland's bionic linker finds its DT_NEEDED libs (libX11.so,
        // libxcb.so, …) alongside the binary in apk_data_file context.
        Os.setenv("TAWC_NATIVE_LIB_DIR", applicationInfo.nativeLibraryDir, true)

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
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // START_STICKY: the system recreates the service after a kill so the
        // compositor comes back even if every Activity has been destroyed.
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        Log.d(TAG, "CompositorService onDestroy — stopping compositor")
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
        /** Lock for [ensureMesaGfxstreamExtracted] — see method KDoc. */
        private val MESA_GFXSTREAM_EXTRACT_LOCK = Any()

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
         * `assets/mesa-gfxstream/` by Gradle's `packMesaGfxstream`. Listed
         * here so [ensureMesaGfxstreamExtracted] and
         * [me.phie.tawc.install.BridgeInstallProvider] agree on what to
         * stage / install — change once, both sides follow.
         */
        const val MESA_GFXSTREAM_LIB_ASSET = "libvulkan_gfxstream.so"
        const val MESA_GFXSTREAM_ICD_ASSET = "gfxstream_vk_icd.aarch64.json"

        /**
         * Extract `assets/mesa-gfxstream/{libvulkan_gfxstream.so,
         * gfxstream_vk_icd.aarch64.json}` into `<filesDir>/mesa-gfxstream/`.
         * Same versioned-stamp + atomic-rename pattern as
         * [ensureLibhybrisExtracted], minus the tar walk — these are two
         * raw asset files with no symlink topology to preserve.
         *
         * Returns true if extracted (or already up to date), false if the
         * asset isn't shipped (e.g. x86_64 emulator build — the bridge is
         * aarch64-only today).
         */
        fun ensureMesaGfxstreamExtracted(context: Context): Boolean = synchronized(MESA_GFXSTREAM_EXTRACT_LOCK) {
            val available = try {
                context.assets.open("mesa-gfxstream/$MESA_GFXSTREAM_LIB_ASSET").close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
                Log.i(TAG, "No mesa-gfxstream asset shipped; gfxstream backend unavailable")
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
                context.assets.open("mesa-gfxstream/$name").use { input ->
                    File(stagingDir, name).outputStream().use { out -> input.copyTo(out) }
                }
            }
            atomicReplaceDir(stagingDir, destDir)
            File(destDir, ".version").writeText(currentStamp)
            Log.i(TAG, "Extracted mesa-gfxstream to $destDir")
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
         * [ensureLibhybrisExtracted]. Returns true on success or if no
         * asset is shipped (eg. emulator x86_64 build) — Xwayland just
         * won't spawn (X11 clients see :0 connection-refused) and
         * Wayland clients keep working.
         */
        fun ensureXwaylandExtracted(context: Context): Boolean = synchronized(XWAYLAND_EXTRACT_LOCK) {
            val assetPath = "xwayland/share.tar"
            val available = try {
                context.assets.open(assetPath).close()
                true
            } catch (_: java.io.IOException) {
                false
            }
            if (!available) {
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
            val nativeLibDir = context.applicationInfo.nativeLibraryDir
            val binDir = File(stagingDir, "bin").apply { mkdirs() }
            Os.symlink("$nativeLibDir/libxwayland.so", File(binDir, "Xwayland").absolutePath)
            Os.symlink("$nativeLibDir/libxkbcomp.so", File(binDir, "xkbcomp").absolutePath)

            atomicReplaceDir(stagingDir, destDir)
            File(destDir, ".version").writeText(currentStamp)
            Log.i(TAG, "Staged Xwayland under $destDir (binaries -> $nativeLibDir)")
            return true
        }
    }
}
