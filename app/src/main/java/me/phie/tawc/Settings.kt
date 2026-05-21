package me.phie.tawc

import android.content.Context
import android.content.SharedPreferences
import android.os.Build

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
    private const val KEY_TINT_BUFFERS_BY_TYPE = "tint_buffers_by_type"
    private const val KEY_OUTPUT_SCALE = "output_scale"
    private const val KEY_GTK3_BROKEN_MENUS_WORKAROUND = "gtk3_broken_menus_workaround"

    const val MIN_OUTPUT_SCALE = 0.5f
    const val MAX_OUTPUT_SCALE = 4.0f
    const val OUTPUT_SCALE_STEP = 0.25f
    const val DEFAULT_OUTPUT_SCALE = 2.0f

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

    /**
     * Whether the compositor tints surfaces by buffer type so the
     * fallback path is visually obvious — today this means SHM
     * surfaces get a magenta wash. Default on; useful to disable when
     * showing the app off or when the tint clashes with what's being
     * debugged. Read live by the renderer (no restart required).
     */
    var tintBuffersByType: Boolean
        get() = requirePrefs().getBoolean(KEY_TINT_BUFFERS_BY_TYPE, true)
        set(value) {
            requirePrefs().edit().putBoolean(KEY_TINT_BUFFERS_BY_TYPE, value).apply()
        }

    /**
     * Physical pixels per Wayland logical pixel. Stored as a snapped float so
     * the UI, broker, and compositor all speak the same 0.25x grid.
     */
    var outputScale: Float
        get() = snapOutputScale(requirePrefs().getFloat(KEY_OUTPUT_SCALE, DEFAULT_OUTPUT_SCALE))
        set(value) {
            requirePrefs().edit().putFloat(KEY_OUTPUT_SCALE, snapOutputScale(value)).apply()
        }

    /**
     * Workaround for GTK3 native Wayland menubars on touch-only seats. When
     * enabled, the compositor exposes a wl_pointer and briefly enters/leaves
     * each new toplevel at its center so GTK3 initializes its pointer crossing
     * state before the first touch on a server-side-decorated menubar.
     */
    var gtk3BrokenMenusWorkaround: Boolean
        get() = requirePrefs().getBoolean(KEY_GTK3_BROKEN_MENUS_WORKAROUND, true)
        set(value) {
            requirePrefs().edit().putBoolean(KEY_GTK3_BROKEN_MENUS_WORKAROUND, value).apply()
        }

    fun snapOutputScale(value: Float): Float {
        if (!value.isFinite()) return DEFAULT_OUTPUT_SCALE
        val clamped = value.coerceIn(MIN_OUTPUT_SCALE, MAX_OUTPUT_SCALE)
        val steps = ((clamped - MIN_OUTPUT_SCALE) / OUTPUT_SCALE_STEP).toIntWithRound()
        return MIN_OUTPUT_SCALE + steps * OUTPUT_SCALE_STEP
    }

    fun formatOutputScale(value: Float): String {
        return String.format(java.util.Locale.US, "%.2f", snapOutputScale(value))
    }

    private fun Float.toIntWithRound(): Int =
        kotlin.math.floor(this + 0.5f).toInt()
}

/**
 * GPU driver path used by the in-rootfs Wayland clients.
 *
 * Stored as a string so additional options (software rendering,
 * future bridges, …) can be added without breaking already-saved
 * preferences.
 */
enum class GraphicsBackend(val key: String, val displayName: String) {
    /**
     * Today's default: load the Android vendor GPU blob into the
     * chroot via libhybris. Lowest overhead (no IPC), but tied to
     * the libhybris stack's per-vendor quirks.
     */
    LIBHYBRIS("libhybris", "libhybris"),

    /**
     * Distro Mesa + Zink (Gallium driver translating GL/GLES to
     * Vulkan), with libhybris's Vulkan as the only ICD. Same vendor
     * blob path as [LIBHYBRIS], but routed through Mesa+Zink so
     * desktop-GL apps (kitty, alacritty, anything with `#version 140`
     * shaders) work — the [LIBHYBRIS] backend is GLES-only via the
     * `gl-shims/` wrappers, which can't run desktop-GL shaders. Cost:
     * GLES now goes Zink → SPIR-V → Vulkan instead of straight
     * libhybris GLES (single-digit % overhead on most workloads). See
     * [notes/libhybris-zink.md](../../../../../../../notes/libhybris-zink.md).
     */
    LIBHYBRIS_ZINK("libhybris-zink", "libhybris+zink"),

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
    GFXSTREAM("gfxstream", "gfxstream"),

    /**
     * Pure software rendering. No vendor blob, no command-stream
     * forwarding — Mesa's `llvmpipe` (GL/GLES) and `lavapipe` (Vulkan,
     * if the distro ships `vulkan-swrast`) handle every draw on the
     * CPU. Slow and AHB-less (every client falls back to `wl_shm`,
     * which the compositor optionally tints magenta — see
     * [Settings.tintBuffersByType]), but useful when the GPU
     * paths are broken or unavailable. No libhybris or gfxstream env
     * is set; the distro's own Mesa picks llvmpipe via
     * `LIBGL_ALWAYS_SOFTWARE=1` + `GALLIUM_DRIVER=llvmpipe`.
     */
    CPU("cpu", "CPU");

    companion object {
        /**
         * Default backend picked when nothing is saved yet.
         *
         * On x86_64 (emulator) libhybris can't load against bionic
         * (notes/emulator.md "libhybris on x86_64"), so libhybris would
         * just no-op and every GPU client would fall back to SHM. The
         * gfxstream bridge is the only working GPU path there — make
         * it the default so a fresh install Just Works on the AVD.
         * Everywhere else (aarch64 physical), libhybris stays the
         * default — proven, lower latency, no IPC.
         */
        val DEFAULT: GraphicsBackend = when (Build.SUPPORTED_ABIS.firstOrNull()) {
            "x86_64" -> GFXSTREAM
            else -> LIBHYBRIS
        }

        fun fromKeyOrDefault(key: String?): GraphicsBackend {
            val match = entries.firstOrNull { it.key == key } ?: return DEFAULT
            // Defensive: an APK that turns off a backend (via -PtawcGraphics
            // or a downgrade) shouldn't keep returning the disabled enum
            // for its persisted prefs. Fall back to the build default —
            // which is itself guaranteed-enabled (validated at build time
            // in `app/build.gradle.kts`).
            return if (me.phie.tawc.install.EnabledGraphicsBackends.isEnabled(match)) match else DEFAULT
        }
    }
}
