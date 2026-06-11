package me.phie.tawc.install

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.file.Files

/**
 * [RootfsTmpSweeper.sweep] semantics on a synthetic tmp tree: age
 * gating, dirs-only-when-emptied, the pre-captured dir mtime (unlink
 * bumps the parent's mtime — a dir emptied by the sweep must still
 * count as old), the `.X11-unix` skip, and symlink no-follow.
 */
class RootfsTmpSweeperTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private val now = System.currentTimeMillis()
    private val oldMtime = 1_000L

    private fun File.makeOld() {
        assertTrue("setLastModified failed for $this", setLastModified(oldMtime))
    }

    @Test
    fun oldFilesGoFreshFilesStay() {
        val root = tmp.newFolder("tmp")
        val old = File(root, "stale.sock").apply { createNewFile(); makeOld() }
        val fresh = File(root, "fresh.sock").apply { createNewFile() }

        val stats = RootfsTmpSweeper.sweep(root, now - 1)

        assertFalse(old.exists())
        assertTrue(fresh.exists())
        assertTrue(root.exists())
        assertEquals(1, stats.deleted)
        assertEquals(0, stats.failed)
    }

    @Test
    fun oldDirEmptiedBySweepIsDeletedDespiteMtimeBump() {
        val root = tmp.newFolder("tmp")
        val dir = File(root, "gpg-XXXX").apply { mkdir() }
        File(dir, "S.gpg-agent").apply { createNewFile(); makeOld() }
        // Set the dir's mtime after creating the child (creation
        // already bumped it once).
        dir.makeOld()

        val stats = RootfsTmpSweeper.sweep(root, now - 1)

        // Unlinking the child bumps the dir mtime to now; the sweep
        // must still delete it based on its pre-sweep age.
        assertFalse(dir.exists())
        assertEquals(2, stats.deleted)
    }

    @Test
    fun oldDirWithFreshChildSurvives() {
        val root = tmp.newFolder("tmp")
        val dir = File(root, "session").apply { mkdir() }
        val fresh = File(dir, "live.sock").apply { createNewFile() }
        dir.makeOld()

        RootfsTmpSweeper.sweep(root, now - 1)

        assertTrue(dir.exists())
        assertTrue(fresh.exists())
    }

    @Test
    fun x11DirIsSkippedAtTopLevel() {
        val root = tmp.newFolder("tmp")
        val x11 = File(root, ".X11-unix").apply { mkdir() }
        val sock = File(x11, "X0").apply { createNewFile(); makeOld() }
        x11.makeOld()

        RootfsTmpSweeper.sweep(root, now - 1)

        assertTrue(x11.exists())
        assertTrue(sock.exists())
    }

    @Test
    fun symlinksAreUnlinkedNotFollowed() {
        val root = tmp.newFolder("tmp")
        val target = tmp.newFolder("outside")
        val precious = File(target, "precious").apply { createNewFile() }
        val link = File(root, "link")
        Files.createSymbolicLink(link.toPath(), target.toPath())

        // Future cutoff: everything (including the just-created link,
        // whose own lstat mtime can't portably be aged) counts as old.
        RootfsTmpSweeper.sweep(root, now + 60_000)

        assertFalse(Files.isSymbolicLink(link.toPath()))
        assertTrue(target.exists())
        assertTrue(precious.exists())
        assertTrue(root.exists())
    }
}
