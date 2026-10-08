package me.phie.tawc.launcher

import android.content.Context
import me.phie.tawc.R
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.TawcrootMethod
import org.json.JSONArray

/**
 * One launchable Linux application discovered inside a chroot rootfs.
 * Mirrors the JSON shape returned by [NativeBridge.nativeLauncherScan] —
 * the Rust scanner is the source of truth for what counts as launchable
 * (Type=Application, not NoDisplay/Hidden, has Exec). [builtin] entries
 * are the app's own, added Kotlin-side ([withBuiltins]).
 */
data class LauncherEntry(
    /** Filename minus `.desktop`, used as a stable id. */
    val id: String,
    val name: String,
    val comment: String,
    /** Exec line with field codes (`%f`, `%u`, …) already stripped. */
    val exec: String,
    val terminal: Boolean,
    /**
     * Absolute path to a PNG icon file inside the rootfs, or empty if
     * none was findable. The Rust scanner only ever returns PNGs (Android
     * can't decode SVG natively); SVG-only icons end up empty here and
     * the row renders without an icon.
     */
    val iconPath: String,
    /**
     * Absolute host path of the `.desktop` file this entry was parsed
     * from. Distinguishes managed (user-editable) entries from distro
     * ones; empty only for malformed scanner output.
     */
    val path: String = "",
    /**
     * Path of the lower-priority copy of this id the scan hid (the
     * packaged file a managed-dir override shadows), or empty. A
     * managed entry with [shadows] is an override (the editor offers
     * Reset); without, a personal entry (Delete).
     */
    val shadows: String = "",
    val builtin: Builtin? = null,
) {
    /**
     * App-provided entries. Ids carry a `tawc:` prefix no scanned id is
     * allowed to share ([withBuiltins]), so hide state and pinned
     * shortcut ids work unchanged.
     */
    enum class Builtin(val id: String, val nameRes: Int, val iconRes: Int) {
        /** New shell tab. */
        TERM("tawc:term", R.string.builtin_term, R.drawable.ic_terminal),
        /** New command tab running the distro's upgrade command. */
        UPDATE("tawc:update", R.string.builtin_update, R.drawable.ic_update),
        /** The `.desktop` editor; always sorts last. */
        ADD_ENTRY("tawc:add-entry", R.string.builtin_add_entry, R.drawable.ic_add_entry);

        /** Opens a terminal tab, so tawcroot-only and pinnable. */
        val opensTerminal: Boolean get() = this != ADD_ENTRY

        companion object {
            fun fromId(id: String): Builtin? = entries.firstOrNull { it.id == id }
        }
    }

    companion object {
        private const val BUILTIN_PREFIX = "tawc:"

        fun builtin(context: Context, kind: Builtin): LauncherEntry =
            LauncherEntry(
                id = kind.id,
                name = context.getString(kind.nameRes),
                comment = "",
                exec = "",
                terminal = kind.opensTerminal,
                iconPath = "",
                builtin = kind,
            )

        /** The built-ins [inst] offers: terminal ones on tawcroot,
         *  Add entry where the editor can write (not chroot). */
        fun builtinsFor(context: Context, inst: Installation): List<LauncherEntry> =
            Builtin.entries
                .filter {
                    if (it.opensTerminal) inst.method == TawcrootMethod.KEY
                    else inst.method != Installation.METHOD_CHROOT
                }
                .map { builtin(context, it) }

        /**
         * [scan] plus [builtinsFor]: what the apps list shows before
         * filtering. Blocking, like [scan].
         */
        fun list(context: Context, inst: Installation, rootfs: String): List<LauncherEntry> =
            withBuiltins(scan(rootfs), builtinsFor(context, inst))

        /**
         * Merge [builtins] into the name-sorted [scanned] list in the
         * scanner's order (lowercased name, then id). Scanned ids with
         * the reserved prefix are dropped.
         */
        fun withBuiltins(scanned: List<LauncherEntry>, builtins: List<LauncherEntry>): List<LauncherEntry> =
            (scanned.filter { !it.id.startsWith(BUILTIN_PREFIX) } + builtins)
                .sortedWith(compareBy<LauncherEntry> { it.name.lowercase() }.thenBy { it.id })


        /**
         * Scan [rootfs] with the Rust scanner and parse the result — the
         * one entry point every consumer (launcher list, shortcut
         * trampoline, debug broker) goes through. A native failure yields
         * an empty list, same as "no apps". Blocking file I/O; call on
         * [kotlinx.coroutines.Dispatchers.IO] from UI code.
         */
        fun scan(rootfs: String): List<LauncherEntry> =
            parseList(runCatching { NativeBridge.nativeLauncherScan(rootfs) }.getOrNull())

        /**
         * Hidden-state + search filtering, the pure core of the launcher's
         * list state. Entries with an id in [hiddenIds] are dropped unless
         * [showHidden], or a query matches only hidden apps (so searching
         * a hidden app still finds it). [query] is a case-insensitive substring match
         * against name + id + comment; name-prefix matches sort first (so
         * typing "fire" surfaces Firefox above "WireFire"), everything
         * else keeps the scanner's name order. Add entry always goes
         * last.
         */
        fun filter(
            entries: List<LauncherEntry>,
            hiddenIds: Set<String>,
            showHidden: Boolean,
            query: String,
        ): List<LauncherEntry> {
            val q = query.trim().lowercase()
            val all = if (q.isEmpty()) entries else match(entries, q)
            val unhidden = all.filter { it.id !in hiddenIds }
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
                            )
                        )
                    }
                }
            }.getOrDefault(emptyList())
        }
    }
}
