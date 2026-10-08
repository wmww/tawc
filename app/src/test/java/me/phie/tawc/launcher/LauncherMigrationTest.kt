package me.phie.tawc.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** [LauncherMigration]: metadata fields and old-editor `.desktop` files
 *  into the store. */
class LauncherMigrationTest {

    @get:Rule
    val tmp = TemporaryFolder()

    @Test
    fun metadataFieldsBecomeOverrides() {
        val shortcut = LauncherEntries().withShortcut("htop", EntryFields(exec = "htop"))
        val e = LauncherMigration.withMetadata(
            shortcut,
            LauncherMigration.Fields(
                hidden = listOf("xterm", "tawc:term", "htop"),
                graphics = mapOf("firefox" to "cpu", "tawc:term" to "cpu"),
                pointer = mapOf("firefox" to "full"),
            ),
        )
        assertEquals(EntryFields(hidden = true), e.overrides["xterm"])
        assertEquals(EntryFields(hidden = true), e.overrides["tawc:term"])
        assertEquals(EntryFields(graphics = "cpu", pointer = "full"), e.overrides["firefox"])
        // A converted personal entry's settings land in its shortcut.
        assertEquals(EntryFields(exec = "htop", hidden = true), e.shortcuts["htop"])
        assertFalse("htop" in e.overrides)
    }

    @Test
    fun personalEntryBecomesAShortcut() {
        val text = "[Desktop Entry]\nType=Application\nName=Top\nExec=env TERM=xterm A=\"x y\" htop %u\n" +
            "Icon=utilities-system-monitor\nTerminal=true\n"
        assertEquals(
            EntryFields(
                name = "Top", exec = "htop", terminal = true, icon = "utilities-system-monitor",
                env = mapOf("TERM" to "xterm", "A" to "x y"),
            ),
            LauncherMigration.convert(text, null),
        )
    }

    @Test
    fun overrideKeepsOnlyWhatDiffers() {
        val packaged = "[Desktop Entry]\nType=Application\nName=Firefox\nExec=firefox %u\nIcon=firefox\n"
        val managed = "[Desktop Entry]\nType=Application\nName=Firefox (CPU)\nExec=firefox %u\nIcon=firefox\n"
        assertEquals(EntryFields(name = "Firefox (CPU)"), LauncherMigration.convert(managed, packaged))
        val withEnv = managed.replace("Exec=firefox", "Exec=env MOZ=1 firefox")
        assertEquals(EntryFields(name = "Firefox (CPU)", env = mapOf("MOZ" to "1")), LauncherMigration.convert(withEnv, packaged))
    }

    @Test
    fun handMadeFilesAreLeftAlone() {
        val base = "[Desktop Entry]\nType=Application\nName=X\nExec=x\n"
        assertTrue(LauncherMigration.editorOnly("# mine\n\n$base"))
        for (text in listOf(
            "${base}Name[de]=Y\n",
            "${base}Categories=Utility;\n",
            "${base}Actions=new;\n[Desktop Action new]\nName=N\nExec=n\n",
            base.replace("Application", "Link"),
            "Name=X\n$base",
            "[Desktop Entry]\nName=X\nExec=x\n",
        )) {
            assertFalse(text, LauncherMigration.editorOnly(text))
            assertNull(text, LauncherMigration.convert(text, null))
        }
        assertNull(LauncherMigration.convert("[Desktop Entry]\nType=Application\nName=X\n", null))
    }

    @Test
    fun fieldCodesStripLikeTheScanner() {
        assertEquals("a b 50%", LauncherMigration.stripFieldCodes("a  %U b %F 50%%"))
        assertEquals("x %", LauncherMigration.stripFieldCodes("x %"))
    }

    private class FakeLegacy(var fields: LauncherMigration.Fields?) : LauncherMigration.Legacy {
        var cleared = false
        override fun exists(id: String) = true
        override fun read(id: String) = fields
        override fun clear(id: String) { cleared = true }
    }

    @Test
    fun runConvertsManagedFilesAndClearsMetadata() {
        val legacy = FakeLegacy(LauncherMigration.Fields(hidden = listOf("mine"), graphics = mapOf("ff" to "cpu")))
        lateinit var store: LauncherStore
        val shadows = HashMap<String, String>()
        store = LauncherStore({ File(tmp.root, it) }, legacy, { shadows }, { _, _, _ -> false })
        val rootfs = store.rootfs("a")
        val managed = File(rootfs, LauncherMigration.MANAGED_SUBDIR).apply { mkdirs() }
        val packagedDir = File(rootfs, "usr/share/applications").apply { mkdirs() }
        File(packagedDir, "ff.desktop").writeText("[Desktop Entry]\nType=Application\nName=FF\nExec=ff\n")
        val override = File(managed, "ff.desktop").apply { writeText("[Desktop Entry]\nType=Application\nName=Fox\nExec=ff\n") }
        val mine = File(managed, "mine.desktop").apply {
            writeText("[Desktop Entry]\nType=Application\nName=Mine\nExec=mine\nIcon=tawc-mine\n")
        }
        val hand = File(managed, "hand.desktop").apply { writeText("[Desktop Entry]\nType=Application\nName=H\nExec=h\nX-Foo=1\n") }
        val icon = File(rootfs, "root/.local/share/icons/hicolor/256x256/apps/tawc-mine.png").apply {
            parentFile.mkdirs()
            writeBytes(byteArrayOf(9, 9))
        }
        shadows[override.canonicalPath] = File(packagedDir, "ff.desktop").path
        shadows[mine.canonicalPath] = ""
        shadows[hand.canonicalPath] = ""

        val e = store.load("a")!!
        assertEquals(1, e.migrated)
        assertEquals(EntryFields(name = "Fox", graphics = "cpu"), e.overrides["ff"])
        assertEquals(
            EntryFields(name = "Mine", exec = "mine", terminal = false, iconFile = "tawc-mine.png", hidden = true),
            e.shortcuts["mine"],
        )
        assertTrue(File(store.iconsDir("a"), "tawc-mine.png").readBytes().contentEquals(byteArrayOf(9, 9)))
        assertTrue(icon.exists())
        assertFalse(override.exists())
        assertFalse(mine.exists())
        assertTrue(hand.exists())
        assertTrue(legacy.cleared)

        // Idempotent: a second load changes nothing.
        legacy.cleared = false
        assertEquals(e, store.load("a"))
        assertFalse(legacy.cleared)
    }

    @Test
    fun failedScanDefersManagedFiles() {
        val store = LauncherStore({ File(tmp.root, it) }, FakeLegacy(null), { emptyMap() }, { _, _, _ -> false })
        val f = File(store.rootfs("a"), "${LauncherMigration.MANAGED_SUBDIR}/x.desktop").apply {
            parentFile.mkdirs()
            writeText("[Desktop Entry]\nType=Application\nName=X\nExec=x\n")
        }
        assertEquals(0, store.load("a")!!.migrated)
        assertTrue(f.exists())
    }
}
