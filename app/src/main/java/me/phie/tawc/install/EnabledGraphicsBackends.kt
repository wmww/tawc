package me.phie.tawc.install

import me.phie.tawc.BuildConfig
import me.phie.tawc.GraphicsBackend

/**
 * Build-time gate for the graphics backends this APK ships. Driven by
 * the `GRAPHICS_*_ENABLED` BuildConfig fields set in
 * `app/build.gradle.kts` (override with
 * `-PtawcGraphics=libhybris,libhybris-zink,gfxstream,cpu`).
 *
 * Default: all four enabled. Disabling `libhybris-zink` is the only
 * toggle that changes the build (skips the Mesa-Zink cross-compile +
 * drops the ~22 MB `assets/mesa-zink/<abi>/mesa-zink.tar` asset); the
 * other three always compile in and this gate only controls whether
 * they appear in the in-app Settings picker. Pattern mirrors
 * [EnabledMethods].
 *
 * Settings UI calls [enabled] to filter the picker.
 * [me.phie.tawc.install.MesaZinkInstallProvider] short-circuits to
 * empty when libhybris-zink is disabled. [Settings.fromKeyOrDefault]
 * falls back to [GraphicsBackend.DEFAULT] when a persisted pref
 * resolves to a backend this APK doesn't ship — covers users who
 * downgrade or switch flag sets between builds.
 */
object EnabledGraphicsBackends {
    val libhybris: Boolean = BuildConfig.GRAPHICS_LIBHYBRIS_ENABLED
    val libhybrisZink: Boolean = BuildConfig.GRAPHICS_LIBHYBRIS_ZINK_ENABLED
    val gfxstream: Boolean = BuildConfig.GRAPHICS_GFXSTREAM_ENABLED
    val cpu: Boolean = BuildConfig.GRAPHICS_CPU_ENABLED

    fun isEnabled(backend: GraphicsBackend): Boolean = when (backend) {
        GraphicsBackend.LIBHYBRIS -> libhybris
        GraphicsBackend.LIBHYBRIS_ZINK -> libhybrisZink
        GraphicsBackend.GFXSTREAM -> gfxstream
        GraphicsBackend.CPU -> cpu
    }

    /** Backends this APK ships, in enum declaration order. Settings
     *  UI iterates this; absent backends never get shown. */
    val enabled: List<GraphicsBackend> = GraphicsBackend.entries.filter { isEnabled(it) }
}
