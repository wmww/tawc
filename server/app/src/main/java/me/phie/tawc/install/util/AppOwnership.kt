package me.phie.tawc.install.util

import me.phie.tawc.install.Su
import java.io.File

/**
 * Reset ownership of [dir] (only the dir node itself, not its contents)
 * to the app's current uid:gid so the app process can `open(O_WRONLY)`
 * files inside it. We `stat` `/data/data/<pkg>` to learn our uid since
 * `Process.myUid()` would require importing `android.os.Process` here
 * just for one place.
 *
 * Used by the installer right after `mkdirs()` of `<distros>/<id>/`,
 * before the in-app code writes `metadata.json` and `enter.sh` into the
 * dir. Without this the dir can end up root-owned (e.g. when an earlier
 * `su` process briefly created it) and subsequent app-uid writes fail.
 */
object AppOwnership {
    fun chownAppDirNonRecursive(dir: File) {
        val anchor = dir.parentFile?.parentFile ?: return // /data/data/<pkg>
        Su.run(
            """
            ANCHOR='${anchor.absolutePath}'
            UIDGID=${'$'}(stat -c '%u:%g' "${'$'}ANCHOR")
            chown "${'$'}UIDGID" '${dir.absolutePath}'
            """.trimIndent()
        )
    }
}
