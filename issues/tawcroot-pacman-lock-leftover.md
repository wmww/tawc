# tawcroot: pacman leaves `/var/lib/pacman/db.lck` after a clean exit

After a successful `pacman -Syyu --needed --noconfirm <packages>` run inside a tawcroot rootfs (e.g. the install flow's `installBasePackages` step), `<rootfs>/var/lib/pacman/db.lck` is still present. The next `pacman` invocation aborts with:

    error: failed to synchronize all databases (unable to lock database)

Reproduction:

1. `am start -n me.phie.tawc/.install.InstallActivity --ez autoStart true --es id manjaro --es method tawcroot --es distro manjaro` (any tawcroot install).
2. Wait for `[stage:DONE] Installation complete`.
3. `TAWC_INSTALL_ID=manjaro bash testing/install-test-deps.sh` — fails on the very first pacman call.
4. `su -c rm <rootfs>/var/lib/pacman/db.lck` — workaround that lets subsequent pacman runs succeed.

Background: pacman creates `db.lck` on entry and deletes it from a libalpm cleanup path that's wired up via `atexit(3)`. The bootstrap install logs show `[stage:DONE]` (so the shell script's `pacman -Syyu` returned 0), yet the lock survives — which means pacman either skipped the atexit cleanup, or something deleted it after the unlock and we didn't see it. The most plausible cause is that tawcroot terminates the tracee in a way that bypasses `atexit` handlers (`_exit` / SIGKILL on parent shutdown). chroot is faked as EPERM (`syscalls_control.c::tawcroot_control_register`), so a hook trying to `chroot` in libalpm's pre/post-install path will fail loudly but pacman tolerates that — not the same bug.

Suggested next steps:

- Confirm pacman's exit path in tawcroot: strap a tiny C program that registers `atexit` and check whether its handler runs to completion under tawcroot in the install codepath.
- If atexit is being skipped, fix tawcroot's exit/teardown so traced processes get a chance to run their atexit handlers (or, separately, defend chroot installs by `rm -f .../db.lck` in `installBasePackages` after the pacman call).
- The chroot/proot install paths don't seem to leave the lock — this is currently a tawcroot-specific failure.
