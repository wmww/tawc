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
) {
    fun rootfsDir(store: InstallationStore): File = store.rootfsDir(id)
    fun metadataFile(store: InstallationStore): File = store.metadataFile(id)

    fun toJson(): String = JSONObject().apply {
        put("id", id)
        put("distro", distro)
        put("arch", arch)
        put("method", method)
        put("installedAtMillis", installedAtMillis)
        put("sourceUrl", sourceUrl)
        put("state", state.name)
        if (failure != null) put("failure", failure)
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
        const val METHOD_CHROOT = "chroot"

        fun fromJson(text: String): Installation {
            val obj = JSONObject(text)
            // distro / method default to the historical-only values
            // ("arch" / "chroot") so any pre-distro-field record loads
            // without a hard error. arch has no sensible default; if
            // it's missing the record is too broken to use.
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
            )
        }
    }
}
