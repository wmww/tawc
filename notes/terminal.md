# In-App Terminal

A per-distro terminal (home-screen "Terminal" button under "Manage")
giving an interactive shell into an installed rootfs without the
compositor/graphics stack.

## Termux terminal modules

The UI is termux's terminal widget, vendored as `deps/termux-app`
(pinned in `deps/deps.list`) and wired in unpatched as Gradle included
projects `:terminal-emulator` and `:terminal-view`
(`settings.gradle.kts` `projectDir` redirects; the dep is ensured at
settings-evaluation time because included projects must exist before
configuration):

- `terminal-emulator` — VT emulation plus a small JNI
  (`libtermux.so`, built by ndk-build during the app build) that opens
  the pty pair, forks, `setsid()`s, wires the slave to stdio, sets a
  caller-supplied env, and execs an arbitrary argv.
- `terminal-view` — `TerminalView`, a plain `android.view.View` with
  IME/scroll/selection/mouse-reporting handling.

Both modules are **Apache-2.0** (the explicit exception in termux-app's
`LICENSE.md`; they descend from jackpal's Android-Terminal-Emulator).
Termux packages/bootstrap are not involved at all; the shell is the
distro's own `/bin/bash`.

The extra-keys row (ESC/TAB/CTRL/arrows above the IME) is termux's
`ExtraKeysView` + `TerminalExtraKeys`, cherry-picked from the
`termux-shared` module by the in-repo `:termux-extrakeys` shim
(`termux-extrakeys/build.gradle.kts`): it compiles just those classes
straight out of the vendored checkout (an include-filtered `srcDir`),
plus a local trimmed `ThemeUtils` stand-in so the rest of
termux-shared (Logger → guava, markwon, NDK code, ...) stays out of
the build. **License**: those classes are GPLv3-only, a deliberate
exception to the otherwise-MIT app (decided 2026-06; the repo sources
stay MIT, but distributed APKs are subject to GPLv3). Config is
termux's default double-row layout, inlined in `TerminalActivity`;
held CTRL/ALT/SHIFT/FN flow through the `read*Key()`
`TerminalViewClient` callbacks, same as termux.

The rest of `termux-shared` and the GPLv3 `app` module are still not
built or shipped.

The modules read `minSdkVersion`/`targetSdkVersion`/`compileSdkVersion`/
`ndkVersion` from root `gradle.properties` (keys added there; keep in
sync with `app/build.gradle.kts`).

Known wart: `TerminalSession.wrapFileDescriptor` reflects on the
private `FileDescriptor.descriptor` field (greylisted hidden API).
Works on current Android; termux and UserLAnd ship the same code.

## Spawn path

`TawcrootMethod.ptyShellExec` builds the same tawcroot envelope as
`startInside` (binds, `env -i` rootfs env, `bash -l`) minus the
`setsid` prefix — the termux JNI setsid()s the child itself, which
keeps the rootfs-session invariant (rootfs-sessions.md) and makes bash
the session leader of the pty, so job control/readline/curses work
(unlike the pipe-fed `RunCommandOp`/exec-broker paths). `TERM`/
`COLORTERM` are appended after the shared `RootfsEnv` map, which the
non-tty paths don't want.

tawcroot-only: chroot spawns via `su` (no pty fd to hand over) and
proot is dev-only, so the button is gated on
`Installation.method == tawcroot` + state READY.

## Session model

`TerminalSessions` is a process-wide registry of installation id → an
ordered list of `TerminalSession`s plus the selected index: multiple
shells per distro shown as tabs, reattached (sessions, labels, and
selection) on reopen/rotation (`TerminalActivity` uses the
CompositorActivity document trick — `documentLaunchMode="intoExisting"`
+ `tawc://terminal/<id>` URI — for one activity/recents card per
distro). The registry is dumb bookkeeping (`@Synchronized` order +
selection, JVM-unit-tested); tab policy lives in the activity. One
`TerminalView` shows the selected session via `attachSession()`
(termux-app's multi-session pattern: it resets emulator state and
`updateSize()`s, so background tabs keep a stale pty size until
selected). Last shell exiting (or its tab closed) finishes the
activity and drops the recents card; swiping the card kills all of the
distro's shells. No foreground service: sessions die if Android kills
the app process in the background; promote to a service only if that
becomes a real complaint.

Tab labels are the session's xterm window title (OSC 0/2, parsed by
the vendored emulator, surfaced via `TerminalSession.getTitle()` /
`onTitleChanged`), and apps that set their own title (vim, htop, ssh)
show through while running. tawc's shipped shell defaults
(`ShellDefaults`) set a cwd-only title (user@host carries no info —
always root@localhost) by embedding the escape in PS1, which is
emitted after any distro PROMPT_COMMAND title each prompt, so it wins
by default yet stays user-overridable like the rest of the defaults
file. On rootfses without the defaults (pre-ShellDefaults installs,
or sourcing opted out) the label is whatever the distro sets: Arch's
`/etc/bash.bashrc` PROMPT_COMMAND gives `root@localhost:~` (needs
`USER`, which `RootfsEnv` sets — login(1) never runs to set it),
Debian-root sets nothing (its escape lives only in
`/etc/skel/.bashrc`). A title of exactly `~` (the cwd default at
home, i.e. every fresh tab) is shown as `Term <n>` by tab position —
app-side, since the number must follow the index as tabs close.
While the title is null/blank the label is a static "Terminal";
duplicate labels are fine (desktop terminals behave the same).
Verified on-device 2026-06-10. The compact
`TerminalTabBar` (fixed dark palette against the always-black terminal
surface) replaced the scaffold toolbar; system back still just
backgrounds the task.

The compositor is *not* started or waited for. The Wayland/X11 env
vars are still set, so GUI apps launched from the terminal connect
only if a compositor session is already up; CLI work needs nothing.

## Running Android commands

`ando <cmd>` (installed in every rootfs at `/usr/local/bin/ando`) runs
a command as a plain Android process — useful from a terminal session
for `getprop`, `am`/`pm`, copying into shared storage, or `ando su -c
'…'` on rooted devices. Since the terminal child holds the real pty,
ando children inherit working tty semantics (no job control — the
rootfs shell owns the pty session). See [ando.md](ando.md).
