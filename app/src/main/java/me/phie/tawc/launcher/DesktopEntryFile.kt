package me.phie.tawc.launcher

import java.io.File
import java.nio.file.Files

/**
 * Read-only bits of the `.desktop` format TAWC still needs: the old
 * editor's keys ([parse]) and its `env` Exec prefix ([splitExec]) for
 * [LauncherMigration], and argv0 for [EntryLauncher.guiCommand]. TAWC
 * no longer writes `.desktop` files (notes/launcher.md "Entry store").
 */
internal object DesktopEntryFile {

    private const val GROUP = "[Desktop Entry]"

    /**
     * The readable file behind [path], if it is a regular file inside
     * [rootfs]; else null. Symlinks resolve as the guest sees them
     * (absolute targets re-root at [rootfs]): packaged entries like
     * LibreOffice's are absolute links that dangle on the host.
     */
    fun fileInRootfs(path: String, rootfs: File): File? {
        val root = canonical(rootfs)
        // Canonicalize the parent only: the leaf may dangle on the host.
        val raw = File(path)
        val f = File(canonical(raw.parentFile ?: return null), raw.name)
        val rel = f.path.removePrefix(root.path + "/").takeIf { it != f.path } ?: return null
        return resolveInRootfs(root, rel)?.takeIf { it.isFile }
    }

    /** Mirrors the Rust scanner's `resolve_in_rootfs`: walk [rel]
     *  under canonical [root], following symlinks without leaving it. */
    private fun resolveInRootfs(root: File, rel: String): File? {
        val todo = ArrayDeque(rel.split('/'))
        var cur = root
        var hops = 0
        while (todo.isNotEmpty()) {
            val comp = todo.removeFirst()
            when (comp) {
                "", "." -> continue
                ".." -> { if (cur != root) cur = cur.parentFile ?: root; continue }
            }
            val next = File(cur, comp)
            val link = next.toPath()
            if (!Files.isSymbolicLink(link)) { cur = next; continue }
            if (++hops > 40) return null
            val target = Files.readSymbolicLink(link).toString()
            if (target.startsWith("/")) cur = root
            target.split('/').asReversed().forEach { todo.addFirst(it) }
        }
        return cur
    }

    private fun canonical(f: File): File = runCatching { f.canonicalFile }.getOrDefault(f)

    /** The old editor's keys. */
    data class Draft(
        val name: String = "",
        val exec: String = "",
        val comment: String = "",
        val icon: String = "",
        val terminal: Boolean = false,
    )

    /** The old editor's keys from [text]'s `[Desktop Entry]` group. */
    fun parse(text: String): Draft {
        var draft = Draft()
        var inGroup = false
        for (raw in text.lines()) {
            val line = raw.trim()
            if (line.startsWith("[")) {
                inGroup = line == GROUP
                continue
            }
            if (!inGroup || line.startsWith("#")) continue
            val eq = line.indexOf('=')
            if (eq <= 0) continue
            val value = line.substring(eq + 1).trim()
            draft = when (line.substring(0, eq).trim()) {
                "Name" -> draft.copy(name = value)
                "Exec" -> draft.copy(exec = value)
                "Comment" -> draft.copy(comment = value)
                "Icon" -> draft.copy(icon = value)
                "Terminal" -> draft.copy(terminal = value.equals("true", ignoreCase = true))
                else -> draft
            }
        }
        return draft
    }

    /** An `Exec=` value split into leading `env` assignments (the XDG
     *  way to set variables for an entry) and the command. */
    data class ExecLine(val env: List<Pair<String, String>>, val command: String)

    private val ENV_NAME = Regex("[A-Za-z_][A-Za-z0-9_]*")

    fun isValidEnvName(name: String): Boolean = ENV_NAME.matches(name)

    /**
     * Split a leading `env NAME=value …` off [exec]. Anything else —
     * no `env`, `env` options like `-i`, no command after the
     * assignments, an unterminated quote — comes back as all command,
     * so nothing is lost from an Exec it can't represent.
     * Values unquote with shell rules: `'…'` literal, `"…"` with `\`
     * escaping `"`, `` ` ``, `$` and `\`, bare `\x` → `x`; `%%` is the
     * Exec spec's literal `%`.
     */
    fun splitExec(exec: String): ExecLine {
        val whole = ExecLine(emptyList(), exec)
        var i = exec.indexOfFirst { !it.isWhitespace() }
        if (i < 0 || !exec.startsWith("env", i) || exec.getOrNull(i + 3)?.isWhitespace() != true) return whole
        i += 3
        val env = ArrayList<Pair<String, String>>()
        while (true) {
            while (i < exec.length && exec[i].isWhitespace()) i++
            val eq = exec.indexOf('=', i)
            if (eq < 0 || !isValidEnvName(exec.substring(i, eq))) break
            val name = exec.substring(i, eq)
            val value = StringBuilder()
            var j = eq + 1
            while (j < exec.length && !exec[j].isWhitespace()) {
                when (val c = exec[j]) {
                    '\'' -> {
                        val end = exec.indexOf('\'', j + 1)
                        if (end < 0) return whole
                        value.append(exec, j + 1, end)
                        j = end + 1
                    }
                    '"' -> {
                        j++
                        while (j < exec.length && exec[j] != '"') {
                            if (exec[j] == '\\' && j + 1 < exec.length && exec[j + 1] in "\"`$\\") j++
                            value.append(exec[j++])
                        }
                        if (j >= exec.length) return whole
                        j++
                    }
                    '\\' -> {
                        if (j + 1 >= exec.length) return whole
                        value.append(exec[j + 1])
                        j += 2
                    }
                    else -> { value.append(c); j++ }
                }
            }
            env += name to value.toString().replace("%%", "%")
            i = j
        }
        val command = exec.substring(i).trim()
        return if (env.isEmpty() || command.isEmpty()) whole else ExecLine(env, command)
    }
}
