package me.phie.tawc.install

import org.json.JSONArray
import org.json.JSONObject

/**
 * One user-configured bind of a host directory into a rootfs, persisted
 * in [Installation.externalBinds]. The canonical use is exposing shared
 * storage (`/storage/emulated/0`) inside the rootfs so files survive
 * uninstall and are visible to other Android apps — see
 * notes/external-binds.md.
 *
 * Consumed by [TawcrootMethod.bindSpecs] only; the chroot/proot debug
 * methods ignore the list (chroot uses real kernel mounts whose
 * uninstall interaction hasn't been reviewed).
 *
 * No `writable` flag yet, but tawcroot now supports it: `-b
 * src:dst:ro` marks a bind read-only (notes/tawcroot/
 * path-translation.md §"Read-only binds"). Add the flag here + the
 * Manage-binds UI toggle when a workload wants it; bindSpecs then
 * emits the `:ro` suffix.
 */
data class ExternalBind(
    /** Absolute host directory, e.g. `/storage/emulated/0`. Picked by
     * the user (or a common-dir suggestion); never auto-created. */
    val hostPath: String,
    /** Absolute in-rootfs path the host dir appears at. Pre-created
     * before each spawn. */
    val guestPath: String,
) {
    fun toJson(): JSONObject = JSONObject().apply {
        // "kind" reserves room for non-path bind sources later;
        // fromJsonArray skips kinds it doesn't know.
        put("kind", KIND_PATH)
        put("hostPath", hostPath)
        put("guestPath", guestPath)
    }

    /**
     * Structural validity check shared by every surface that accepts a
     * bind (manage-binds dialog, install service, spawn path). Returns
     * a human-readable problem or null when the shape is fine. Runtime
     * access checks (host dir exists, all-files access granted) are
     * separate — see [TawcrootMethod].
     *
     * The `:` rejection is load-bearing: binds travel to tawcroot as
     * `-b src:dst` argv entries, so a colon in either path would split
     * wrong.
     */
    fun validationError(): String? {
        for ((name, path) in listOf("host path" to hostPath, "guest path" to guestPath)) {
            if (!path.startsWith("/")) return "$name '$path' must be absolute"
            if (path.contains(":")) return "$name '$path' must not contain ':'"
            if (path.any { it < ' ' }) return "$name must not contain control characters"
            if (path.splitToSequence('/').any { it == ".." }) {
                return "$name '$path' must not contain '..'"
            }
        }
        if (guestPath == "/") return "guest path must not be '/'"
        return null
    }

    companion object {
        const val KIND_PATH = "path"

        /**
         * Per-install cap, enforced by every accepting surface.
         * tawcroot's bind table holds `TAWCROOT_MAX_BINDS` (32) entries
         * and the built-in system/share set uses up to 10; past the
         * table limit tawcroot exits before the fail-closed Kotlin
         * checks can produce a readable error, so cap well below it.
         */
        const val MAX_BINDS = 16

        fun toJsonArray(binds: List<ExternalBind>): JSONArray =
            JSONArray().apply { for (b in binds) put(b.toJson()) }

        /** Parse a persisted bind list. Entries with an unknown `kind`
         * are skipped (forward compat — ditto unknown keys, e.g. the
         * retired `label`); malformed entries throw. */
        fun fromJsonArray(arr: JSONArray): List<ExternalBind> = buildList {
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                if (o.optString("kind", KIND_PATH) != KIND_PATH) continue
                add(ExternalBind(
                    hostPath = o.getString("hostPath"),
                    guestPath = o.getString("guestPath"),
                ))
            }
        }
    }
}
