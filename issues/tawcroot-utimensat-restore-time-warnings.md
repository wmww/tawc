# tawcroot: pacman/libarchive prints "Can't restore time" for every extracted file

During a `pacman -Syyu` inside a tawcroot rootfs, libarchive emits one
"warning: warning given when extracting <path> (Can't restore time)"
line per file. The extraction itself succeeds, but every file ends up
with mtime = extraction time instead of the package's recorded mtime.

The comment on `handle_utimensat` in
`tawcroot/src/syscalls_fs.c:531-540` says trapping the syscall is
specifically supposed to fix this:

> "Without this trapping, libarchive (used by pacman) hits the kernel
>  with a guest-visible path that fails to resolve — the package
>  extraction completes but every file gets the current mtime instead
>  of the archive's recorded one, drowning install logs in
>  'Can't restore time' warnings."

So the handler exists and is wired into `tawcroot_dispatch_install`
(`tawcroot/src/syscalls_fs.c:1092`). But the warnings still appear and
files still get extraction-time mtimes. Spot-checked
`/data/data/me.phie.tawc/distros/manjaro/rootfs/usr/lib/libgtk-4.so.1`
right after a fresh tawcroot Manjaro install; mtime ≈ extraction time,
not archive time.

Reproduction:

1. Install Manjaro via tawcroot (any tawcroot install will do).
2. `TAWC_INSTALL_ID=manjaro bash testing/install-test-deps.sh` —
   watch the log fill with "Can't restore time" warnings.
3. `adb shell "su -c 'stat -c %Y <some lib in rootfs>'"` returns the
   approximate extraction time, not the archive-recorded time.

Likely areas to investigate:

- libarchive uses `futimens(fd, ts)` (= `utimensat(fd, NULL, ts, 0)`).
  Our handler forwards the NULL-path branch directly, but only after
  `fetch_and_translate_at` has run on the openat that produced `fd`.
  Verify the dirfd we hand back from openat is the host fd (it should
  be) and that the kernel actually accepts utimensat with that fd
  uid-wise on Android.
- libarchive also tries `lutimes(2)` (utimensat with AT_SYMLINK_NOFOLLOW)
  for symlinks. Our handler forwards `args->d` as the flags argument,
  which is correct, but worth confirming that path goes through
  `fetch_and_translate_at` cleanly when dirfd is AT_FDCWD and the
  guest path is absolute.
- Finally, Android may simply reject mtime changes on `/data/data/...`
  when the file's owner uid matches the caller — check by stracing a
  bare `touch -d '2020-01-01' /data/data/me.phie.tawc/.../foo` from
  the tawcroot CLI vs from a chroot with real `chroot(2)` access.

Cosmetic at present: integration tests pass and the build proceeds
despite the warnings. Worth fixing because the noise drowns out real
warnings during install, and packages that rely on mtime ordering
(autotools timestamps in particular) can break in subtle ways.
