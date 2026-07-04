package me.phie.tawc.launcher

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class EntryShortcutsTest {

    @Test
    fun shortcutIdRoundTrip() {
        val id = EntryShortcuts.shortcutId("debian-1", "org.gnome.Calculator")
        assertEquals("debian-1/org.gnome.Calculator", id)
        assertEquals("debian-1" to "org.gnome.Calculator", EntryShortcuts.splitShortcutId(id))
    }

    @Test
    fun splitKeepsSlashesInDesktopId() {
        assertEquals("d" to "a/b", EntryShortcuts.splitShortcutId("d/a/b"))
    }

    @Test
    fun splitRejectsMalformedIds() {
        assertNull(EntryShortcuts.splitShortcutId(""))
        assertNull(EntryShortcuts.splitShortcutId("no-slash"))
        assertNull(EntryShortcuts.splitShortcutId("/leading"))
        assertNull(EntryShortcuts.splitShortcutId("trailing/"))
    }

    @Test
    fun fitCentersSquareIconInSafeZone() {
        // 2/3 of 324 = 216, centered with 54px margins.
        assertArrayEquals(intArrayOf(54, 54, 270, 270), EntryShortcuts.pinIconFit(324, 128, 128))
    }

    @Test
    fun fitPreservesAspectRatio() {
        // 2:1 icon: 216 wide, 108 tall, centered both ways.
        assertArrayEquals(intArrayOf(54, 108, 270, 216), EntryShortcuts.pinIconFit(324, 256, 128))
        // 1:2 icon: transposed.
        assertArrayEquals(intArrayOf(108, 54, 216, 270), EntryShortcuts.pinIconFit(324, 128, 256))
    }

    @Test
    fun fitRejectsDegenerateSizesSoCallerFallsBackToAppIcon() {
        assertNull(EntryShortcuts.pinIconFit(324, 0, 128))
        assertNull(EntryShortcuts.pinIconFit(324, 128, -1))
        assertNull(EntryShortcuts.pinIconFit(0, 128, 128))
        // Extreme aspect scales the short edge to zero.
        assertNull(EntryShortcuts.pinIconFit(324, 2000, 1))
    }
}
