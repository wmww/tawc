package me.phie.tawc.install

import android.util.Log
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Tiny HTTP downloader. Streams to disk, reports progress every ~256 KiB,
 * follows redirects manually so we can switch HTTP↔HTTPS hops cleanly.
 *
 * Resume via Range header is intentionally not implemented — the bootstrap
 * tarballs are ~120–200 MB and re-downloading on a failure is acceptable;
 * resume adds significant complexity (server must support it, partial-file
 * validation on completion, etc.).
 */
object Downloader {
    private const val TAG = "tawc-install"
    private const val MAX_REDIRECTS = 5
    private const val CHUNK_BYTES = 64 * 1024
    private const val PROGRESS_BYTES = 256 * 1024

    /**
     * Download [url] to [dest]. If [dest] already exists and has the
     * matching size advertised by the server, the download is skipped.
     *
     * @param onProgress called with (bytesRead, totalBytes-or-null).
     */
    fun download(
        url: String,
        dest: File,
        onProgress: (Long, Long?) -> Unit = { _, _ -> },
    ) {
        val (resolvedUrl, contentLength) = try {
            head(url)
        } catch (e: java.io.IOException) {
            // HEAD failed (likely DNS / network). If we already have a
            // non-empty cache, trust it — the integrity layer
            // (CrossMirrorMd5 / signature verify) catches a corrupt
            // file. Without this fallback an offline retry of an
            // already-downloaded bootstrap aborts here.
            if (dest.exists() && dest.length() > 0) {
                Log.d(TAG, "Cached (HEAD failed: ${e.message}): ${dest.name} (${dest.length()} bytes)")
                onProgress(dest.length(), null)
                return
            }
            throw e
        }

        if (dest.exists() && contentLength != null && dest.length() == contentLength) {
            Log.d(TAG, "Cached: ${dest.name} (${dest.length()} bytes)")
            onProgress(dest.length(), contentLength)
            return
        }

        // Caller (BootstrapCache) owns the parent dir lifecycle and has
        // already mkdir'd by the time we're here.
        val tmp = File(dest.parentFile, dest.name + ".part")
        tmp.delete()

        val conn = (URL(resolvedUrl).openConnection() as HttpURLConnection).apply {
            connectTimeout = 30_000
            readTimeout = 60_000
            instanceFollowRedirects = true
        }

        try {
            val total = contentLength ?: conn.contentLengthLong.takeIf { it > 0 }
            var read = 0L
            var lastReported = 0L
            conn.inputStream.use { input ->
                tmp.outputStream().use { out ->
                    val buf = ByteArray(CHUNK_BYTES)
                    while (true) {
                        // HttpURLConnection's stream isn't an
                        // interruptible NIO channel, so a thread
                        // interrupt by `runInterruptible` doesn't
                        // abort `input.read(buf)` directly. Check the
                        // flag explicitly between chunks so cancel
                        // takes effect at the next 64 KiB boundary
                        // (≤ a few hundred ms over LTE). The
                        // `conn.disconnect()` in `finally` then frees
                        // the socket; the `.part` file is left for
                        // the next attempt to overwrite.
                        if (Thread.currentThread().isInterrupted) {
                            throw InterruptedIOException("download cancelled")
                        }
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        read += n
                        if (read - lastReported >= PROGRESS_BYTES) {
                            onProgress(read, total)
                            lastReported = read
                        }
                    }
                }
            }
            onProgress(read, total)
        } finally {
            conn.disconnect()
        }

        if (!tmp.renameTo(dest)) {
            throw IOException("Failed to rename ${tmp.path} -> ${dest.path}")
        }
    }

    /**
     * HEAD the URL to learn the final URL (after redirects) and content
     * length. Falls through gracefully if HEAD isn't supported.
     */
    private fun head(initialUrl: String): Pair<String, Long?> {
        var url = initialUrl
        repeat(MAX_REDIRECTS) {
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "HEAD"
                connectTimeout = 15_000
                readTimeout = 15_000
                instanceFollowRedirects = false
            }
            try {
                val code = conn.responseCode
                when (code) {
                    in 300..399 -> {
                        val loc = conn.getHeaderField("Location")
                            ?: throw IOException("Redirect without Location header")
                        url = if (loc.startsWith("http")) loc else URL(URL(url), loc).toString()
                    }
                    in 200..299 -> {
                        val len = conn.contentLengthLong.takeIf { it > 0 }
                        return url to len
                    }
                    else -> throw IOException("HEAD $url returned HTTP $code")
                }
            } finally {
                conn.disconnect()
            }
        }
        throw IOException("Too many redirects following $initialUrl")
    }
}
