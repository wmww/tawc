package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.compositor.CompositorService
import java.io.File
import java.io.IOException

/**
 * Wire the APK-bundled libhybris tree into a freshly-installed rootfs
 * via symlinks under `<rootfs>/usr/local/lib/`. Each file/dir in the
 * extracted `filesDir/libhybris/lib/` is mirrored as a symlink with
 * an absolute target path; the install method's bind mount of the
 * app data dir (chroot via [ChrootMounter], proot via the `-b
 * /data/data/<pkg>:/data/data/<pkg>` flag) makes those targets
 * reachable from inside the chroot view.
 *
 * Runs through [InstallationMethod.runOutside] with `ln -sfn` rather
 * than direct `Os.symlink` so the chroot path (rootfs owned by root)
 * and the proot path (rootfs owned by app uid) take the same shape —
 * same pattern as
 * [me.phie.tawc.install.distro.arch.ArchPacmanCommon.configure].
 *
 * This runs in the CONFIGURING stage of [Installer.install], so it
 * only fires on a fresh `(no dir)` install. Re-installs / upgrades of
 * existing chroots are out of scope.
 */
object LibhybrisLinker {
    /**
     * @return true if symlinks were created, false if no libhybris
     *   asset is shipped for this device's ABI (e.g. emulator builds
     *   where libhybris is unsupported).
     */
    fun link(
        context: Context,
        method: InstallationMethod,
        rootfsPath: String,
        log: (String) -> Unit,
    ): Boolean {
        if (!CompositorService.ensureLibhybrisExtracted(context)) {
            log("libhybris: no asset for this ABI — skipping rootfs symlinks")
            return false
        }
        // Symlink targets must use the canonical `/data/data/<pkg>/...`
        // path: inside the chroot only `/data/data/` is bind-mounted by
        // ChrootMounter (and `-b /data/data/<pkg>` for proot), while
        // `context.filesDir` returns the Android `/data/user/0/...`
        // view that doesn't exist in the chroot's filesystem namespace.
        val srcLib = File(context.filesDir, "libhybris/lib").canonicalFile
        if (!srcLib.isDirectory) {
            log("libhybris: extracted tree missing $srcLib — skipping")
            return false
        }
        val entries = srcLib.listFiles()?.sortedBy { it.name }.orEmpty()
        if (entries.isEmpty()) {
            log("libhybris: $srcLib is empty — skipping")
            return false
        }

        // Build a single shell script: mkdir + per-entry `ln -sfn`.
        // Using `ln -sfn` so re-runs replace existing symlinks
        // atomically and don't follow into a previously-symlinked dir
        // (the `-n` flag). `--` separates the absolute target from the
        // destination so a future `lib*.so` filename starting with a
        // dash doesn't get misparsed as a flag.
        // Single-quote both target and destination through the same
        // quoter — a future libhybris filename containing $ ` or \ in
        // a double-quoted "$DST/$name" interpolation would otherwise
        // run live. Today's filenames are all `lib*.so`-shaped so this
        // is defence-in-depth, not a bug fix.
        fun sq(s: String) = "'" + s.replace("'", "'\\''") + "'"
        val script = buildString {
            appendLine("DST=${sq("$rootfsPath/usr/local/lib")}")
            appendLine("mkdir -p \"\$DST\"")
            for (src in entries) {
                // entries from listFiles() carry the parent's path
                // verbatim — re-canonicalise each so the literal
                // /data/data path goes into the symlink, not /data/user/0.
                appendLine("ln -sfn -- ${sq(src.canonicalPath)} \"\$DST\"/${sq(src.name)}")
            }
            appendLine("echo OK")
        }
        val r = method.runOutside(script) { log("libhybris-link: $it") }
        if (!r.ok) {
            throw IOException("LibhybrisLinker failed:\n${r.output}")
        }
        log("libhybris: linked ${entries.size} entries into $rootfsPath/usr/local/lib")
        return true
    }
}
