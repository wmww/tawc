# Settings: per-distro section at the top

Builds on the open-distro home screen (done; notes/android.md "Home
screen") for `OpenDistro.resolve` / `Settings.openDistroId`.

## Goal

One Settings page. The first card is titled with the open distro's label
and holds the settings that really are stored per install. Every other
card stays as it is: global under the hood, and from the user's point of
view there is simply one settings page.

## Today

`SettingsActivity` has five global cards (Graphics driver, Debug
rendering, Compatibility, Display scale, About). The two per-install
controls live on `DistroInfoActivity`: the ando toggle
(`AndoToggleRow.kt`, `Installation.andoEnabled`) and the "Storage binds"
button (`ManageBindsActivity`, `Installation.externalBinds`).

## Design

New first card, built in `onResume` (not `onCreate`) so a distro switch
or an uninstall while Settings was in the back stack re-renders:

- Title: the open distro's `DistroRegistry.displayLabel`, e.g.
  "Debian sid". No "Distro:" prefix; the label alone matches the drawer
  and the home card. If the label equals the registry display name,
  nothing else; otherwise a dimmed distro line under the title, as the
  home card does.
- ando toggle row, moved verbatim from `DistroInfoActivity.buildAndoRow`
  including the single-thread commit executor and the state gate
  (READY or FAILED at write time).
- "Storage binds" tonal button, same gating as today (tawcroot, all-files
  access declared, READY or FAILED).
- Both controls state-gated as today; when neither is available (e.g.
  slot INSTALLING) the card still shows the title plus a one-line
  explanation, so the page does not silently lose its top section.
- No install: the card is omitted. The rest of the page is unchanged.
- No Delete here; that stays on Distro info.

The existing cards do not move or change. Do not add a per-distro
override for any global setting; if one is wanted later, it is a
separate change with its own storage.

## Steps

1. Extract the ando row builder and its executor from
   `DistroInfoActivity` into something both screens could use, then
   delete it from Distro info (only Settings uses it after this).
2. Add the card to `SettingsActivity`; move card population to
   `onResume` for this card only (the global cards can stay in
   `onCreate`, they read live values on build).
3. Strings: reuse `distro_info_manage_binds`; add the "not available
   while installing" line.
4. Verify on the `.tawctarget` device: toggle ando from Settings and
   confirm `AndoBrokers.refresh` still fires (broker listener comes up /
   goes down); open binds; switch distro in the drawer and reopen
   Settings; uninstall the open distro with Settings in the back stack.
5. Notes: `notes/ando.md` and `notes/external-binds.md` name Distro info
   as the UI entry point; point them at Settings.
