# Terminal tabs

Replace TerminalActivity's scaffold toolbar (~56dp MaterialToolbar with
title + up arrow, mostly dead space) with a compact tab bar, and allow
multiple shell sessions per distro as tabs.

## UX spec

- Compact bar (~40dp) at the top of the terminal screen, replacing the
  toolbar entirely. No up arrow — system back/gesture already
  backgrounds the task and stays unchanged (`moveTaskToBack(true)`,
  shells keep running; the recents-card-swipe kill path is unaffected).
- Bar layout: `[ scrollable tab strip ……………… ][+]`. The strip is a
  `HorizontalScrollView` with `layout_weight=1`; the `+` button sits
  outside it, pinned at the right edge, always visible no matter how
  many tabs exist. The strip scrolls by horizontal swipe and
  auto-scrolls to reveal a newly created or newly selected tab.
- Each tab: ellipsized title (max width ~180dp) + a small `×` close
  button. Tapping the tab body selects it; tapping `×` kills that
  tab's shell and removes the tab.
- Selected tab is highlighted (filled), unselected tabs plain. Fixed
  dark palette regardless of day/night theme — the bar sits against
  the always-black terminal/extra-keys surface, so theme-following
  tonal colors would clash in light mode.
- `+` spawns a fresh shell via the existing `ptyShellExec` path,
  appends a tab, selects it.
- Closing the last tab (via `×` or shell exit) finishes the activity
  and drops the recents card (`finishAndRemoveTask`), same as shell
  exit today.

### Tab naming

Desktop-terminal style: the tab label is the session's xterm window
title (OSC 0/2), which the vendored termux emulator already parses
(`TerminalEmulator.mTitle`, surfaced as `TerminalSession.getTitle()`
with a `onTitleChanged` client callback that TerminalActivity
currently stubs out). Debian-family rootfses set
`user@host: ~/dir` automatically — their default PS1 includes the
title escape when `TERM` matches `xterm*`, and we export
`TERM=xterm-256color` — so labels track the cwd with zero work, and
apps that set their own title (vim, htop, ssh) show through.

Fallback while the title is null/blank (shell just spawned, or a
distro whose bashrc never sets one): static "Terminal" (new string
resource), matching gnome-terminal's fallback. Not the distro label —
every tab in this activity is the same distro, and it's too long for
a compact tab. Duplicate labels across tabs are fine (desktop
terminals have the same behavior).

## Architecture

### Session registry (TerminalSessions.kt)

Today: `HashMap<String, TerminalSession>`, one session per distro.
Becomes a per-distro ordered tab list plus selection:

```kotlin
internal object TerminalSessions {
    // per distro id: ordered live sessions + selected index
    fun list(id: String): List<TerminalSession>
    fun add(id: String, session: TerminalSession)        // appends
    fun remove(id: String, session: TerminalSession)     // drops if present, clamps selection
    fun selected(id: String): Int
    fun setSelected(id: String, index: Int)
}
```

All `@Synchronized`, registry stays dumb bookkeeping (order +
selection); tab policy (what to select after a close, when to finish)
lives in the activity. Selection is stored here so recreation
(rotation, system pressure) restores both the tab set and which tab
was showing. `DetachedTerminalClient` keeps its role — swapped in per
session on `onDestroy` — and its `onSessionFinished` now removes the
session from the list (covers a background-tab shell dying while no
activity is attached).

`TerminalSession`'s constructor is pure field assignment (no JNI until
the view sizes it), so the registry's list/selection bookkeeping is
unit-testable with real `TerminalSession` objects in a plain JVM test.

### One TerminalView, sessions swapped via attachSession()

Keep the single `TerminalView` + `ExtraKeysView`; switching tabs calls
`terminalView.attachSession(session)`. This is termux-app's own
multi-session pattern: `attachSession` resets the emulator/top-row
state and calls `updateSize()`, which (re)initializes or resizes the
session's pty (SIGWINCH) for the current view dimensions. Background
sessions keep a stale size until selected — harmless, standard
desktop-tab behavior. Pinch-zoom font size stays a single
activity-level value shared by all tabs; the extra-keys row needs no
changes (it targets the view, not the session).

A new tab is attached immediately on creation, so every session gets
its initial size from a live view — no session ever needs sizing
while hidden.

### Client callback routing

The activity remains the sole `TerminalSessionClient` for all live
sessions (each callback carries `changedSession`):

- `onTextChanged` / `onColorsChanged` / cursor: act only when
  `changedSession === activeSession`, otherwise drop (background tabs
  keep accumulating transcript via the pty reader threads regardless).
- `onTitleChanged`: look up the tab for `changedSession`, update its
  label whether or not it's selected.
- `onSessionFinished`: remove that session's tab (kill not needed —
  it already exited). If tabs remain, select the clamped neighbor and
  attach it; if none remain, `finishAndRemoveTask()` as today.
- Enter-on-dead-session in `onKeyDown` becomes "close that tab" instead
  of finishing the activity (only reachable in the race window before
  `onSessionFinished` lands).

`onCreate` reattach loop: for each registered session,
`updateTerminalSessionClient(this)`; attach the selected one.
`onDestroy`: swap every session to a `DetachedTerminalClient`; on the
swiped-from-recents path (existing `isFinishing && !taskInRecents`
check), `finishIfRunning()` + remove **all** sessions for the distro,
not just one.

### Tab bar widget

New `app/src/main/java/me/phie/tawc/terminal/TerminalTabBar.kt`: a
plain imperative custom view (horizontal `LinearLayout` containing the
`HorizontalScrollView` strip + the `+` `ImageButton`), consistent with
the app's no-XML-layout style. API sketch:

```kotlin
class TerminalTabBar(context) : LinearLayout {
    var onTabSelected: (Int) -> Unit
    var onTabCloseClicked: (Int) -> Unit
    var onNewTabClicked: () -> Unit
    fun addTab(label: CharSequence): Int   // returns index, scrolls into view
    fun removeTab(index: Int)
    fun setSelected(index: Int)            // restyles + scrolls into view
    fun setLabel(index: Int, label: CharSequence)
}
```

The activity maps tab index ↔ `TerminalSessions.list(distroId)` index
(same ordering — the bar never reorders). Needs two new vector
drawables, `ic_add` and `ic_close` (standard Material paths; the
existing drawable set has neither). Content descriptions ("New tab",
"Close tab") as string resources for TalkBack.

### Activity layout rework

`TerminalActivity.onCreate` stops using `buildChildScreen` and builds
its own vertical `LinearLayout`: tab bar (fixed ~40dp) / terminalView
(`weight=1`) / extraKeysView. The existing IME-inclusive insets
listener moves onto this root unchanged (it already replaced the
scaffold's listener wholesale, so nothing from Scaffold.kt is actually
needed anymore). Keep `setTitle(distro label)` on the activity so the
recents card and accessibility still name the screen.

### Failure modes

- `+` spawn failure (`ptyShellExec` IOException — revoked external
  bind etc.): toast the message, add no tab, activity stays up on the
  existing tabs. Only the initial spawn in `onCreate` keeps the
  current toast+`finish()` behavior (zero tabs = nothing to show).
- No cap on tab count: per-tab cost is one shell + a 4000-row
  transcript; the strip scrolls.

## Non-goals

- No foreground service: all of a distro's shells still die with the
  app process (unchanged policy from notes/terminal.md).
- No tab reordering/drag, no per-tab font size, no rename UI.
- No injecting PROMPT_COMMAND/PS1 into rootfses that don't set titles
  (Alpine/Arch will just show "Terminal"); revisit only if the static
  fallback proves annoying.
- Tabs are per-distro: one terminal activity/recents card per distro
  stays as is (`documentLaunchMode` trick untouched).

## Testing

- JVM unit tests (`./gradlew :app:testDebugUnitTest`) for the
  reworked `TerminalSessions`: add/remove ordering, selection
  clamping on close (first/middle/last/selected), remove-if-same
  guard, detached-client removal.
- Manual on-device pass (`.tawctarget` is currently `none` — needs a
  target set or `TAWC_TARGET=` override):
  - Open terminal on a Debian-family rootfs: tab label becomes
    `user@host: ~` and tracks `cd`; `vim` sets its own title.
  - `+` creates/selects a fresh shell; switching back retains the
    first tab's scrollback and running job (e.g. `top`).
  - `×` on background and selected tabs; `exit` in a background tab
    closes its tab without disturbing the selected one.
  - Last tab closed (either way) → activity and recents card gone.
  - Back backgrounds; reopening from home/recents restores all tabs,
    labels, and selection. Rotation likewise.
  - Many tabs: strip scrolls, `+` stays visible, new tab scrolls into
    view.
  - Swipe recents card → all shells for that distro die (check via
    `scripts/tawc-exec.sh` `ps`).
  - IME behavior unchanged: keyboard resizes terminal, prompt stays
    visible, extra-keys row works on whichever tab is selected.

## Deliverables / order of work

1. `TerminalSessions.kt` rework + JVM unit tests.
2. `TerminalTabBar.kt`, `ic_add`/`ic_close` drawables, strings.
3. `TerminalActivity.kt`: layout rework, tab wiring, per-session
   client routing, multi-session destroy/finish paths.
4. Docs: notes/terminal.md "Session model" section (list + selection,
   tab naming, fallback) and the TerminalActivity doc comment;
   architecture.md one-liner if it still says "one shell per distro".
