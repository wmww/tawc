package me.phie.tawc.launcher

import org.json.JSONArray

/**
 * One launchable Linux application discovered inside a chroot rootfs.
 * Mirrors the JSON shape returned by `NativeBridge.nativeLauncherScan` —
 * the Rust scanner is the source of truth for what counts as launchable
 * (Type=Application, not NoDisplay/Hidden, has Exec).
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
) {
    companion object {
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
                            )
                        )
                    }
                }
            }.getOrDefault(emptyList())
        }
    }
}
