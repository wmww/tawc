package me.phie.tawc.launcher

import android.content.Context
import me.phie.tawc.R
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.TawcrootMethod
import org.json.JSONArray
import org.json.JSONObject

/**
 * One launchable entry of a distro: a scanned `.desktop` file with the
 * [LauncherStore]'s field overrides applied, a store shortcut, or one
 * of the app's [builtin]s (added Kotlin-side, [withBuiltins]). Mirrors
 * the JSON shape returned by [NativeBridge.nativeLauncherScan] — the
 * Rust scanner is the source of truth for what counts as launchable
 * (Type=Application, not NoDisplay/Hidden, has Exec) and does the merge.
 */
data class LauncherEntry(
    /** Filename minus `.desktop`, or the shortcut's store key. */
    val id: String,
    val name: String,
    val comment: String,
    /** Exec line; a scanned one has field codes (`%f`, `%u`, …) stripped. */
    val exec: String,
    val terminal: Boolean,
    /**
     * Absolute path to a PNG icon file inside the rootfs, or empty if
     * none was findable. The Rust scanner only ever returns PNGs (Android
     * can't decode SVG natively); SVG-only icons end up empty here and
     * the row renders without an icon.
     */
    val iconPath: String,
    /** Absolute host path of the `.desktop` file; empty for shortcuts
     *  and built-ins. */
    val path: String = "",
    /** Path of the lower-priority copy of this id the scan hid, or empty. */
    val shadows: String = "",
    /** Effective `Icon=` value (theme name or in-rootfs path). */
    val icon: String = "",
    /** Effective store icon file name (`launcher/icons/`), or empty. */
    val iconFile: String = "",
    val source: Source = Source.DESKTOP,
    /** Store fields applied over the scanned file (`hidden` excluded). */
    val overridden: Set<String> = emptySet(),
    /** Scanned value of each overridden `name`/`comment`/`exec`/`icon`
     *  (and `terminal`, as `"true"`/`"false"`). */
    val packaged: Map<String, String> = emptyMap(),
    val hidden: Boolean = false,
    /** Exported before [exec] at launch. */
    val env: Map<String, String> = emptyMap(),
    /** [me.phie.tawc.GraphicsBackend.key] override, or null. */
    val graphics: String? = null,
    /** [me.phie.tawc.PointerEmulation.key] override, or null. */
    val pointer: String? = null,
    val builtin: Builtin? = null,
) {
    enum class Source { DESKTOP, SHORTCUT, BUILTIN }

    /** The scanned value of [field]: its [packaged] value when
     *  overridden, else the effective one. */
    fun packagedValue(field: String): String = packaged[field] ?: when (field) {
        "name" -> name
        "comment" -> comment
        "exec" -> exec
        "icon" -> icon
        "terminal" -> terminal.toString()
        else -> ""
    }

    /**
     * App-provided entries. Ids carry a `tawc:` prefix no scanned id is
     * allowed to share (the scanner drops them), so hide state and
     * pinned shortcut ids work unchanged.
     */
    enum class Builtin(val id: String, val nameRes: Int, val iconRes: Int) {
        /** New shell tab. */
        TERM("tawc:term", R.string.builtin_term, R.drawable.ic_terminal),
        /** New command tab running the distro's upgrade command. */
        UPDATE("tawc:update", R.string.builtin_update, R.drawable.ic_update),
        /** The entry editor, for a new shortcut; always sorts last. */
        ADD_ENTRY("tawc:add-entry", R.string.builtin_add_entry, R.drawable.ic_add_entry);

        /** Opens a terminal tab, so tawcroot-only and pinnable. */
        val opensTerminal: Boolean get() = this != ADD_ENTRY

        companion object {
            fun fromId(id: String): Builtin? = entries.firstOrNull { it.id == id }
        }
    }

    companion object {
        fun builtin(context: Context, kind: Builtin, hidden: Boolean = false): LauncherEntry =
            LauncherEntry(
                id = kind.id,
                name = context.getString(kind.nameRes),
                comment = "",
                exec = "",
                terminal = kind.opensTerminal,
                iconPath = "",
                source = Source.BUILTIN,
                hidden = hidden,
                builtin = kind,
            )

        /** The built-ins [inst] offers (terminal ones on tawcroot only),
         *  with their hide state from [store]. */
        fun builtinsFor(context: Context, inst: Installation, store: LauncherEntries?): List<LauncherEntry> =
            Builtin.entries
                .filter { !it.opensTerminal || inst.method == TawcrootMethod.KEY }
                .map { builtin(context, it, store?.isHidden(it.id) == true) }

        /**
         * The store (migrated first, see [LauncherStore.load]) merged
         * into a scan, plus [builtinsFor]: what the apps list shows
         * before filtering. Blocking, like [scan].
         */
        fun list(context: Context, inst: Installation): List<LauncherEntry> {
            val launcher = LauncherStore(context)
            val store = launcher.load(inst.id)
            return withBuiltins(scan(launcher.rootfs(inst.id).path), builtinsFor(context, inst, store))
        }

        /** The merged entry [id] of [inst] (built-ins excluded), or null.
         *  Migrates the store first. Blocking. */
        fun find(context: Context, inst: Installation, id: String): LauncherEntry? {
            val launcher = LauncherStore(context)
            launcher.load(inst.id)
            return scan(launcher.rootfs(inst.id).path).firstOrNull { it.id == id }
        }

        /** Merge [builtins] into the name-sorted [scanned] list in the
         *  scanner's order (lowercased name, then id). */
        fun withBuiltins(scanned: List<LauncherEntry>, builtins: List<LauncherEntry>): List<LauncherEntry> =
            (scanned + builtins).sortedWith(compareBy<LauncherEntry> { it.name.lowercase() }.thenBy { it.id })

        /**
         * Scan [rootfs] with the Rust scanner (which merges the store
         * beside it) and parse the result. A native failure yields an
         * empty list, same as "no apps". Does not migrate the store:
         * consumers go through [list] or [find]. Blocking file I/O;
         * call on [kotlinx.coroutines.Dispatchers.IO] from UI code.
         */
        fun scan(rootfs: String): List<LauncherEntry> =
            parseList(runCatching { NativeBridge.nativeLauncherScan(rootfs) }.getOrNull())

        /**
         * Hidden-state + search filtering, the pure core of the launcher's
         * list state. [hidden] entries are dropped unless [showHidden], or
         * a query matches only hidden apps (so searching a hidden app
         * still finds it). [query] is a case-insensitive substring match
         * against name + id + comment; name-prefix matches sort first (so
         * typing "fire" surfaces Firefox above "WireFire"), everything
         * else keeps the scanner's name order. Add entry always goes
         * last.
         */
        fun filter(
            entries: List<LauncherEntry>,
            showHidden: Boolean,
            query: String,
        ): List<LauncherEntry> {
            val q = query.trim().lowercase()
            val all = if (q.isEmpty()) entries else match(entries, q)
            val unhidden = all.filter { !it.hidden }
            val matches = when {
                showHidden -> all
                q.isNotEmpty() && unhidden.all { it.builtin == Builtin.ADD_ENTRY } -> all
                else -> unhidden
            }
            val (add, rest) = matches.partition { it.builtin == Builtin.ADD_ENTRY }
            return rest + add
        }

        private fun match(visible: List<LauncherEntry>, q: String): List<LauncherEntry> {
            val prefix = ArrayList<LauncherEntry>()
            val other = ArrayList<LauncherEntry>()
            for (e in visible) {
                val n = e.name.lowercase()
                if (n.startsWith(q)) prefix.add(e)
                else if (n.contains(q) || e.id.lowercase().contains(q) ||
                    e.comment.lowercase().contains(q)) other.add(e)
            }
            return prefix + other
        }

        fun parseList(json: String?): List<LauncherEntry> {
            if (json.isNullOrBlank()) return emptyList()
            return runCatching {
                val arr = JSONArray(json)
                buildList(arr.length()) {
                    for (i in 0 until arr.length()) {
                        val o = arr.getJSONObject(i)
                        add(
                            LauncherEntry(
                                id = o.optString("id"),
                                name = o.optString("name"),
                                comment = o.optString("comment"),
                                exec = o.optString("exec"),
                                terminal = o.optBoolean("terminal", false),
                                iconPath = o.optString("iconPath"),
                                path = o.optString("path"),
                                shadows = o.optString("shadows"),
                                icon = o.optString("icon"),
                                iconFile = o.optString("iconFile"),
                                source = if (o.optString("source") == "shortcut") Source.SHORTCUT else Source.DESKTOP,
                                overridden = o.optJSONArray("overridden")?.let { a ->
                                    (0 until a.length()).map { a.getString(it) }.toSet()
                                } ?: emptySet(),
                                packaged = strings(o.optJSONObject("packaged")),
                                hidden = o.optBoolean("hidden", false),
                                env = strings(o.optJSONObject("env")),
                                graphics = o.optString("graphics").ifEmpty { null },
                                pointer = o.optString("pointer").ifEmpty { null },
                            )
                        )
                    }
                }
            }.getOrDefault(emptyList())
        }

        /** An object's values as strings (`true`/`false` for booleans). */
        private fun strings(o: JSONObject?): Map<String, String> =
            o?.let { m -> buildMap { for (k in m.keys()) put(k, m.get(k).toString()) } } ?: emptyMap()
    }
}
