Tess's Android Wayland Compositor (tawc) is an Android app plus rootfs/build scripts for running desktop Linux programs on Android.

## Quick Reference
- Build APK: `JAVA_HOME=/usr/lib/jvm/java-21-openjdk ANDROID_HOME=$HOME/Android/Sdk ./gradlew assembleDebug`
- Build/install/launch: `scripts/app-build-install.sh` (`--no-build`, `--no-launch` supported)
- Run in rootfs: `scripts/rootfs-run.sh '<command>'` or interactive with no command
- Run Firefox: `scripts/rootfs-run.sh 'firefox --no-remote'`
- Run lxterminal: `scripts/rootfs-run.sh 'lxterminal'`
- Integration tests: `scripts/run-integration-tests.sh [filter]` (`--no-build` reuses existing deploy)
- Test deps in rootfs: `scripts/install-test-deps.sh` (rerun after editing `tests/apps/**`)
- tawcroot tests: `tawcroot/test.sh [--host|--device] [--no-build] [FILTER...]`
- Logs: `adb logcat -s tawc-native` for Rust, `adb logcat -s tawc` for Kotlin
- Screenshot: use `/data/local/tmp/tawc-dev/`; analyze screenshots with a sub-agent, then delete device and host copies.

## Current Project Shape
- Install methods: `tawcroot` is default and the only release-supported method. `proot` and `chroot` are debug-only dev-loop options.
- Graphics backends: `libhybris`, `libhybris-zink`, `gfxstream`, and `cpu` ship by default. `libhybris` works on all tested physical devices and is the production/default path. `gfxstream` is experimental/partial; it is the x86_64 emulator default only because libhybris is unsupported there. See [notes/gpu-strategy.md](notes/gpu-strategy.md), [notes/libhybris-zink.md](notes/libhybris-zink.md), and [notes/gfxstream-bridge.md](notes/gfxstream-bridge.md).
- The debug exec broker is the normal host-to-app command path. Host helper: `tools/tawc-exec/`; wrapper: `scripts/tawc-exec.sh`; protocol notes: [notes/exec-broker.md](notes/exec-broker.md).
- SHM buffers are intentionally tinted magenta by default to expose fallback paths. Do not remove this unless explicitly asked.

## Operating Rules
- Keep docs compact here. Put durable design/build details in `notes/`; start with [notes/README.md](notes/README.md).
- This is an agent-written project. Existing code and notes may be wrong; verify against source/scripts before trusting old prose.
- If you add or change a build dependency, host package, vendored repo, env var, or toolchain version, update [notes/building.md](notes/building.md) in the same change.
- Use existing scripts instead of one-off adb/chroot commands when possible, if the scripts are broken fix them (or at least open an issue).
- Only commit, amend, tag, or push when explicitly asked. Git push may hang without user approval.
- Keep prose, comments, errors, and commit messages short unless extra detail is genuinely useful.

## Issues
- Issues live in `issues/`. Do not solve them unless asked or the fix falls out of current work.
- Create/update issues for nontrivial problems discovered during other work.
- Delete confirmed-solved issues; move still-useful context into notes first.

## Device Safety
- The physical phone is precious: do not reboot it unless explicitly asked.
- Emulator management is fair game, but always use `scripts/emulator.sh start|stop [rooted|rootless]`; never launch qemu/emulator directly.
- The standing device target is `./.tawctarget` (`physical`, `emulator`, or `none`). Never edit it. For one command, use `TAWC_TARGET=physical|emulator` or `ANDROID_SERIAL=<serial>`.
- Missing/`none` target means no device interaction. If a requested task needs a device, ask which target to use.
- No silent target fallback: if `.tawctarget=emulator` and only a phone is connected, do not substitute the phone.
- If `su` is available on a phone, prefer it over `adb root`.

## On-Device Files
- Rootfs installs live under `/data/data/me.phie.tawc/distros/<id>/rootfs/`.
- Test/debug scratch outside app-private data may only use `/data/local/tmp/tawc-dev/`, exposed by `scripts/lib/tawc-scratch.sh` and the Rust integration crate. Delete screenshots/debug artifacts when done.
- Production code must not write `/data/local/...` or `/sdcard/...`; app-owned runtime state should live under `/data/data/me.phie.tawc/` so uninstall removes it.
- Do not clone external repos into `$HOME`. Use `deps/` for vendored/tooling checkouts, and delete temporary checkouts before finishing.

## Vendored Deps
- `deps/deps.list` is the single source of truth for pinned git deps. Build scripts call `dep_ensure <name>` from `scripts/lib/deps.sh`, cloning if missing and erroring if HEAD differs from the pin. Uncommitted edits are tolerated; wrong HEAD is not.
- To follow manifest changes, run `scripts/update-deps.sh [name...]`. It is the only command that should mutate dep checkouts behind your back.
- When updating a vendored git dep, update its commit in `deps/deps.list` in the same change. Tarball deps such as `talloc`/`libmd` are versioned by URLs in build scripts, not `deps.list`.

## Cache Proxy
- Always use the dev mirror cache for install/test paths that download distro packages: pass `--arg mirrorProxy=http://127.0.0.1:8080/proxy/` to install actions and equivalent install tests.
- Never start the cache proxy yourself. If `127.0.0.1:8080` is refused, ask the user to run `scripts/cache-proxy.sh run`.
- A `404` from `/` or `/proxy/` means the proxy is up; upstream URLs are appended after `/proxy/`.
- Never wipe `build/cache-proxy/cache/`; ask the user if manual cache cleanup is needed.

## Libhybris Fork
- Fork: https://github.com/wmww/libhybris, pinned in `deps/deps.list`, local checkout `deps/libhybris`, docs in `deps/libhybris/TAWC_FORK.md`.
- Build: `scripts/build-libhybris.sh [--clean]`. Output is packed as `assets/libhybris/arm64-v8a.tar`; runtime extraction plus `LibhybrisInstallProvider` copy it into each rootfs at `/usr/lib/hybris/` as real files.
- Treat post-upstream libhybris history as a clean patch stack, not chronological history. Combine logical changes; old commits may be edited.
- The final libhybris commit is always the `TAWC_FORK.md` commit. Changes to `TAWC_FORK.md` are amended into that final commit, not added as a new commit.
- Only commit to libhybris when asked. After rewriting/amending the fork, bump the `libhybris` pin in `deps/deps.list` in the same change. When updating the fork, tag it `tawc-<DD-Mon-YYYY>-<n>`.
