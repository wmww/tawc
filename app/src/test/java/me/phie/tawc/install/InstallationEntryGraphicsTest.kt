package me.phie.tawc.install

import me.phie.tawc.GraphicsBackend
import me.phie.tawc.launcher.EntryLauncher
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * [Installation.entryGraphics] (the editor's per-entry graphics
 * override, notes/launcher.md): legacy default, round-trip,
 * omit-when-empty, the [Installation.withEntryGraphics] helper, and
 * how [EntryLauncher.graphicsFor] resolves it.
 */
class InstallationEntryGraphicsTest {

    private val base = Installation.fromJson("""{"id": "arch", "arch": "arm64-v8a"}""")

    @Test
    fun legacyRecordDefaultsToEmpty() {
        assertTrue(base.entryGraphics.isEmpty())
    }

    @Test
    fun roundTripsThroughJson() {
        val inst = base.withEntryGraphics("firefox", "cpu").withEntryGraphics("kitty", "libhybris-zink")
        assertEquals(
            mapOf("firefox" to "cpu", "kitty" to "libhybris-zink"),
            Installation.fromJson(inst.toJson()).entryGraphics,
        )
    }

    @Test
    fun emptyOmittedFromJson() {
        assertFalse(base.toJson().contains("entryGraphics"))
        assertFalse(base.withEntryGraphics("x", "cpu").withEntryGraphics("x", null).toJson().contains("entryGraphics"))
    }

    @Test
    fun withEntryGraphicsReplacesAndClears() {
        val set = base.withEntryGraphics("firefox", "cpu").withEntryGraphics("firefox", "gfxstream")
        assertEquals(mapOf("firefox" to "gfxstream"), set.entryGraphics)
        assertTrue(set.withEntryGraphics("firefox", null).entryGraphics.isEmpty())
    }

    @Test
    fun launchResolvesOverrideOrNull() {
        // No override → null (the global setting), not a hard pin.
        assertNull(EntryLauncher.graphicsFor(base, "firefox"))
        assertNull(EntryLauncher.graphicsFor(base.withEntryGraphics("firefox", "no-such-backend"), "firefox"))
        assertEquals(
            GraphicsBackend.CPU,
            EntryLauncher.graphicsFor(base.withEntryGraphics("firefox", "cpu"), "firefox"),
        )
    }
}
