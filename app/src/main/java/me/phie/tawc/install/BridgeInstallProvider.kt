package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.compositor.CompositorService
import java.io.File

/**
 * Lays the gfxstream-bridge guest-side bits into a rootfs:
 *  - `libvulkan_gfxstream.so` at [GUEST_LIB_PATH]
 *  - the Vulkan ICD JSON at [GUEST_ICD_PATH]
 *
 * Both ride in the APK as raw assets under `assets/mesa-gfxstream/`,
 * built by Gradle's `packMesaGfxstream` and extracted to
 * `<filesDir>/mesa-gfxstream/` by
 * [CompositorService.ensureMesaGfxstreamExtracted]. From there
 * [TawcInstaller] copies them into each rootfs at install time.
 *
 * Always installed when the asset is shipped (currently aarch64 only),
 * regardless of which [me.phie.tawc.GraphicsBackend] is selected — the
 * pref controls which env [RootfsEnv] sets, not which files exist on
 * disk. Two unused files cost nothing; making the manifest depend on a
 * runtime pref would invalidate the cached install on every toggle.
 *
 * Returns an empty list on devices where the asset isn't shipped (today:
 * x86_64 emulator). The compositor-side kumquat thread is also
 * aarch64-gated, so this matches.
 */
internal object BridgeInstallProvider : TawcInstallProvider {
    override val name: String = "mesa-gfxstream"

    /** Where Mesa's gfxstream-vk lands inside the rootfs. The
     *  matching `VK_ICD_FILENAMES` value lives in [RootfsEnv]; keep
     *  these two strings in sync. `/usr/local/` because the user's
     *  distro Mesa already owns `/usr/lib/`. */
    const val GUEST_LIB_PATH = "/usr/local/lib/${CompositorService.MESA_GFXSTREAM_LIB_ASSET}"
    const val GUEST_ICD_PATH = "/usr/local/share/vulkan/icd.d/${CompositorService.MESA_GFXSTREAM_ICD_ASSET}"

    override fun entries(context: Context): List<TawcInstall> {
        if (!CompositorService.ensureMesaGfxstreamExtracted(context)) return emptyList()
        val srcDir = File(context.filesDir, "mesa-gfxstream")
        return listOf(
            TawcInstall(
                src = File(srcDir, CompositorService.MESA_GFXSTREAM_LIB_ASSET).absolutePath,
                dest = GUEST_LIB_PATH,
                type = TawcInstall.Type.COPY,
            ),
            TawcInstall(
                src = File(srcDir, CompositorService.MESA_GFXSTREAM_ICD_ASSET).absolutePath,
                dest = GUEST_ICD_PATH,
                type = TawcInstall.Type.COPY,
            ),
        )
    }
}
