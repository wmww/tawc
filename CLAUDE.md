Hi Claude! This project is Tess's Android Wayland Compositor (tawc). It's an Android app and set of chroot scripts to run desktop Linux programs on Android.

## Plan
[plan.md](plan.md) has the implementation roadmap.

## Building
**[notes/building.md](notes/building.md) is the source of truth for build-time dependencies and the fresh-system build flow.** Consult it before building on a new machine, and **update it in the same change** whenever you add or change a build-time dep (host package, vendored repo, env var, toolchain version). The Quick Reference at the bottom of this file is a cheat sheet for already-set-up systems — building.md is the doc to follow when setting up.

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
- [installation.md](notes/installation.md) -- in-app Kotlin chroot installer (separate from `client/arch-chroot-*`); broadcast command interface for adb-driven workflows
- [proot.md](notes/proot.md) -- rootless install method: vendored Termux/proot fork, why upstream proot doesn't work on Android, Android quirks worked around
- [distro-options.md](notes/distro-options.md) -- survey of viable glibc distros (Debian, Void, Manjaro ARM, …) and why musl/bionic alternatives don't fit

Keep notes up to date with new choices, discoveries and project state. This is an agent-written project, existing code/notes may be wrong. Stay vigilant, and fix/record problems as you find them (even when working on something else).

## Issues
- Issues are located in `issues/`
- Do not solve issues unless either you're asked to work on them, or a solution falls out of the work you are asked to do
- Create and update issues as needed, in particular create an issue when you find a problem that's nontrivial to fix while working on something else
- Include detailed steps to reproduce if relevant
- Delete issues when they are confirmed fully solved
- If an issue has important info that remains relevant after its solved, integrate that info into your notes when deleting it
- Issues should be markdown documents starting with a title and one-line explanation, followed by a complete description with all relevant details

## Workflow
- Debugging against both a real Android phone via adb or an emulator (x86_64 AVD with Magisk) are supported
- libhybris/GPU drivers are not supported on thet emulator — see [emulator.md](notes/emulator.md)
- A device can be selected with `TAWC_TARGET=device` or `TAWC_TARGET=emulator`, however generally default target should be automatically picked up (specified in `./.tawctarget`)
- Use the default device unless explicitly requested/approved by the user.
- Work autonomously when possible. If you need human help to set up your dev loop, ask
- When analyzing screenshots, use a sub-agent so the image doesn't end up in main context
- If `su` is available on the phone, use it instead of `adb root`
- Use existing scripts (eg `client/tawc-chroot-run`) instead of one-shotting commands
- Feel free to edit scripts/files that have problems
- Only git commit when told to
- Feel free to check out different commits/bisect/etc when needed, but always end up back where you started unless explicitly asked
- Git push hangs on this system without user approval, only push if explicitly asked
- Never `/schedule` anything, and never suggest using `/schedule` unprompted — only use the feature when the user explicitly asks for it

## Background
The compositor shows a vertical gradient background (black at top, bright blue-turquoise at bottom) when no app is covering the screen. This is rendered via a custom Smithay texture shader on a 1x1 dummy texture (`background.rs`). The color constant is `TURQUOISE` in that file.

## Wayland Buffers
We support both hardware buffers (AHB) and SHM buffers. SHM buffers are tinted magenta to make fallback paths obvious. Do not remove magenta tinting unless explicitly asked.

## Device Support
Goal: run on all modern Android phones without firmware modifications. Requiring root is viable (especially for testing), but ideally rootless.

## Safety
I'm letting you play with my phone, try not to fuck it up.
- Do not reboot the physical phone unless explicitly asked to.
- The emulator is fair game — feel free to reboot, kill qemu, wipe the AVD, etc.

## Organization
Avoid junking up devices (delete screenshots when done). On the phone, the chroot lives in the app's private data dir (`/data/data/me.phie.tawc/distros/<id>/rootfs/`); host-side scratch goes in `/data/local/claude-debug` (**NOT** `/data/local/tmp`).

Do not clone external repos into `$HOME/`. If you need a third-party
checkout (e.g. for cross-compilation source like libxkbcommon, or
tooling like rootAVD), clone it into the tawc project directory or a
child path. When you're done, either `.gitignore` it (matching the
existing `libhybris/`, `smithay/`, `rootAVD/` entries) or delete it.
Do NOT leave it in `$HOME/` for future sessions to find — that has
bitten us with hardcoded `/home/ai/libxkbcommon` paths in
`compositor/build.rs`.

## Libhybris fork
- **libhybris fork:** https://github.com/wmww/libhybris -- clone to `./libhybris` if needed (`git clone https://github.com/wmww/libhybris.git ./libhybris`). Already in `.gitignore`.
- **Build:** `bash client/build-libhybris-aarch64 [--clean]`. Cross-compiles for aarch64 glibc using the host toolchain — see [notes/building.md](notes/building.md) for deps. Output staged in `build/libhybris-aarch64/install/` and packed into the APK as `assets/libhybris/arm64-v8a.tar` by Gradle's `packLibhybris` task. The Kotlin installer extracts the asset on first compositor start and symlinks the tree into `<rootfs>/usr/local/lib/` at chroot install time (`LibhybrisLinker.kt`).
- **Fork docs:** `libhybris/TAWC_FORK.md` documents the fork's lineage and our changes. Keep it up to date when modifying libhybris.
- Our libhybris fork is a set of clean, self-contained commits on top of https://github.com/libhybris/libhybris
- We combine commits that are logically part of the same change, unless the commits originally came from other forks (not written by us)
- The libhybris commit history post-upstream is not a "history", it is a set of commits. Old commits can and should be edited when our implementation of that feature/fix changes
- The final libhybris commit is always the one that adds TAWC_FORK.md. Changes to TAWC_FORK.md are always amended into this final commit rather than being added as additional commits like you would normally do
- TAWC_FORK.md does not reference any of our commits by hash, as those would need to be kept up to date every change
- Only commit to libhybris when and how you are told to
- Each time we update our libhybris fork, we tag it based on the date and a counter, like `tawc-15-Apr-2026-0` (would be `...-1` for next update on same day, etc) and push the tags. This allows us to track and revert history of our changes while keeping git history clean
- Git can be tricky, and this is an atypical workflow. Be careful and think things true when updating the libhybris git history

## Quick Reference
- **Building from a fresh system:** see [notes/building.md](notes/building.md) for host packages, env, vendored repos, and the full walkthrough. Keep that doc in sync as you change build deps.
- **Build (compositor + libhybris asset):** `cd server && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug` — Gradle invokes `client/build-libhybris-aarch64` automatically.
- **Build (libhybris standalone):** `bash client/build-libhybris-aarch64 [--clean]`. Aarch64 glibc cross-build, output in `build/libhybris-aarch64/install/`; bundled into the APK by the Gradle `packLibhybris` task.
- **Build (libxkbcommon):** `bash client/build-libxkbcommon [--abi=aarch64|x86_64|both] [--clean]`. Clones a pinned upstream tag into `./libxkbcommon/` (gitignored) if missing — no patches. Run once after fresh clone or when bumping the version pin.
- **Build (proot):** `bash client/build-proot [--abi=aarch64|x86_64|both] [--clean]`. Clones the pinned [Termux fork](https://github.com/termux/proot) into `./proot/` and downloads talloc into `./proot-deps/` (both gitignored). Outputs `libproot.so` + `libproot-loader.so` jniLibs. See [notes/proot.md](notes/proot.md) for why we use Termux's fork and not upstream proot-me.
- **Install (proot, rootless):** add `--es method proot` to the install activity intent: `adb shell am start -n me.phie.tawc/.install.InstallActivity --es autoStart true --es id arch --es method proot`. The radio in InstallActivity defaults to chroot when `su` works, proot otherwise. `client/tawc-chroot-run` works against either method (reads `metadata.json` to dispatch).
- **Install & launch:** `adb install -r server/app/build/outputs/apk/debug/app-debug.apk && adb shell am force-stop me.phie.tawc && adb shell am start -n me.phie.tawc/.compositor.CompositorActivity`
- **Install chroot:** `adb shell am start -n me.phie.tawc/.install.InstallActivity --es autoStart true --es id arch` (then `adb logcat -s tawc-install` to watch). The chroot lives at `/data/data/me.phie.tawc/distros/arch/rootfs/`.
- **Chroot (interactive):** `bash client/tawc-chroot-run`
- **Run Wayland app:** `bash client/tawc-chroot-run '<command>'` (env vars set by profile)
- **Firefox:** `bash client/tawc-chroot-run 'GDK_GL=gles:always firefox --no-remote'`
- **Screenshot:** `adb shell "su -c 'screencap -p /sdcard/screenshot.png'" && adb pull /sdcard/screenshot.png /tmp/screenshot.png` (analyze with sub-agent, then clean up both files)
- **Logs:** `adb logcat -s tawc-native` (Rust) or `adb logcat -s tawc` (Kotlin). Filter frame spam: `grep -v renderer_gles2_frame`
- **Kill Firefox:** `adb shell "su -c 'killall firefox'"`
- **Restart compositor:** `adb shell am force-stop me.phie.tawc && adb shell am start -n me.phie.tawc/.compositor.CompositorActivity`
- **Simulate touch:** `adb shell input tap X Y` (screen pixel coords, 1:1 with SurfaceView due to immersive fullscreen)
- **Touch debug loop:** Screenshot -> identify coords -> tap -> screenshot -> verify. Compositor uses 2x scale (logical = physical/2). Nearby UI elements are easy to confuse.
- **Install test deps in chroot:** `bash testing/install-test-deps.sh` (run once per chroot install — gtk3/gtk4/weston/mesa-utils/vulkan-tools/pkg-config; tests do not auto-install)
- **Integration tests:** `bash testing/run-integration-tests.sh [filter]` (builds everything, deploys, runs all tests; the optional arg is a libtest substring filter, e.g. `apps::` or `apps::test_firefox`)
- **Integration tests (skip rebuild):** add `--no-build` to reuse the already-deployed APK / libhybris / chroot helpers
- (`run-integration-tests.sh` sources `client/select-device.sh` itself; when both targets are connected, run with `TAWC_TARGET=device` or `TAWC_TARGET=emulator`.)
- **Build debug app:** `bash testing/build-debug-app.sh` (gtk4-debug-app)
- **Run GTK4 debug app:** `bash client/tawc-chroot-run '/tmp/gtk4-debug-app/gtk4-debug-app text-input'`
- **Inject text (for testing):** `adb shell am broadcast -a me.phie.tawc.TEXT_INPUT --es text "hello"`
- **Inject keyevent (for testing):** `adb shell am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode 67`
