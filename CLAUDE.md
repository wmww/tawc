Hi Claude! This project is Tess's Android Wayland Compositor (tawc). It's an Android app and set of chroot scripts to run desktop Linux programs on Android.

## Plan
[plan.md](plan.md) has the implementation roadmap.

## Building
**[notes/building.md](notes/building.md) is the source of truth for build-time dependencies and the fresh-system build flow.** Consult it before building on a new machine, and **update it in the same change** whenever you add or change a build-time dep (host package, vendored repo, env var, toolchain version). The Quick Reference at the bottom of this file is a cheat sheet for already-set-up systems — building.md is the doc to follow when setting up.

## Vendored deps
**`deps/deps.list` is the single source of truth for every vendored git dep** (libhybris, android-headers, libxkbcommon, proot, cleat, smithay, xwayland-src/*). Each entry pins a commit hash. Build scripts call `dep_ensure <name>` from `scripts/lib/deps.sh`, which clones if missing and **errors loudly** if the existing checkout is at the wrong commit (uncommitted edits are silently tolerated — only HEAD matters).

Used by: `scripts/build-libhybris.sh`, `scripts/build-libxkbcommon.sh`, `scripts/build-proot.sh`, `scripts/build-xwayland.sh`, `scripts/setup-smithay.sh`, `tawcroot/build`. Gradle declares `deps/deps.list` and `scripts/lib/deps.sh` as inputs of `buildLibxkbcommon<Abi>`, `buildLibhybris`, `buildXwayland`, `setupSmithay`, so a manifest bump invalidates the cache. Smithay is the only Rust-side dep — it's consumed via `[patch.crates-io] smithay = { path = "../deps/smithay" }` in `compositor/Cargo.toml`, which means cargo errors up front if the checkout is missing; gradle's `setupSmithay` task fixes that automatically.

When you update a dep — bumping libhybris patches, pulling new xwayland, etc. — **bump the commit in `deps/deps.list` in the same commit**. Otherwise other checkouts will silently keep building against the old version (or in our case, will fail loudly, which is the point). To pull a manifest update onto an existing checkout, run `bash scripts/update-deps.sh` (or `bash scripts/update-deps.sh <name>`) — it's the only command that mutates dep checkouts behind your back.

Tarball deps (talloc, libmd) aren't in `deps.list` — their version is baked into the URL in the build script, so version bumps auto-fetch.

## Notes
The `notes/` directory contains architecture and implementation notes. Edit/create/split/merge/rename notes as needed without being asked. Key files:
- [architecture.md](notes/architecture.md) -- compositor module layout, design decisions, Smithay integration
- [building.md](notes/building.md) -- build instructions and deployment
- [gpu-strategy.md](notes/gpu-strategy.md) -- GPU driver strategy, libhybris, AHB buffer sharing
- [wsi-layer.md](notes/wsi-layer.md) -- client-side EGL WSI layer
- [rendering.md](notes/rendering.md) -- window management, coordinate system, SHM buffers
- [multi-activity.md](notes/multi-activity.md) -- plan for one Android task per window
- [input.md](notes/input.md) -- touch input architecture
- [text-input.md](notes/text-input.md) -- text input design (Android IME <-> zwp_text_input_v3)
- [firefox.md](notes/firefox.md) -- Firefox-specific setup and issues
- [android.md](notes/android.md) -- socket sharing, SELinux, chroot setup
- [emulator.md](notes/emulator.md) -- AVD setup, Magisk root, x86_64 chroot, what works/doesn't
- [installation.md](notes/installation.md) -- in-app Kotlin chroot installer; broker `--action install/uninstall` CLI for adb-driven workflows
- [launcher.md](notes/launcher.md) -- per-distro app picker + Rust .desktop scanner with icon resolution
- [log-screen.md](notes/log-screen.md) -- generic Operation/registry/LogScreen abstraction shared between install, uninstall, and future broker actions
- [proot.md](notes/proot.md) -- rootless install method: vendored Termux/proot fork, why upstream proot doesn't work on Android, Android quirks worked around
- [xwayland.md](notes/xwayland.md) -- bionic-build Xwayland for X11 client support; current dep build state and pending stages
- [tawcroot.md](notes/tawcroot.md) -- design + implementation plan for the from-scratch C systrap-based proot replacement (lives in `tawcroot/`)
- [distro-options.md](notes/distro-options.md) -- survey of viable glibc distros (Debian, Void, Manjaro ARM, …) and why musl/bionic alternatives don't fit
- [cache-proxy.md](notes/cache-proxy.md) -- dev-time host nginx caching reverse proxy for distro mirrors (adb reverse + URL-passthrough format)
- [chroot-sessions.md](notes/chroot-sessions.md) -- invariant: every chroot invocation must run in its own session, and where each launch path upholds it

Keep notes up to date with new choices, discoveries and project state. This is an agent-written project, existing code/notes may be wrong. Stay vigilant, and fix/record problems as you find them (even when working on something else).

## Issues
- Issues are located in `issues/`
- Do not solve issues unless either you're asked to work on them, or a solution falls out of the work you are asked to do
- Create and update issues as needed, in particular create an issue when you find a problem that's nontrivial to fix while working on something else
- Include detailed steps to reproduce if relevant
- Delete issues when they are confirmed fully solved
- If an issue has important info that remains relevant after its solved, integrate that info into your notes when deleting it
- Issues should be markdown documents starting with a title and one-line explanation, followed by a complete description with all relevant details

## Cache proxy
The dev-time host nginx caching reverse proxy lives at `build/cache-proxy/` (started by `bash scripts/cache-proxy.sh run`; see [notes/cache-proxy.md](notes/cache-proxy.md))
- **Always use the cache proxy for installs during development and testing.** Pass `mirrorProxy=http://127.0.0.1:8080/proxy/` on every `bash scripts/install-distro.sh …` invocation, and on every test path that exercises an install.
- **Never start the cache proxy yourself.** If a test fails because `127.0.0.1:8080` is refused, the right response is "ask the user to run the proxy with `bash scripts/cache-proxy.sh run`", not "start it for them".
- **Never wipe `build/cache-proxy/cache/`.** If you suspect something is broken, ask the user to delete stuff manually.

## Workflow
- Debugging against both a real Android phone via adb or an emulator (x86_64 AVD with Magisk) are supported
- libhybris/GPU drivers are not supported on thet emulator — see [emulator.md](notes/emulator.md)
- The standing target is set in `./.tawctarget` (one word: `physical`, `emulator`, or `none`). A missing file or `none` means **no device interaction is permitted** — every host script (`scripts/tawc-chroot-run.sh`, `scripts/run-integration-tests.sh`, `scripts/install-test-deps.sh`, `tawcroot/test --device`, …) sources `scripts/lib/select-device.sh` and will refuse to run. This is the default on a fresh checkout.
- **Never edit `.tawctarget` yourself** — it represents the user's standing choice. To use a different target for one command, set `TAWC_TARGET=physical` or `TAWC_TARGET=emulator` (or `ANDROID_SERIAL=<serial>`) on the command line. Don't rewrite the file.
- Single-target auto-fallback is intentionally absent: with no opt-in, host scripts won't talk to whatever happens to be plugged in. A target mismatch (e.g. `.tawctarget=emulator` but the AVD isn't running) is also a hard error — bring the right target up, or override `TAWC_TARGET` for that command. Never silently substitute the other kind.
- If the target is `none` and the user asks you to do something that needs a device, ask them which target to use rather than picking one yourself.
- **Always manage the emulator with `bash scripts/emulator.sh start|stop [rooted|rootless]`.** Never launch qemu / `emulator -avd ...` directly.
- Work autonomously when possible. If you need human help to set up your dev loop, ask
- When analyzing screenshots, use a sub-agent so the image doesn't end up in main context
- If `su` is available on the phone, use it instead of `adb root`
- Use existing scripts (eg `scripts/tawc-chroot-run.sh`) instead of one-shotting commands
- Feel free to edit scripts/files that have problems
- Only git commit when told to
- Keep commit messages short — a few lines or at most one paragraph explaining the why. Real documentation (design docs, verification logs, structural commentary, point-by-point breakdowns) belongs in `notes/`, not in the git history
- Feel free to check out different commits/bisect/etc when needed, but always end up back where you started unless explicitly asked
- Git push hangs on this system without user approval, only push if explicitly asked
- Never `/schedule` anything, and never suggest using `/schedule` unprompted — only use the feature when the user explicitly asks for it

## Less is more
Don't be too long-winded in docs, comments, error messages, output to user and prose generally. Keep stuff compact and to-the-point, you're not being paid by the word. Only include extensive details where appropriate and actually useful.

## Background
The compositor clears every frame to a flat color matching the rest of the app's UI (`tawc_window_bg`, `#1F1B22` in dark mode). The constant is `BACKGROUND_COLOR` in `render.rs`; the matching Android resource is `tawc_window_bg` in `values-night/colors.xml`. Keep them in sync.

## Wayland Buffers
We support both hardware buffers (AHB) and SHM buffers. SHM buffers are tinted magenta to make fallback paths obvious. Do not remove magenta tinting unless explicitly asked.

## Device Support
Goal: run on all modern Android phones without firmware modifications. Requiring root is viable (especially for testing), but ideally rootless.

## Safety
I'm letting you play with my phone, try not to fuck it up.
- Do not reboot the physical phone unless explicitly asked to.
- The emulator is fair game — feel free to reboot, kill qemu, wipe the AVD, etc.

## Organization
Avoid junking up devices (delete screenshots when done). On the phone, the chroot lives in the app's private data dir (`/data/data/me.phie.tawc/distros/<id>/rootfs/`).

**On-device path policy:** the only place anything outside the app's private dir may write is `/data/local/tmp/tawc-dev/` (exposed as `$TAWC_SCRATCH` from `scripts/lib/tawc-scratch.sh`, and `TAWC_SCRATCH` from the Rust integration crate). Test/debug artefacts, `adb push` staging, screenshots, fake-bwrap, firefox configs, tawcroot test binaries — **all** go there. **Production must never touch `/data/local/...` or `/sdcard/...`** — everything the running app needs ships in the APK and lives under `/data/data/me.phie.tawc/`, so `pm uninstall` removes every trace. New scripts that need on-device scratch should `source scripts/lib/tawc-scratch.sh` (or use the Rust constant) rather than reaching for `/data/local/tmp/` directly.

Do not clone external repos into `$HOME/`. If you need a third-party
checkout (e.g. for cross-compilation source like libxkbcommon, or
tooling like rootAVD), clone it into `deps/` (which is gitignored
wholesale by `deps/.gitignore` — only the manifest, the libhybris
shims, and the xwayland patches are tracked) or delete it when you're
done. Do NOT leave it in `$HOME/` for future sessions to find — that
has bitten us with hardcoded `/home/ai/libxkbcommon` paths in
`compositor/build.rs`.

## Libhybris fork
- **libhybris fork:** https://github.com/wmww/libhybris — pinned by commit hash in `deps/deps.list`, auto-cloned to `deps/libhybris` by `scripts/build-libhybris.sh` on first run (and HEAD-verified on every run). Already gitignored via `deps/.gitignore`.
- **Build:** `bash scripts/build-libhybris.sh [--clean]`. Cross-compiles for aarch64 glibc using the host toolchain — see [notes/building.md](notes/building.md) for deps. Output staged in `build/libhybris-aarch64/install/` and packed into the APK as `assets/libhybris/arm64-v8a.tar` by Gradle's `packLibhybris` task. The Kotlin installer extracts the asset on first compositor start and symlinks the tree into `<rootfs>/usr/local/lib/` at chroot install time (`LibhybrisLinker.kt`).
- **Fork docs:** `deps/libhybris/TAWC_FORK.md` documents the fork's lineage and our changes. Keep it up to date when modifying libhybris.
- Our libhybris fork is a set of clean, self-contained commits on top of https://github.com/libhybris/libhybris
- We combine commits that are logically part of the same change, unless the commits originally came from other forks (not written by us)
- The libhybris commit history post-upstream is not a "history", it is a set of commits. Old commits can and should be edited when our implementation of that feature/fix changes
- The final libhybris commit is always the one that adds TAWC_FORK.md. Changes to TAWC_FORK.md are always amended into this final commit rather than being added as additional commits like you would normally do
- TAWC_FORK.md does not reference any of our commits by hash, as those would need to be kept up to date every change
- Only commit to libhybris when and how you are told to
- Each time we update our libhybris fork, we tag it based on the date and a counter, like `tawc-15-Apr-2026-0` (would be `...-1` for next update on same day, etc) and push the tags. This allows us to track and revert history of our changes while keeping git history clean
- After amending/rewriting libhybris commits, **bump the libhybris commit hash in `deps/deps.list`** in the same change. Otherwise other checkouts will refuse to build until they re-run `scripts/update-deps.sh`. (That refusal is the system working — silent drift would be far worse.)
- Git can be tricky, and this is an atypical workflow. Be careful and think things true when updating the libhybris git history

## Quick Reference
- **Building from a fresh system:** see [notes/building.md](notes/building.md) for host packages, env, vendored repos, and the full walkthrough. Keep that doc in sync as you change build deps.
- **Vendored deps:** pinned in `deps/deps.list`. Build scripts auto-clone + verify HEAD; `bash scripts/update-deps.sh [name…]` is the one command that mutates dep checkouts (use after pulling a manifest update or after a "wrong commit" build error). Bumping a dep = bumping `deps.list` + running `update-deps`.
- **Dev exec broker:** debug-build-only in-app `LocalServerSocket` that lets host scripts run commands as the app uid + SELinux domain — same context as a user-launched run. Started from `TawcApplication.onCreate` when `BuildConfig.DEBUG`. Replaces nearly every previous `run-as` / `su -c` call: chroot-run, install-test-deps, integration tests, the install-id probe, the readiness check. The remaining privileged paths are the `chroot` install method (`chroot(2)` needs root), `tawcroot/test --device` (handler tests need `mknod` privilege), and `scripts/emulator.sh` setup. Host helper at `tools/tawc-exec/`; sourced library at `scripts/lib/tawc-exec.sh`. See [notes/exec-broker.md](notes/exec-broker.md) for the protocol and `SO_PEERCRED` security model.
- **Build (everything):** `JAVA_HOME=/usr/lib/jvm/java-21-openjdk ANDROID_HOME=$HOME/Android/Sdk ./gradlew assembleDebug` — Gradle auto-invokes `scripts/build-libhybris.sh`, `scripts/build-libxkbcommon.sh`, `scripts/build-xwayland.sh`, `scripts/build-proot.sh`, and `tawcroot/build` as needed (each task is up-to-date when its output binary already exists).
- **Build (libhybris standalone):** `bash scripts/build-libhybris.sh [--clean]`. Aarch64 glibc cross-build, output in `build/libhybris-aarch64/install/`; bundled into the APK by the Gradle `packLibhybris` task.
- **Build (libxkbcommon standalone):** `bash scripts/build-libxkbcommon.sh [--abi=aarch64|x86_64|both] [--clean]`. Clones a pinned upstream tag into `deps/libxkbcommon/` (gitignored) if missing — no patches.
- **Build (proot standalone):** `bash scripts/build-proot.sh [--abi=aarch64|x86_64|both] [--clean]`. Clones the pinned [Termux fork](https://github.com/termux/proot) into `./deps/proot/` and downloads talloc into `./deps/proot-deps/` (both gitignored). Outputs `libproot.so` + `libproot-loader.so` jniLibs. See [notes/proot.md](notes/proot.md) for why we use Termux's fork and not upstream proot-me.
- **Build (tawcroot standalone):** `bash tawcroot/build [--abi=aarch64|x86_64|both|host] [--testhost] [--clean]`. Static non-PIE ET_EXEC, `-nostdlib` freestanding. `aarch64`/`x86_64` are NDK cross-builds staged at `app/src/main/jniLibs/<abi>/libtawcroot.so` (production only — no test scaffolding). `host` is a native glibc x86_64 build at `build/tawcroot-host/tawcroot` for fast iteration; also produces `tawcroot-testhost` (test-driving twin) and `tests` (cleat orchestrator). The host build is incremental — driven by `tawcroot/Makefile` with `gcc -MMD -MP` header dep tracking and `-j$(nproc)` parallel compile (cold build ~700ms, warm ~30ms). For inner-loop iteration you can run `make -C tawcroot` directly; `tawcroot/build --abi=host` adds the cleat clone step. `--testhost` adds `tawcroot-testhost` for cross-ABIs (used by `tawcroot/test --device`). cleat is cloned into `./deps/cleat/` on first `--abi=host` and is built **only** into the test orchestrator — never into either tawcroot binary. See [notes/tawcroot.md](notes/tawcroot.md).
- **Test (tawcroot):** `bash tawcroot/test [--host|--device] [--no-build] [FILTER...]`. Default: `--host`; flips to `--device` if `TAWC_TARGET=physical|emulator` is set in the env (an explicit `--host`/`--device` flag wins). Host mode execs the cleat-driven runner at `build/tawcroot-host/tests`, which forks `tawcroot-testhost` for handler-layer cases (phase-0 foundation smoke, phase-1 path translation + runtime invariants), forks production `tawcroot` for integration cases, and runs unit cases in-process. `FILTER` args are full-match regexes against `module`, `name`, or `module::name` (e.g. `'.*phase.*'`, `phase0_foundation_smoke`, `handler/test_phase1`). Exit code is 0 on pass, 1 on any failure. `--device` mode pushes the NDK-cross-built cleat orchestrator (`build/tawcroot-<abi>/tests`) plus `tawcroot`, `tawcroot-testhost`, and fixtures to `$TAWC_SCRATCH` (`/data/local/tmp/tawc-dev/`), then runs the **same** four-layer cleat suite there via `su -c`. `FILTER` args propagate verbatim. Device target is picked the same way as other scripts (`.tawctarget` / `TAWC_TARGET=physical|emulator`).
- **Install methods:** `tawcroot` is the default and the **only officially supported** method — `chroot` and `proot` are debug-only (release builds ship just tawcroot; see `notes/installation.md` "Install methods"). All three are exposed in debug builds for the dev loop. Build-time override: `-PtawcMethods=tawcroot[,proot[,chroot]]`. `scripts/tawc-chroot-run.sh` works against any method (reads `metadata.json` to dispatch).
- **Install distro (CLI):** `bash scripts/install-distro.sh <id> [tawcroot|proot|chroot] [key=value …]`. Triggers an install via the dev exec broker (`tawc-exec --action install`); progress + log lines stream to your TTY and the in-app `LogScreenActivity` opens automatically. Method defaults to **tawcroot** (recommended); `proot`/`chroot` are debug-build-only and rejected on release APKs. Extra `key=value` args become broker `--arg` flags — supported keys are `distro=<key>`, `label=<text>`, `mirrorProxy=<url>`. The slot id `<id>` is the on-disk dir name; rootfs lands at `/data/data/me.phie.tawc/distros/<id>/rootfs/`. Test/run scripts auto-detect the unique slot; with multiple installs, pin one via `TAWC_INSTALL_ID=<id>`. Ctrl-C cancels the install (broker socket close → action calls `Operation.cancel`).
- **Uninstall distro (CLI):** `bash scripts/uninstall-distro.sh <id>`. Same broker path; no confirm prompt at the host (matches the in-app behaviour where Cancel during uninstall doesn't confirm — see notes/installation.md).
- **Build, install & launch:** `bash scripts/app-build-install.sh` (uses `.tawctarget` / `TAWC_TARGET`; `--no-build` reuses the existing APK; `--no-launch` installs without starting). The launch path is `MainActivity` — `am start … .compositor.CompositorActivity` directly does not work.
- **Tawc-exec (run any command as app uid):** `bash -c '. scripts/lib/select-device.sh; . scripts/lib/tawc-exec.sh; tawc_exec /system/bin/cat /data/data/me.phie.tawc/distros/<id>/metadata.json'`. Useful for inspecting app data without `run-as`/`su`. Auto-launches the app if the broker isn't bound.
- **Chroot (interactive):** `bash scripts/tawc-chroot-run.sh`
- **Run Wayland app:** `bash scripts/tawc-chroot-run.sh '<command>'` (env vars set by profile)
- **Firefox:** `bash scripts/tawc-chroot-run.sh 'GDK_GL=gles:always firefox --no-remote'`
- **Screenshot:** `adb shell screencap -p /data/local/tmp/tawc-dev/screenshot.png && adb pull /data/local/tmp/tawc-dev/screenshot.png /tmp/screenshot.png` (analyze with sub-agent, then `adb shell rm /data/local/tmp/tawc-dev/screenshot.png && rm /tmp/screenshot.png`). No `su` needed — `screencap` works as the `shell` uid and the scratch dir is shell-writable.
- **Logs:** `adb logcat -s tawc-native` (Rust) or `adb logcat -s tawc` (Kotlin). Filter frame spam: `grep -v renderer_gles2_frame`
- **Kill Firefox:** `adb shell "su -c 'killall firefox'"`
- **Restart compositor:** `bash scripts/app-build-install.sh --no-build` (force-stops, reinstalls the existing APK, launches MainActivity)
- **Simulate touch:** `adb shell input tap X Y` (screen pixel coords, 1:1 with SurfaceView due to immersive fullscreen)
- **Touch debug loop:** Screenshot -> identify coords -> tap -> screenshot -> verify. Compositor uses 2x scale (logical = physical/2). Nearby UI elements are easy to confuse.
- **Install test deps in chroot:** `bash scripts/install-test-deps.sh` (run once per chroot install — gtk3/gtk4/weston/mesa-utils/vulkan-tools/pkg-config; tests do not auto-install)
- **Integration tests:** `bash scripts/run-integration-tests.sh [filter]` (builds everything, deploys, runs all tests; the optional arg is a libtest substring filter, e.g. `apps::` or `apps::test_firefox`)
- **Integration tests (skip rebuild):** add `--no-build` to reuse the already-deployed APK / libhybris / chroot helpers
- (`run-integration-tests.sh` sources `scripts/lib/select-device.sh` itself; when both targets are connected, run with `TAWC_TARGET=physical` or `TAWC_TARGET=emulator`.)
- **Build debug app:** `bash scripts/build-debug-app.sh` (gtk4-debug-app)
- **Run GTK4 debug app:** `bash scripts/tawc-chroot-run.sh '/tmp/gtk4-debug-app/gtk4-debug-app text-input'`
- **Inject text (for testing):** `adb shell am broadcast -a me.phie.tawc.TEXT_INPUT --es text "hello"`
- **Inject keyevent (for testing):** `adb shell am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode 67`
