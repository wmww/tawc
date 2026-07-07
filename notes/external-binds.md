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
  (`openSettings`).

SAF was rejected as an alternative: a `content://` tree URI has no
filesystem path tawcroot could bind; faking one would mean a multi-week
broker-backed VFS with poor POSIX fidelity.

## Data model

`Installation.externalBinds` — a list of `ExternalBind(hostPath,
guestPath)` persisted in `metadata.json` (absent on legacy records =
empty). Entries carry `"kind": "path"`; unknown kinds are skipped on
parse so future bind sources stay forward-compatible (ditto unknown
keys, e.g. the retired `label`). There
is intentionally no `writable` flag yet — tawcroot now supports
read-only binds (`-b src:dst:ro`, notes/tawcroot/path-translation.md
§"Read-only binds"); add the flag plus the Manage-binds UI toggle
when a workload wants it, and have `TawcrootMethod.bindSpecs` emit
the `:ro` suffix.

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

## Install-time binds

No binds exist by default. Binds configured on the install form are
persisted in the initial metadata write, so they're live during the
installation process and first boot. `InstallationService.startInstall`
takes an optional JSON list (`externalBinds` intent extra / broker
`--arg`): an explicit list is honoured as-is; absent means none. The
install form warns (grant / install anyway) when the pending binds need
a grant that's missing, since the fail-closed error would otherwise hit
mid-install.

## UI

- `ManageBindsActivity` — add/edit/remove. `AllFilesAccess.
  commonDirBinds()` is the suggested set: `/android` ⇐ `/` (the Android
  root; much of it unreadable to the app uid — expected),
  `/home/android` ⇐ shared storage, and the shared-storage folders with
  a standard name on both sides (Download→`/root/Downloads`, Documents,
  Pictures, Music, Movies→`/root/Videos`, plus non-XDG DCIM). Unbound
  common dirs (matched by guest path, skipping host dirs that
  verifiably don't exist) render below the active binds as suggestion
  cards with a one-tap accent Add. Two modes: editing an
  existing install's metadata (from `DistroInfoActivity`, gated to
  READY/FAILED so edits don't race the service's metadata writes;
  FAILED included because editing binds is how a user recovers a
  fail-closed slot), or round-tripping a JSON list via activity result
  (from `InstallActivity`, pre-install). Shows a grant notice with a
  settings deep link whenever the all-files grant is missing.
- `DirectoryPickerActivity` — minimal in-app browser over real paths
  for picking host dirs (deliberately not SAF; see above). Host paths
  can also be typed, which matters when the grant isn't given yet and
  the picker can't list shared storage.

## Testing

- Unit: `app/src/test/.../InstallationExternalBindsTest.kt` (metadata
  parse/round-trip/validator), `./gradlew :app:testDebugUnitTest`.
- Integration: **deliberate coverage gap.** There used to be a full
  lifecycle test (`tests/integration/tests/external_binds.rs`, deleted
  2026-07) covering invalid-binds reject, metadata edits taking effect
  on the next spawn, both-direction shared-storage round-trip,
  revoked-grant fail-closed, and contents surviving uninstall. It was
  removed on purpose: it performed a real multi-GB distro install
  through the dev cache proxy and flipped the persistent
  MANAGE_EXTERNAL_STORAGE appop (`appops set --uid me.phie.tawc ...`),
  which broke the app for later tests/sessions whenever it died
  mid-run. Policy now: integration tests must not install distros, hit
  the cache proxy, or mutate persistent app/device state. If bind
  coverage is wanted again, it needs a design that spawns into a
  fabricated (KB-scale) slot and injects the grant state without
  appops. To exercise this manually: install a disposable slot with
  binds, run the shared-storage round-trip from the old test by hand,
  and flip the grant in Android settings.
