package me.phie.tawc.launcher

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** `entries.json` schema ([LauncherEntries], [EntryFields]) and
 *  [LauncherStore]'s writes. */
class LauncherStoreTest {

    @get:Rule
    val tmp = TemporaryFolder()

    @Test
    fun parsesAndRoundTrips() {
        val text = """
            {"version": 1, "migrated": 1,
             "overrides": {"firefox": {"name": "Firefox (CPU)", "graphics": "cpu", "hidden": true,
                                       "env": {"MOZ_ENABLE_WAYLAND": "1"}}},
             "shortcuts": {"tawc:app:htop": {"name": "htop", "exec": "htop", "terminal": true,
                                             "iconFile": "htop.png"}}}
        """
        val e = LauncherEntries.parse(text)
        assertEquals(1, e.migrated)
        val ff = e.overrides.getValue("firefox")
        assertEquals("Firefox (CPU)", ff.name)
        assertEquals("cpu", ff.graphics)
        assertEquals(true, ff.hidden)
        assertEquals(mapOf("MOZ_ENABLE_WAYLAND" to "1"), ff.env)
        assertEquals(EntryFields(name = "htop", exec = "htop", terminal = true, iconFile = "htop.png"), e.shortcuts["tawc:app:htop"])
        assertEquals(e, LauncherEntries.parse(e.toJson()))
    }

    @Test
    fun unknownFieldsSurviveARewrite() {
        val text = """
            {"version": 1, "future": [1, {"a": "b"}],
             "overrides": {"x": {"name": "X", "colour": "red", "nested": {"k": 2}, "n": 1.5, "nul": null}}}
        """
        val e = LauncherEntries.parse(text).withField("x", "name", "Y")
        val o = JSONObject(LauncherEntries.parse(e.toJson()).toJson())
        assertEquals("[1,{\"a\":\"b\"}]", o.getJSONArray("future").toString())
        val x = o.getJSONObject("overrides").getJSONObject("x")
        assertEquals("Y", x.getString("name"))
        assertEquals("red", x.getString("colour"))
        assertEquals(2, x.getJSONObject("nested").getInt("k"))
        assertEquals(1.5, x.getDouble("n"), 0.0)
        assertTrue(x.has("nul") && x.isNull("nul"))
    }

    @Test
    fun wronglyTypedKnownFieldsAreDropped() {
        val e = LauncherEntries.parse("""{"overrides": {"x": {"name": 3, "hidden": "yes", "exec": "e"}}}""")
        assertEquals(EntryFields(exec = "e"), e.overrides["x"])
    }

    @Test
    fun newerVersionIsRefused() {
        try {
            LauncherEntries.parse("""{"version": 2}""")
            fail()
        } catch (e: IllegalArgumentException) {
            assertTrue(e.message!!.contains("newer"))
        }
    }

    @Test
    fun withFieldRoutesToShortcutsAndDropsEmptyOverrides() {
        var e = LauncherEntries().withShortcut("tawc:app:a", EntryFields(exec = "a"))
        e = e.withField("tawc:app:a", "hidden", true).withField("firefox", "hidden", true)
        assertEquals(true, e.shortcuts.getValue("tawc:app:a").hidden)
        assertTrue(e.isHidden("tawc:app:a") && e.isHidden("firefox"))
        assertFalse("tawc:app:a" in e.overrides)
        e = e.withField("firefox", "hidden", null)
        assertFalse("firefox" in e.overrides)
    }

    @Test
    fun builtinsTakeHiddenOnly() {
        val e = LauncherEntries().withField("tawc:term", "hidden", true)
        assertTrue(e.isHidden("tawc:term"))
        try {
            e.withField("tawc:term", "name", "x")
            fail()
        } catch (_: IllegalArgumentException) {
        }
    }

    @Test
    fun badFieldsAreRejected() {
        for ((field, value) in listOf("nope" to "x", "terminal" to "yes", "name" to true, "env" to mapOf("A" to 1))) {
            try {
                EntryFields().with(field, value)
                fail("$field=$value accepted")
            } catch (_: IllegalArgumentException) {
            }
        }
    }

    @Test
    fun resetKeepsHideState() {
        val e = LauncherEntries().withOverride("x", EntryFields(name = "N", graphics = "cpu", hidden = true))
        assertEquals(EntryFields(hidden = true), e.withoutOverrides("x").overrides["x"])
        assertNull(LauncherEntries().withOverride("y", EntryFields(name = "N")).withoutOverrides("y").overrides["y"])
    }

    @Test
    fun newShortcutIdsSlugAndCount() {
        var e = LauncherEntries()
        assertEquals("tawc:app:my-tool", e.newShortcutId("My Tool"))
        e = e.withShortcut("tawc:app:my-tool", EntryFields(exec = "x"))
        assertEquals("tawc:app:my-tool-2", e.newShortcutId("My Tool"))
        assertEquals("tawc:app:app", e.newShortcutId("!!!"))
    }

    // ---- LauncherStore ----------------------------------------------

    private class FakeLegacy(var fields: LauncherMigration.Fields? = null) : LauncherMigration.Legacy {
        var cleared = false
        override fun exists(id: String) = id != "gone"
        override fun read(id: String) = fields
        override fun clear(id: String) { cleared = true; fields = null }
    }

    private fun store(legacy: LauncherMigration.Legacy = FakeLegacy(), shadows: Map<String, String> = emptyMap()) =
        LauncherStore({ File(tmp.root, it) }, legacy, { shadows }, { _, _, _ -> false })

    @Test
    fun firstLoadMigratesAndWrites() {
        val s = store()
        val e = s.load("a")!!
        assertEquals(1, e.migrated)
        assertTrue(s.file("a").isFile)
        assertNull(s.load("gone"))
        assertFalse(s.file("gone").exists())
    }

    @Test
    fun updateStoresIconsAndPrunesUnreferenced() {
        val s = store()
        val png = byteArrayOf(1, 2, 3)
        s.update("a", png to "cat") { e, f -> e.withShortcut("tawc:app:c", EntryFields(exec = "c", iconFile = f)) }
        assertEquals("cat.png", s.load("a")!!.shortcuts.getValue("tawc:app:c").iconFile)
        assertTrue(File(s.iconsDir("a"), "cat.png").readBytes().contentEquals(png))
        // Same bytes reuse the file; the shortcut going away prunes it.
        s.update("a", png to "cat") { e, f -> assertEquals("cat.png", f); e }
        s.update("a") { e, _ -> e.withoutShortcut("tawc:app:c") }
        assertFalse(File(s.iconsDir("a"), "cat.png").exists())
    }

    @Test
    fun abortedUpdateWritesNothing() {
        val s = store()
        s.load("a")
        val before = s.file("a").readText()
        assertNull(s.update("a") { _, _ -> null })
        assertEquals(before, s.file("a").readText())
    }

    @Test
    fun newerStoreIsNotOverwritten() {
        val s = store()
        s.dir("a").mkdirs()
        s.file("a").writeText("""{"version": 9}""")
        assertNull(s.load("a"))
        assertNull(s.update("a") { e, _ -> e.withField("x", "hidden", true) })
        assertEquals("""{"version": 9}""", s.file("a").readText())
    }
}
