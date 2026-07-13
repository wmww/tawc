# App-shipped assets: copy → RO bind

Replace the per-rootfs copy of app-owned asset dirs with a whole-dir
`-b SRC:DST:ro` bind. Follow-on to the system-partition `:ro` work,
which shipped 2026-07: `TawcrootMethod.bindSpecs()` now returns
`BindSpec(src, dst, ro)`, `rootfsArgv()` emits `src:dst:ro` when set,
and the `LIBHYBRIS_BIND_DIRS` set is bound RO (unit-pinned in
app/src/test/.../TawcrootBindSpecsTest.kt, spot-verified on emulator:
guest writes into `/system`/`/vendor` return `EROFS`).

**Status: plan, deferred.** Assessed 2026-07 and not executed —
see the blockers below and notes/installation.md §"Why copy, not
bind", which records the same findings.

## Scope

Only whole, app-owned dirs with no distro-managed siblings qualify
(bind = replacement, not merge — a bind shadows distro-shipped
siblings and single-file binds don't appear in parent `readdir`).
`/usr/lib/hybris/` (`LibhybrisInstallProvider.GUEST_LIB_DIR`) is the
clean candidate. Files that must coexist with distro siblings (glvnd
vendor JSON in `/usr/share/glvnd/egl_vendor.d/`) stay copied.

Payoff: ~12 MB per arm64 install and no per-upgrade copy. Cost: one
more built-in bind plus the work below.

## Blockers found at assessment (must be solved first)

- **Manifest is method-agnostic.** Dropping the libhybris COPY/LINK
  entries from `TawcInstaller` removes `/usr/lib/hybris` from
  proot/chroot rootfses too — proot has no RO primitive and doesn't
  bind the asset dir. The provider API needs to know the install
  method (bind for tawcroot, keep copying for the debug methods), or
  the debug methods knowingly lose libhybris.
- **Spawn-time src guarantee.** tawcroot opens every bind src at
  startup and refuses to spawn if one is missing. The extract dir
  `<filesDir>/libhybris/` is only assured by the `TawcInstaller`
  refresh path (`ensureLibhybrisExtracted` inside `provider.entries`),
  which the stamp fast-path skips. A bound asset dir needs a
  spawn-path existence guard/extract trigger — currently kept off the
  hot path by design (`TawcInstaller` kdoc).
- **Verification needs the physical device.** libhybris GPU init +
  uninstall/reinstall/upgrade cycles can't be exercised on the x86_64
  emulator (no libhybris asset there); don't attempt this while
  `.tawctarget=emulator`.

## Work (after the blockers)

- Add the extracted asset dir as a `ro = true` whole-dir `BindSpec`
  (host = `<filesDir>/libhybris`, guest = `/usr/lib/hybris`) and drop
  the corresponding `LibhybrisInstallProvider` copy entries.
- Reconcile `Installation.tawcInstalls`/`tawcStamp` bookkeeping: a
  bound dir has no COPY/LINK manifest entry, so stamp/refresh logic
  must not expect one. Keep the empty-manifest x86_64 fast path.
- Keep the glvnd JSON and the `hybris-vulkan-only` LINK as manifest
  entries (dir-merge / cross-dir symlink cases).

## Verification

- Boot the libhybris path on the physical device with the asset dir
  bound RO; confirm GPU init works.
- Uninstall/reinstall and `adb install -r` upgrade cycles: no stale
  copies left behind; the bound dir refreshes with the APK.
