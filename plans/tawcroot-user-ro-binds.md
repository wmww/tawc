# User-configurable read-only binds

Let users mark an `ExternalBind` read-only, so a bound host directory
can be exposed to the guest for reading without trusting every guest
program with writes/deletes into it. Uses tawcroot's `-b SRC:DST:ro`
primitive (notes/tawcroot/path-translation.md §"Read-only binds").

**Status: plan, not started.** The tawcroot primitive ships; this is the
Kotlin data-model + UI work. `ExternalBind`'s KDoc already reserves the
hook (ExternalBind.kt:17-21), and notes/external-binds.md:44-53 records
the intended shape.

The `BindSpec(src, dst, ro)` emit refactor this depended on has
shipped (2026-07): `TawcrootMethod.bindSpecs()` returns `BindSpec`s
and `rootfsArgv()` emits `src:dst:ro` via `BindSpec.arg()`; the
system-partition binds already use it. Only the per-`ExternalBind`
flag + UI remain.

## Motivation

The primary consumer named in the RO-binds note: expose shared storage
without trusting every guest program with deletes. Two current
suggestions in `AllFilesAccess.commonDirBinds()`
(app/src/main/java/me/phie/tawc/install/AllFilesAccess.kt:107-125) want
*different* defaults:

- `/` → `/android` (the whole Android root, browse-only): writes there
  are nonsensical and mostly uid-denied anyway → default **RO**.
- `sharedStorage → /home/android` and the Download/Documents/Pictures/
  Music/Movies/DCIM → `/root/*` folders exist precisely so Linux apps
  can *save into* Android storage → default **RW**.

So there is no single global default; RO-ness is per-bind, chosen at
suggestion time and toggleable by the user.

## Data model

`ExternalBind` (ExternalBind.kt:23-30) gains a read-only flag.

- Add `val readOnly: Boolean = false` (default false = writable, so
  legacy `metadata.json` records and the JSON round-trip stay
  writable). Absent key on parse → `false`.
- Persist as JSON key `"readOnly"` in `toJson()` (`:31-37`) and read it
  in `fromJsonArray()` (`:81-90`) via `optBoolean("readOnly", false)`.
  The `"kind": "path"` skip and unknown-key tolerance already give
  forward compat.
- **Do not** put `:ro` into `hostPath`/`guestPath`. `validationError()`'s
  `:` rejection (`:53`) is load-bearing and stays exactly as is; the
  `:ro` suffix is appended only at argv-emit time in `bindSpecs()`.
- Landlock lookahead (plans/tawcroot-landlock.md threads per-bind input
  into `allowed_access`): `readOnly` is the first per-bind permission.
  If a richer flag set looks likely soon, consider a small
  `access`/flags field instead of a bare bool — but a bool is fine to
  start and is trivially widened later since parse already tolerates
  new keys.

## Spawn path

`TawcrootMethod.bindSpecs()` already builds `BindSpec`s and
`rootfsArgv()` already emits `src:dst:ro`; the only change is the
external-bind loop becoming
`add(BindSpec(bind.hostPath, bind.guestPath, ro = bind.readOnly))`.
No change to `externalBindsFor()`'s validation/grant/exists checks.

## Suggestion defaults

`commonDirBinds()` sets `readOnly = true` on the `/ → /android` entry
(`AllFilesAccess.kt:110`) and leaves the shared-storage + folder entries
`readOnly = false` (`:111-124`). Update the KDoc note "Nothing is bound
by default" region to mention the root suggestion is browse-only/RO.

## UI

- `ManageBindsActivity` (add/edit/remove; notes/external-binds.md §UI):
  add a read-only toggle per bind row and in the add/edit dialog. When a
  suggestion card is one-tap Added, carry its default RO-ness. Make sure
  the copy doesn't imply write access for RO binds, and that the
  all-files-access grant notice wording still reads correctly for a
  browse-only root bind.
- `InstallActivity` pre-install flow (`pendingBinds`, InstallActivity.kt:
  57) and the `EXTRA_EXTERNAL_BINDS` JSON round-trip
  (InstallationService.kt:876): the flag rides the same JSON, so the
  activity-result path needs no new plumbing beyond serializing
  `readOnly` — already covered by the data-model change.

## Testing

- Unit (`app/src/test/.../InstallationExternalBindsTest.kt`): extend
  metadata parse/round-trip to cover `readOnly` present-true,
  present-false, and absent-defaults-false; and that
  `commonDirBinds()` marks the root suggestion RO and the storage
  folders RW. `./gradlew :app:testDebugUnitTest`.
- Emit assertion: `bindSpecs()` produces `src:dst:ro` for an RO external
  bind and `src:dst` for a writable one.
- Integration: same deliberate coverage gap as external binds generally
  (notes/external-binds.md §Testing — no distro installs / cache-proxy /
  persistent-appop mutation in integration tests). Manual check: bind a
  dir RO into a disposable slot, confirm a guest write returns `EROFS`
  and a read succeeds.

## Out of scope

- The built-in system-partition and app-asset binds — those are
  [tawcroot-default-binds-ro.md](tawcroot-default-binds-ro.md).
- Any per-bind permission beyond read-only (exec-deny, etc.) — that is
  the Landlock plan's territory.
