package me.phie.tawc.install

import org.json.JSONObject
import java.io.File

/**
 * Persistent metadata for a single installed Linux environment. Stored as
 * `metadata.json` next to the rootfs in
 * `<app data>/distros/<id>/`.
 *
 * The presence of this file plus its [state] field together encode the
 * installation state machine documented in `notes/installation.md`. The
 * file is written by [InstallationStore] alone; every other piece of
 * the system reads it but never writes.
 */
data class Installation(
    val id: String,
    val distro: String,
    val arch: String,
    val method: String,
    val installedAtMillis: Long,
    val sourceUrl: String,
    val state: State = State.READY,
    val failure: String? = null,
    val schemaVersion: Int = CURRENT_SCHEMA_VERSION,
    val installedAtAppVersionCode: Long = 0L,
    /**
     * Free-form display name set by the user at install time. The id
     * (folder name on disk) is derived from the label by slugifying;
     * the label itself is what every UI surface renders. Null on legacy
     * records that predate this field — callers fall back to the
     * registry-resolved displayName so old installs still read sensibly.
     */
    val label: String? = null,
) {
    fun rootfsDir(store: InstallationStore): File = store.rootfsDir(id)
    fun metadataFile(store: InstallationStore): File = store.metadataFile(id)

    fun toJson(): String = JSONObject().apply {
        put("schemaVersion", schemaVersion)
        put("id", id)
        put("distro", distro)
        put("arch", arch)
        put("method", method)
        put("installedAtMillis", installedAtMillis)
        put("installedAtAppVersionCode", installedAtAppVersionCode)
        put("sourceUrl", sourceUrl)
        put("state", state.name)
        if (failure != null) put("failure", failure)
        if (label != null) put("label", label)
    }.toString(2)

    /**
     * Lifecycle of one installation slot. See `notes/installation.md`
     * for the full transition table; the gate that enforces it lives in
     * [InstallationService]. `READY` is the default for missing-field
     * legacy metadata so pre-state-machine installs aren't lost.
     */
    enum class State { INSTALLING, READY, UNINSTALLING, FAILED }

    companion object {
        const val DISTRO_ARCH = "arch"
        const val DISTRO_MANJARO = "manjaro"
        const val DISTRO_VOID = "void"
        // Kept as constants for the metadata schema; the runtime
        // mapping to InstallationMethod implementations lives in
        // [InstallationMethod.forKey] and the impl objects' KEY fields.
        const val METHOD_CHROOT = "chroot"
        const val METHOD_PROOT = "proot"

        // Allowlist for the id component of `<app data>/distros/<id>/`.
        // The id flows into shell scripts (via `installDir.absolutePath`)
        // that are run as root on the chroot path, so anything outside
        // [a-z0-9_-] would be a path-traversal / shell-metachar foothold
        // (InstallActivity is exported and any installed app can launch
        // it with a hostile `--es id` extra). 32 chars is plenty for
        // foreseeable use; tighten further later if we ever care.
        private val ID_PATTERN = Regex("^[a-z0-9][a-z0-9_-]{0,31}$")

        fun isValidId(id: String): Boolean = ID_PATTERN.matches(id)

        /**
         * Reduce [label] to an [isValidId]-compatible slug. Lowercases,
         * collapses every run of non-`[a-z0-9_-]` into a single `-`,
         * trims leading/trailing `-` so the result starts with an
         * alphanumeric, and clamps to 32 chars. Returns `null` if the
         * cleaned slug is empty (label was whitespace-only or
         * punctuation-only); callers should treat that as "label is
         * not yet usable" in form validation.
         */
        fun slugifyLabel(label: String): String? {
            val cleaned = buildString {
                var lastDash = false
                for (c in label.lowercase()) {
                    val ok = (c in 'a'..'z') || (c in '0'..'9') || c == '_' || c == '-'
                    if (ok) {
                        append(c)
                        lastDash = (c == '-')
                    } else if (!lastDash && isNotEmpty()) {
                        append('-')
                        lastDash = true
                    }
                }
            }.trimEnd('-').take(32)
            return cleaned.takeIf { it.isNotEmpty() && isValidId(it) }
        }

        // Bump when adding a field that downstream code can't safely
        // default. Pure additive fields with safe defaults don't need a
        // bump — fromJson tolerates them. See notes/installation.md
        // "Upgrade policy" for what changes warrant a bump and how to
        // handle one.
        const val CURRENT_SCHEMA_VERSION = 1

        fun fromJson(text: String): Installation {
            val obj = JSONObject(text)
            // distro / method default to the historical-only values
            // ("arch" / "chroot") so any pre-distro-field record loads
            // without a hard error. arch has no sensible default; if
            // it's missing the record is too broken to use.
            // schemaVersion defaults to 1 (the only version that exists
            // today); installedAtAppVersionCode defaults to 0 for pre-
            // version-tracking records, which downstream "if older than
            // X" checks will read as "very old, treat conservatively".
            return Installation(
                id = obj.getString("id"),
                distro = obj.optString("distro", DISTRO_ARCH),
                arch = obj.getString("arch"),
                method = obj.optString("method", METHOD_CHROOT),
                installedAtMillis = obj.optLong("installedAtMillis", 0L),
                sourceUrl = obj.optString("sourceUrl", ""),
                state = obj.optString("state", State.READY.name)
                    .let { runCatching { State.valueOf(it) }.getOrDefault(State.READY) },
                failure = if (obj.has("failure") && !obj.isNull("failure")) obj.getString("failure") else null,
                schemaVersion = obj.optInt("schemaVersion", 1),
                installedAtAppVersionCode = obj.optLong("installedAtAppVersionCode", 0L),
                label = if (obj.has("label") && !obj.isNull("label")) obj.getString("label") else null,
            )
        }
    }
}
