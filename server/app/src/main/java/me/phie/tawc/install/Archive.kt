package me.phie.tawc.install

import android.system.Os
import android.system.OsConstants
import android.util.Log
import com.github.luben.zstd.ZstdInputStream
import org.tukaani.xz.XZInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.util.concurrent.atomic.AtomicReference

/**
 * Bootstrap-tarball helpers.
 *
 * Extraction is delegated to the system's `toybox tar` running as root
 * via [Su] so every file lands with root ownership and tar's special-file
 * handling (symlinks, hardlinks, devices) Just Works inside the chroot.
 *
 * Toybox tar reads gzip natively but not zstd or xz, and we can't pipe
 * the decompressed bytes into the shell's stdin (the shell pre-buffers
 * past the script body, tar then sees garbage — "tar: Not tar"). So for
 * `.tar.zst` and `.tar.xz` inputs we stream the decompressed bytes
 * through a named pipe: a writer thread on the app side feeds the
 * decompressor → FIFO, while tar reads `tar -xf <fifo>` as a positional
 * argument (not stdin, so the shell never touches the bytes). Peak
 * install-time disk usage stays at `compressed tarball + rootfs` — we
 * never materialise the ~700 MB uncompressed tar on disk.
 *
 * Toybox tar can't `--strip-components`, so when the bootstrap is wrapped
 * in a single top-level dir (`root.x86_64/`) we extract verbatim and
 * flatten the wrapper with `mv` afterwards.
 */
object Archive {

    /**
     * Extract [tarball] (a `.tar`, `.tar.gz`, or `.tar.zst`) into [destDir]
     * via toybox `tar` running under [Su]. Used by the chroot install
     * method only — proot extracts via [ProotArchiveExtractor] in pure
     * Kotlin to dodge toybox's lack of a "skip-recorded-mode" knob.
     *
     * Creates [destDir] if it doesn't exist; **does not** clear an
     * existing directory — the install pipeline's state-machine gate
     * guarantees we only ever run against a `(no dir)` slot. Root
     * permissions are required, so every file lands with the uids
     * the tarball recorded (which is what the chroot view wants).
     *
     * For `.tar.zst` inputs the bytes flow through [tempFifo] (a named
     * pipe owned by the cache); the FIFO is recreated on entry (so a
     * crash leftover never gets reused) and deleted in the `finally`.
     * For `.tar` / `.tar.gz` inputs [tempFifo] is unused.
     *
     * (Wiping is the sole job of [InstallationMethod.wipe], called from
     * the uninstall path. If install ever wiped here, a single missed
     * unmount could let `rm` walk through a live `/dev` bind and unlink
     * host system nodes — see notes/installation.md.)
     *
     * If [stripPrefix] is non-null and exactly that single top-level
     * directory exists in the tarball, its contents are flattened into
     * [destDir] (and the now-empty prefix dir removed).
     *
     * @throws IOException on any extraction failure.
     */
    fun extractAsRoot(
        tarball: File,
        destDir: String,
        tempFifo: File,
        stripPrefix: String? = null,
        onLine: (String) -> Unit = {},
    ) {
        require(tarball.exists()) { "Tarball not found: $tarball" }
        val name = tarball.name.lowercase()
        when {
            name.endsWith(".tar") ||
            name.endsWith(".tar.gz") ||
            name.endsWith(".tgz") -> {
                runTarScript(tarball.absolutePath, destDir, stripPrefix, onLine)
            }
            name.endsWith(".tar.zst") || name.endsWith(".tzst") -> {
                extractStreamViaFifo(tarball, destDir, tempFifo, stripPrefix, onLine) {
                    ZstdInputStream(it)
                }
            }
            name.endsWith(".tar.xz") || name.endsWith(".txz") -> {
                extractStreamViaFifo(tarball, destDir, tempFifo, stripPrefix, onLine) {
                    XZInputStream(it)
                }
            }
            else -> throw IOException("Unsupported tarball extension: ${tarball.name}")
        }
    }

    /**
     * Stream decompressed bytes through a FIFO so tar consumes them
     * straight out of the kernel pipe buffer without ever touching disk.
     * The shell-stdin trick (where the shell pre-buffers past our script
     * and tar then sees garbage) is bypassed because the shell never
     * sees the tar bytes — tar opens the FIFO directly via the path we
     * pass on its command line.
     *
     * [decompressor] wraps the raw file input in a format-specific
     * decompressing stream (zstd-jni or xz-java). Toybox tar reads gzip
     * natively, so the gzip path doesn't go through here.
     *
     * Failure modes the cleanup must cope with:
     *   - tar fails fast / never opens the FIFO → writer is blocked on
     *     `open(O_WRONLY)` waiting for a reader. The `finally` opens
     *     the FIFO `O_RDONLY|O_NONBLOCK` and closes it, which both
     *     unblocks the writer's open() and signals "no readers" so its
     *     subsequent write() returns EPIPE.
     *   - writer throws (e.g. corrupt input) → tar sees EOF mid-stream
     *     and fails; the script's non-OK exit (with full tar/su output)
     *     surfaces as the primary error and the writer's exception is
     *     attached as a suppressed cause. If the script somehow ran OK
     *     but the writer still failed, the writer error is thrown
     *     directly.
     */
    private fun extractStreamViaFifo(
        tarball: File,
        destDir: String,
        fifo: File,
        stripPrefix: String?,
        onLine: (String) -> Unit,
        decompressor: (InputStream) -> InputStream,
    ) {
        // mkfifo refuses if the target already exists — clear any
        // crash leftover (and a regular file too, just in case).
        fifo.delete()
        Os.mkfifo(fifo.absolutePath, OsConstants.S_IRUSR or OsConstants.S_IWUSR)

        val writerError = AtomicReference<Throwable?>(null)
        val writer = Thread({
            try {
                BufferedInputStream(tarball.inputStream(), 256 * 1024).use { raw ->
                    decompressor(raw).use { zin ->
                        FileOutputStream(fifo).use { out ->
                            zin.copyTo(out)
                        }
                    }
                }
            } catch (t: Throwable) {
                writerError.set(t)
            }
        }, "tawc-bootstrap-decompress")
        writer.isDaemon = true
        writer.start()

        var primary: Throwable? = null
        try {
            runTarScript(fifo.absolutePath, destDir, stripPrefix, onLine)
        } catch (t: Throwable) {
            primary = t
            throw t
        } finally {
            // If the script never opened the FIFO (su unavailable,
            // tar binary missing, script aborted before the tar line,
            // …) the writer is still blocked on open(O_WRONLY). A
            // brief O_RDONLY|O_NONBLOCK open + close lets the writer's
            // open() return; closing our reader fd then makes its
            // next write() get EPIPE — either way the thread terminates.
            if (writer.isAlive) {
                try {
                    val fd = Os.open(
                        fifo.absolutePath,
                        OsConstants.O_RDONLY or OsConstants.O_NONBLOCK,
                        0,
                    )
                    Os.close(fd)
                } catch (_: Exception) {
                    // best-effort
                }
            }
            writer.join(FIFO_JOIN_TIMEOUT_MILLIS)
            if (writer.isAlive) {
                // Should be unreachable — flag it loudly if it fires so
                // we notice the leaked daemon thread instead of silently
                // proceeding.
                Log.w(TAG, "zstd writer thread still alive after ${FIFO_JOIN_TIMEOUT_MILLIS}ms join")
            }
            fifo.delete()
            // The tar/su error is the rich diagnostic; a writer error
            // is almost always its downstream effect (corrupt input →
            // tar EOF → both throw). Throwing from `finally` would
            // *replace* the in-flight `primary`, so when both fail we
            // attach the writer error as suppressed and let the tar
            // error propagate. Only if the script returned cleanly do
            // we throw the writer error directly.
            val werr = writerError.get()
            if (werr != null) {
                if (primary != null) primary.addSuppressed(werr)
                else throw IOException("decompression into FIFO failed", werr)
            }
        }
    }

    private fun runTarScript(
        tarPath: String,
        destDir: String,
        stripPrefix: String?,
        onLine: (String) -> Unit,
    ) {
        val script = buildString {
            appendLine("mkdir -p '$destDir'")
            // -z is autodetected by toybox tar when the file's first
            // bytes look like gzip; we don't need to spell it out.
            appendLine("tar -xf '$tarPath' -C '$destDir'")
            if (stripPrefix != null) {
                appendLine("if [ -d '$destDir/$stripPrefix' ]; then")
                appendLine("    cd '$destDir/$stripPrefix'")
                appendLine("    for entry in * .[!.]* ..?*; do")
                appendLine("        [ -e \"\$entry\" ] || continue")
                appendLine("        mv -- \"\$entry\" '$destDir/'")
                appendLine("    done")
                appendLine("    cd '$destDir'")
                appendLine("    rmdir '$destDir/$stripPrefix'")
                appendLine("fi")
            }
            appendLine("echo OK")
        }
        val result = Su.run(script, onLine = onLine)
        if (!result.ok) {
            throw IOException(
                "Tar extraction failed (exit=${result.exitCode}). Output:\n${result.output}"
            )
        }
        if (!result.output.lineSequence().any { it.trim() == "OK" }) {
            throw IOException("Tar extraction did not report OK. Output:\n${result.output}")
        }
    }

    /**
     * Generous bound — even a 700 MB decompressed tar streams through
     * the FIFO in well under a minute on real devices. The join only
     * approaches this timeout if a prior step left the writer thread
     * stuck despite the FIFO-drain unblock; better to bail than block
     * the install thread forever.
     */
    private const val FIFO_JOIN_TIMEOUT_MILLIS: Long = 60_000

    private const val TAG = "tawc-install"
}
