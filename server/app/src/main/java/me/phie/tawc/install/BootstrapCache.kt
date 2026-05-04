package me.phie.tawc.install

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.attribute.FileTime

/**
 * Sole owner of the bootstrap-tarball cache at `<cacheDir>/install/`.
 *
 * Centralises everything that touches that directory: the canonical
 * filename scheme (`bootstrap-<arch>.tar.{zst,gz}`), the FIFO used by
 * [Archive] to stream zstd → tar (`bootstrap-<arch>.tar.fifo`),
 * mtime-based "last used" tracking, and the [sweepStale] janitor that
 * runs at app start.
 *
 * Why TTL on top of `context.cacheDir`: Android only evicts cacheDir
 * under storage pressure or via "Clear Cache". On a phone with plenty
 * of free space a 200 MB tarball can sit there indefinitely, competing
 * with caches from apps the user actually uses. The TTL bounds that.
 *
 * `BootstrapCache` is intentionally stateless — callers (`InstallationService`)
 * may construct it ad-hoc from any context.
 */
class BootstrapCache(private val dir: File) {
    constructor(context: Context) : this(File(context.cacheDir, "install"))

    init {
        dir.mkdirs()
    }

    /**
     * Download [url] for [arch] (using [format] for the cache filename),
     * touch its mtime so the TTL counts from "last used", and return the
     * cached file. The single entry point — [ArchInstaller] doesn't see
     * `pathFor`/`markUsed` separately, so there's no way to forget the
     * freshness touch.
     *
     * `arch` and `format` are passed independently because the arch→format
     * mapping is upstream-mirror policy that lives in
     * [ArchInstaller.selectBootstrap]; duplicating the `when` here would
     * be the kind of split this class exists to prevent.
     *
     * mtime is touched both before and after [Downloader.download]: the
     * pre-touch shrinks the cache-hit-vs-sweep race window (sweep stat()s
     * a fresh mtime even on the cache-hit early-return path), the
     * post-touch covers the cold-cache path where the file didn't exist
     * before the call.
     */
    fun download(
        arch: String,
        url: String,
        format: BootstrapFormat,
        onProgress: (Long, Long?) -> Unit,
    ): File {
        val dest = pathFor(arch, format)
        markUsed(dest)
        Downloader.download(url, dest, onProgress)
        markUsed(dest)
        return dest
    }

    /**
     * Path to the FIFO that [Archive.extractAsRoot] streams zstd-
     * decompressed bytes through when extracting `.tar.zst` bootstraps
     * (toybox tar can't read zstd, and piping into `su`'s stdin loses
     * bytes — see Archive.kt). Lifecycle is bounded by one
     * [Archive.extractAsRoot] call: `mkfifo`'d on entry (with any
     * leftover `unlink`ed first) and deleted in the `finally`. Crash
     * leftovers are reaped by [sweepStale] unconditionally on app start.
     */
    fun tempFifoFor(arch: String): File = File(dir, "bootstrap-$arch.tar.fifo")

    /**
     * Two-pass janitor:
     *
     * 1. **TTL pass** — canonical `bootstrap-<arch>.tar.{zst,gz}` files
     *    older than [MAX_AGE_MILLIS] are deleted.
     * 2. **Unconditional pass** — any `bootstrap-<arch>.tar.fifo`
     *    (the [Archive] FIFO; or `.tmp`, the historical zstd-
     *    decompression target left over from older builds) or `*.part`
     *    (the [Downloader] in-flight suffix) is deleted regardless of
     *    mtime, since these are never valid across processes — they
     *    only exist inside one install-time call's `try/finally`.
     *    Anything found at app start is by definition stranded by a
     *    crash.
     *
     * Returns the number of files deleted, for logging.
     */
    fun sweepStale(nowMillis: Long = System.currentTimeMillis()): Int {
        val files = dir.listFiles() ?: return 0
        var deleted = 0
        for (f in files) {
            // `isFile` is false for FIFOs, so we only filter out
            // directories (none expected in this dir, but defensive).
            if (f.isDirectory) continue
            val name = f.name
            val canonical = CANONICAL_NAME_RE.matches(name)
            val transient = TRANSIENT_NAME_RE.matches(name)
            val evict = when {
                transient -> true
                canonical -> (nowMillis - f.lastModified()) >= MAX_AGE_MILLIS
                else -> false
            }
            if (!evict) continue
            if (f.delete()) {
                Log.d(TAG, "Evicted bootstrap cache: $name")
                deleted++
            } else {
                Log.w(TAG, "Failed to delete bootstrap cache: $name")
            }
        }
        return deleted
    }

    private fun pathFor(arch: String, format: BootstrapFormat): File =
        File(dir, "bootstrap-$arch.${format.ext}")

    /**
     * Mark [file] as freshly used. Uses NIO `setLastModifiedTime` rather
     * than `File.setLastModified` because the latter silently no-ops on
     * some Android FS/SDK combos; NIO's variant is honoured reliably on
     * API 26+ (`minSdk = 29`). Failures are logged and swallowed —
     * sweep still works on stale mtimes; this is a freshness hint.
     */
    private fun markUsed(file: File) {
        if (!file.exists()) return
        try {
            Files.setLastModifiedTime(file.toPath(), FileTime.fromMillis(System.currentTimeMillis()))
        } catch (e: IOException) {
            Log.w(TAG, "setLastModifiedTime failed for ${file.name}", e)
        }
    }

    companion object {
        private const val TAG = "tawc-install"
        private const val DAY_MILLIS = 24L * 60 * 60 * 1000

        /**
         * 7 days. Long enough that an install→test→uninstall→reinstall
         * loop within the same week reuses the cache; short enough that
         * a forgotten cache doesn't squat for months.
         */
        private const val MAX_AGE_MILLIS = 7 * DAY_MILLIS

        /** `bootstrap-<arch>.tar.{zst,gz,xz}`. */
        private val CANONICAL_NAME_RE = Regex("""^bootstrap-[A-Za-z0-9_-]+\.tar\.(zst|gz|xz)$""")

        /**
         * `bootstrap-<arch>.tar.fifo` (Archive's FIFO; `.tmp` is the
         * historical zstd-decompression target from older builds) or
         * `bootstrap-<arch>.tar.{zst,gz,xz}.part` (Downloader's in-flight
         * suffix). Always evicted on app start.
         */
        private val TRANSIENT_NAME_RE =
            Regex("""^bootstrap-[A-Za-z0-9_-]+\.tar\.(fifo|tmp|(zst|gz|xz)\.part)$""")
    }
}

/**
 * Compression format of a bootstrap tarball. Determines the cache
 * filename extension and (downstream of [Archive.extractAsRoot])
 * whether the bytes are streamed through a FIFO (zstd, since toybox
 * tar can't read it) or read by tar directly (gzip / plain `.tar`).
 */
enum class BootstrapFormat(val ext: String) {
    ZSTD("tar.zst"),
    GZIP("tar.gz"),
    XZ("tar.xz"),
}
