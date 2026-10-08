package me.phie.tawc.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.file.Files
import java.nio.file.Paths

/** Parsing, `env` splitting and in-rootfs symlinks for [DesktopEntryFile]. */
class DesktopEntryFileTest {

    @get:Rule
    val tmp = TemporaryFolder()

    @Test
    fun parseReadsTheEditorKeys() {
        val d = DesktopEntryFile.parse(
            "[Desktop Entry]\nType=Application\nName = a b\nExec=run  now\nIcon=x\nComment=c\nTerminal=TRUE\n",
        )
        assertEquals(DesktopEntryFile.Draft("a b", "run  now", "c", "x", true), d)
    }

    @Test
    fun commentsAndBlanksAreSkipped() {
        val draft = DesktopEntryFile.parse(
            "# created by hand\n\n[Desktop Entry]\nType=Application\nName=X\nExec=x\n",
        )
        assertEquals("X", draft.name)
    }

    @Test
    fun extraGroupKeysDontLeakIntoDraft() {
        val parsed = DesktopEntryFile.parse(
            "[Desktop Entry]\nType=Application\nName=X\nExec=x\n" +
                "[Desktop Action new]\nName=Other\nExec=y\n",
        )
        assertEquals("X", parsed.name)
        assertEquals("x", parsed.exec)
    }

    @Test
    fun splitExecTakesLeadingEnvAssignments() {
        assertEquals(
            DesktopEntryFile.ExecLine(listOf("A" to "1", "B_2" to "x y", "C" to "it's", "D" to "50%"), "prog --flag"),
            DesktopEntryFile.splitExec("env A=1 B_2=\"x y\" C=\"it's\" D=50%% prog --flag"),
        )
        assertEquals(listOf("Q" to "a b"), DesktopEntryFile.splitExec("env Q='a b' x").env)
        assertEquals(listOf("E" to "\"\$`\\"), DesktopEntryFile.splitExec("env E=\"\\\"\\$\\`\\\\\" x").env)
    }

    @Test
    fun splitExecLeavesWhatItCantRepresent() {
        for (exec in listOf("firefox %u", "env -i A=1 prog", "env A=1", "env A=\"open prog", "envoy A=1 x", "env 1A=x prog")) {
            assertEquals(DesktopEntryFile.ExecLine(emptyList(), exec), DesktopEntryFile.splitExec(exec))
        }
    }

    @Test
    fun absoluteSymlinksResolveInsideRootfs() {
        val rootfs = tmp.newFolder("rootfs").canonicalFile
        val real = File(rootfs, "usr/lib/libreoffice/share/xdg/writer.desktop")
        real.parentFile.mkdirs()
        real.writeText("[Desktop Entry]\n")
        val apps = File(rootfs, "usr/share/applications").apply { mkdirs() }
        val link = File(apps, "libreoffice-writer.desktop")
        Files.createSymbolicLink(link.toPath(), Paths.get("/usr/lib/libreoffice/share/xdg/writer.desktop"))
        assertEquals(real, DesktopEntryFile.fileInRootfs(link.path, rootfs))
        val escape = File(apps, "escape.desktop")
        Files.createSymbolicLink(escape.toPath(), Paths.get("../../../../etc/x.desktop"))
        assertNull(DesktopEntryFile.fileInRootfs(escape.path, rootfs))
    }
}
