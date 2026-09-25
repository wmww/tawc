package me.phie.tawc

import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore

/**
 * The one distro the home screen shows ([Settings.openDistroId]).
 * Screens call [resolve] on resume rather than caching it, so an
 * uninstall or a switch elsewhere is picked up on return.
 */
object OpenDistro {
    /**
     * The stored open install if it still exists, else the first READY
     * one, else the first of any state, else null. Writes the fallback
     * back so the choice sticks.
     */
    fun resolve(store: InstallationStore): Installation? = resolve(store.list())

    /** [resolve] against an already-listed [installs]. */
    fun resolve(installs: List<Installation>): Installation? {
        val stored = Settings.openDistroId
        val picked = pick(stored, installs)
        if (picked?.id != stored) Settings.openDistroId = picked?.id
        return picked
    }

    fun set(id: String) {
        Settings.openDistroId = id
    }

    /** Pure fallback order behind [resolve]. */
    fun pick(storedId: String?, installs: List<Installation>): Installation? =
        installs.firstOrNull { it.id == storedId }
            ?: installs.firstOrNull { it.state == Installation.State.READY }
            ?: installs.firstOrNull()
}
