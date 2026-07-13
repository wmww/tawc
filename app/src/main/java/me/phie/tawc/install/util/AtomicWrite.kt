package me.phie.tawc.install.util

import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * Write [text] to [target] so a crash or power loss leaves either the
 * old contents or the new ones — never a truncated file. Stages a
 * sibling `<name>.tmp`, fsyncs it, then `rename(2)`s into place.
 *
 * The fsync is load-bearing: a bare write+rename survives an app crash
 * but not power loss, where the filesystem journal can commit the
 * rename while the new data blocks are still unwritten — the classic
 * empty-file-after-rename hazard.
 */
fun atomicWriteText(target: File, text: String) {
    val tmp = File(target.parentFile, target.name + ".tmp")
    tmp.outputStream().use { out ->
        out.write(text.toByteArray())
        out.fd.sync()
    }
    Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE)
}
