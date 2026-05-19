package me.phie.tawc.tasks

import android.system.Os
import java.io.File

/**
 * Walks `/proc` from the app uid (no `su`) and reports every process
 * whose kernel-side `/proc` paths point into a rootfs. The normal
 * task-manager scan classifies paths under `<distros>/<id>/rootfs`
 * first, then maps `<id>` to either a known install or an orphan slot.
 * Tawcroot guests can hide the real guest binary from `/proc/<pid>/exe`,
 * so executable file mappings from `/proc/<pid>/maps` are used as a
 * fallback after the cheap `cwd` / `exe` / `root` links.
 *
 * Does not see real-`chroot(2)` guests on stock Android — those live
 * under uid 0 and the proc files are masked by hidepid. [SuProcfsScanner]
 * picks them up via `su`.
 *
 * One pass over `/proc`, regardless of how many installs are passed in.
 * The common path extracts the install slot id directly from
 * `<distros>/<id>/rootfs`; scoped rootfs prefixes are only a fallback
 * for callers that intentionally scan one rootfs without orphan data.
 */
internal object AppUidProcfsScanner {

    /**
     * Walk `/proc` and return one [ProcessInfo] per matched pid.
     *
     * @param installs `(rootfsAbsPath, installId)` pairs for every
     *   known slot we want to surface. When [orphanPattern] is present,
     *   paths are first classified under
     *   `rootfsParentPath/<id>/rootfs` and this list supplies the known
     *   install ids. Without [orphanPattern], these paths are the
     *   scoped prefixes to match.
     * @param orphanPattern Optional `(rootfsParentPath, knownIds)` —
     *   processes whose path lives under
     *   `rootfsParentPath/<id>/rootfs` (e.g.
     *   `/data/data/me.phie.tawc/distros/<id>/rootfs`) but whose
     *   extracted slot id isn't in `knownIds` are reported as orphans
     *   (with the slot id preserved on [ProcessInfo.orphanRootfsId]).
     *   Pass `null` to disable orphan detection.
     * @param ownPid Skip this pid in the walk (the calling app's own
     *   pid) so the task manager never lists itself.
     * @param extraCmdlinePath Optional substring; processes whose
     *   `/proc/<pid>/cmdline` contains this are also included. Used
     *   by the install-cancel sweep to catch out-of-rootfs helpers
     *   (`tar`, `find`) running against the install dir from outside.
     *   Tagged with [extraCmdlineId] when matched only by cmdline.
     */
    fun scan(
        installs: List<Pair<String, String>>,
        orphanPattern: OrphanPattern?,
        ownPid: Int,
        extraCmdlinePath: String? = null,
        extraCmdlineId: String? = null,
    ): List<ProcessInfo> {
        if (installs.isEmpty() && orphanPattern == null && extraCmdlinePath == null) {
            return emptyList()
        }

        // Sort longest-first so the scoped fallback remains robust for
        // hypothetical nested rootfs paths. The normal task-manager path
        // uses orphanPattern.parentPath to classify all rootfs paths
        // under distros/ first, then assigns the extracted slot id to
        // a known install or orphan.
        val sortedInstalls = installs
            .map { (path, id) -> RootfsPrefix(path.trimEnd('/'), id) }
            .sortedByDescending { it.rootfs.length }
        val classifier = RootfsClassifier(
            distrosParent = orphanPattern?.parentPath?.trimEnd('/'),
            knownIds = orphanPattern?.knownIds.orEmpty(),
            scopedInstalls = sortedInstalls,
        )

        val out = mutableListOf<ProcessInfo>()
        val procDir = File("/proc")
        val entries = procDir.list() ?: return emptyList()

        for (name in entries) {
            val pid = name.toIntOrNull() ?: continue
            if (pid == ownPid) continue
            val procEntry = "/proc/$pid"

            var matchedInstall: String? = null
            var matchedOrphan: String? = null
            val match = classify(procEntry, classifier)
            if (match != null) {
                matchedInstall = match.ownerInstallId
                matchedOrphan = match.orphanRootfsId
            }
            // Only read cmdline if we either need it for the result
            // (a path match hit) or for the cmdline substring filter.
            val needCmdline = matchedInstall != null || matchedOrphan != null ||
                extraCmdlinePath != null
            val cmdline = if (needCmdline) readCmdline("$procEntry/cmdline") else ""
            if (matchedInstall == null && matchedOrphan == null && extraCmdlinePath != null) {
                if (cmdline.contains(extraCmdlinePath)) {
                    matchedInstall = extraCmdlineId
                }
            }
            if (matchedInstall == null && matchedOrphan == null) continue

            val comm = readSmall("$procEntry/comm")?.trim().orEmpty()
            val parentPid = readParentPid("$procEntry/stat")
            val guestCommand = guestCommand(cmdline, comm, match?.commandPath)
            val cwd = guestCwd(procEntry, classifier)
            out += ProcessInfo(
                pid = pid,
                parentPid = parentPid,
                ownerInstallId = matchedInstall,
                orphanRootfsId = matchedOrphan,
                comm = comm,
                cmdline = cmdline,
                cwd = cwd,
                guestCommand = guestCommand,
                displayCommand = binaryName(guestCommand),
                requiresSu = false,
            )
        }
        return out
    }

    private fun readParentPid(path: String): Int {
        val stat = readSmall(path) ?: return 0
        val afterComm = stat.substringAfterLast(") ", missingDelimiterValue = "")
        if (afterComm.isBlank()) return 0
        val parts = afterComm.trim().split(WHITESPACE, limit = 3)
        return parts.getOrNull(1)?.toIntOrNull() ?: 0
    }

    private fun guestCwd(procEntry: String, classifier: RootfsClassifier): String {
        val cwd = readlinkOrNull("$procEntry/cwd") ?: return ""
        return classifier.match(cwd)?.commandPath ?: cwd
    }

    /**
     * Installed slots beat orphan slots across all signals. Link checks
     * are the cheap ownership path; maps gives tawcroot rows the guest
     * executable because kernel cmdline still names the wrapper.
     */
    private fun classify(
        procEntry: String,
        classifier: RootfsClassifier,
    ): RootfsMatch? {
        val paths = arrayOf(
            readlinkOrNull("$procEntry/cwd"),
            readlinkOrNull("$procEntry/exe"),
            readlinkOrNull("$procEntry/root"),
        )
        val linkMatch = firstRootfsMatch(paths.asSequence(), classifier)
        val mapsMatch = firstMapsMatch("$procEntry/maps", classifier)

        if (linkMatch != null) {
            if (mapsMatch != null &&
                mapsMatch.ownerInstallId == linkMatch.ownerInstallId &&
                mapsMatch.orphanRootfsId == linkMatch.orphanRootfsId
            ) {
                return linkMatch.copy(commandPath = mapsMatch.commandPath)
            }
            return linkMatch
        }
        return mapsMatch
    }

    private fun readlinkOrNull(path: String): String? = try {
        val raw = Os.readlink(path) ?: return null
        // The kernel appends " (deleted)" to readlink output for
        // unlinked-but-still-open files. Strip it so prefix matching
        // still works for orphans whose rootfs has been wiped.
        if (raw.endsWith(" (deleted)")) raw.removeSuffix(" (deleted)") else raw
    } catch (_: Throwable) {
        null
    }

    /** Up to 4 KiB; comm is short, cmdline is bounded by ARG_MAX. */
    private fun readSmall(path: String): String? = try {
        File(path).inputStream().use { s ->
            val buf = ByteArray(4096)
            val n = s.read(buf)
            if (n <= 0) "" else String(buf, 0, n, Charsets.UTF_8)
        }
    } catch (_: Throwable) {
        null
    }

    /** `/proc/<pid>/cmdline` is NUL-separated; render as space-separated. */
    private fun readCmdline(path: String): String {
        val raw = readSmall(path) ?: return ""
        return raw.trimEnd('\u0000').replace('\u0000', ' ')
    }

    private fun guestCommand(
        cmdline: String,
        comm: String,
        mappedGuestPath: String?,
    ): String {
        val cleanWrapper = strippedTawcrootCommand(cmdline)
        return when {
            cleanWrapper.isNotBlank() -> cleanWrapper
            isTawcrootExecChild(cmdline) -> mappedGuestPath ?: UNKNOWN_COMMAND
            else -> cmdline.ifBlank { mappedGuestPath ?: comm.ifBlank { UNKNOWN_COMMAND } }
        }
    }

    private fun isTawcrootExecChild(cmdline: String): Boolean {
        val args = cmdline.split(' ').filter { it.isNotEmpty() }
        return args.size >= 3 && isTawcrootArg(args[0]) && args[1] == "--exec-child"
    }

    private fun strippedTawcrootCommand(cmdline: String): String {
        val args = cmdline.split(' ').filter { it.isNotEmpty() }
        val marker = args.indexOf("--")
        if (marker < 0 || marker + 1 >= args.size) return ""
        if (!args.take(marker).any { isTawcrootArg(it) }) return ""
        var start = marker + 1
        if (start + 1 < args.size && args[start].endsWith("/env") && args[start + 1] == "-i") {
            start += 2
            while (start < args.size && ROOTFS_ENV_ASSIGNMENT.matches(args[start])) start++
        }
        if (start >= args.size) return ""
        if (start + 2 < args.size &&
            args[start].endsWith("/bash") &&
            args[start + 1] == "-lc"
        ) {
            return args.drop(start + 2).joinToString(" ")
        }
        return args.drop(start).joinToString(" ")
    }

    private fun isTawcrootArg(arg: String): Boolean =
        arg == "tawcroot" || arg.endsWith("/libtawcroot.so")

    private fun firstMapsMatch(path: String, classifier: RootfsClassifier): RootfsMatch? = try {
        var firstOrphan: RootfsMatch? = null
        var firstInstalled: RootfsMatch? = null
        File(path).bufferedReader().use { reader ->
            while (true) {
                val line = reader.readLine() ?: break
                val candidate = executableMapsPath(line) ?: continue
                val match = classifier.match(candidate) ?: continue
                if (match.ownerInstallId != null) {
                    if (isPrimaryExecutablePath(match.commandPath)) return match
                    if (firstInstalled == null) firstInstalled = match
                    continue
                }
                if (isPrimaryExecutablePath(match.commandPath)) return match
                if (firstOrphan == null) firstOrphan = match
            }
        }
        firstInstalled ?: firstOrphan
    } catch (_: Throwable) {
        null
    }

    private fun isPrimaryExecutablePath(path: String?): Boolean {
        if (path.isNullOrBlank()) return false
        val name = path.substringAfterLast('/')
        if (name.startsWith("ld-") || name.startsWith("ld-linux")) return false
        if (name.endsWith(".so") || ".so." in name) return false
        return true
    }

    private fun binaryName(command: String): String {
        if (command == UNKNOWN_COMMAND) return command
        val first = command.trim().split(WHITESPACE, limit = 2).firstOrNull().orEmpty()
        return first.substringAfterLast('/').ifBlank { UNKNOWN_COMMAND }
    }

    private fun executableMapsPath(line: String): String? {
        val parts = line.trimStart().split(MAPS_FIELD_SPLIT, limit = 6)
        if (parts.size < 6) return null
        val perms = parts[1]
        if (perms.length < 3 || perms[2] != 'x') return null
        val path = stripDeletedSuffix(parts[5])
        return if (path.startsWith("/") && !path.startsWith("[")) path else null
    }

    private fun firstRootfsMatch(
        paths: Sequence<String?>,
        classifier: RootfsClassifier,
    ): RootfsMatch? {
        var firstOrphan: RootfsMatch? = null
        for (path in paths) {
            val match = classifier.match(path ?: continue) ?: continue
            if (match.ownerInstallId != null) return match
            if (firstOrphan == null) firstOrphan = match
        }
        return firstOrphan
    }

    private fun stripDeletedSuffix(path: String): String =
        if (path.endsWith(" (deleted)")) path.removeSuffix(" (deleted)") else path

    private fun pathInRootfs(path: String, rootfs: String): Boolean =
        path == rootfs || path.startsWith("$rootfs/")

    private data class RootfsPrefix(val rootfs: String, val id: String)

    private data class RootfsMatch(
        val ownerInstallId: String?,
        val orphanRootfsId: String?,
        val commandPath: String?,
    )

    private class RootfsClassifier(
        private val distrosParent: String?,
        private val knownIds: Set<String>,
        private val scopedInstalls: List<RootfsPrefix>,
    ) {
        fun match(rawPath: String): RootfsMatch? {
            val path = stripDeletedSuffix(rawPath)
            matchDistrosSlot(path)?.let { return it }

            for (install in scopedInstalls) {
                if (pathInRootfs(path, install.rootfs)) {
                    return RootfsMatch(
                        ownerInstallId = install.id,
                        orphanRootfsId = null,
                        commandPath = guestPathForRootfsPath(path, install.rootfs),
                    )
                }
            }
            return null
        }

        private fun matchDistrosSlot(path: String): RootfsMatch? {
            val parent = distrosParent ?: return null
            val prefix = "$parent/"
            if (!path.startsWith(prefix)) return null
            val rest = path.substring(prefix.length)
            val slotEnd = rest.indexOf('/')
            if (slotEnd <= 0) return null
            val afterSlot = rest.substring(slotEnd + 1)
            if (afterSlot != "rootfs" && !afterSlot.startsWith("rootfs/")) return null
            val commandPath = when {
                afterSlot == "rootfs" -> "/"
                else -> "/" + afterSlot.removePrefix("rootfs/")
            }
            return rest.substring(0, slotEnd)
                .let { slot ->
                    if (slot in knownIds) {
                        RootfsMatch(
                            ownerInstallId = slot,
                            orphanRootfsId = null,
                            commandPath = commandPath,
                        )
                    } else {
                        RootfsMatch(
                            ownerInstallId = null,
                            orphanRootfsId = slot,
                            commandPath = commandPath,
                        )
                    }
                }
        }

        private fun guestPathForRootfsPath(path: String, rootfs: String): String =
            if (path == rootfs) "/" else "/" + path.removePrefix("$rootfs/")
    }

    /** See [scan]. */
    data class OrphanPattern(val parentPath: String, val knownIds: Set<String>)

    private val MAPS_FIELD_SPLIT = Regex("\\s+")
    private val WHITESPACE = Regex("\\s+")
    private val ROOTFS_ENV_ASSIGNMENT = Regex("[A-Za-z_][A-Za-z0-9_]*=.*")
    private const val UNKNOWN_COMMAND = "unknown command"
}
