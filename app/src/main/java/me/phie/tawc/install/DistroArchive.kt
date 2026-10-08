package me.phie.tawc.install

import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.json.JSONObject
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The distro export archive format (notes/installation.md "Export /
 * import"): a zstd-compressed PAX tar of `<distros>/<id>/`, framed by
 * a [Manifest] first entry and a [Trailer] last entry. Shared by
 * [DistroExporter] and [DistroImporter].
 */
internal object DistroArchive {
    /** Bump on an incompatible change; importers refuse newer.
     *  2: adds the `launcher/` store (format 1 imports still migrate). */
    const val FORMAT = 2

    const val MANIFEST = "tawc-export.json"
    const val METADATA = "metadata.json"
    const val TRAILER = "tawc-export-end.json"

    const val MIME = "application/zstd"
    const val EXTENSION = ".tawc.tar.zst"

    /** PAX key prefix for xattrs (GNU tar / star convention). Only
     *  `user.*` names are exported and restored. */
    const val XATTR_PAX_PREFIX = "SCHILY.xattr."

    /** Size cap for the JSON entries; anything bigger isn't ours. */
    const val MAX_JSON_BYTES = 256 * 1024

    /** Newest link-store format this build's tawcroot speaks
     *  (`STORE_VERSION_SUPPORTED` in tawcroot/src/linkstore.c). */
    const val LINK_STORE_VERSION = 1

    /** The install-dir children an archive carries. */
    const val ROOTFS = "rootfs"
    const val STORE = "tawcroot"
    /** [me.phie.tawc.launcher.LauncherStore]'s dir. */
    const val LAUNCHER = "launcher"
    val ROOTS = listOf(ROOTFS, STORE, LAUNCHER)

    fun suggestedFileName(id: String, nowMillis: Long): String =
        "$id-${SimpleDateFormat("yyyyMMdd", Locale.US).format(Date(nowMillis))}$EXTENSION"

    data class Manifest(
        val format: Int,
        val createdAtMillis: Long,
        val appVersionName: String,
        val appVersionCode: Long,
        val sourcePackage: String,
        val id: String,
        val label: String?,
        val distro: String,
        val arch: String,
        val method: String,
        /** Advisory (pre-walk); the trailer is authoritative. */
        val uncompressedBytes: Long,
        val entries: Long,
    ) {
        fun toJson(): String = JSONObject().apply {
            put("format", format)
            put("createdAtMillis", createdAtMillis)
            put("appVersionName", appVersionName)
            put("appVersionCode", appVersionCode)
            put("sourcePackage", sourcePackage)
            put("id", id)
            if (label != null) put("label", label)
            put("distro", distro)
            put("arch", arch)
            put("method", method)
            put("uncompressedBytes", uncompressedBytes)
            put("entries", entries)
        }.toString(2)

        companion object {
            /** Throws [IllegalArgumentException] for a too-new format or
             *  a malformed manifest. */
            fun parse(text: String): Manifest {
                val o = try {
                    JSONObject(text)
                } catch (e: org.json.JSONException) {
                    throw IllegalArgumentException("bad export manifest: ${e.message}")
                }
                val format = o.optInt("format", -1)
                require(format >= 1) { "bad export manifest: no format" }
                require(format <= FORMAT) {
                    "export format $format is newer than supported $FORMAT (made by a newer app version?)"
                }
                return try {
                    Manifest(
                        format = format,
                        createdAtMillis = o.optLong("createdAtMillis", 0L),
                        appVersionName = o.optString("appVersionName", ""),
                        appVersionCode = o.optLong("appVersionCode", 0L),
                        sourcePackage = o.optString("sourcePackage", ""),
                        id = o.getString("id"),
                        label = if (o.has("label") && !o.isNull("label")) o.getString("label") else null,
                        distro = o.getString("distro"),
                        arch = o.getString("arch"),
                        method = o.getString("method"),
                        uncompressedBytes = o.optLong("uncompressedBytes", 0L),
                        entries = o.optLong("entries", 0L),
                    )
                } catch (e: org.json.JSONException) {
                    throw IllegalArgumentException("bad export manifest: ${e.message}")
                }
            }
        }
    }

    /** [entries] counts every entry before the trailer; [sha256] is
     *  the [EntryDigest] over them. */
    data class Trailer(val entries: Long, val sha256: String) {
        fun toJson(): String = JSONObject().apply {
            put("entries", entries)
            put("sha256", sha256)
        }.toString(2)

        companion object {
            fun parse(text: String): Trailer = try {
                val o = JSONObject(text)
                Trailer(o.getLong("entries"), o.getString("sha256"))
            } catch (e: org.json.JSONException) {
                throw java.io.IOException("bad export end marker: ${e.message}")
            }
        }
    }

    /**
     * SHA-256 over every entry before the trailer: a canonical header
     * line (type, mode, mtime seconds, size, name, link target, xattrs)
     * plus the entry's content. Covers exactly what extraction consumes, and
     * unlike a hash of raw tar bytes it doesn't depend on how the tar
     * library blocks or pads records.
     */
    class EntryDigest {
        private val md = MessageDigest.getInstance("SHA-256")
        var count: Long = 0
            private set

        fun header(e: TarArchiveEntry) {
            count++
            val type = when {
                e.isDirectory -> 'd'
                e.isSymbolicLink -> 'l'
                e.isLink -> 'h'
                e.isFile -> 'f'
                else -> '?'
            }
            val size = if (e.isFile && !e.isLink) e.size else 0L
            val xattrs = e.extraPaxHeaders.filterKeys { it.startsWith(XATTR_PAX_PREFIX) }
                .toSortedMap().entries.joinToString("") { "\u0000${it.key}=${it.value}" }
            val line = "$type\u0000${e.mode and 0xFFF}\u0000${Math.floorDiv(e.modTime.time, 1000L)}" +
                "\u0000$size\u0000${e.name}\u0000${e.linkName ?: ""}$xattrs\n"
            md.update(line.toByteArray(Charsets.UTF_8))
        }

        fun content(buf: ByteArray, off: Int, len: Int) {
            if (len > 0) md.update(buf, off, len)
        }

        fun hex(): String = md.digest().joinToString("") { "%02x".format(it) }
    }

    /** How the exporter treats one install-dir-relative path. */
    enum class ExportRule {
        INCLUDE,
        /** The dir itself, none of its children. */
        DIR_ONLY,
        EXCLUDE,
        /** Its presence makes the export fail. */
        FAIL,
    }

    /**
     * Export policy for [rel] (relative to the install dir, `/`
     * separated). Only `rootfs/`, the `tawcroot/` link store and the
     * `launcher/` store travel; `metadata.json` is written separately
     * as the second entry, and
     * everything else at the top (`ando/`, `bootstrap-work/`,
     * `metadata.json.tmp`) is per-device runtime state.
     */
    fun exportRule(rel: String): ExportRule {
        val top = rel.substringBefore('/')
        if (top !in ROOTS) return ExportRule.EXCLUDE
        // An atomic write's staging file.
        if (top == LAUNCHER && rel.endsWith(".tmp")) return ExportRule.EXCLUDE
        return when (rel) {
            // Runtime sockets and agent state; the tmp sweeper already
            // treats it as disposable.
            "$ROOTFS/tmp" -> ExportRule.DIR_ONLY
            // Recreated on demand (O_CREAT in linkstore.c).
            "$STORE/lock" -> ExportRule.EXCLUDE
            // A pending journal records host-real paths of this app;
            // quiesce must have resolved it.
            "$STORE/intent" -> ExportRule.FAIL
            // A torn, never-renamed journal write; ignored by recovery.
            "$STORE/intent.new" -> ExportRule.EXCLUDE
            // Stray O_TMPFILE files, and staged entries parked by a
            // lost race. Without a pending intent both are leaks
            // (counts stay high, never low), so dropping them is safe.
            "$STORE/tmp", "$STORE/work" -> ExportRule.DIR_ONLY
            else -> ExportRule.INCLUDE
        }
    }

    /**
     * Why archive entry [e] may not be extracted on import, or null if
     * it may. The extractor's canonical-path containment still applies
     * on top; this narrows it to [ROOTS], keeps the link store's own
     * dirs real directories, and allows only dirs and regular files
     * under `launcher/` (its icon paths are trusted as files).
     */
    fun importRejection(e: TarArchiveEntry): String? {
        val name = e.name.removeSuffix("/")
        pathRejection(name)?.let { return "${e.name}: $it" }
        if (e.isSymbolicLink && name in PINNED_DIRS) {
            return "${e.name}: must be a directory"
        }
        if (name.substringBefore('/') == LAUNCHER && (e.isSymbolicLink || e.isLink)) {
            return "${e.name}: links are not allowed in $LAUNCHER/"
        }
        if (name == "$STORE/intent" || name.startsWith("$STORE/intent.") ||
            name.startsWith("$STORE/work/")
        ) {
            return "${e.name}: pending link-store state is not portable"
        }
        if (e.isLink) {
            pathRejection(e.linkName.removeSuffix("/"))?.let { return "${e.name} -> ${e.linkName}: $it" }
        }
        return null
    }

    private val PINNED_DIRS = setOf(ROOTFS, STORE, LAUNCHER, "$STORE/link", "$STORE/tmp", "$STORE/work")

    private fun pathRejection(name: String): String? {
        if (name.isEmpty() || name.startsWith("/")) return "absolute or empty path"
        val parts = name.split('/')
        if (parts.any { it.isEmpty() || it == "." || it == ".." }) return "non-canonical path"
        if (parts[0] !in ROOTS) return "outside ${ROOTS.joinToString("/, ")}/"
        return null
    }
}
