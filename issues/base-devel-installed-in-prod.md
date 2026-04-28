# `base-devel` installed unconditionally in every chroot

`BASE_PACKAGES` in `server/app/src/main/java/me/phie/tawc/install/distro/arch/ArchPacmanCommon.kt:407` includes `base-devel`, and `installBasePackages()` (line 391, `pacman -Syyu --needed`) runs on every install — chroot and proot, production and test. Nothing in the production install or runtime path invokes a compiler: libhybris is cross-compiled on the host and shipped in the APK, proot is cross-compiled and shipped as a jniLib, and distro setup only installs pre-built pacman binary packages. So `base-devel` (gcc, make, autoconf, binutils, …) is dead weight on user devices, costing install time and disk for nothing.

The only consumer is the integration test suite: `testing/integration/src/chroot.rs:53` (`ensure_debug_app`) pushes `gtk4-debug-app.c` into the chroot and runs `gcc` via `testing/gtk4-debug-app/build.sh:6`. That's the one and only place anything actually gets compiled on-device, and it's test-only — `testing/install-test-deps.sh` already installs `gtk4` and `pkg-config` for it and could just as easily pull in the toolchain.

## Proposal
Drop `base-devel` from `BASE_PACKAGES`. Move the toolchain dependency to `testing/install-test-deps.sh` — add `gcc` (or `base-devel`, if other test artifacts end up needing more of it) to the `PKGS` array there so `ensure_debug_app` keeps working. Update the comment at `testing/gtk4-debug-app/build.sh:3` ("Requires: gcc (base-devel), gtk4") to match wherever the dep ends up living.

## Verification
- Fresh production install (chroot and proot) succeeds without `base-devel`; compositor + Firefox + a GTK demo still launch.
- `testing/run-integration-tests.sh` passes after `testing/install-test-deps.sh` runs once on the fresh chroot.
- Confirm install time / rootfs size actually drop (sanity-check the win is real).

## Notes
- Anything else `base-devel` was implicitly providing for non-test paths needs to be accounted for. A grep for compiler/make invocations in the install + runtime code paths came up empty, but worth re-confirming once removed.
- If a future feature genuinely needs an on-device toolchain in production, this issue is moot — re-add it then.
