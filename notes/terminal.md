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

`TerminalSessions` is a process-wide map of installation id →
`TerminalSession`: one shell per distro, reattached on reopen/rotation
(`TerminalActivity` uses the CompositorActivity document trick —
`documentLaunchMode="intoExisting"` + `tawc://terminal/<id>` URI — for
one activity/recents card per distro). Shell exit finishes the
activity and drops the session. No foreground service: sessions die if
Android kills the app process in the background; promote to a service
only if that becomes a real complaint.

The compositor is *not* started or waited for. The Wayland/X11 env
vars are still set, so GUI apps launched from the terminal connect
only if a compositor session is already up; CLI work needs nothing.
