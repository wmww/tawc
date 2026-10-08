package me.phie.tawc.launcher

import android.util.Log
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption

/**
 * One-time move of launcher state from where older versions kept it
 * into the [LauncherStore] (notes/launcher.md "Entry store"). Runs on
 * an install's first store load, so it also covers a format-1 export
 * imported later; it ends by setting `migrated` and is idempotent. If
 * the scan it needs for step 2 comes back empty, `migrated` stays 0
 * and the next load tries again.
 *
 * 1. `metadata.json`'s `hiddenDesktopIds`, `entryGraphics` and
 *    `entryPointerEmulation` become fields. The store is written
 *    first, then they are removed from the record.
 * 2. `.desktop` files the old editor wrote into the managed dir
 *    ([MANAGED_SUBDIR]): an override copy becomes field overrides, a
 *    personal entry a shortcut under its old id (pins keep working).
 *    Only files holding nothing beyond the editor's keys convert;
 *    anything else stays as a hand-made entry. A converted file is
 *    deleted after the store write.
 */
internal object LauncherMigration {

    private const val TAG = "tawc"

    /** The old editor's write dir: the guest's XDG per-user dir. */
    const val MANAGED_SUBDIR = "root/.local/share/applications"

    /** Where the old editor's icon imports went. */
    private const val USER_HICOLOR = "root/.local/share/icons/hicolor"

    /** `[Desktop Entry]` keys the old editor wrote. */
    private val EDITOR_KEYS = setOf("Type", "Version", "Name", "Exec", "Icon", "Terminal", "Comment")

    /** Size the old editor's SVG imports are rasterized at. */
    private const val ICON_PX = 256

    /** The legacy `metadata.json` fields. */
    data class Fields(
        val hidden: List<String> = emptyList(),
        val graphics: Map<String, String> = emptyMap(),
        val pointer: Map<String, String> = emptyMap(),
    ) {
        val isEmpty get() = hidden.isEmpty() && graphics.isEmpty() && pointer.isEmpty()
    }

    /** Access to the install record, split out for JVM tests. */
    interface Legacy {
        /** The slot has metadata; nothing is written for one without. */
        fun exists(id: String): Boolean
        fun read(id: String): Fields?
        fun clear(id: String)
    }

    class InstallationLegacy(private val installs: InstallationStore) : Legacy {
        override fun exists(id: String) = installs.metadataFile(id).exists()

        override fun read(id: String): Fields? = installs.load(id)
            ?.takeIf { it.state != Installation.State.CORRUPT }
            ?.let { Fields(it.legacyHiddenDesktopIds, it.legacyEntryGraphics, it.legacyEntryPointerEmulation) }

        override fun clear(id: String) {
            installs.update(id) {
                it.copy(
                    legacyHiddenDesktopIds = emptyList(),
                    legacyEntryGraphics = emptyMap(),
                    legacyEntryPointerEmulation = emptyMap(),
                )
            }
        }
    }

    /** Migrate [cur] (unmigrated) and write it. Caller holds the lock. */
    fun run(
        store: LauncherStore,
        id: String,
        cur: LauncherEntries,
        legacy: Legacy,
        scanShadows: (File) -> Map<String, String>,
        rasterize: (File, File, Int) -> Boolean,
    ): LauncherEntries {
        val converted = ArrayList<File>()
        // Managed files first, so metadata settings of a converted
        // personal entry land in its shortcut. Null = try again on the
        // next load.
        val managed = try {
            managed(store, id, cur, scanShadows, rasterize, converted)
        } catch (ex: Exception) {
            Log.w(TAG, "launcher migration for '$id': managed dir: $ex")
            converted.clear()
            cur
        }
        var e = managed ?: cur
        val fields = legacy.read(id)
        if (fields != null) e = withMetadata(e, fields)
        if (managed != null) e = e.copy(migrated = 1)
        store.save(id, e)
        if (fields != null && !fields.isEmpty) legacy.clear(id)
        for (f in converted) f.delete()
        if (converted.isNotEmpty()) Log.i(TAG, "launcher migration for '$id': converted ${converted.size} file(s)")
        return e
    }

    /** Step 1: metadata fields into [e]. Built-ins take `hidden` only;
     *  anything [LauncherEntries.withField] refuses is dropped. */
    fun withMetadata(e: LauncherEntries, f: Fields): LauncherEntries {
        var out = e
        fun set(id: String, field: String, value: Any) {
            runCatching { out = out.withField(id, field, value) }
        }
        for (id in f.hidden) set(id, "hidden", true)
        for ((id, key) in f.graphics) set(id, "graphics", key)
        for ((id, key) in f.pointer) set(id, "pointer", key)
        return out
    }

    private fun managed(
        store: LauncherStore,
        installId: String,
        cur: LauncherEntries,
        scanShadows: (File) -> Map<String, String>,
        rasterize: (File, File, Int) -> Boolean,
        converted: MutableList<File>,
    ): LauncherEntries? {
        val rootfs = store.rootfs(installId)
        val files = File(rootfs, MANAGED_SUBDIR).listFiles { f ->
            f.name.endsWith(".desktop") && Files.isRegularFile(f.toPath(), LinkOption.NOFOLLOW_LINKS)
        }?.sortedBy { it.name } ?: return cur
        if (files.isEmpty()) return cur
        // Winning entry path → the copy it shadows. Empty means the scan
        // failed: without it overrides can't be told apart, so wait.
        val shadows = scanShadows(rootfs)
        if (shadows.isEmpty()) return null
        var e = cur
        for (f in files) {
            val id = f.name.removeSuffix(".desktop")
            if (id.startsWith("tawc:")) continue
            val bytes = runCatching { f.readBytes() }.getOrNull() ?: continue
            val text = String(bytes)
            if (!text.toByteArray().contentEquals(bytes)) continue
            val shadowPath = shadows[f.canonicalPath]?.takeIf { it.isNotEmpty() }
            val packaged = shadowPath?.let { p ->
                runCatching { DesktopEntryFile.fileInRootfs(p, rootfs)?.readText() }.getOrNull()
            }
            // Can't diff against an unreadable packaged file.
            if (shadowPath != null && packaged == null) continue
            var fields = convert(text, packaged) ?: continue
            fields.icon?.takeIf { it.startsWith(IconImport.PREFIX) }?.let { name ->
                importIcon(store, installId, rootfs, name, rasterize)?.let { fields = fields.copy(icon = null, iconFile = it) }
            }
            e = if (packaged != null) e.withOverride(id, fields) else e.withShortcut(id, fields)
            converted += f
        }
        return e
    }

    /**
     * The fields for a managed file's [text]: overrides against its
     * [packaged] file's text, or a whole shortcut when null. Null when
     * the file isn't one the old editor wrote: other groups, keys
     * beyond [EDITOR_KEYS] (locale keys, `Actions`, …), not an
     * Application, or no Exec. A leading `env` in Exec becomes `env`.
     */
    fun convert(text: String, packaged: String?): EntryFields? {
        if (!editorOnly(text)) return null
        val d = DesktopEntryFile.parse(text)
        val line = DesktopEntryFile.splitExec(d.exec)
        val exec = stripFieldCodes(line.command)
        if (exec.isBlank()) return null
        val env = line.env.toMap().takeIf { it.isNotEmpty() }
        if (packaged == null) {
            return EntryFields(
                name = d.name.ifBlank { null },
                comment = d.comment.ifBlank { null },
                exec = exec,
                terminal = d.terminal,
                icon = d.icon.ifBlank { null },
                env = env,
            )
        }
        val p = DesktopEntryFile.parse(packaged)
        return EntryFields(
            name = d.name.takeIf { it != p.name },
            comment = d.comment.takeIf { it != p.comment },
            exec = exec.takeIf { it != stripFieldCodes(p.exec) },
            terminal = d.terminal.takeIf { it != p.terminal },
            icon = d.icon.takeIf { it != p.icon },
            env = env,
        )
    }

    /** One `[Desktop Entry]` group of [EDITOR_KEYS] with
     *  `Type=Application`; comments and blank lines allowed. */
    fun editorOnly(text: String): Boolean {
        var groups = 0
        var app = false
        for (raw in text.lines()) {
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            if (line.startsWith("[")) {
                if (line != "[Desktop Entry]" || ++groups > 1) return false
                continue
            }
            val eq = line.indexOf('=')
            if (groups == 0 || eq <= 0) return false
            val key = line.substring(0, eq).trim()
            if (key !in EDITOR_KEYS) return false
            if (key == "Type") app = line.substring(eq + 1).trim() == "Application"
        }
        return groups == 1 && app
    }

    /** The scanner's `strip_field_codes`: drop `%X`, `%%` → `%`,
     *  collapse whitespace. */
    fun stripFieldCodes(exec: String): String {
        val out = StringBuilder()
        var i = 0
        while (i < exec.length) {
            val c = exec[i++]
            if (c != '%') { out.append(c); continue }
            if (i >= exec.length) { out.append('%'); break }
            if (exec[i++] == '%') out.append('%')
        }
        return out.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
    }

    /** An old editor import `Icon=tawc-*` copied into the store as PNG. */
    private fun importIcon(
        store: LauncherStore,
        installId: String,
        rootfs: File,
        name: String,
        rasterize: (File, File, Int) -> Boolean,
    ): String? {
        val png = File(rootfs, "$USER_HICOLOR/256x256/apps/$name.png")
        val svg = File(rootfs, "$USER_HICOLOR/scalable/apps/$name.svg")
        val bytes = when {
            png.isFile -> png.readBytes()
            svg.isFile -> {
                val tmp = File(store.dir(installId).apply { mkdirs() }, "import.png")
                try {
                    if (rasterize(svg, tmp, ICON_PX)) tmp.readBytes() else null
                } finally {
                    tmp.delete()
                }
            }
            else -> null
        } ?: return null
        return store.addIcon(installId, bytes, name)
    }
}
