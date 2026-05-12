package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.compositor.CompositorService
import java.io.File

/**
 * Lays the APK-bundled Mesa-Zink tree (`libEGL_mesa.so.0`,
 * `libgallium-<ver>.so`, `libgbm.so.1`, soname symlinks) into a rootfs
 * at [GUEST_LIB_DIR] = `/usr/lib/mesa-zink/`. Consumed only by the
 * [me.phie.tawc.GraphicsBackend.LIBHYBRIS_ZINK] backend — see
 * [notes/libhybris-zink.md](../../../../../../../notes/libhybris-zink.md)
 * for the design + Mesa patch (`06-tawc-zink-nokms.patch`).
 *
 * Shipped unconditionally alongside libhybris and gfxstream-vk:
 * always installing both costs ~20 MB on disk per rootfs but means
 * toggling the [me.phie.tawc.GraphicsBackend] pref is purely an env
 * change (no install invalidation). The libs only get loaded when
 * `RootfsEnv` puts this dir on `LD_LIBRARY_PATH`, which only happens
 * for `LIBHYBRIS_ZINK`.
 *
 * Returns an empty list on devices where no Mesa-Zink asset is shipped
 * for the host ABI (e.g. an x86_64-only build); [TawcInstaller] still
 * records the empty manifest + stamp so subsequent app starts hit the
 * no-op path. The same shape as [LibhybrisInstallProvider].
 */
internal object MesaZinkInstallProvider : TawcInstallProvider {
    override val name: String = "mesa-zink"

    /** Guest-side install root for the Mesa-Zink stack.
     *  `RootfsEnv` adds this dir to `LD_LIBRARY_PATH` under the
     *  `LIBHYBRIS_ZINK` backend so our patched `libEGL_mesa.so.0`
     *  wins over the distro's. */
    const val GUEST_LIB_DIR = "/usr/lib/mesa-zink"

    override fun entries(context: Context): List<TawcInstall> {
        // Build-time disabled (`-PtawcGraphics=...` without libhybris-zink):
        // no APK asset shipped, nothing to install. Avoids hitting
        // `ensureMesaZinkExtracted`'s "no asset" branch (which would log
        // a misleading "LIBHYBRIS_ZINK backend unavailable" line on every
        // app start).
        if (!EnabledGraphicsBackends.libhybrisZink) return emptyList()
        if (!CompositorService.ensureMesaZinkExtracted(context)) return emptyList()
        val srcDir = File(context.filesDir, "mesa-zink").canonicalFile
        if (!srcDir.isDirectory) return emptyList()
        val entries = mutableListOf<TawcInstall>()
        walk(srcDir, srcDir, GUEST_LIB_DIR, entries)
        return entries
    }

    /** Recursively walk [dir] and append a [TawcInstall] per file or
     *  symlink, stripping [root] off the source path. Mirrors
     *  [LibhybrisInstallProvider.walk] — the two providers ship similar
     *  symlink-heavy trees and the logic is identical, but keeping the
     *  copy local avoids cross-provider visibility. Skips the
     *  `.version` stamp written by
     *  [CompositorService.ensureMesaZinkExtracted]. */
    private fun walk(
        root: File,
        dir: File,
        destBase: String,
        out: MutableList<TawcInstall>,
    ) {
        val children = dir.listFiles()?.sortedBy { it.name } ?: return
        for (child in children) {
            if (child.parentFile == root && child.name == ".version") continue
            val rel = child.relativeTo(root).path
            val dest = "$destBase/$rel"
            val path = child.toPath()
            if (java.nio.file.Files.isSymbolicLink(path)) {
                val target = java.nio.file.Files.readSymbolicLink(path).toString()
                out += TawcInstall(
                    src = target,
                    dest = dest,
                    type = TawcInstall.Type.LINK,
                )
            } else if (child.isDirectory) {
                walk(root, child, destBase, out)
            } else if (child.isFile) {
                out += TawcInstall(
                    src = child.absolutePath,
                    dest = dest,
                    type = TawcInstall.Type.COPY,
                )
            }
        }
    }
}
