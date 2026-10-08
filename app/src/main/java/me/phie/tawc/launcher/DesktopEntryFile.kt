package me.phie.tawc.launcher

import java.io.File
import me.phie.tawc.install.Installation

/**
 * Read/write model for the `.desktop` files the in-app editor
 * ([DesktopFileEditorActivity]) edits. Every write lands in
 * [MANAGED_SUBDIR]; editing a packaged entry writes an override copy
 * there that shadows it (the scanner's user-first de-dup). Existing
 * files are [patch]ed — only the editor's keys change, everything else
 * survives byte for byte; [serialize] is for brand-new entries only.
 */
internal object DesktopEntryFile {

    /** The managed dir under a rootfs: the XDG per-user applications
     *  dir (the guest runs as fake root, so `$HOME` is `/root`). */
    const val MANAGED_SUBDIR = "root/.local/share/applications"

    private const val GROUP = "[Desktop Entry]"

    /**
     * Canonicalized so path comparisons work across the
     * `/data/user/0/<pkg>` (Kotlin's `context.dataDir`) vs
     * `/data/data/<pkg>` (symlinked; the Rust scanner canonicalizes its
     * walk roots, so [LauncherEntry.path] uses this form) split.
     */
    fun managedDir(rootfs: File): File = File(canonical(rootfs), MANAGED_SUBDIR)

    /** Is [entryPath] (a [LauncherEntry.path]) inside [rootfs]'s managed dir? */
    fun isManaged(entryPath: String, rootfs: File): Boolean =
        canonical(File(entryPath)).path.startsWith(managedDir(rootfs).path + "/")

    /** [path] canonicalized, if it is a regular file inside [rootfs]; else null. */
    fun fileInRootfs(path: String, rootfs: File): File? {
        val f = canonical(File(path))
        return f.takeIf { it.path.startsWith(canonical(rootfs).path + "/") && it.isFile }
    }

    /**
     * The desktop id of the file at [path], derived the way the
     * scanner's `freedesktop-desktop-entry` crate does: the part after
     * the last `/applications/`, `/` → `-`, minus `.desktop`. So
     * `usr/share/applications/kde4/foo.desktop` is `kde4-foo`.
     */
    fun idFor(path: String): String {
        val rel = path.substringAfterLast("/applications/", File(path).name)
        return rel.removeSuffix(".desktop").replace('/', '-')
    }

    /**
     * Where an edit of [source] is saved: [source] itself when it is
     * already managed, else `<managedDir>/<id>.desktop` — same id, so
     * the copy shadows the packaged file and inherits its hide state
     * and pins.
     */
    fun targetFor(source: File, rootfs: File): File =
        if (isManaged(source.path, rootfs)) source
        else File(managedDir(rootfs), "${idFor(source.path)}.desktop")

    private fun canonical(f: File): File = runCatching { f.canonicalFile }.getOrDefault(f)

    /** The editor's field set. Values are kept verbatim except newlines
     *  (stripped on write — a `.desktop` value is one line). */
    data class Draft(
        val name: String = "",
        val exec: String = "",
        val comment: String = "",
        val icon: String = "",
        val terminal: Boolean = false,
    )

    fun serialize(draft: Draft): String = buildString {
        appendLine(GROUP)
        appendLine("Type=Application")
        appendLine("Name=${oneLine(draft.name)}")
        appendLine("Exec=${oneLine(draft.exec)}")
        oneLine(draft.comment).takeIf { it.isNotEmpty() }?.let { appendLine("Comment=$it") }
        oneLine(draft.icon).takeIf { it.isNotEmpty() }?.let { appendLine("Icon=$it") }
        if (draft.terminal) appendLine("Terminal=true")
    }

    private fun oneLine(value: String): String =
        value.replace('\n', ' ').replace('\r', ' ').trim()

    /** The editor's keys from [text]'s `[Desktop Entry]` group. */
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

    /**
     * [source] with [draft] applied: inside `[Desktop Entry]`, each
     * changed key's value is replaced in place (appended to the group
     * when absent; an empty Icon/Comment removes the key, a false
     * Terminal is only written when the key exists). A changed
     * Name/Comment also drops its locale variants (`Name[de]=`), which
     * would otherwise keep winning. Every other line, group and
     * comment is kept byte for byte, so an unchanged draft returns
     * [source] unchanged.
     */
    fun patch(source: String, draft: Draft): String {
        val old = parse(source)
        val lines = source.split('\n').toMutableList()
        val start = lines.indexOfFirst { it.trim() == GROUP }
        if (start < 0) return serialize(draft) + source

        // Key → new value (null = remove), for the keys that changed.
        val changes = LinkedHashMap<String, String?>()
        fun change(key: String, oldValue: String, newValue: String, dropEmpty: Boolean) {
            val v = oneLine(newValue)
            if (v != oldValue) changes[key] = if (dropEmpty && v.isEmpty()) null else v
        }
        change("Name", old.name, draft.name, dropEmpty = false)
        change("Exec", old.exec, draft.exec, dropEmpty = false)
        change("Comment", old.comment, draft.comment, dropEmpty = true)
        change("Icon", old.icon, draft.icon, dropEmpty = true)
        if (draft.terminal != old.terminal) changes["Terminal"] = draft.terminal.toString()
        if (changes.isEmpty()) return source
        val localized = setOf("Name", "Comment").filter { it in changes }

        var end = start + 1
        while (end < lines.size && !lines[end].trim().startsWith("[")) end++
        val seen = HashSet<String>()
        val out = ArrayList<String>(lines.size + changes.size)
        out.addAll(lines.subList(0, start + 1))
        for (raw in lines.subList(start + 1, end)) {
            val line = raw.trim()
            val eq = line.indexOf('=')
            val key = if (line.startsWith("#") || eq <= 0) null else line.substring(0, eq).trim()
            when {
                key != null && key in changes -> {
                    seen += key
                    val v = changes[key] ?: continue
                    out += "$key=$v" + if (raw.endsWith("\r")) "\r" else ""
                }
                key != null && localized.any { key.startsWith("$it[") } -> Unit
                else -> out += raw
            }
        }
        // Missing keys go after the group's last non-blank line.
        val crlf = lines[start].endsWith("\r")
        val missing = changes.filter { (k, v) -> k !in seen && v != null && !(k == "Terminal" && v == "false") }
        var at = out.size
        while (at > start + 1 && out[at - 1].isBlank()) at--
        out.addAll(at, missing.map { (k, v) -> "$k=$v" + if (crlf) "\r" else "" })
        out.addAll(lines.subList(end, lines.size))
        return out.joinToString("\n")
    }

    /** An `Exec=` value as the editor shows it: `env` assignments
     *  (the XDG way to set variables for an entry) and the command. */
    data class ExecLine(val env: List<Pair<String, String>>, val command: String)

    private val ENV_NAME = Regex("[A-Za-z_][A-Za-z0-9_]*")
    private val BARE_VALUE = Regex("[A-Za-z0-9_./:,@%+=-]*")

    fun isValidEnvName(name: String): Boolean = ENV_NAME.matches(name)

    /**
     * Split a leading `env NAME=value …` off [exec]. Anything else —
     * no `env`, `env` options like `-i`, no command after the
     * assignments, an unterminated quote — comes back as all command,
     * so the form never loses part of an Exec it can't represent.
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

    /** [line] back to an `Exec=` value; values that need it are
     *  double-quoted (valid for both the Exec spec and bash), and `%`
     *  doubled so the scanner doesn't take it for a field code. */
    fun joinExec(line: ExecLine): String {
        if (line.env.isEmpty()) return line.command
        val vars = line.env.joinToString(" ") { (k, v) -> "$k=${quoteValue(v).replace("%", "%%")}" }
        return "env $vars ${line.command.trim()}"
    }

    private fun quoteValue(v: String): String =
        if (v.isNotEmpty() && BARE_VALUE.matches(v)) v
        else "\"" + v.replace(Regex("[\"`$\\\\]")) { "\\" + it.value } + "\""

    /**
     * Pick a filename for a new entry named [name] in [dir]:
     * `<slug>.desktop` with a `-2` / `-3` … suffix on collision. The
     * filename is the entry id (pins and hidden-state reference it), so
     * editing an existing file must keep its name — this is only for
     * creation. Unslugifiable names (punctuation-only) fall back to
     * "program".
     */
    fun newFile(dir: File, name: String): File {
        val slug = Installation.slugifyLabel(name) ?: "program"
        var candidate = File(dir, "$slug.desktop")
        var n = 2
        while (candidate.exists()) {
            candidate = File(dir, "$slug-$n.desktop")
            n++
        }
        return candidate
    }
}
