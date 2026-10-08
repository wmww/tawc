package me.phie.tawc.install

import me.phie.tawc.install.distro.DistroRegistry
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The user-facing support tier (`Distro.supported` → install form main
 * list vs "Other distros") is a deliberate decision, so pin it here:
 * adding or promoting a distro should be a visible test change, not a
 * one-line flip nobody notices.
 *
 * Arch x86_64 is supported only because it is the emulator's stand-in
 * for ALARM; Ubuntu x86_64 ships for the same reason.
 */
class DistroSupportTierTest {
    private fun tier(supported: Boolean): Set<Pair<String, String>> =
        DistroRegistry.all.filter { it.supported == supported }
            .map { it.key to it.androidAbi }
            .toSet()

    @Test
    fun `supported tier is ALARM, Debian sid and Ubuntu 24_04`() {
        assertEquals(
            setOf(
                "arch" to "arm64-v8a",
                "arch" to "x86_64",
                "debian-sid" to "arm64-v8a",
                "debian-sid" to "x86_64",
                "ubuntu" to "arm64-v8a",
                "ubuntu" to "x86_64",
            ),
            tier(supported = true),
        )
    }

    @Test
    fun `manjaro and void still ship as dev-only`() {
        assertEquals(
            setOf(
                "manjaro" to "arm64-v8a",
                "void" to "arm64-v8a",
                "void" to "x86_64",
            ),
            tier(supported = false),
        )
    }
}
