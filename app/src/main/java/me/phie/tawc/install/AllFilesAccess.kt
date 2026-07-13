package me.phie.tawc.install

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Environment
import android.provider.Settings

/**
 * Runtime gate for the external-storage binds feature
 * (notes/external-binds.md), built on Android 11+'s "all files access"
 * (`MANAGE_EXTERNAL_STORAGE`).
 *
 * Two independent layers:
 *   - [declared]: the permission is in this APK's manifest at all. A
 *     `-PtawcAllFilesAccess=false` build strips it; every binds UI
 *     surface hides itself when this is false.
 *   - [granted]: the user has flipped the "All files access" toggle in
 *     system settings ([openSettings] deep-links there). Binds whose
 *     host path needs the grant ([requiresGrant]) refuse to spawn
 *     without it — fail closed, never an empty stand-in dir.
 */
object AllFilesAccess {
    private const val PERMISSION = "android.permission.MANAGE_EXTERNAL_STORAGE"

    fun declared(context: Context): Boolean {
        // The all-files-access model is Android 11+; on the one older
        // API level we support (29) the feature is simply absent.
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.R) return false
        val pi = try {
            context.packageManager.getPackageInfo(
                context.packageName, PackageManager.GET_PERMISSIONS,
            )
        } catch (_: PackageManager.NameNotFoundException) {
            return false
        }
        return pi.requestedPermissions?.contains(PERMISSION) == true
    }

    fun granted(): Boolean =
        android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R &&
            Environment.isExternalStorageManager()

    /**
     * Open this app's "All files access" toggle in system settings.
     * Try-then-catch rather than `resolveActivity`: with target SDK
     * 30+ package-visibility filtering, resolveActivity returns null
     * for Settings without a `<queries>` declaration even though
     * startActivity succeeds. Falls back to the all-apps list screen
     * for settings apps that don't handle the per-app form.
     */
    fun openSettings(context: Context) {
        val perApp = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:${context.packageName}"),
        )
        try {
            context.startActivity(perApp)
        } catch (_: android.content.ActivityNotFoundException) {
            context.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    /**
     * Does binding [hostPath] need the all-files grant? Shared storage
     * (`/storage/...`, `/sdcard/...`) is unreadable to the app uid
     * without it. Everything else (e.g. the suggested `/` bind) is
     * governed by ordinary SELinux/DAC, where partial unreadability is
     * expected and not a config error.
     */
    fun requiresGrant(hostPath: String): Boolean {
        val p = hostPath.trimEnd('/')
        return p == "/storage" || hostPath.startsWith("/storage/") ||
            p == "/sdcard" || hostPath.startsWith("/sdcard/")
    }

    fun requiresGrant(binds: List<ExternalBind>): Boolean =
        binds.any { requiresGrant(it.hostPath) }

    /**
     * Is [hostPath] known to be missing? Shared-storage paths are
     * unstattable without the grant — those return false (can't
     * verify), so configuring binds stays possible pre-grant. Shared
     * rule for every surface that pre-checks a bind's host dir; the
     * spawn path stays fail-closed regardless.
     */
    fun hostDirVerifiablyMissing(hostPath: String): Boolean {
        if (requiresGrant(hostPath) && !granted()) return false
        return !java.io.File(hostPath).isDirectory
    }

    /**
     * The suggested bind set the manage-binds screen offers: the
     * Android root at /android (much of it unreadable to the app uid —
     * expected), shared storage as the Android home at /home/android,
     * and the shared-storage folders with a standard name on both
     * sides — Android's public directories mapped to the matching XDG
     * user dir under the in-rootfs home (`/root`, see RootfsEnv).
     * Movies maps to XDG's Videos; DCIM has no XDG name and keeps its
     * own; XDG dirs with no Android equivalent (Desktop, Templates,
     * Public) are omitted. Nothing is bound by default.
     *
     * The root suggestion is browse-only (read-only): writes into the
     * Android root are nonsensical and mostly uid-denied anyway. The
     * shared-storage binds stay writable — they exist so Linux apps
     * can save into Android storage. The user can flip either in the
     * manage-binds UI.
     *
     * [sharedStorage] is injectable for unit tests, where
     * [Environment.getExternalStorageDirectory] is unavailable.
     */
    fun commonDirBinds(
        sharedStorage: String = Environment.getExternalStorageDirectory().absolutePath,
    ): List<ExternalBind> = buildList {
        add(ExternalBind(hostPath = "/", guestPath = "/android", readOnly = true))
        add(ExternalBind(hostPath = sharedStorage, guestPath = "/home/android"))
        for ((androidDir, guestName) in listOf(
            // Literal Environment.DIRECTORY_* values (non-final
            // statics, null in plain unit tests; the names are fixed
            // public API).
            "Download" to "Downloads",
            "Documents" to "Documents",
            "Pictures" to "Pictures",
            "Music" to "Music",
            "Movies" to "Videos",
            "DCIM" to "DCIM",
        )) {
            add(ExternalBind("$sharedStorage/$androidDir", "/root/$guestName"))
        }
    }
}
