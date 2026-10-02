package me.phie.tawc.install

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Metadata parsing for [Installation.mirrorRegion]: legacy records have
 * no region (the distro's built-in mirror list), the field round-trips,
 * and it is only serialized when set so untouched records keep their
 * byte-identical JSON shape.
 */
class InstallationMirrorRegionTest {

    private fun minimalRecord(extra: String = ""): String = """
        {
          "id": "arch",
          "arch": "arm64-v8a"
          $extra
        }
    """.trimIndent()

    @Test
    fun legacyRecordHasNoRegion() {
        assertNull(Installation.fromJson(minimalRecord()).mirrorRegion)
    }

    @Test
    fun regionRoundTripsThroughJson() {
        val picked = Installation.fromJson(minimalRecord()).copy(mirrorRegion = "cn")
        assertEquals("cn", Installation.fromJson(picked.toJson()).mirrorRegion)
    }

    @Test
    fun clearingTheRegionDropsTheField() {
        val picked = Installation.fromJson(minimalRecord()).copy(mirrorRegion = "cn")
        val cleared = Installation.fromJson(picked.toJson()).copy(mirrorRegion = null)
        assertNull(Installation.fromJson(cleared.toJson()).mirrorRegion)
        assertTrue(!cleared.toJson().contains("mirrorRegion"))
    }

    @Test
    fun unsetRegionIsOmittedFromJson() {
        assertTrue(!Installation.fromJson(minimalRecord()).toJson().contains("mirrorRegion"))
    }

    @Test
    fun explicitNullParsesAsUnset() {
        val inst = Installation.fromJson(minimalRecord(""", "mirrorRegion": null"""))
        assertNull(inst.mirrorRegion)
    }
}
