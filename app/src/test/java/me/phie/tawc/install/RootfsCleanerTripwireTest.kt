package me.phie.tawc.install

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Tripwire for [RootfsCleaner]'s "one and only deleter" contract:
 * scans the production sources for recursive deletion primitives
 * outside the engine. If you trip this with a new feature (e.g. a
 * reset-rootfs button), route the deletion through [RootfsCleaner] —
 * or, for a genuinely non-distro-slot delete, add the file to the
 * allowlist with a one-line reason.
 *
 * Comment lines are skipped, so prose may mention the primitives;
 * lines inside multi-line strings (in-rootfs shell scripts) are not,
 * which is deliberate — a wipe hidden in a script is still a wipe.
 *
 * The allowlist is file-granular: a new recursive delete added to an
 * allowlisted file passes silently. Accepted tradeoff — finer
 * granularity (line patterns) would rot with every refactor. Hand-
 * rolled deleters (manual recursion + `File.delete()`) are outside
 * the pattern's reach entirely; allowlist them anyway when found, so
 * the list stays the inventory of every non-engine deleter.
 */
class RootfsCleanerTripwireTest {

    private val allowlist = mapOf(
        // The engine itself.
        "install/RootfsCleaner.kt" to "the one deletion engine",
        // atomicReplaceDir + staging cleanup for extracted Xwayland
        // assets under <filesDir>; never touches <distros>.
        "compositor/CompositorService.kt" to "xwayland asset staging",
        // removeFromRootfs: targeted removal of previously-installed
        // tawc files inside a live rootfs, driven by the recorded
        // tawcInstalls list — not slot deletion.
        "install/TawcInstaller.kt" to "tawc-file uninstall inside rootfs",
        // Age sweep inside a live rootfs's /tmp — never slot deletion.
        // Manual recursion (pre-order mtimes + post-order deletes), so
        // the pattern can't see it; listed for the inventory.
        "install/RootfsTmpSweeper.kt" to "age sweep of rootfs /tmp",
        // In-rootfs package-cache/cruft cleanup scripts that run
        // inside the guest during install configure.
        "install/distro/apt/AptCommon.kt" to "in-rootfs apt cleanup",
        "install/distro/arch/ArchPacmanCommon.kt" to "in-rootfs pacman cleanup",
        "install/distro/voidlinux/VoidCommon.kt" to "in-rootfs xbps cleanup",
    )

    private val pattern =
        Regex("""deleteRecursively|walkBottomUp|walkTopDown|Files\.walk|rm\s+-(rf|fr)\b|find\s[^\n]*-delete""")

    @Test
    fun recursiveDeletesLiveOnlyInTheEngine() {
        // Gradle runs JVM unit tests with workdir = the module dir;
        // fall back to repo-root-relative for IDE runners.
        val srcRoot = listOf("src/main/java/me/phie/tawc", "app/src/main/java/me/phie/tawc")
            .map { File(it) }
            .firstOrNull { it.isDirectory }
            ?: error("cannot locate app sources from ${File("").absolutePath}")

        val offenders = mutableListOf<String>()
        srcRoot.walkTopDown()
            .filter { it.isFile && it.extension == "kt" }
            .forEach { file ->
                val rel = file.relativeTo(srcRoot).invariantSeparatorsPath
                if (rel in allowlist) return@forEach
                file.readLines().forEachIndexed { i, raw ->
                    val line = raw.trim()
                    val isComment = line.startsWith("//") || line.startsWith("*") ||
                        line.startsWith("/*") || line.startsWith("#")
                    if (!isComment && pattern.containsMatchIn(line)) {
                        offenders += "$rel:${i + 1}: $line"
                    }
                }
            }

        assertTrue(
            "recursive deletion primitive(s) outside RootfsCleaner — route distro-slot " +
                "deletion through the engine (see RootfsCleaner kdoc):\n" +
                offenders.joinToString("\n"),
            offenders.isEmpty(),
        )
    }
}
