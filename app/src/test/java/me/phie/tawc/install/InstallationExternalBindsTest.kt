package me.phie.tawc.install

import org.json.JSONArray
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Metadata parsing for [Installation.externalBinds] / [ExternalBind]:
 * legacy-record defaults, round-trip, forward compat, and the
 * structural validator every accepting surface shares.
 */
class InstallationExternalBindsTest {

    private fun minimalRecord(extra: String = ""): String = """
        {
          "id": "arch",
          "arch": "arm64-v8a"
          $extra
        }
    """.trimIndent()

    @Test
    fun legacyRecordParsesToNoBinds() {
        val inst = Installation.fromJson(minimalRecord())
        assertTrue(inst.externalBinds.isEmpty())
    }

    @Test
    fun bindsRoundTripThroughJson() {
        val binds = listOf(
            ExternalBind("/", "/android", readOnly = true),
            ExternalBind("/storage/emulated/0", "/home/android"),
        )
        val inst = Installation.fromJson(minimalRecord()).copy(externalBinds = binds)
        val reparsed = Installation.fromJson(inst.toJson())
        assertEquals(binds, reparsed.externalBinds)
    }

    @Test
    fun readOnlyParsesAndDefaultsFalse() {
        val parsed = ExternalBind.fromJsonArray(JSONArray("""
            [
              {"hostPath": "/", "guestPath": "/android", "readOnly": true},
              {"hostPath": "/a", "guestPath": "/b", "readOnly": false},
              {"hostPath": "/c", "guestPath": "/d"}
            ]
        """.trimIndent()))
        assertEquals(
            listOf(
                ExternalBind("/", "/android", readOnly = true),
                ExternalBind("/a", "/b", readOnly = false),
                ExternalBind("/c", "/d", readOnly = false),
            ),
            parsed,
        )
    }

    @Test
    fun emptyBindsListOmittedFromJson() {
        val inst = Installation.fromJson(minimalRecord())
        assertTrue(!inst.toJson().contains("externalBinds"))
    }

    @Test
    fun unknownBindKindIsSkipped() {
        val arr = """
            [
              {"kind": "saf-tree", "uri": "content://something"},
              {"hostPath": "/storage/emulated/0", "guestPath": "/home/android"}
            ]
        """.trimIndent()
        val binds = ExternalBind.fromJsonArray(JSONArray(arr))
        assertEquals(listOf(ExternalBind("/storage/emulated/0", "/home/android")), binds)
    }

    @Test
    fun missingKindDefaultsToPath() {
        val inst = Installation.fromJson(
            minimalRecord(""",  "externalBinds": [{"hostPath": "/", "guestPath": "/android"}]""")
        )
        assertEquals(listOf(ExternalBind("/", "/android")), inst.externalBinds)
    }

    @Test
    fun retiredLabelKeyIsIgnored() {
        val parsed = ExternalBind.fromJsonArray(
            JSONArray("""[{"hostPath": "/", "guestPath": "/android", "label": "Android root"}]""")
        )
        assertEquals(listOf(ExternalBind("/", "/android")), parsed)
    }

    @Test
    fun validBindPassesValidation() {
        assertNull(ExternalBind("/storage/emulated/0", "/home/android").validationError())
        assertNull(ExternalBind("/", "/android").validationError())
    }

    @Test
    fun invalidBindsFailValidation() {
        // Relative paths.
        assertNotNull(ExternalBind("storage", "/home/android").validationError())
        assertNotNull(ExternalBind("/storage/emulated/0", "home/android").validationError())
        // Guest root would alias the rootfs itself.
        assertNotNull(ExternalBind("/storage/emulated/0", "/").validationError())
        // ':' would split the tawcroot `-b src:dst` argv pair.
        assertNotNull(ExternalBind("/storage/a:b", "/home/android").validationError())
        assertNotNull(ExternalBind("/storage/emulated/0", "/home/a:b").validationError())
        // '..' escapes the rootfs on the guest-target mkdir path.
        assertNotNull(ExternalBind("/storage/../data", "/home/android").validationError())
        assertNotNull(ExternalBind("/storage/emulated/0", "/home/../../evil").validationError())
        // Control characters.
        assertNotNull(ExternalBind("/storage/emulated/0", "/home/an\ndroid").validationError())
    }

    @Test
    fun commonDirBindsAreValidAndWellFormed() {
        val common = AllFilesAccess.commonDirBinds("/storage/emulated/0")
        for (bind in common) {
            assertNull(bind.validationError())
        }
        // Unique guest paths (the manage-binds duplicate rule) and room
        // under the cap.
        assertEquals(common.size, common.map { it.guestPath }.toSet().size)
        assertTrue(common.size <= ExternalBind.MAX_BINDS)
        // Android root and home. Only the browse-only root suggestion
        // is read-only; the storage binds exist to be saved into.
        assertTrue(common.any { it.hostPath == "/" && it.guestPath == "/android" && it.readOnly })
        assertTrue(common.any { it.hostPath == "/storage/emulated/0" && it.guestPath == "/home/android" })
        assertTrue(common.filter { it.hostPath != "/" }.none { it.readOnly })
        // The two platform-specific renames in the XDG mapping.
        assertTrue(common.any { it.hostPath.endsWith("/Movies") && it.guestPath == "/root/Videos" })
        assertTrue(common.any { it.hostPath.endsWith("/Download") && it.guestPath == "/root/Downloads" })
    }
}
