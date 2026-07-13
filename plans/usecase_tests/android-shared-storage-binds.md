# Usecase test: shared storage binds (Linux ↔ Android files)

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user wants their phone's Downloads/photos visible in Linux and files created in Linux visible to Android apps.

Integration coverage for external binds was deliberately deleted
(notes/external-binds.md) — this manual pass is the only exercise it
gets. Read that note first: binds are tawcroot path rewrites, per-install
metadata, gated fail-closed on "all files access".

## Prerequisites

- Grant all-files access:
  `adb shell appops set --uid me.phie.tawc MANAGE_EXTERNAL_STORAGE allow`
  (verify with `appops get`). Record the prior value so you can restore
  it.
- Binds are added per-install: either at install time
  (`--arg externalBinds=<json>` on the install action) or afterwards via
  the ManageBinds UI (`ManageBindsActivity` — find the activity name in
  the manifest, drive it with `adb shell am start`, screenshots, and
  taps). Prefer the UI path if workable — it's what users touch and it's
  untested.

## Steps

1. Add the suggested RW bind: shared storage → `/home/android`.
2. Rootfs → Android: write `/home/android/Download/uc-bind-test.txt`
   from the rootfs; verify content via
   `adb shell cat /sdcard/Download/uc-bind-test.txt`.
3. Android → rootfs: `adb shell 'echo from-android > /sdcard/Download/uc-bind-rev.txt'`;
   read it from the rootfs; append from the rootfs; re-read via adb.
4. Add a **read-only** bind (any suggestion, e.g. Pictures RO at another
   guest path): reads succeed, writes fail with a sane errno (documented
   RO errno shapes diverge slightly — notes/tawcroot/status.md; clean
   failure is what matters).
5. Behavior without the appop: revoke all-files access
   (`appops set --uid me.phie.tawc MANAGE_EXTERNAL_STORAGE deny`),
   start a fresh rootfs session, and confirm the bind fails **closed**
   (no access, ideally a comprehensible failure), then re-grant.
6. While in the ManageBinds UI with several binds: check
   `issues/manage-binds-last-card-under-add-button.md` (cosmetic overlap)
   — note if still present, don't fail on it.

## Expected results

- RW bind works both directions with content intact; RO bind enforces
  read-only; missing appop fails closed rather than silently exposing or
  crashing.

## Known issues / caveats

- `issues/manage-binds-last-card-under-add-button.md` (cosmetic).
- Android's FUSE emulated storage may impose case-insensitivity and
  ownership quirks on `/home/android` — surprising `stat` results there
  are worth recording but judge them against Android platform reality.

## Cleanup

Remove test files (both via adb and rootfs view), remove the binds you
added, restore the MANAGE_EXTERNAL_STORAGE appop to its prior value.
