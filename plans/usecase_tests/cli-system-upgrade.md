# Usecase test: full system upgrade (`pacman -Syu`)

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user keeps their rolling-release install current. Every Arch user will run `-Syu`; it has to not brick the rootfs.

Note this test intentionally mutates the install (that's what an upgrade
is) — there is no rollback. That's acceptable; flag it in the commit
message.

## Prerequisites

- Cache proxy up (README step 6). Mind the two proxy issues below before
  starting.

## Steps

1. Record the before state: `pacman -Q | wc -l`, kernel-agnostic sanity
   (`bash --version`, `ls`, `git --version`).
2. `scripts/rootfs-run.sh 'pacman -Syu --noconfirm'` (use an op title so
   progress shows in-app). Watch the full output.
3. Triage warnings: file-capability warnings on `newuidmap` /
   `gst-ptp-helper`-style files are documented-expected
   (notes/tawcroot/status.md); `.pacnew` drops on files tawc manages
   (`/etc/pacman.d/mirrorlist`, `/etc/resolv.conf`) should be recorded —
   check the live copies were **not** clobbered (mirrorlist must still
   point at the proxy; resolv.conf must still resolve).
4. Post-upgrade smoke: run a few binaries that were upgraded (check
   `pacman -Qu` before / the transaction log for what moved), open a new
   `rootfs-run.sh` session, run something GUI-trivial if a compositor
   test is cheap (`weston-info`/`wayland-info` or similar) to confirm
   the tawc-installed pieces (`/usr/lib/hybris`, `/usr/local/bin/ando`)
   weren't damaged.
5. Confirm slimming held: `/usr/share/man` should not have regrown
   (NoExtract policy).

## Expected results

- Upgrade completes; rootfs still functions (shells, git, package
  queries); tawc-managed files intact; only the documented warnings.

## Known issues / caveats

- `issues/cache-proxy-stale-pacman-db.md`: a year-cached repo DB can 404
  on rolled packages. If downloads 404, that's the known issue — the fix
  is `scripts/cache-proxy.sh wipe '\.db$'` **but** listing first and
  being conservative; re-read the issue before acting.
- `issues/cache-proxy-abandoned-fills-block-installs.md` if things time
  out.
- setcap-using packages fail their scriptlets (documented divergence) —
  note which, don't fail the test on it.

## Cleanup

Nothing to revert (upgrades are one-way). Remove any scratch files;
clear pacman's package cache inside the rootfs
(`pacman -Sc --noconfirm`) to return the disk space.
