package me.phie.tawc.install

/**
 * tawc's default interactive-shell config (colored prompt, color
 * aliases), split across two ownership domains:
 *
 *  - [GUEST_BASHRC_PATH] — app-owned, shipped by
 *    [ShellDefaultsInstallProvider] and refreshed in every rootfs on
 *    app upgrade. Defaults can change later and existing installs
 *    pick them up.
 *  - `/root/.bashrc` + `/root/.bash_profile` — user-owned stubs
 *    written exactly once at install configure time ([configureScript]).
 *    `.bashrc` just sources the app-owned file (guarded), so the user
 *    can delete that line to opt out permanently on that rootfs.
 *    `.bash_profile` makes login shells (`bash -l`, the terminal's
 *    spawn mode) read `.bashrc` on every distro family.
 *
 * This is deliberately NOT under `/etc/profile.d/` — profile.d runs
 * early in login-shell startup and the distros' shipped bashrc files
 * would clobber PS1 afterwards; `~/.bashrc` is the file that wins.
 * The "no env state under /etc/profile.d/" policy
 * (notes/installation.md) is unaffected: env still comes solely from
 * [RootfsEnv] on every spawn.
 */
internal object ShellDefaults {
    /** App-owned defaults file inside the rootfs (tawc namespace).
     *  NOT under `/usr/share/tawc/` — that dir is bind-mounted over
     *  with `<appData>/share` (wayland socket) at runtime, which
     *  would shadow anything the installer writes there. */
    const val GUEST_BASHRC_PATH = "/usr/lib/tawc/bashrc"

    /**
     * Contents of [GUEST_BASHRC_PATH]. The user is always root, so
     * `\u@\h` carries no information — the prompt is just the cwd
     * (bold green) plus `\$`, which keeps it short on small screens.
     * The xterm title (= terminal tab label) is cwd-only for the same
     * reason. Embedded in PS1 rather than PROMPT_COMMAND so it's
     * emitted after any distro PROMPT_COMMAND title each prompt
     * (last escape wins), while apps that set their own title (vim,
     * htop) still show through while running.
     */
    val GUEST_BASHRC_CONTENT = """
        # tawc shell and prompt defaults:
        case ${'$'}- in *i*) ;; *) return ;; esac
        PS1='\[\e[1;32m\]\w\[\e[0m\] \${'$'} '
        case ${'$'}TERM in xterm*) PS1='\[\e]0;\w\a\]'${'$'}PS1 ;; esac
        alias ls='ls --color=auto'
        alias grep='grep --color=auto'
    """.trimIndent() + "\n"

    /**
     * Shell fragment for the distro configure() scripts. Expects
     * `${'$'}ROOTFS` to be set; writes the one-time user-owned stubs.
     * Heredoc terminators must stay at column 0.
     */
    fun configureScript(): String = buildString {
        appendLine("# One-time shell-defaults stubs; user-owned after this.")
        appendLine("mkdir -p \"\$ROOTFS/root\"")
        appendLine("cat > \"\$ROOTFS/root/.bashrc\" <<'TAWC_BASHRC_EOF'")
        appendLine("# tawc shell and prompt defaults:")
        appendLine("[ -f $GUEST_BASHRC_PATH ] && . $GUEST_BASHRC_PATH")
        appendLine("TAWC_BASHRC_EOF")
        appendLine("cat > \"\$ROOTFS/root/.bash_profile\" <<'TAWC_BASH_PROFILE_EOF'")
        appendLine("[ -f ~/.bashrc ] && . ~/.bashrc")
        appendLine("TAWC_BASH_PROFILE_EOF")
    }
}
