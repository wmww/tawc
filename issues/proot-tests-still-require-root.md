# Integration test harness requires root even against a proot install

The proot install + runtime path is genuinely rootless — `adb.rs:90` dispatches `run-as $PKG` for proot installs, and `client/tawc-chroot-run` does the same. But the integration test scaffolding ignores the install method and goes straight through `su -c` for any operation that doesn't enter the chroot via `enter.sh`. Result: running `testing/run-integration-tests.sh` against a proot install still triggers a superuser-permission popup (observed in practice) and silently requires the device to be rooted.

This is a real gap. One of the headline reasons to ship proot at all is to support unrooted devices, and the test suite is what we'd point a contributor at to verify that path actually works.

## Where root is currently required

All unconditional `su -c` — no proot branch:

- `testing/integration/src/chroot_process.rs:106` — `test -d /proc/<pid>` for liveness checks
- `testing/integration/src/chroot_process.rs:141` — reads `/proc/<pid>/<file>`
- `testing/integration/src/chroot_process.rs:153` — reads `/proc/<pid>/stat`
- `testing/integration/src/chroot_process.rs:169` — `rm -f` of the pidfile on the device
- `testing/integration/src/chroot_process.rs:209` — copies `tawc-pidfile-exec` into the rootfs and chmods it
- `testing/integration/src/chroot_process.rs:264` — `ps -eo pid,ppid,pgid` for the global process listing
- `testing/integration/src/compositor.rs:163` — waits for `wayland-0` socket inside the rootfs via `test -e`
- `testing/integration/src/chroot.rs:86-89` — `ensure_debug_app` copies sources into the rootfs and `chmod -R a+rwX`
- `testing/install-test-deps.sh:55` — installs `fake-bwrap` over `/usr/bin/bwrap` in the rootfs
- `testing/install-test-deps.sh:81` — installs `firefox.cfg` autoconfig into the rootfs
- `testing/run-integration-tests.sh:81` — `test -x .../enter.sh`
- `testing/run-integration-tests.sh:97` — copies `tawc-pidfile-exec` into the rootfs and chmods it
- `testing/run-integration-tests.sh:118` — polls for the wayland socket
- `testing/build-debug-app.sh:23` — copies `gtk4-debug-app` sources into the rootfs

The `chroot.rs:78-85` comment already acknowledges this — it knows files land owned by uid 0 and chmods world-writable so the in-chroot proot-uid build can replace the output. That workaround is enough for `ensure_debug_app` but not the rest.

## Proposal

Route every device-side filesystem and `/proc` poke through a method-aware helper, the same way `adb::chroot_run` already dispatches on the install method:

- For proot installs: use `run-as $PKG` for anything inside `/data/data/me.phie.tawc/...`, and read `/proc/<pid>/...` for processes the app launched (the app uid owns them under proot, so `run-as` reads work). `ps` of arbitrary pids isn't needed if we track the proot tracee tree ourselves.
- For chroot installs: keep the existing `su -c` path.

Concretely:
- Add an `adb::chroot_shell(cmd)` (or similar) that dispatches `run-as` vs `su -c` based on the install method already cached by `adb::chroot_run`.
- Replace all the `su -c` sites listed above with the new helper.
- `chroot_process.rs` PID tracking: under proot, the tracee processes are children of the app-uid proot supervisor, so `/proc/<pid>` is readable via `run-as`. The global `ps -eo pid,ppid,pgid` (`chroot_process.rs:264`) probably needs a proot-specific variant that scopes to the proot tree rather than scanning every pid on the device.
- `install-test-deps.sh` and `build-debug-app.sh`: same dispatch — `run-as ... cat > ...` instead of `su -c install ...`.

## Verification

- On an unrooted device (or emulator with Magisk disabled / `su` missing), `bash testing/run-integration-tests.sh` against a proot install completes without prompting for superuser and without errors.
- On a rooted device, both proot and chroot installs still pass.
- No "granted su permission" popup during a proot test run.

## Notes

- A few files (`fake-bwrap`, `firefox.cfg`, autoconfig) need to land in the rootfs — `run-as` writes are fine since the rootfs is under the app's data dir.
- This is also a precondition for honestly claiming we test the rootless path in CI on unrooted emulators.
