Hi Claude! This project is Tess's Android Wayland Compositor (tawc). It's an Android app and set of chroot scripts to run desktop Linux programs on Android.

## Plan
[plan.md](plan.md) has the implementation roadmap.

## Notes
The `notes/` directory contains architecture and implementation notes. Edit/create/split/merge/rename notes as needed without being asked. Key files:
- [architecture.md](notes/architecture.md) -- compositor module layout, design decisions, Smithay integration
- [building.md](notes/building.md) -- build instructions and deployment
- [gpu-strategy.md](notes/gpu-strategy.md) -- GPU driver strategy, libhybris, AHB buffer sharing
- [wsi-layer.md](notes/wsi-layer.md) -- client-side EGL WSI layer
- [rendering.md](notes/rendering.md) -- window management, coordinate system, SHM buffers
- [input.md](notes/input.md) -- touch input architecture
- [text-input.md](notes/text-input.md) -- text input design (Android IME <-> zwp_text_input_v3)
- [firefox.md](notes/firefox.md) -- Firefox-specific setup and issues
- [android.md](notes/android.md) -- socket sharing, SELinux, chroot setup

Keep notes up to date with new choices, discoveries and project state. This is an agent-written project, existing code/notes may be wrong. Stay vigilant, and fix/record problems as you find them (even when working on something else).

## Issues
- Issues are located in `issues/`
- Do not solve issues unless either you're asked to work on them, or a solution falls out of the work you are asked to do
- Create and update issues as needed, in particular create an issue when you find a problem that's nontrivial to fix while working on something else
- Include detailed steps to reproduce if relevant
- Delete issues when they are confirmed fully solved
- If an issue has important info that remains relevant after its solved, integrate that info into your notes when deleting it

## Workflow
- Debug against a real Android phone via adb
- Work autonomously when possible. If you need human help to set up your dev loop, ask
- When analyzing screenshots, use a sub-agent so the image doesn't end up in main context
- If `su` is available on the phone, use it instead of `adb root`
- Use existing scripts (eg arch-chroot-run) instead of one-shotting commands
- Feel free to edit scripts/files that have problems
- Only git commit when told to
- Feel free to check out different commits/bisect/etc when needed, but always end up back where you started unless explicitly asked
- Git push hangs on this system without user approval, only push if explicitly asked

## Background
The compositor shows a vertical gradient background (black at top, bright blue-turquoise at bottom) when no app is covering the screen. This is rendered via a custom Smithay texture shader on a 1x1 dummy texture (`background.rs`). The color constant is `TURQUOISE` in that file.

## Wayland Buffers
We support both hardware buffers (AHB) and SHM buffers. SHM buffers are tinted magenta to make fallback paths obvious. Do not remove magenta tinting unless explicitly asked.

## Device Support
Goal: run on all modern Android phones without firmware modifications. Requiring root is viable (especially for testing), but ideally rootless.

## Safety
I'm letting you play with my phone, try not to fuck it up.
- Do not reboot the Android device unless explicitly asked to.

## Organization
Avoid junking up devices (delete screenshots when done). On the phone, things stay in `/data/local/arch-chroot/`, `/data/local/claude-debug` (**NOT** `/data/local/tmp`).

## Libhybris fork
- **libhybris fork:** https://github.com/wmww/libhybris -- clone to `./libhybris` if needed (`git clone https://github.com/wmww/libhybris.git ./libhybris`). Already in `.gitignore`.
- **Build libhybris from local source:** `bash client/build-libhybris [--clean]` — syncs `./libhybris` to phone and builds inside chroot.
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
- **Build (compositor):** `cd server && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug`
- **Build (libhybris):** `bash client/build-libhybris` (or `--clean` to reconfigure). Edit `./libhybris` locally, script syncs to phone.
- **Install & launch:** `adb install -r server/app/build/outputs/apk/debug/app-debug.apk && adb shell am force-stop me.phie.tawc && adb shell am start -n me.phie.tawc/.MainActivity`
- **Chroot:** `adb push client/arch-chroot-run /data/local/tmp/ && adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run"` (SELinux must be permissive: `adb shell su -c setenforce 0`)
- **Run Wayland app:** `adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run '<command>'"` (env vars set by profile)
- **Firefox:** `adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run 'GDK_GL=gles:always MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 DISPLAY= firefox --no-remote'"`
- **Screenshot:** `adb shell su -c "screencap -p /sdcard/screenshot.png" && adb pull /sdcard/screenshot.png /tmp/screenshot.png` (analyze with sub-agent, then clean up both files)
- **Logs:** `adb logcat -s tawc-native` (Rust) or `adb logcat -s tawc` (Kotlin). Filter frame spam: `grep -v renderer_gles2_frame`
- **Kill Firefox:** `adb shell su -c "killall firefox"`
- **Restart compositor:** `adb shell am force-stop me.phie.tawc && adb shell am start -n me.phie.tawc/.MainActivity`
- **Simulate touch:** `adb shell input tap X Y` (screen pixel coords, 1:1 with SurfaceView due to immersive fullscreen)
- **Touch debug loop:** Screenshot -> identify coords -> tap -> screenshot -> verify. Compositor uses 2x scale (logical = physical/2). Nearby UI elements are easy to confuse.
- **Integration tests (full):** `bash testing/run-integration-tests.sh` (builds everything, deploys, runs tests. Feel free to do these as-needed instead of using this script)
- **Integration tests (tests only):** `cd testing/integration && cargo test -- --nocapture --test-threads=1`
- **Build debug apps:** `bash testing/build-debug-app.sh` (both gtk3+gtk4; or `... gtk3` / `... gtk4`)
- **Run GTK3 debug app:** `adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run 'GDK_GL=gles:always /tmp/gtk3-debug-app/gtk3-debug-app text-input'"` (must build first)
- **Run GTK4 debug app:** `adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run '/tmp/gtk4-debug-app/gtk4-debug-app text-input'"`
- **Inject text (for testing):** `adb shell am broadcast -a me.phie.tawc.TEXT_INPUT --es text "hello"`
- **Inject keyevent (for testing):** `adb shell am broadcast -a me.phie.tawc.KEY_EVENT --ei keycode 67`
