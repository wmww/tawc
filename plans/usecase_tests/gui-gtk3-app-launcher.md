# Usecase test: install a GTK app and launch it from the launcher

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** physical only — GTK renders via GL (`GDK_GL=gles:always`)
which needs libhybris; the emulator's gfxstream GL path isn't ready
(notes/emulator.md), and SHM fallback renders black there.
**Usecase:** a user pacman-installs a GUI app and expects it to show up in tawc's launcher and run — the full "app store to screen" loop.

## Prerequisites

- Cache proxy up (README step 6).
- `pacman -S --noconfirm galculator` (small GTK calculator with a
  `.desktop` entry; pick another small GTK3 app if it's unavailable).

## Steps

1. Launcher discovery: `scripts/tawc-exec.sh --action launcher-list
   --arg installId=<id>` — the new `.desktop` entry must appear
   (notes/launcher.md). `gtk3-demo` entries from the preinstalled
   `gtk3-demos` may be there too; note what a user actually sees.
2. Launch it the way a user would: through the launcher UI (home screen
   → distro launcher), driving with screenshots + taps. If UI driving
   proves impractical, fall back to `scripts/rootfs-run.sh 'galculator' &`
   but record that the launcher-UI path went unexercised.
3. Screenshot: the window should render **without** magenta tint (GTK3
   is forced onto the GL path — notes/firefox.md context on `GDK_GL`).
4. Interact: tap a few calculator buttons via injected touch, verify the
   display updates (screenshots).
5. Menus, the known GTK3 sore spot (notes/gtk3-broken-menus-workaround.md):
   open `gtk3-demo` (preinstalled), pop a menu/combobox via touch, and
   check it opens and dismisses sanely. Check the workaround setting
   state first (`get-gtk3-broken-menus-workaround` broker action) and
   test with it as-found.
6. Close everything via Android Back; verify toplevels return to
   baseline (`query-state`).

## Expected results

- Fresh package's `.desktop` appears in the launcher without any manual
  refresh dance; app launches, renders un-tinted, responds to touch;
  menus usable per the documented workaround behavior.

## Known issues / caveats

- GTK4 apps need ≥4.22 on libhybris/Adreno (notes/rendering.md context)
  — irrelevant here but don't drift into installing GTK4 apps.
- Menu behavior on touch-only devices is exactly what the workaround
  note covers; judge against that note, not desktop expectations.

## Cleanup

Close apps, `pacman -Rns galculator`, verify launcher-list no longer
shows it, delete screenshots on device and host.
