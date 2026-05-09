package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.compositor.CompositorService
import java.io.File

/**
 * Lays the APK-bundled libhybris tree into a rootfs as real files,
 * not symlinks (the previous `LibhybrisLinker` approach) and not
 * binds (the bind approach was abandoned — see `notes/installation.md`
 * "Why copy, not bind"). Installs to `/usr/lib/hybris/` (a tawc-owned
 * namespace; `/usr/local/lib/` stays free for the user's own installs
 * inside the rootfs).
 *
 * Two kinds of entries:
 *   - Every regular file under `<filesDir>/libhybris/lib/` (ie. the
 *     `.so` files plus the `gl-shims/` and `libhybris/` subdir
 *     contents) is a [TawcInstall.Type.COPY].
 *   - Symlinks in the source tree become [TawcInstall.Type.LINK]s
 *     with the same link target. libhybris's autotools build emits
 *     the usual `libFoo.so` → `libFoo.so.1` → `libFoo.so.1.0.0` chain;
 *     we preserve the chain literally so the soname-based dlopen path
 *     still resolves.
 *   - The glvnd vendor JSON is materialised on the host side at
 *     `<filesDir>/libhybris-glvnd/00_libhybris.json` (real file we
 *     own + control) and then copied into the rootfs at
 *     `/usr/share/glvnd/egl_vendor.d/00_libhybris.json`. We don't
 *     bind the glvnd dir over (`/usr/share/glvnd/egl_vendor.d/` is
 *     populated by the distro's libglvnd package; pacman writes
 *     `50_mesa.json` there). Both files coexist; the `00_` prefix
 *     wins glvnd's lex-order vendor scan and libhybris dispatches
 *     first. Mesa stays enumerated but is never reached for an EGL
 *     call, which is fine.
 *
 * Returns an empty list on devices where no libhybris asset is
 * shipped for the host ABI (today: x86_64 emulator —
 * notes/emulator.md). [TawcInstaller] still records the empty
 * manifest + stamp so subsequent app starts hit the no-op path.
 */
internal object LibhybrisInstallProvider : TawcInstallProvider {
    override val name: String = "libhybris"

    /** Guest-side install root — see kdoc above for why this isn't
     *  `/usr/local/lib/`. Kept as a public constant so [RootfsEnv]
     *  can build LD_LIBRARY_PATH / HYBRIS_*_DIR from one place. */
    const val GUEST_LIB_DIR = "/usr/lib/hybris"

    /** Guest-side path for the eglplatform / vulkanplatform / linker
     *  plugins. libhybris's `PKGLIBDIR` and `LINKER_PLUGIN_DIR` macros
     *  bake `$libdir/libhybris/` and `$libdir/libhybris/linker` at
     *  autotools build time; `scripts/build-libhybris.sh` runs with
     *  `--libdir=/usr/lib/hybris` so both line up here without an
     *  env-var override. */
    const val GUEST_PLUGIN_DIR = "$GUEST_LIB_DIR/libhybris"

    /** Guest-side gl-shims dir — first on LD_LIBRARY_PATH so the
     *  libGL/libGLESv2 wrappers shadow any distro-shipped libs. */
    const val GUEST_GL_SHIMS_DIR = "$GUEST_LIB_DIR/gl-shims"

    /** Where glvnd's EGL dispatcher scans for `*.json` vendor files. */
    private const val GUEST_GLVND_DIR = "/usr/share/glvnd/egl_vendor.d"

    /**
     * GLVND EGL vendor JSON pointing at libhybris's libEGL via
     * [GUEST_LIB_DIR]. `00_` prefix wins glvnd's lex-order scan over
     * Mesa's `50_mesa.json`.
     */
    private val glvndVendorJson: String = """{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "$GUEST_LIB_DIR/libEGL.so.1"
    }
}
"""

    override fun entries(context: Context): List<TawcInstall> {
        if (!CompositorService.ensureLibhybrisExtracted(context)) return emptyList()
        // Asset tar's contents are now flat (libEGL.so, libhybris/,
        // gl-shims/, … at the tar root) so the extracted tree lives
        // directly at `<filesDir>/libhybris/`, not `<filesDir>/libhybris/lib/`.
        val srcLib = File(context.filesDir, "libhybris").canonicalFile
        if (!srcLib.isDirectory) return emptyList()

        val entries = mutableListOf<TawcInstall>()
        // Recursive walk: libhybris's tree contains regular files at
        // the top level plus two subdirs (gl-shims, libhybris) with
        // their own files. Walking everything as files (no dir
        // entries) means [TawcInstaller] creates parent dirs on
        // demand and never has to reason about dir lifecycle.
        walk(srcLib, srcLib, GUEST_LIB_DIR, entries)

        // glvnd vendor JSON — materialise on the host first so we
        // have a stable file to copy from. Sibling to <filesDir>/libhybris/
        // so the periodic re-extract (which atomically replaces the
        // libhybris dir) doesn't wipe our generated content.
        val glvndSrc = File(context.filesDir, "libhybris-glvnd/00_libhybris.json")
        glvndSrc.parentFile?.mkdirs()
        if (!glvndSrc.exists() || glvndSrc.readText() != glvndVendorJson) {
            glvndSrc.writeText(glvndVendorJson)
        }
        entries += TawcInstall(
            src = glvndSrc.absolutePath,
            dest = "$GUEST_GLVND_DIR/00_libhybris.json",
            type = TawcInstall.Type.COPY,
        )
        return entries
    }

    /** Recursively walk [dir] and append a [TawcInstall] for each
     *  file / symlink, stripping [root] off the source path to
     *  compute the dest relative to [destBase]. Skips the
     *  `.version` stamp written by [CompositorService.ensureLibhybrisExtracted]
     *  since it sits in the same dir as the asset contents and isn't
     *  part of the libhybris install. */
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
            // Symlinks in the source must be preserved as symlinks in
            // the rootfs. Otherwise libhybris's libEGL.so → libEGL.so.1
            // → libEGL.so.1.0.0 chain collapses into three identical
            // file copies and the per-soname lookup gets ambiguous.
            // java.io.File.canonicalFile won't tell us; isFile() also
            // follows. Use NIO's isSymbolicLink which inspects the
            // dirent itself.
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
