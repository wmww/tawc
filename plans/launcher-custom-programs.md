# Custom programs in the launcher — terminal entries, user dirs, .desktop editor

Make the launcher usable for the user's own programs, including CLI
scripts. Three parts: route `Terminal=true` entries into tawc's native
terminal, scan the user-writable `.desktop` dirs, and add a minimal
in-app `.desktop` editor for personal entries.
**Depends on [launcher-entry-management.md](launcher-entry-management.md)**
(action menu, overflow menu, `entry.path`, `EntryLauncher`,
`launcher-list` broker action).

## A. `Terminal=true` → native terminal

Today `LauncherActivity.launchEntry` ignores `entry.terminal` and runs
everything headless with stdio to `/dev/null` — a terminal program runs
invisibly or dies. New behavior in `EntryLauncher`:

- **tawcroot installs**: terminal entries open `TerminalActivity` with
  the existing `EXTRA_ID`, the per-distro document URI
  (`tawc://terminal/<id>`), and a new `EXTRA_COMMAND` carrying
  `entry.exec`.
- **proot/chroot**: unchanged headless launch + a logcat warn. The
  terminal is tawcroot-only by design (see the gate in MainActivity and
  the TerminalActivity class kdoc); those methods are debug-only.

### Spawn path

`TawcrootMethod.ptyShellExec` gains an optional command:

```kotlin
fun ptyShellExec(rootfs: String, graphics: GraphicsBackend? = null,
                 command: String? = null): PtyExec
```

`command == null` → `-l` as today; else `-lc <wrapped>` (login shell so
profile env still fires, matching `startInside`).

### Hold-open wrapper

`onSessionFinished` removes the tab immediately, so a script's output
would vanish at exit. Wrap the command (one constant next to the spawn
code):

```
<exec>; __c=$?; printf '\n[exited %d — press any key]\n' "$__c"; read -rsn1
```

The session is still alive during `read`, so no session-lifecycle
changes: a keypress ends the shell and the normal tab-removal flow
runs. `exec` is a shell fragment (same trust level as today's
`"${entry.exec} </dev/null …"` concatenation), so `;`-joining is fine.

### TerminalActivity plumbing

- `onCreate` with `EXTRA_COMMAND`: spawn the command session (no extra
  default shell) and consume the extra (`intent.removeExtra` /
  `setIntent`) so recreation doesn't respawn it.
- `documentLaunchMode="intoExisting"` delivers repeat launches for the
  same distro URI to `onNewIntent` — implement it: spawn a new tab
  running the command and select it (default-shell launches keep
  today's behavior: just bring the task forward).
- Tab label: use the entry name (pass it as an extra) until an OSC
  title arrives — `labelFor` already prefers `session.title`.

## B. Scan directories

`launcher.rs::APPS_SUBDIRS`:

- `usr/local/share/applications` is **already scanned** — no change,
  but assert it in the new tests.
- Add `root/.local/share/applications` — the guest runs as (fake) root,
  so `$HOME` is `/root` and this is the XDG per-user dir.

**Precedence fix** (flagged by the existing comment in
`scan_entries`): de-dup currently runs *after* the name-sort, so the
per-id winner is name-order-arbitrary, not directory-priority. Reorder
`APPS_SUBDIRS` user-first (`root/.local`, `usr/local`, `usr/share`,
flatpak, snap) and de-dup by id in walk order *before* sorting, so a
user's entry shadows the packaged one — required for "hide the packaged
entry behind my edited copy" workflows.

`resolve_metadata_for_app_id` shares `scan_entries`, so window
icons/titles pick up user entries too. Fine.

## C. `.desktop` editor

### Managed dir = the package-manager boundary

- **`/root/.local/share/applications/`** is the managed dir: the editor
  creates files only here, and only files here are editable/deletable
  ("not package-managed" is tracked by directory, nothing else).
  `/usr/local` stays read-only to the app — technically not
  package-managed either, but `make install`-style entries there are
  exactly the complex foreign files the editor shouldn't touch.
- Editable check is Kotlin-side: `entry.path` (plan 1) starts with
  `<rootfs>/root/.local/share/applications/`.
- Method gate: writes are plain Kotlin file I/O into the rootfs, which
  works for tawcroot/proot (app-uid-owned) but not chroot (root-owned,
  see notes/launcher.md "Access model") — hide New/Edit for chroot
  installs, consistent with the terminal gating.

### Editor activity

`launcher/DesktopFileEditorActivity` (`exported="false"`), hand-built
form matching the app's UI style. Fields: **Name** (required),
**Exec** (required), **Comment**, **Terminal** checkbox, **Icon**
(optional freeform `Icon=` value — resolved by the existing
`resolve_icon` on next scan; no picker). Explicit non-goals: locale
keys, actions, `%f`-style field codes, multiple groups — this makes
personal launchers, not production `.desktop` files.

Saving writes the file wholesale:

```
[Desktop Entry]
Type=Application
Name=<name>
Exec=<exec>
Comment=<comment>        (if set)
Icon=<icon>              (if set)
Terminal=true            (if set)
```

- Values are written verbatim minus stripped newlines; validate
  Name/Exec non-empty. Keys written are standard, so the
  `freedesktop-desktop-entry` scan round-trips them.
- New file: filename = `Installation.slugifyLabel`-style slug of Name +
  `.desktop`, `-2`/`-3` suffix on collision; `mkdirs` the managed dir.
  Editing keeps the existing filename (the id is the filename — pins
  and hidden-state reference it).
- Foreign files in the managed dir (keys/groups outside the editor's
  set): load the known keys, and show a notice that saving rewrites the
  file and drops the rest. "Works well for files it created" is the
  contract; anything else gets a warning, not silent data loss.
- **Delete** button (with confirm) for managed files — cheap and
  obviously paired with create.

### Entry points

- Plan 1's overflow menu gains **"New program…"** (gated on method, as
  above).
- Plan 1's entry action menu gains **"Edit"**, shown only when
  `entry.path` is under the managed dir.
- After save/delete, the launcher rescans and re-renders.

## Tests

Host `cargo test` for the compositor crate is unavailable (ndk-sys is
Android-only), so scanner behavior is verified through the app:

- Integration (`tests/integration`), using plan 1's `launcher-list`
  broker action: drop `.desktop` fixtures into a test rootfs via the
  broker (`RUNINSIDE` + heredoc) covering
  - a file in `/root/.local/share/applications` appears in the list;
  - one in `/usr/local/share/applications` appears;
  - same id in `root/.local` and `usr/share` → the user copy's
    name/exec wins (precedence fix);
  - `Terminal=true` is reported.
- Terminal routing: manual on `.tawctarget` — launch a `Terminal=true`
  entry, see output in the native terminal, `[exited N]` hold, keypress
  closes the tab; second launch while the terminal task is open adds a
  tab (onNewIntent path).
- Editor: app unit test for the serializer (fields → file text →
  reparse) and the slug/collision logic; manual create/edit/delete on
  device, confirming the launcher and a pinned shortcut (plan 2) pick
  up the edit.

## Doc updates (same change)

- notes/launcher.md: scan-dir list + precedence rule, terminal routing
  + hold-open wrapper, managed-dir contract and editor scope.
- This plan is deleted and folded into notes/launcher.md when done.
