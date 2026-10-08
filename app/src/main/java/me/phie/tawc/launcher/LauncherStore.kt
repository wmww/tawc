package me.phie.tawc.launcher

import android.content.Context
import android.util.Log
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.util.atomicWriteBytes
import me.phie.tawc.install.util.atomicWriteText
import org.json.JSONObject
import org.json.JSONTokener
import java.io.File
import java.io.IOException

/**
 * One entry's TAWC-side fields in `entries.json`, all optional. As an
 * override, a present field replaces the scanned value; as a shortcut,
 * the fields are the whole entry. [extra] keeps fields this build
 * doesn't know (key → JSON text), so an older app doesn't drop a newer
 * one's data on rewrite.
 */
data class EntryFields(
    val name: String? = null,
    val comment: String? = null,
    val exec: String? = null,
    val terminal: Boolean? = null,
    /** Theme name or in-rootfs path, like `Icon=`. */
    val icon: String? = null,
    /** File name under `launcher/icons/`; wins over [icon]. */
    val iconFile: String? = null,
    /** Exported before `exec` at launch. */
    val env: Map<String, String>? = null,
    /** [me.phie.tawc.GraphicsBackend.key]. */
    val graphics: String? = null,
    /** [me.phie.tawc.PointerEmulation.key]. */
    val pointer: String? = null,
    val hidden: Boolean? = null,
    val extra: Map<String, String> = emptyMap(),
) {
    val isEmpty: Boolean get() = this == EntryFields()

    /** Copy with [field] set to [value] (null clears it). Throws
     *  [IllegalArgumentException] for an unknown field or a value of
     *  the wrong type. */
    fun with(field: String, value: Any?): EntryFields {
        fun str() = value?.let { it as? String ?: bad(field, value) }
        fun bool() = value?.let { it as? Boolean ?: bad(field, value) }
        return when (field) {
            "name" -> copy(name = str())
            "comment" -> copy(comment = str())
            "exec" -> copy(exec = str())
            "terminal" -> copy(terminal = bool())
            "icon" -> copy(icon = str())
            "iconFile" -> copy(iconFile = str())
            "env" -> copy(env = value?.let { v ->
                (v as? Map<*, *> ?: bad(field, v)).entries.associate { (k, x) ->
                    (k as? String ?: bad(field, v)) to (x as? String ?: bad(field, v))
                }
            })
            "graphics" -> copy(graphics = str())
            "pointer" -> copy(pointer = str())
            "hidden" -> copy(hidden = bool())
            else -> throw IllegalArgumentException("unknown entry field '$field'")
        }
    }

    fun toJson(): JSONObject = JSONObject().apply {
        for ((k, v) in extra) put(k, JSONTokener(v).nextValue())
        name?.let { put("name", it) }
        comment?.let { put("comment", it) }
        exec?.let { put("exec", it) }
        terminal?.let { put("terminal", it) }
        icon?.let { put("icon", it) }
        iconFile?.let { put("iconFile", it) }
        env?.let { put("env", JSONObject(it.toSortedMap())) }
        graphics?.let { put("graphics", it) }
        pointer?.let { put("pointer", it) }
        hidden?.let { put("hidden", it) }
    }

    companion object {
        val FIELDS = listOf(
            "name", "comment", "exec", "terminal", "icon", "iconFile", "env", "graphics", "pointer", "hidden",
        )

        private fun bad(field: String, value: Any?): Nothing =
            throw IllegalArgumentException("bad value for '$field': $value")

        /** Wrongly typed known fields are dropped, not kept as extra. */
        fun fromJson(o: JSONObject): EntryFields {
            fun str(k: String) = o.opt(k) as? String
            fun bool(k: String) = o.opt(k) as? Boolean
            return EntryFields(
                name = str("name"),
                comment = str("comment"),
                exec = str("exec"),
                terminal = bool("terminal"),
                icon = str("icon"),
                iconFile = str("iconFile"),
                env = o.optJSONObject("env")?.let { e ->
                    buildMap { for (k in e.keys()) (e.opt(k) as? String)?.let { put(k, it) } }
                },
                graphics = str("graphics"),
                pointer = str("pointer"),
                hidden = bool("hidden"),
                extra = buildMap { for (k in o.keys()) if (k !in FIELDS) put(k, jsonText(o.get(k))) },
            )
        }

        /** A value from [JSONObject.get] as JSON text. */
        internal fun jsonText(v: Any): String = if (v is String) JSONObject.quote(v) else v.toString()
    }
}

/**
 * `<distros>/<id>/launcher/entries.json`: field [overrides] on scanned
 * entries and TAWC [shortcuts], by entry id. See notes/launcher.md
 * ("Entry store"). Immutable; changes go through [LauncherStore.update].
 */
data class LauncherEntries(
    val version: Int = VERSION,
    /** 1 once [LauncherStore]'s migration ran for this install. */
    val migrated: Int = 0,
    val overrides: Map<String, EntryFields> = emptyMap(),
    val shortcuts: Map<String, EntryFields> = emptyMap(),
    /** Unknown top-level keys, as JSON text. */
    val extra: Map<String, String> = emptyMap(),
) {
    /**
     * Set [field] of [id] to [value] (null clears it): in the shortcut
     * when [id] is one, else in its override. Built-ins take `hidden`
     * only. An override left empty is dropped.
     */
    fun withField(id: String, field: String, value: Any?): LauncherEntries {
        shortcuts[id]?.let { return copy(shortcuts = shortcuts + (id to it.with(field, value))) }
        require(LauncherEntry.Builtin.fromId(id) == null || field == "hidden") {
            "built-in '$id' takes hidden only"
        }
        return withOverride(id, (overrides[id] ?: EntryFields()).with(field, value))
    }

    fun withOverride(id: String, fields: EntryFields): LauncherEntries =
        copy(overrides = if (fields.isEmpty) overrides - id else overrides + (id to fields))

    /** Every override of [id] dropped except `hidden` (and fields this
     *  build doesn't know). */
    fun withoutOverrides(id: String): LauncherEntries {
        val old = overrides[id] ?: return this
        return withOverride(id, EntryFields(hidden = old.hidden, extra = old.extra))
    }

    fun withShortcut(id: String, fields: EntryFields) = copy(shortcuts = shortcuts + (id to fields))

    fun withoutShortcut(id: String) = copy(shortcuts = shortcuts - id)

    /** A free `tawc:app:<slug>` id for a shortcut named [name]. */
    fun newShortcutId(name: String): String {
        val slug = Installation.slugifyLabel(name) ?: "app"
        var id = "$SHORTCUT_PREFIX$slug"
        var n = 2
        while (id in shortcuts) id = "$SHORTCUT_PREFIX$slug-${n++}"
        return id
    }

    fun isHidden(id: String): Boolean = (shortcuts[id] ?: overrides[id])?.hidden == true

    /** `iconFile` names some entry uses. */
    fun referencedIcons(): Set<String> =
        (overrides.values + shortcuts.values).mapNotNull { it.iconFile }.toSet()

    fun toJson(): String = JSONObject().apply {
        for ((k, v) in extra) put(k, JSONTokener(v).nextValue())
        put("version", version)
        put("migrated", migrated)
        put("overrides", JSONObject().apply { for ((k, v) in overrides.toSortedMap()) put(k, v.toJson()) })
        put("shortcuts", JSONObject().apply { for ((k, v) in shortcuts.toSortedMap()) put(k, v.toJson()) })
    }.toString(2)

    companion object {
        const val VERSION = 1
        const val SHORTCUT_PREFIX = "tawc:app:"
        private val KEYS = setOf("version", "migrated", "overrides", "shortcuts")

        /** Throws [IllegalArgumentException] for a newer [version] or
         *  text that isn't a JSON object. */
        fun parse(text: String): LauncherEntries {
            val o = try {
                JSONObject(text)
            } catch (e: org.json.JSONException) {
                throw IllegalArgumentException("bad entries.json: ${e.message}")
            }
            val version = o.optInt("version", VERSION)
            require(version <= VERSION) {
                "entries.json version $version is newer than supported $VERSION (written by a newer app version?)"
            }
            fun map(key: String): Map<String, EntryFields> = o.optJSONObject(key)?.let { m ->
                buildMap { for (k in m.keys()) m.optJSONObject(k)?.let { put(k, EntryFields.fromJson(it)) } }
            } ?: emptyMap()
            return LauncherEntries(
                version = version,
                migrated = o.optInt("migrated", 0),
                overrides = map("overrides"),
                shortcuts = map("shortcuts"),
                extra = buildMap {
                    for (k in o.keys()) if (k !in KEYS) put(k, EntryFields.jsonText(o.get(k)))
                },
            )
        }
    }
}

/**
 * Owner of every write to an install's launcher store,
 * `<distros>/<id>/launcher/` (`entries.json` + `icons/`): an app-owned
 * sibling of the rootfs, so it works for every method, travels with
 * exports and goes away on uninstall. Writes are an atomic rename,
 * read-modify-write under a per-install lock ([update]), like
 * [InstallationStore.update]. The Rust scan only reads it.
 *
 * The first [load] of an install runs [LauncherMigration] (metadata
 * fields and TAWC-written `.desktop` files from older versions).
 */
class LauncherStore internal constructor(
    private val installDir: (String) -> File,
    private val legacy: LauncherMigration.Legacy,
    private val scanShadows: (rootfs: File) -> Map<String, String>,
    private val rasterize: (src: File, dst: File, px: Int) -> Boolean,
) {
    constructor(context: Context) : this(InstallationStore(context))

    constructor(installs: InstallationStore) : this(
        installs::installationDir,
        LauncherMigration.InstallationLegacy(installs),
        ::scanShadows,
        ::nativeRasterize,
    )

    fun dir(id: String) = File(installDir(id), "launcher")
    fun file(id: String) = File(dir(id), "entries.json")
    fun iconsDir(id: String) = File(dir(id), "icons")
    fun rootfs(id: String) = File(installDir(id), "rootfs")

    /**
     * [id]'s store, migrated. Null when the slot has no metadata (gone,
     * or mid-uninstall) or the file is unreadable or from a newer app.
     */
    fun load(id: String): LauncherEntries? = synchronized(lockFor(id)) {
        try {
            loadLocked(id)
        } catch (e: Exception) {
            Log.w(TAG, "launcher store for '$id': $e")
            null
        }
    }

    /**
     * Read-modify-write [id]'s store. With [icon] (PNG bytes, name slug)
     * the icon is added to `icons/` first and its file name passed to
     * [mutate]. [mutate] returns the store to write, or null to abort.
     * Icon files no entry references afterwards are deleted. Returns the
     * written store, or null when nothing was written. Throws
     * [IOException] on a failed write and [IllegalArgumentException]
     * from [mutate].
     */
    fun update(
        id: String,
        icon: Pair<ByteArray, String>? = null,
        mutate: (LauncherEntries, iconFile: String?) -> LauncherEntries?,
    ): LauncherEntries? = synchronized(lockFor(id)) {
        val cur = try {
            loadLocked(id)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "launcher store for '$id': $e")
            null
        } ?: return null
        val iconFile = icon?.let { (bytes, slug) -> addIcon(id, bytes, slug) }
        val next = mutate(cur, iconFile) ?: return null
        save(id, next)
        next
    }

    private fun loadLocked(id: String): LauncherEntries? {
        if (!legacy.exists(id)) return null
        val f = file(id)
        val cur = if (f.exists()) LauncherEntries.parse(f.readText()) else LauncherEntries()
        if (cur.migrated >= 1) return cur
        return LauncherMigration.run(this, id, cur, legacy, scanShadows, rasterize)
    }

    /** Write [entries] and prune unreferenced icons. Caller holds the lock. */
    internal fun save(id: String, entries: LauncherEntries) {
        dir(id).mkdirs()
        atomicWriteText(file(id), entries.toJson())
        val keep = entries.referencedIcons()
        iconsDir(id).listFiles()?.forEach { if (it.name !in keep) it.delete() }
    }

    /** Store [bytes] (a PNG) as `<slug>.png`, `-2`, … reusing a file
     *  with identical bytes. Returns the file name. */
    internal fun addIcon(id: String, bytes: ByteArray, slug: String): String {
        val dir = iconsDir(id).apply { mkdirs() }
        val name = IconImport.chooseName(slug, bytes) { File(dir, it).takeIf { f -> f.isFile }?.readBytes() }
        val f = File(dir, name)
        if (!f.isFile) atomicWriteBytes(f, bytes)
        return name
    }

    companion object {
        private const val TAG = "tawc"

        /** Process-global, like [InstallationStore]'s: instances are
         *  made ad hoc. */
        private val locks = java.util.concurrent.ConcurrentHashMap<String, Any>()

        private fun lockFor(id: String): Any = locks.computeIfAbsent(id) { Any() }

        /** Winning entry path → the copy it shadows, from a scan. */
        private fun scanShadows(rootfs: File): Map<String, String> =
            LauncherEntry.scan(rootfs.path).associate { it.path to it.shadows }

        private fun nativeRasterize(src: File, dst: File, px: Int): Boolean =
            runCatching { NativeBridge.nativeRasterizeIcon(src.path, dst.path, px) }.getOrDefault(false)
    }
}
