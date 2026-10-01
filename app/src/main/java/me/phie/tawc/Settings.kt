package me.phie.tawc

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import androidx.core.content.edit

/**
 * Process-global settings facade. Production uses [SharedPreferences];
 * integration tests can swap in an in-memory store with factory defaults.
 *
 * Initialised once from [TawcApplication.onCreate] so non-Activity code
 * (e.g. [me.phie.tawc.install.RootfsEnv], which runs on the broker
 * thread without a Context) can read settings without threading a
 * Context through every call site.
 *
 * Enum-like persisted settings (e.g. [GraphicsBackend]) keep their
 * wire-format key in code so adding a new variant later doesn't break
 * installs that already chose one of the existing values.
 */
object Settings {
    private const val PREFS_NAME = "tawc-settings"
    private const val KEY_GRAPHICS_BACKEND = "graphics_backend"
    private const val KEY_TINT_BUFFERS_BY_TYPE = "tint_buffers_by_type"
    private const val KEY_OUTPUT_SCALE = "output_scale"
    private const val KEY_REFRESH_RATE_CAP_MHZ = "refresh_rate_cap_mhz"
    private const val KEY_OUTPUT_REFRESH_MHZ = "output_refresh_mhz"
    private const val KEY_TERMINAL_SCALE = "terminal_scale"
    private const val KEY_XWAYLAND = "xwayland"
    private const val KEY_GTK3_BROKEN_MENUS_WORKAROUND = "gtk3_broken_menus_workaround"
    private const val KEY_OPEN_DISTRO = "open_distro"
    private const val KEY_HOME_PANE = "home_pane"
    private const val KEY_REMOTE_RELAY = "remote_relay"
    private const val KEY_REMOTE_IDLE_CLOSE = "remote_idle_close"
    private const val KEY_REMOTE_MODE = "remote_mode"
    private const val KEY_REMOTE_LOGIN = "remote_login"
    private const val KEY_REMOTE_KEY_USER = "remote_key_user"
    private const val KEY_REMOTE_PASTED_KEY = "remote_pasted_key"
    private const val KEY_BATTERY_PROMPT_SHOWN = "battery_prompt_shown"

    const val MIN_OUTPUT_SCALE = 0.5f
    const val MAX_OUTPUT_SCALE = 4.0f
    const val OUTPUT_SCALE_STEP = 0.25f
    const val DEFAULT_OUTPUT_SCALE = 2.0f

    /**
     * Refresh-rate cap in mHz; [RefreshRate.MAX_MHZ] (0) follows the panel.
     * Stored in mHz so the value can be handed to the compositor and
     * written into `wl_output.mode` without a units conversion in between.
     */
    const val DEFAULT_REFRESH_RATE_CAP_MHZ = RefreshRate.MAX_MHZ

    /**
     * Last rate that was actually resolved from the panel, in mHz; 0 means
     * "not resolved yet". Persisted because the compositor needs a correct
     * rate *before* any client connects, and only an Activity can read the
     * display (see [RefreshRate]). Without it the first `wl_output.mode` a
     * client sees is the 60 Hz default, and a client that samples once at
     * startup — Firefox sets its refresh-driver target that way — keeps
     * pacing at 60 even after the mode is corrected.
     */
    const val DEFAULT_OUTPUT_REFRESH_MHZ = 0
    const val MIN_TERMINAL_SCALE = 0.5f
    const val MAX_TERMINAL_SCALE = 2.0f
    const val TERMINAL_SCALE_STEP = 0.1f
    const val DEFAULT_TERMINAL_SCALE = 1.0f
    const val DEFAULT_REMOTE_RELAY = "https://sshyeet.com"
    const val REMOTE_IDLE_SECONDS = 5 * 60L
    const val REMOTE_MODE_LOCAL = "local"
    const val REMOTE_MODE_RELAY = "relay"
    const val REMOTE_LOGIN_SECRET = "secret"
    const val REMOTE_LOGIN_PASTE = "paste"
    val DEFAULT_TINT_BUFFERS_BY_TYPE = BuildConfig.TINT_BUFFERS_BY_TYPE_DEFAULT

    private interface Store {
        var graphicsBackend: GraphicsBackend
        var tintBuffersByType: Boolean
        var outputScale: Float
        var refreshRateCapMhz: Int
        var outputRefreshMhz: Int
        var terminalScale: Float
        var xwayland: Boolean
        var gtk3BrokenMenusWorkaround: Boolean
        var openDistroId: String?
        var homePane: HomePane
        var remoteRelay: String
        var remoteIdleClose: Boolean
        var remoteMode: String
        var remoteLogin: String
        var remoteKeyUser: String
        var remotePastedKey: String
        var batteryPromptShown: Boolean
    }

    private class SharedPreferencesStore(private val prefs: SharedPreferences) : Store {
        override var graphicsBackend: GraphicsBackend
            get() {
                val raw = prefs.getString(KEY_GRAPHICS_BACKEND, null)
                return GraphicsBackend.fromKeyOrDefault(raw)
            }
            set(value) {
                prefs.edit { putString(KEY_GRAPHICS_BACKEND, value.key) }
            }

        override var tintBuffersByType: Boolean
            get() = prefs.getBoolean(KEY_TINT_BUFFERS_BY_TYPE, DEFAULT_TINT_BUFFERS_BY_TYPE)
            set(value) {
                prefs.edit { putBoolean(KEY_TINT_BUFFERS_BY_TYPE, value) }
            }

        override var refreshRateCapMhz: Int
            get() = RefreshRate.sanitizeCapMhz(
                prefs.getInt(KEY_REFRESH_RATE_CAP_MHZ, DEFAULT_REFRESH_RATE_CAP_MHZ)
            )
            set(value) {
                prefs.edit { putInt(KEY_REFRESH_RATE_CAP_MHZ, RefreshRate.sanitizeCapMhz(value)) }
            }

        override var outputRefreshMhz: Int
            get() = prefs.getInt(KEY_OUTPUT_REFRESH_MHZ, DEFAULT_OUTPUT_REFRESH_MHZ)
            set(value) { prefs.edit { putInt(KEY_OUTPUT_REFRESH_MHZ, value) } }

        override var outputScale: Float
            get() = snapOutputScale(prefs.getFloat(KEY_OUTPUT_SCALE, DEFAULT_OUTPUT_SCALE))
            set(value) {
                prefs.edit { putFloat(KEY_OUTPUT_SCALE, snapOutputScale(value)) }
            }
        override var terminalScale: Float
            get() = snapTerminalScale(prefs.getFloat(KEY_TERMINAL_SCALE, DEFAULT_TERMINAL_SCALE))
            set(value) {
                prefs.edit { putFloat(KEY_TERMINAL_SCALE, snapTerminalScale(value)) }
            }

        override var xwayland: Boolean
            get() = prefs.getBoolean(KEY_XWAYLAND, true)
            set(value) {
                prefs.edit { putBoolean(KEY_XWAYLAND, value) }
            }

        override var gtk3BrokenMenusWorkaround: Boolean
            get() = prefs.getBoolean(KEY_GTK3_BROKEN_MENUS_WORKAROUND, true)
            set(value) {
                prefs.edit { putBoolean(KEY_GTK3_BROKEN_MENUS_WORKAROUND, value) }
            }

        override var openDistroId: String?
            get() = prefs.getString(KEY_OPEN_DISTRO, null)
            set(value) {
                prefs.edit { if (value == null) remove(KEY_OPEN_DISTRO) else putString(KEY_OPEN_DISTRO, value) }
            }

        override var homePane: HomePane
            get() = HomePane.fromKey(prefs.getString(KEY_HOME_PANE, null))
            set(value) {
                prefs.edit { putString(KEY_HOME_PANE, value.key) }
            }

        override var remoteRelay: String
            get() = prefs.getString(KEY_REMOTE_RELAY, null) ?: DEFAULT_REMOTE_RELAY
            set(value) {
                prefs.edit { putString(KEY_REMOTE_RELAY, value) }
            }

        override var remoteIdleClose: Boolean
            get() = prefs.getBoolean(KEY_REMOTE_IDLE_CLOSE, true)
            set(value) {
                prefs.edit { putBoolean(KEY_REMOTE_IDLE_CLOSE, value) }
            }

        override var remoteMode: String
            get() = prefs.getString(KEY_REMOTE_MODE, null) ?: REMOTE_MODE_LOCAL
            set(value) {
                prefs.edit { putString(KEY_REMOTE_MODE, value) }
            }

        override var remoteLogin: String
            get() = prefs.getString(KEY_REMOTE_LOGIN, null) ?: REMOTE_LOGIN_SECRET
            set(value) {
                prefs.edit { putString(KEY_REMOTE_LOGIN, value) }
            }

        override var remoteKeyUser: String
            get() = prefs.getString(KEY_REMOTE_KEY_USER, null) ?: ""
            set(value) {
                prefs.edit { putString(KEY_REMOTE_KEY_USER, value) }
            }

        override var remotePastedKey: String
            get() = prefs.getString(KEY_REMOTE_PASTED_KEY, null) ?: ""
            set(value) {
                prefs.edit { putString(KEY_REMOTE_PASTED_KEY, value) }
            }

        override var batteryPromptShown: Boolean
            get() = prefs.getBoolean(KEY_BATTERY_PROMPT_SHOWN, false)
            set(value) {
                prefs.edit { putBoolean(KEY_BATTERY_PROMPT_SHOWN, value) }
            }
    }

    private class TestStore : Store {
        @Volatile override var graphicsBackend: GraphicsBackend = GraphicsBackend.DEFAULT
        @Volatile override var tintBuffersByType: Boolean = DEFAULT_TINT_BUFFERS_BY_TYPE
        @Volatile override var outputScale: Float = DEFAULT_OUTPUT_SCALE
            set(value) { field = snapOutputScale(value) }
        @Volatile override var refreshRateCapMhz: Int = DEFAULT_REFRESH_RATE_CAP_MHZ
        @Volatile override var outputRefreshMhz: Int = DEFAULT_OUTPUT_REFRESH_MHZ
            set(value) { field = RefreshRate.sanitizeCapMhz(value) }
        @Volatile override var terminalScale: Float = DEFAULT_TERMINAL_SCALE
            set(value) { field = snapTerminalScale(value) }
        @Volatile override var xwayland: Boolean = true
        @Volatile override var gtk3BrokenMenusWorkaround: Boolean = true
        @Volatile override var openDistroId: String? = null
        @Volatile override var homePane: HomePane = HomePane.DEFAULT
        @Volatile override var remoteRelay: String = DEFAULT_REMOTE_RELAY
        @Volatile override var remoteIdleClose: Boolean = true
        @Volatile override var remoteMode: String = REMOTE_MODE_LOCAL
        @Volatile override var remoteLogin: String = REMOTE_LOGIN_SECRET
        @Volatile override var remoteKeyUser: String = ""
        @Volatile override var remotePastedKey: String = ""
        @Volatile override var batteryPromptShown: Boolean = false
    }

    @Volatile private var store: Store? = null

    /** Called from [TawcApplication.onCreate]. Idempotent. */
    fun init(context: Context) {
        if (store == null) {
            store = SharedPreferencesStore(context.applicationContext
                .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            )
        }
    }

    private fun requireStore(): Store =
        store ?: error("Settings.init(context) was not called — see TawcApplication.onCreate")

    /**
     * Swap all settings reads/writes to in-memory factory defaults. Debug
     * broker tests use this so no persisted user setting can leak into a
     * test, and no test mutation can survive app process death.
     */
    fun enterTestMode() {
        requireStore()
        store = TestStore()
    }

    var graphicsBackend: GraphicsBackend
        get() = requireStore().graphicsBackend
        set(value) { requireStore().graphicsBackend = value }

    /**
     * Whether the compositor tints surfaces by buffer type so the
     * fallback path is visually obvious — today this means SHM
     * surfaces get a magenta wash. Debug builds default on; release
     * builds default off. Read live by the renderer (no restart
     * required).
     */
    var tintBuffersByType: Boolean
        get() = requireStore().tintBuffersByType
        set(value) { requireStore().tintBuffersByType = value }

    /**
     * Physical pixels per Wayland logical pixel. Stored as a snapped float so
     * the UI, broker, and compositor all speak the same 0.25x grid.
     */
    var outputScale: Float
        get() = requireStore().outputScale
        set(value) { requireStore().outputScale = snapOutputScale(value) }

    var refreshRateCapMhz: Int
        get() = requireStore().refreshRateCapMhz
        set(value) { requireStore().refreshRateCapMhz = RefreshRate.sanitizeCapMhz(value) }

    var outputRefreshMhz: Int
        get() = requireStore().outputRefreshMhz
        set(value) { requireStore().outputRefreshMhz = value }

    /** Multiplier on the terminal's sp text size (so it also follows system font size). */
    var terminalScale: Float
        get() = requireStore().terminalScale
        set(value) { requireStore().terminalScale = snapTerminalScale(value) }

    /**
     * Enable the compositor-owned Xwayland server for X11 applications.
     * Toggled live: disabling drops the current Xwayland process and
     * enabling starts a fresh one without restarting the compositor.
     */
    var xwayland: Boolean
        get() = requireStore().xwayland
        set(value) { requireStore().xwayland = value }

    /**
     * Workaround for GTK3 native Wayland menubars on touch-only seats. When
     * enabled, the compositor exposes a wl_pointer and briefly enters/leaves
     * each new toplevel at its center so GTK3 initializes its pointer crossing
     * state before the first touch on a server-side-decorated menubar.
     */
    var gtk3BrokenMenusWorkaround: Boolean
        get() = requireStore().gtk3BrokenMenusWorkaround
        set(value) { requireStore().gtk3BrokenMenusWorkaround = value }

    /**
     * Install id the home screen shows. May be stale (uninstalled);
     * read it through [OpenDistro.resolve], not directly.
     */
    var openDistroId: String?
        get() = requireStore().openDistroId
        set(value) { requireStore().openDistroId = value }

    /**
     * Which pane a READY tawcroot distro opens on. One global value,
     * written only by the home screen's FAB and ⋮ Apps/Terminal.
     */
    var homePane: HomePane
        get() = requireStore().homePane
        set(value) { requireStore().homePane = value }

    /** Remote access relay base URL (the start screen's field). */
    var remoteRelay: String
        get() = requireStore().remoteRelay
        set(value) { requireStore().remoteRelay = value }

    /** Remote access: close after [REMOTE_IDLE_SECONDS] with no connection. */
    var remoteIdleClose: Boolean
        get() = requireStore().remoteIdleClose
        set(value) { requireStore().remoteIdleClose = value }

    /** Remote access: [REMOTE_MODE_LOCAL] or [REMOTE_MODE_RELAY]. */
    var remoteMode: String
        get() = requireStore().remoteMode
        set(value) { requireStore().remoteMode = value }

    /** Remote access login: [REMOTE_LOGIN_SECRET], [REMOTE_LOGIN_PASTE], or
     *  a [me.phie.tawc.remote.KeyHost] key. */
    var remoteLogin: String
        get() = requireStore().remoteLogin
        set(value) { requireStore().remoteLogin = value }

    /** Remote access: the code-host username whose keys may log in. */
    var remoteKeyUser: String
        get() = requireStore().remoteKeyUser
        set(value) { requireStore().remoteKeyUser = value }

    /** Remote access: pasted public key(s). */
    var remotePastedKey: String
        get() = requireStore().remotePastedKey
        set(value) { requireStore().remotePastedKey = value }

    /** "Keep awake": the battery-optimization prompt was offered once. */
    var batteryPromptShown: Boolean
        get() = requireStore().batteryPromptShown
        set(value) { requireStore().batteryPromptShown = value }

    fun snapOutputScale(value: Float): Float {
        if (!value.isFinite()) return DEFAULT_OUTPUT_SCALE
        val clamped = value.coerceIn(MIN_OUTPUT_SCALE, MAX_OUTPUT_SCALE)
        val steps = ((clamped - MIN_OUTPUT_SCALE) / OUTPUT_SCALE_STEP).toIntWithRound()
        return MIN_OUTPUT_SCALE + steps * OUTPUT_SCALE_STEP
    }

    fun formatOutputScale(value: Float): String {
        return String.format(java.util.Locale.US, "%.2f", snapOutputScale(value))
    }

    fun snapTerminalScale(value: Float): Float {
        if (!value.isFinite()) return DEFAULT_TERMINAL_SCALE
        val clamped = value.coerceIn(MIN_TERMINAL_SCALE, MAX_TERMINAL_SCALE)
        val steps = ((clamped - MIN_TERMINAL_SCALE) / TERMINAL_SCALE_STEP).toIntWithRound()
        return MIN_TERMINAL_SCALE + steps * TERMINAL_SCALE_STEP
    }

    fun formatTerminalScale(value: Float): String {
        return String.format(java.util.Locale.US, "%.1f", snapTerminalScale(value))
    }

    private fun Float.toIntWithRound(): Int =
        kotlin.math.floor(this + 0.5f).toInt()
}

/** Home screen pane for a usable distro (notes/android.md "Home screen"). */
enum class HomePane(val key: String) {
    TERMINAL("terminal"),
    APPS("apps");

    companion object {
        val DEFAULT = TERMINAL

        fun fromKey(key: String?): HomePane = entries.firstOrNull { it.key == key } ?: DEFAULT
    }
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
     * more robust to vendor / Android-version drift. When this APK
     * ships the backend, the kumquat server runs as a thread of the
     * compositor app;
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
         * it the default when this APK ships it. If gfxstream is
         * disabled at build time, x86_64 falls back to CPU when present.
         * Everywhere else (aarch64 physical), libhybris stays the
         * default — proven, lower latency, no IPC.
         */
        val DEFAULT: GraphicsBackend
            get() {
                val preferred = when (Build.SUPPORTED_ABIS.firstOrNull()) {
                    "x86_64" -> GFXSTREAM
                    else -> LIBHYBRIS
                }
                if (me.phie.tawc.install.EnabledGraphicsBackends.isEnabled(preferred)) {
                    return preferred
                }
                if (Build.SUPPORTED_ABIS.firstOrNull() == "x86_64" &&
                    me.phie.tawc.install.EnabledGraphicsBackends.isEnabled(CPU)) {
                    return CPU
                }
                return me.phie.tawc.install.EnabledGraphicsBackends.enabled.first()
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
