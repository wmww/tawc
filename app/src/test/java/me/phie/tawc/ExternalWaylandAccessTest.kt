package me.phie.tawc

import me.phie.tawc.compositor.ExternalWaylandAccess
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Access matrix for [ExternalWaylandAccess] (external Wayland clients). */
class ExternalWaylandAccessTest {

    private val own = 10100

    private fun decide(
        uid: Int,
        packages: List<String>,
        mode: ExternalAccessMode,
        allowed: Set<String> = emptySet(),
    ) = ExternalWaylandAccess.decide(uid, packages, own, mode, allowed)

    @Test
    fun ownUidAlwaysAllowed() {
        for (mode in ExternalAccessMode.entries) {
            assertTrue(
                "mode $mode",
                decide(own, listOf("me.phie.tawc"), mode).allowed,
            )
        }
    }

    @Test
    fun disabledRejectsEveryoneElse() {
        assertFalse(decide(10549, listOf("com.termux"), ExternalAccessMode.DISABLED).allowed)
        assertFalse(
            decide(10549, listOf("com.termux"), ExternalAccessMode.DISABLED, setOf("com.termux")).allowed
        )
    }

    @Test
    fun anyAcceptsAnyResolvablePackage() {
        assertTrue(decide(10549, listOf("com.termux"), ExternalAccessMode.ANY).allowed)
        assertTrue(decide(10234, listOf("org.example.app"), ExternalAccessMode.ANY).allowed)
    }

    @Test
    fun anyRejectsUnresolvableUid() {
        // An empty package list is not "any app"; it is "we don't know".
        assertFalse(decide(10549, emptyList(), ExternalAccessMode.ANY).allowed)
    }

    @Test
    fun allowlistAcceptsListedPackage() {
        assertTrue(
            decide(10549, listOf("com.termux"), ExternalAccessMode.ALLOWLIST, setOf("com.termux")).allowed
        )
    }

    @Test
    fun allowlistRejectsUnlistedPackage() {
        assertFalse(
            decide(10234, listOf("org.example.app"), ExternalAccessMode.ALLOWLIST, setOf("com.termux")).allowed
        )
    }

    @Test
    fun allowlistEmptyRejectsEverything() {
        assertFalse(
            decide(10549, listOf("com.termux"), ExternalAccessMode.ALLOWLIST, emptySet()).allowed
        )
    }

    @Test
    fun sharedUidMatchesOnAnyPackage() {
        // Two packages share one uid; only one is allowlisted.
        assertTrue(
            decide(
                10549,
                listOf("com.termux", "com.termux.api"),
                ExternalAccessMode.ALLOWLIST,
                setOf("com.termux.api"),
            ).allowed
        )
    }

    @Test
    fun defaultModeKeepsTermuxWorking() {
        // The upgrade path: a user who never opens settings still gets
        // the Termux launcher accepted.
        assertEquals(ExternalAccessMode.ALLOWLIST, ExternalAccessMode.DEFAULT)
        assertTrue(Settings.DEFAULT_EXTERNAL_ALLOWED_PACKAGES.contains("com.termux"))
        assertTrue(
            decide(
                10549,
                listOf("com.termux"),
                ExternalAccessMode.DEFAULT,
                Settings.DEFAULT_EXTERNAL_ALLOWED_PACKAGES,
            ).allowed
        )
    }

    @Test
    fun modeKeysRoundTrip() {
        for (mode in ExternalAccessMode.entries) {
            assertEquals(mode, ExternalAccessMode.fromKeyOrNull(mode.key))
        }
        assertEquals(null, ExternalAccessMode.fromKeyOrNull("nonsense"))
        assertEquals(null, ExternalAccessMode.fromKeyOrNull(null))
    }

    @Test
    fun candidatesIncludeLaunchableAndAlreadyAllowed() {
        val list = ExternalWaylandAccess.allowlistCandidates(
            launchable = listOf("org.b", "org.a"),
            alreadyAllowed = listOf("com.termux", "org.a"),
        )
        assertEquals(listOf("com.termux", "org.a", "org.b"), list)
    }
}
