Hi Claude, this project is Tess's Android Wayland Compositor (tawc)

## Plan
[plan.md](plan.md) has the implementation roadmap (phases 1-10, current phase: 6 text input).

## Notes
The `notes/` directory contains architecture and implementation notes. Key files:
- [architecture.md](notes/architecture.md) -- compositor module layout, design decisions, Smithay integration
- [building.md](notes/building.md) -- build instructions and deployment
- [gpu-strategy.md](notes/gpu-strategy.md) -- GPU driver strategy, libhybris, AHB buffer sharing
- [wsi-layer.md](notes/wsi-layer.md) -- client-side EGL WSI layer
- [rendering.md](notes/rendering.md) -- window management, coordinate system, SHM buffers
- [input.md](notes/input.md) -- touch input architecture
- [text-input.md](notes/text-input.md) -- text input design (Android IME <-> zwp_text_input_v3)
- [firefox.md](notes/firefox.md) -- Firefox-specific setup and issues
- [android.md](notes/android.md) -- socket sharing, SELinux, chroot setup

Keep notes up to date with new choices and discoveries. This is an agent-written project -- existing code/notes may be wrong. Stay vigilant.

## Workflow
- Debug against a real Android phone via adb
- Work autonomously when possible. If you need human help to set up your dev loop, ask
- When analyzing screenshots, use a sub-agent so the image doesn't end up in main context
- If `su` is available on the phone, use it instead of `adb root`
- Use existing scripts (eg arch-chroot-run) instead of one-shotting commands
- Feel free to edit scripts/files that have problems

## Wayland Buffers
We support both hardware buffers (AHB) and SHM buffers. SHM buffers are tinted magenta to make fallback paths obvious. Do not remove magenta tinting unless explicitly asked.

## Device Support
Goal: run on all modern Android phones without firmware modifications. Requiring root is viable (especially for testing), but ideally rootless.

## Safety
I'm letting you play with my phone, try not to fuck it up.

## Organization
Avoid junking up devices (delete screenshots when done). On the phone, things stay in `/data/local/arch-chroot/`, `/data/local/claude-debug` (**NOT** `/data/local/tmp`).

## Quick Reference
- **Build:** `cd server && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug`
- **Install & launch:** `adb install -r server/app/build/outputs/apk/debug/app-debug.apk && adb shell am force-stop me.phie.tawc && adb shell am start -n me.phie.tawc/.MainActivity`
- **Chroot:** `adb push client/arch-chroot-run /data/local/tmp/ && adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run"` (SELinux must be permissive: `adb shell su -c setenforce 0`)
- **Run Wayland app:** `adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run '<command>'"` (env vars set by profile)
- **Firefox:** `adb shell su -c "/system_ext/bin/bash /data/local/tmp/arch-chroot-run 'GDK_GL=disabled MOZ_ENABLE_WAYLAND=1 MOZ_ACCELERATED=1 MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1 MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 DISPLAY= firefox --no-remote'"`
- **Screenshot:** `adb shell su -c "screencap -p /sdcard/screenshot.png" && adb pull /sdcard/screenshot.png /tmp/screenshot.png` (analyze with sub-agent, then clean up both files)
- **Logs:** `adb logcat -s tawc-native` (Rust) or `adb logcat -s tawc` (Kotlin). Filter frame spam: `grep -v renderer_gles2_frame`
- **Kill Firefox:** `adb shell su -c "killall firefox"`
- **Restart compositor:** `adb shell am force-stop me.phie.tawc && adb shell am start -n me.phie.tawc/.MainActivity`
- **Simulate touch:** `adb shell input tap X Y` (screen pixel coords, 1:1 with SurfaceView due to immersive fullscreen)
- **Touch debug loop:** Screenshot -> identify coords -> tap -> screenshot -> verify. Compositor uses 2x scale (logical = physical/2). Nearby UI elements are easy to confuse.
