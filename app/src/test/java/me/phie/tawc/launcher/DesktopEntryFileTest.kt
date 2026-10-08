package me.phie.tawc.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** Serializer/patch round-trips, ids and filename slugs for the `.desktop` editor. */
class DesktopEntryFileTest {

    @get:Rule
    val tmp = TemporaryFolder()

    @Test
    fun serializeRoundTripsAllFields() {
        val draft = DesktopEntryFile.Draft(
            name = "My Script",
            exec = "/root/bin/run.sh --fast",
            comment = "Does things",
            icon = "utilities-terminal",
            terminal = true,
        )
        val text = DesktopEntryFile.serialize(draft)
        assertEquals(draft, DesktopEntryFile.parse(text))
    }

    @Test
    fun serializeOmitsEmptyOptionalsAndFalseTerminal() {
        val text = DesktopEntryFile.serialize(DesktopEntryFile.Draft(name = "X", exec = "x"))
        assertEquals("[Desktop Entry]\nType=Application\nName=X\nExec=x\n", text)
    }

    @Test
    fun serializeStripsNewlines() {
        val text = DesktopEntryFile.serialize(
            DesktopEntryFile.Draft(name = "a\nb", exec = "run\r\nnow"),
        )
        val parsed = DesktopEntryFile.parse(text)
        assertEquals("a b", parsed.name)
        assertEquals("run  now", parsed.exec)
    }

    @Test
    fun commentsAndBlanksAreSkipped() {
        val draft = DesktopEntryFile.parse(
            "# created by hand\n\n[Desktop Entry]\nType=Application\nName=X\nExec=x\n",
        )
        assertEquals("X", draft.name)
    }

    private val packaged = """
        |# Packaged
        |[Desktop Entry]
        |Type=Application
        |Name=Firefox
        |Name[de]=Feuerfuchs
        |Comment=Browse the web
        |Comment[de]=Im Netz surfen
        |Exec=firefox %u
        |Icon=firefox
        |MimeType=text/html;
        |StartupWMClass=firefox
        |Actions=new-window;
        |
        |[Desktop Action new-window]
        |Name=New Window
        |Exec=firefox --new-window %u
        |""".trimMargin()

    @Test
    fun patchUnchangedDraftIsIdentity() {
        val draft = DesktopEntryFile.parse(packaged)
        assertEquals(packaged, DesktopEntryFile.patch(packaged, draft))
    }

    @Test
    fun patchKeepsForeignKeysGroupsAndComments() {
        val draft = DesktopEntryFile.parse(packaged).copy(exec = "firefox --private %u")
        val out = DesktopEntryFile.patch(packaged, draft)
        assertEquals(packaged.replace("Exec=firefox %u\n", "Exec=firefox --private %u\n"), out)
        // The action group's Exec is a different group.
        assertTrue(out.contains("Exec=firefox --new-window %u"))
    }

    @Test
    fun patchDropsLocalesOnlyForChangedKey() {
        val draft = DesktopEntryFile.parse(packaged).copy(name = "Web")
        val out = DesktopEntryFile.patch(packaged, draft)
        assertTrue(out.contains("\nName=Web\n"))
        assertFalse(out.contains("Name[de]"))
        // Comment didn't change, so its locale variant stays.
        assertTrue(out.contains("Comment[de]=Im Netz surfen"))
        // Other groups' Name is untouched.
        assertTrue(out.contains("Name=New Window"))
    }

    @Test
    fun patchAddsAndRemovesTerminal() {
        val on = DesktopEntryFile.patch(packaged, DesktopEntryFile.parse(packaged).copy(terminal = true))
        // Appended at the end of the group, before its trailing blank.
        assertTrue(on.contains("Actions=new-window;\nTerminal=true\n\n[Desktop Action new-window]"))
        val off = DesktopEntryFile.patch(on, DesktopEntryFile.parse(on).copy(terminal = false))
        assertTrue(off.contains("\nTerminal=false\n"))
        // False and absent: nothing is written.
        assertEquals(packaged, DesktopEntryFile.patch(packaged, DesktopEntryFile.parse(packaged).copy(terminal = false)))
    }

    @Test
    fun patchAppendsMissingAndRemovesEmptiedIcon() {
        val noIcon = DesktopEntryFile.patch(packaged, DesktopEntryFile.parse(packaged).copy(icon = ""))
        assertFalse(noIcon.contains("Icon="))
        val back = DesktopEntryFile.patch(noIcon, DesktopEntryFile.parse(noIcon).copy(icon = "web-browser"))
        assertTrue(back.contains("Actions=new-window;\nIcon=web-browser\n"))
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
    fun joinExecRoundTrips() {
        val line = DesktopEntryFile.ExecLine(
            listOf("PLAIN" to "a/b:c", "SPACE" to "x y", "META" to "\"\$`\\", "PCT" to "50%", "EMPTY" to ""),
            "prog %u",
        )
        val exec = DesktopEntryFile.joinExec(line)
        assertTrue(exec.startsWith("env PLAIN=a/b:c SPACE=\"x y\" "))
        assertTrue(exec.contains("PCT=50%% "))
        assertEquals(line, DesktopEntryFile.splitExec(exec))
        assertEquals("prog", DesktopEntryFile.joinExec(DesktopEntryFile.ExecLine(emptyList(), "prog")))
    }

    @Test
    fun idAndTargetFollowTheScanner() {
        val rootfs = tmp.newFolder("rootfs")
        assertEquals("kde4-foo", DesktopEntryFile.idFor("${rootfs.path}/usr/share/applications/kde4/foo.desktop"))
        val src = File(rootfs, "usr/share/applications/kde4/foo.desktop")
        assertEquals(
            File(DesktopEntryFile.managedDir(rootfs), "kde4-foo.desktop"),
            DesktopEntryFile.targetFor(src, rootfs),
        )
        val managed = File(DesktopEntryFile.managedDir(rootfs), "mine.desktop")
        assertEquals(managed, DesktopEntryFile.targetFor(managed, rootfs))
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
    fun newFileSlugsNameAndSuffixesOnCollision() {
        val dir = tmp.newFolder()
        val first = DesktopEntryFile.newFile(dir, "My Script!")
        assertEquals("my-script.desktop", first.name)
        first.writeText("x")
        val second = DesktopEntryFile.newFile(dir, "My Script!")
        assertEquals("my-script-2.desktop", second.name)
        second.writeText("x")
        assertEquals("my-script-3.desktop", DesktopEntryFile.newFile(dir, "My Script!").name)
    }

    @Test
    fun unslugifiableNameFallsBack() {
        assertEquals("program.desktop", DesktopEntryFile.newFile(tmp.newFolder(), "!!!").name)
    }

    @Test
    fun isManagedMatchesOnlyTheManagedDir() {
        val rootfs = File("/data/data/me.phie.tawc/distros/arch/rootfs")
        assertTrue(
            DesktopEntryFile.isManaged(
                "${rootfs.absolutePath}/root/.local/share/applications/foo.desktop", rootfs,
            ),
        )
        assertFalse(
            DesktopEntryFile.isManaged(
                "${rootfs.absolutePath}/usr/share/applications/foo.desktop", rootfs,
            ),
        )
        assertFalse(
            DesktopEntryFile.isManaged(
                "${rootfs.absolutePath}/usr/local/share/applications/foo.desktop", rootfs,
            ),
        )
    }
}
