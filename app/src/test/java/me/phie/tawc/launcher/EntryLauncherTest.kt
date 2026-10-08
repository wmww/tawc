package me.phie.tawc.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.file.Files

class EntryLauncherTest {

    @get:Rule val tmp = TemporaryFolder()

    @Test
    fun execArgv0() {
        assertEquals("chatgpt", EntryLauncher.execArgv0("chatgpt --foo"))
        assertEquals("/usr/bin/x", EntryLauncher.execArgv0("  /usr/bin/x"))
        assertEquals("/opt/My App/run", EntryLauncher.execArgv0("\"/opt/My App/run\" --a"))
        assertEquals("a\"b", EntryLauncher.execArgv0("\"a\\\"b\""))
        assertNull(EntryLauncher.execArgv0("   "))
        assertNull(EntryLauncher.execArgv0("\"unterminated"))
    }

    /** Runs [guiCommand] for `app` through bash; returns the args `app` saw. */
    private fun argsSeen(chromium: Boolean, exec: String = "app --x"): String {
        val root = tmp.newFolder()
        val out = File(root, "args")
        val real = File(root, "opt/app/app").apply {
            parentFile!!.mkdirs()
            writeText("#!/bin/sh\necho \"${'$'}{TAWC_T:+${'$'}TAWC_T }$*\" > '${out.path}'\n")
            setExecutable(true)
        }
        if (chromium) File(real.parentFile, "chrome_100_percent.pak").writeText("")
        val bin = File(root, "bin").apply { mkdirs() }
        Files.createSymbolicLink(File(bin, "app").toPath(), real.toPath())
        val script = "PATH=${bin.path}:/usr/bin:/bin; " + EntryLauncher.guiCommand(exec)
        val proc = ProcessBuilder("bash", "-c", script).start()
        assertEquals(0, proc.waitFor())
        return out.readText().trim()
    }

    @Test
    fun chromiumGetsNoSandbox() = assertEquals("--x --no-sandbox", argsSeen(chromium = true))

    @Test
    fun otherAppsUntouched() = assertEquals("--x", argsSeen(chromium = false))

    @Test
    fun activationTokenExported() {
        val root = tmp.newFolder()
        val out = File(root, "env")
        val exec = "sh -c 'echo \"${'$'}XDG_ACTIVATION_TOKEN ${'$'}DESKTOP_STARTUP_ID\" > ${out.path}'"
        val proc = ProcessBuilder("bash", "-c", EntryLauncher.guiCommand(exec, "tok'1")).start()
        assertEquals(0, proc.waitFor())
        assertEquals("tok'1 tok'1", out.readText().trim())
    }

    /** The probe looks past an `env K=V` prefix in a packaged Exec. */
    @Test
    fun chromiumBehindEnvGetsNoSandbox() =
        assertEquals("v --x --no-sandbox", argsSeen(chromium = true, exec = "env TAWC_T=v app --x"))

    /** Store `env` is exported before Exec, quoted; bad names are skipped. */
    @Test
    fun entryEnvExported() {
        val root = tmp.newFolder()
        val out = File(root, "env")
        val exec = "sh -c 'echo \"${'$'}A|${'$'}B\" > ${out.path}'"
        val vars = mapOf("A" to "x y'z", "B" to "${'$'}HOME", "1BAD" to "q; touch ${root.path}/pwned")
        val proc = ProcessBuilder("bash", "-c", EntryLauncher.guiCommand(exec, null, vars)).start()
        assertEquals(0, proc.waitFor())
        assertEquals("x y'z|${'$'}HOME", out.readText().trim())
        assertEquals(false, File(root, "pwned").exists())
    }

    @Test
    fun graphicsAndPointerComeFromTheEntry() {
        val e = LauncherEntry("x", "x", "", "x", false, "", graphics = "cpu", pointer = "nonsense")
        assertEquals("cpu", EntryLauncher.graphicsFor(e)?.key)
        assertNull(EntryLauncher.pointerEmulationFor(e))
        assertNull(EntryLauncher.graphicsFor(e.copy(graphics = null)))
    }
}
