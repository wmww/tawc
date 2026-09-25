package me.phie.tawc

import me.phie.tawc.install.Installation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Fallback order of [OpenDistro.pick]. */
class OpenDistroTest {

    private fun inst(id: String, state: Installation.State = Installation.State.READY) =
        Installation(
            id = id, distro = "arch", arch = "arm64-v8a", method = "tawcroot",
            installedAtMillis = 0, sourceUrl = "", state = state,
        )

    @Test
    fun emptyIsNull() {
        assertNull(OpenDistro.pick(null, emptyList()))
        assertNull(OpenDistro.pick("gone", emptyList()))
    }

    @Test
    fun storedIdWinsEvenWhenNotReady() {
        val list = listOf(inst("a"), inst("b", Installation.State.INSTALLING))
        assertEquals("b", OpenDistro.pick("b", list)?.id)
    }

    @Test
    fun staleIdFallsBackToFirstReady() {
        val list = listOf(inst("a", Installation.State.FAILED), inst("b"), inst("c"))
        assertEquals("b", OpenDistro.pick("gone", list)?.id)
        assertEquals("b", OpenDistro.pick(null, list)?.id)
    }

    @Test
    fun noReadyFallsBackToFirst() {
        val list = listOf(
            inst("a", Installation.State.INSTALLING),
            inst("b", Installation.State.FAILED),
        )
        assertEquals("a", OpenDistro.pick("gone", list)?.id)
    }
}
