package me.phie.tawc

import android.content.Context
import android.content.SharedPreferences

/**
 * Process-global settings backed by [SharedPreferences].
 *
 * Initialised once from [TawcApplication.onCreate] so non-Activity code
 * (e.g. [me.phie.tawc.install.RootfsEnv], which runs on the broker
 * thread without a Context) can read settings without threading a
 * Context through every call site.
 *
 * All values are stored as **strings**. Enum-like settings (e.g.
 * [GraphicsBackend]) keep their wire-format key in code so adding a new
 * variant later doesn't break installs that already chose one of the
 * existing values.
 */
object Settings {
    private const val PREFS_NAME = "tawc-settings"
    private const val KEY_GRAPHICS_BACKEND = "graphics_backend"

    @Volatile private var prefs: SharedPreferences? = null

    /** Called from [TawcApplication.onCreate]. Idempotent. */
    fun init(context: Context) {
        if (prefs == null) {
            prefs = context.applicationContext
                .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        }
    }

    private fun requirePrefs(): SharedPreferences =
        prefs ?: error("Settings.init(context) was not called — see TawcApplication.onCreate")

    var graphicsBackend: GraphicsBackend
        get() {
            val raw = requirePrefs().getString(KEY_GRAPHICS_BACKEND, null)
            return GraphicsBackend.fromKeyOrDefault(raw)
        }
        set(value) {
            requirePrefs().edit().putString(KEY_GRAPHICS_BACKEND, value.key).apply()
        }
}

/**
 * GPU driver path used by the in-rootfs Wayland clients.
 *
 * Stored as a string so additional options (software rendering,
 * future bridges, …) can be added without breaking already-saved
 * preferences.
 */
enum class GraphicsBackend(val key: String, val displayName: String, val tagline: String) {
    /**
     * Today's default: load the Android vendor GPU blob into the
     * chroot via libhybris. Lowest overhead (no IPC), but tied to
     * the libhybris stack's per-vendor quirks.
     */
    LIBHYBRIS("libhybris", "libhybris", "fastest"),

    /**
     * Forward GL/Vulkan command streams to an in-compositor-process
     * gfxstream renderer over a kumquat AF_UNIX socket. No vendor
     * blob inside the chroot — slightly slower per-call, but much
     * more robust to vendor / Android-version drift. The kumquat
     * server runs as a thread of the compositor app (always on);
     * the chroot-side `libvulkan_gfxstream.so` + ICD JSON ride in
     * the APK and are laid into each rootfs by
     * [me.phie.tawc.install.BridgeInstallProvider] at install time.
     */
    GFXSTREAM("gfxstream", "gfxstream", "fast, reliable");

    companion object {
        val DEFAULT = LIBHYBRIS

        fun fromKeyOrDefault(key: String?): GraphicsBackend =
            entries.firstOrNull { it.key == key } ?: DEFAULT
    }
}
