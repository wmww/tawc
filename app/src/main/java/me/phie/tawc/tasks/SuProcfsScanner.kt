package me.phie.tawc.tasks

import android.system.Os
import me.phie.tawc.install.Su

/**
 * Su-only scanner for real-`chroot(2)` guests. **Reachable only from
 * the chroot-method code paths** ([RootfsCleaner], and the chroot
 * branch of [ProcessScanner]) — every other caller goes through
 * [AppUidProcfsScanner] and never hits `su`.
 *
 * Quarantining the su path here is deliberate. The project is moving
 * towards `tawcroot` as the primary install method (proot is the
 * rootless fallback); production builds may eventually drop chroot
 * support entirely. Anything inside this file is the cleanest
 * candidate for build-time exclusion when that flip happens — strip
 * the chroot scan branch in [ProcessScanner], strip the chroot path
 * in [RootfsCleaner], and this file becomes dead code.
 *
 * Match strategy is a `dev:ino` comparison against `/proc/<pid>/root`,
 * computed once per install via [Os.stat] from Kotlin (and embedded
 * as literals into a `case` statement). Avoids the readlink-path
 * fragility that bit the app-uid scanner pre-canonicalisation. An
 * optional [extraCmdlinePath] adds a substring match against
 * `/proc/<pid>/cmdline` — used by the install-cancel sweep to catch
 * out-of-chroot helpers (`tar`, `find`) launched against the install
 * dir but not running inside it.
 */
internal object SuProcfsScanner {

    /**
     * Walk `/proc` once via `su` and return matched processes.
     *
     * @param installs `(rootfsAbsPath, installId)` for every chroot
     *   slot we care about. Other methods' rootfs paths are harmless
     *   if passed in — they just produce no matches.
     * @param extraCmdlinePath Optional substring; processes whose
     *   `/proc/<pid>/cmdline` contains this string are also included
     *   and tagged with [extraCmdlineId].
     * @param extraCmdlineId The install id to attach to processes
     *   matched only by [extraCmdlinePath]. Required when that
     *   parameter is non-null.
     *
     * Returns an empty list if su isn't available or the script
     * fails — the caller's [AppUidProcfsScanner] result still surfaces.
     */
    fun scan(
        installs: List<Pair<String, String>>,
        extraCmdlinePath: String? = null,
        extraCmdlineId: String? = null,
    ): List<ProcessInfo> {
        if (installs.isEmpty() && extraCmdlinePath == null) return emptyList()
        if (!Su.rootAvailable()) return emptyList()

        val resolved = installs.mapNotNull { (path, id) ->
            val di = devInodeOrNull(path) ?: return@mapNotNull null
            di to id
        }
        if (resolved.isEmpty() && extraCmdlinePath == null) return emptyList()

        val script = buildScript(resolved, extraCmdlinePath)
        val res = try {
            Su.run(script)
        } catch (_: Throwable) {
            return emptyList()
        }
        if (!res.ok) return emptyList()
        return parse(res.output, extraCmdlineId)
    }

    /**
     * `kill -<signal> <pid>` via su. Fire-and-forget — caller verifies via
     * a re-scan rather than checking the kill exit code.
     */
    fun kill(pid: Int, signal: Int = 9) {
        if (!Su.rootAvailable()) return
        try {
            Su.run("kill -$signal $pid 2>/dev/null || true")
        } catch (_: Throwable) {}
    }

    private fun devInodeOrNull(path: String): String? = try {
        val st = Os.stat(path)
        "${st.st_dev}:${st.st_ino}"
    } catch (_: Throwable) {
        null
    }

    /**
     * `set +e` opt-out: [Su.run] prepends `set -eu`, but every step
     * here is best-effort (procs vanish mid-walk, comm/cmdline may be
     * inaccessible) and the right-side of an `||` would otherwise
     * abort the sweep on the first transient failure.
     */
    private fun buildScript(
        resolved: List<Pair<String, String>>,
        extraCmdlinePath: String?,
    ): String {
        val cases = resolved.joinToString("\n") { (di, id) ->
            // di is "<long>:<long>"; id is regex-validated [a-z0-9_-]+.
            "                $di) id='$id' ;;"
        }
        val cmdlineCheck = if (extraCmdlinePath != null) {
            // Defensively single-quote the substring; install paths
            // come from app dataDir + a validated id and never carry
            // single quotes, but belt-and-braces.
            val q = extraCmdlinePath.replace("'", "'\\''")
            """
                if [ -z "${'$'}id" ]; then
                    if tr '\0' ' ' < "${'$'}entry/cmdline" 2>/dev/null | grep -F -q '$q'; then
                        id='__cmdline__'
                    fi
                fi
            """.trimIndent()
        } else ""

        return """
            set +e
            for entry in /proc/[0-9]*; do
                [ -e "${'$'}entry" ] || continue
                pid=${'$'}{entry##*/}
                [ "${'$'}pid" = "${'$'}${'$'}" ] && continue
                [ "${'$'}pid" = "${'$'}PPID" ] && continue
                [ "${'$'}pid" = "1" ] && continue
                id=
                rdi=${'$'}(stat -L -c '%d:%i' "${'$'}entry/root" 2>/dev/null)
                case "${'$'}rdi" in
$cases
                esac
                $cmdlineCheck
                [ -z "${'$'}id" ] && continue
                comm=${'$'}(cat "${'$'}entry/comm" 2>/dev/null | tr -d '\n\t')
                cmdline=${'$'}(tr '\0\t\r' '   ' < "${'$'}entry/cmdline" 2>/dev/null)
                cwd=${'$'}(readlink "${'$'}entry/cwd" 2>/dev/null | tr -d '\n\t')
                statline=${'$'}(cat "${'$'}entry/stat" 2>/dev/null)
                after_comm=${'$'}{statline##*) }
                set -- ${'$'}after_comm
                ppid=${'$'}{2:-0}
                printf '%s\t%s\t%s\t%s\t%s\t%s\n' "${'$'}pid" "${'$'}ppid" "${'$'}id" "${'$'}comm" "${'$'}cmdline" "${'$'}cwd"
            done
        """.trimIndent()
    }

    private fun parse(text: String, extraCmdlineId: String?): List<ProcessInfo> {
        val out = mutableListOf<ProcessInfo>()
        for (line in text.lineSequence()) {
            if (line.isEmpty()) continue
            val parts = line.split('\t', limit = 6)
            if (parts.size < 5) continue
            val pid = parts[0].toIntOrNull() ?: continue
            val parentPid = parts[1].toIntOrNull() ?: 0
            val rawId = parts[2]
            val ownerId = if (rawId == "__cmdline__") extraCmdlineId else rawId
            val guestCommand = parts[4].trimEnd().ifBlank {
                parts[3].trim().ifBlank { "unknown command" }
            }
            out += ProcessInfo(
                pid = pid,
                parentPid = parentPid,
                ownerInstallId = ownerId,
                orphanRootfsId = null,
                comm = parts[3].trim(),
                cmdline = parts[4].trimEnd(),
                cwd = parts.getOrNull(5)?.trim().orEmpty(),
                guestCommand = guestCommand,
                displayCommand = binaryName(guestCommand),
                requiresSu = true,
            )
        }
        return out
    }

    private fun binaryName(command: String): String {
        if (command == "unknown command") return command
        val first = command.trim().split(WHITESPACE, limit = 2).firstOrNull().orEmpty()
        return first.substringAfterLast('/').ifBlank { "unknown command" }
    }

    private val WHITESPACE = Regex("\\s+")
}
