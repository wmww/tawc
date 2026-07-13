package me.phie.tawc.install

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Forward-compat / corrupt-metadata parsing (notes/installation.md
 * "Schema versioning"): the schemaVersion gate, tolerant TawcInstall
 * parsing, and the CORRUPT marker [InstallationStore] surfaces when
 * [Installation.fromJson] throws.
 */
class InstallationCorruptMetadataTest {

    private fun minimalRecord(extra: String = ""): String = """
        {
          "id": "arch",
          "arch": "arm64-v8a"
          $extra
        }
    """.trimIndent()

    @Test
    fun currentSchemaVersionParses() {
        val inst = Installation.fromJson(
            minimalRecord(""", "schemaVersion": ${Installation.CURRENT_SCHEMA_VERSION}""")
        )
        assertEquals(Installation.CURRENT_SCHEMA_VERSION, inst.schemaVersion)
    }

    @Test
    fun newerSchemaVersionIsRefusedNotMisparsed() {
        val e = assertThrows(IllegalArgumentException::class.java) {
            Installation.fromJson(minimalRecord(""", "schemaVersion": 99"""))
        }
        // The message becomes the CORRUPT marker's failure detail shown
        // to the user; it should name the offending version.
        assertTrue(e.message!!.contains("99"))
    }

    @Test
    fun unknownStateDefaultsToReady() {
        val inst = Installation.fromJson(minimalRecord(""", "state": "SOME_FUTURE_STATE""""))
        assertEquals(Installation.State.READY, inst.state)
    }

    @Test
    fun unknownTawcInstallTypeSkipsEntryOnly() {
        val inst = Installation.fromJson(minimalRecord(""",
            "tawcInstalls": [
              {"src": "a", "dest": "/x/a", "type": "COPY"},
              {"src": "b", "dest": "/x/b", "type": "SOME_FUTURE_TYPE"},
              {"src": "c", "dest": "/x/c", "type": "LINK"}
            ]"""))
        assertEquals(listOf("a", "c"), inst.tawcInstalls.map { it.src })
    }

    @Test
    fun missingArchStillThrows() {
        // No sensible default exists; the record should surface as a
        // CORRUPT marker rather than a half-real installation.
        assertThrows(Exception::class.java) {
            Installation.fromJson("""{"id": "arch"}""")
        }
    }

    @Test
    fun corruptMarkerShape() {
        val m = Installation.corruptMarker("myslot", "boom")
        assertEquals(Installation.State.CORRUPT, m.state)
        assertEquals("myslot", m.id)
        assertEquals("myslot", m.label)
        assertEquals("boom", m.failure)
    }
}
