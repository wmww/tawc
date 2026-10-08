package me.phie.tawc.install

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The launcher fields older versions kept in `metadata.json`
 * (`hiddenDesktopIds`, `entryGraphics`, `entryPointerEmulation`) parse
 * into [Installation]'s legacy fields and survive a rewrite until
 * [me.phie.tawc.launcher.LauncherMigration] moves them out.
 */
class InstallationLegacyLauncherFieldsTest {

    private fun record(extra: String = ""): String = """
        {
          "id": "arch",
          "arch": "arm64-v8a"
          $extra
        }
    """.trimIndent()

    @Test
    fun absentIsEmpty() {
        val inst = Installation.fromJson(record())
        assertTrue(inst.legacyHiddenDesktopIds.isEmpty())
        assertTrue(inst.legacyEntryGraphics.isEmpty())
        assertTrue(inst.legacyEntryPointerEmulation.isEmpty())
    }

    @Test
    fun survivesRewrite() {
        val inst = Installation.fromJson(
            record(
                """, "hiddenDesktopIds": ["xterm"], "entryGraphics": {"firefox": "cpu"},
                   "entryPointerEmulation": {"kitty": "full"}""",
            ),
        )
        val again = Installation.fromJson(inst.copy(label = "x").toJson())
        assertEquals(listOf("xterm"), again.legacyHiddenDesktopIds)
        assertEquals(mapOf("firefox" to "cpu"), again.legacyEntryGraphics)
        assertEquals(mapOf("kitty" to "full"), again.legacyEntryPointerEmulation)
    }

    @Test
    fun emptyOmittedFromJson() {
        val json = Installation.fromJson(record()).toJson()
        assertFalse(json.contains("hiddenDesktopIds"))
        assertFalse(json.contains("entryGraphics"))
        assertFalse(json.contains("entryPointerEmulation"))
    }
}
