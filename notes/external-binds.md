# External storage binds

Per-install binds of host directories into the rootfs, so users can
keep selected rootfs data outside app-private storage — surviving
uninstall and visible to other Android apps. Example: shared storage
(`/storage/emulated/0`) bound at `/home/android`.

Tawcroot-only: the bind list rides the same `-b src:dst` table as the
built-in system/share binds (path rewrites, not kernel mounts — so
uninstall's recursive delete never traverses into a bind source). That
invariant — not the wipe engine — is what protects Android-side data
on OS-level uninstall, where no app code runs. At in-app wipe time,
[RootfsCleaner]'s uniform mount gate (refuse to delete while any mount
sits under the install dir) is the backstop if the invariant is ever
broken (e.g. external binds wired into chroot's real mounts). The
chroot debug method uses real mounts and deliberately does not get
these binds until its uninstall interaction is reviewed; proot doesn't
need them for any dev-loop purpose.

## Permission model

Direct path access to shared storage needs `MANAGE_EXTERNAL_STORAGE`
("all files access", Android 11+), granted by the user via a system
settings toggle. Two independent runtime gates in
`install/AllFilesAccess.kt`:

- `declared(context)` — the permission is in the APK manifest at all.
  `-PtawcAllFilesAccess=false` strips it at build time via the
  build-type manifest overlay `app/src/overlays/no-all-files-access/`
  (Google Play treats the permission as sensitive and only allows
  qualifying core use cases; sideload/F-Droid builds keep it). All
  binds UI hides itself when false; nothing else changes, so the same
  code ships both ways.
- `granted()` — `Environment.isExternalStorageManager()`. The
  manage-binds screen deep-links to the settings toggle
  (`settingsIntent`).

SAF was rejected as an alternative: a `content://` tree URI has no
filesystem path tawcroot could bind; faking one would mean a multi-week
broker-backed VFS with poor POSIX fidelity.

## Data model

`Installation.externalBinds` — a list of `ExternalBind(hostPath,
guestPath, label?)` persisted in `metadata.json` (absent on legacy
records = empty). Entries carry `"kind": "path"`; unknown kinds are
skipped on parse so future bind sources stay forward-compatible. There
is intentionally no `writable` flag — tawcroot's bind table has no
read-only mode (see plans/tawcroot-readonly-binds.md); add the flag
when it does.

`ExternalBind.validationError()` is the shared structural validator
(absolute paths, no `..`, no `:` — a colon would split the `-b src:dst`
argv pair — and guest ≠ `/`). Every accepting surface runs it: the
manage-binds dialog, `InstallationService.startInstall`, and the spawn
path.

## Spawn path

`TawcrootMethod` resolves the install id from the rootfs path
(`<distros>/<id>/rootfs`) and loads the bind list from metadata on
every spawn — so the broker's RUNINSIDE, RunCommandOp, the in-app
terminal, and the install pipeline's own in-rootfs steps all pick up
binds without threading `Installation` through their call chains.
External binds append after all built-in binds; tawcroot's longest-
dst-prefix lookup (first-added wins ties) means they can't shadow the
system/share set. Guest target dirs are pre-created in `prepareSpawn`.

Fail closed: a structurally invalid bind, a missing host dir, or a
shared-storage bind (`/storage/...`, `/sdcard/...`) without the
all-files grant throws `IOException` with an actionable message instead
of spawning. Never substitute an empty app-private dir — a session
"writing to shared storage" that actually lands app-private would be
data loss at uninstall.

## Install-time defaults

Fresh tawcroot installs default to two binds (editable/removable like
any other):

- `/android` ⇐ `/` — the Android root; much of it is unreadable to the
  app uid (SELinux/DAC), which is expected.
- `/home/android` ⇐ `Environment.getExternalStorageDirectory()`
  (`/storage/emulated/0`).

Binds are settled before the install starts and persisted in the
initial metadata write, so they're live during the installation process
and first boot. `InstallationService.startInstall` takes an optional
JSON list (`externalBinds` intent extra / broker `--arg`): an explicit
list is honoured as-is (`[]` = none); absent means "defaults", seeded
only when the permission is declared *and* already granted so a
defaulted CLI install can't fail closed on its own first boot. The
install form warns (grant / install anyway) when the pending binds need
a grant that's missing, since the fail-closed error would otherwise hit
mid-install.

## UI

- `ManageBindsActivity` — add/edit/remove. Two modes: editing an
  existing install's metadata (from `DistroInfoActivity`, gated to
  READY/FAILED so edits don't race the service's metadata writes;
  FAILED included because editing binds is how a user recovers a
  fail-closed slot), or round-tripping a JSON list via activity result
  (from `InstallActivity`, pre-install). Shows a grant banner with a
  settings deep link when a configured bind needs the missing grant.
- `DirectoryPickerActivity` — minimal in-app browser over real paths
  for picking host dirs (deliberately not SAF; see above). Host paths
  can also be typed, which matters when the grant isn't given yet and
  the picker can't list shared storage.

## Testing

- Unit: `app/src/test/.../InstallationExternalBindsTest.kt` (metadata
  parse/round-trip/validator), `./gradlew :app:testDebugUnitTest`.
- Integration: `tests/integration/tests/external_binds.rs` — full
  lifecycle on a disposable `extbinds` install: invalid-binds reject,
  default seeding, both-direction shared-storage round-trip, metadata
  edit taking effect, revoked-grant fail-closed, contents surviving
  uninstall. Needs the dev cache proxy. The grant is flipped from the
  host with `appops set --uid me.phie.tawc MANAGE_EXTERNAL_STORAGE
  allow|deny` (the appop is what `isExternalStorageManager` reads).
