# Usecase test: classic X11 apps via Xwayland

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** physical only — SHM surfaces render black on the x86_64
emulator (`issues/emulator-shm-black-shader-translator.md`), and these
apps are SHM-rendered.
**Usecase:** a user runs old-school X11 programs; Xwayland is supposed to make them Just Work (`DISPLAY=:0` is set on every spawn — notes/xwayland.md).

## Prerequisites

- Cache proxy up (README step 6).
- `pacman -S --noconfirm xterm xorg-xeyes`.

## Steps

1. Launch `xterm` via `scripts/rootfs-run.sh 'xterm' &` (keep the host
   session alive — killing it cancels the guest). Screenshot: the window
   should show a terminal with a prompt, **magenta-tinted** (intentional
   SHM diagnostic — CLAUDE.md; xclock renders the same way in the
   integration suite).
2. Type into it via broker input actions (`hardware-key` — see
   notes/exec-broker.md; `adb shell input` is intercepted by the IME):
   run `ls /`, screenshot, verify the output drew in the xterm.
3. Launch `xeyes` alongside; each X11 toplevel should get its own
   Android task/recents card (notes/xwayland.md). Verify both windows
   exist (screenshot + `query-state` toplevel count, or the recents
   screen).
4. Inject touch/pointer motion (broker `inject-touch` path or documented
   tap injection from notes/input.md) and screenshot xeyes twice — the
   pupils should track.
5. Close both apps (Android Back via the `back` action, or `exit` in
   xterm) and confirm toplevel count returns to baseline and Xwayland
   idles out (`query-state` `xwayland_running` after its `-terminate 5`
   grace).

## Expected results

- Both apps render (magenta-tinted is correct), keyboard input reaches
  xterm, windows map to separate Android tasks, clean shutdown.

## Known issues / caveats

- `issues/hardware-backspace-stuck-down.md` — hardware Backspace can
  stick logically down in terminals; avoid relying on Backspace, and if
  you reproduce it note it against the existing issue.
- Magenta tint is a feature, not a failure (CLAUDE.md).

## Cleanup

Kill any lingering X clients, `pacman -Rns xterm xorg-xeyes`, delete
screenshots on device and host.
