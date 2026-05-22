# Notes Index

Start here when looking for durable project context. `AGENTS.md` keeps only always-needed operating rules; detailed design/build/test information belongs in these notes.

## Build, Install, Test

- [building.md](building.md) - host packages, env vars, vendored deps, component builds, APK assembly.
- [installation.md](installation.md) - in-app installer, distro state machine, install methods, shipped rootfs files, integrity policy.
- [testing.md](testing.md) - integration-test layout, backend pins, test deps, debug app, input injection.
- [cache-proxy.md](cache-proxy.md) - dev mirror cache behavior and safety rules.
- [emulator.md](emulator.md) - AVD setup, rooted/rootless workflows, x86_64 limitations.

## Runtime Architecture

- [architecture.md](architecture.md) - compositor module layout and Smithay integration.
- [smithay-desktop-refactor.md](smithay-desktop-refactor.md) - speculative plan for adopting more Smithay desktop/window abstractions.
- [android.md](android.md) - Android platform integration, sockets, SELinux, app-private state.
- [exec-broker.md](exec-broker.md) - debug broker protocol, host helper, action model, security.
- [rendering.md](rendering.md) - window management, coordinates, SHM/AHB rendering behavior.
- [rootfs-sessions.md](rootfs-sessions.md) - session invariant for rootfs entry paths.
- [audio.md](audio.md) - planned PipeWire/PulseAudio bridge to Android audio.
- [log-screen.md](log-screen.md) - shared operation/log-screen UI abstraction.
- [launcher.md](launcher.md) - distro launcher and `.desktop` scanner.
- [multi-activity.md](multi-activity.md) - planned one-Android-task-per-window design.
- [task-icons-window-index.md](task-icons-window-index.md) - task recents icons/labels and Kotlin open-window metadata mirror.

## Graphics Backends

- [gpu-strategy.md](gpu-strategy.md) - overall GPU strategy and buffer-sharing background; libhybris is the working production default.
- [wsi-layer.md](wsi-layer.md) - libhybris Wayland EGL/Vulkan WSI details.
- [gfxstream-bridge.md](gfxstream-bridge.md) - experimental gfxstream/kumquat backend; Vulkan via AHB works on physical hardware, but it is partial and not production-ready.
- [libhybris-zink.md](libhybris-zink.md) - implemented libhybris+Zink backend; blocked on currently available Vulkan 1.1 hardware.
- [desktop-gl-dispatch.md](desktop-gl-dispatch.md) - older dispatcher design; likely superseded by libhybris-zink.
- [xwayland.md](xwayland.md) - bionic-built Xwayland, TAWC-DRI, and X11 client support.

## Input And Apps

- [input.md](input.md) - touch input and Android Back behavior.
- [gtk3-broken-menus-workaround.md](gtk3-broken-menus-workaround.md) - contained compatibility workaround for GTK3 native Wayland menubars on touch-only devices.
- [text-input.md](text-input.md) - Android IME to `zwp_text_input_v3` bridge.
- [firefox.md](firefox.md) - Firefox setup, libhybris fixes, browser-specific issues.

## Rootfs Methods And Distros

- [tawcroot.md](tawcroot.md) - systrap-based rootless runtime architecture and implementation notes.
- [tawcroot-readonly-binds.md](tawcroot-readonly-binds.md) - design notes for future readonly bind support.
- [proot.md](proot.md) - debug-only Termux/proot install method.
- [chroot.md](chroot.md) - debug-only root `chroot(2)` install method.
- [distro-abstraction.md](distro-abstraction.md) - distro definition/refactor notes.
- [distro-options.md](distro-options.md) - survey of viable glibc distros.

## Maintenance Rules

- Keep notes factual and current. Prefer correcting stale prose over adding warnings around it.
- Move long historical logs out of current-state sections when they start obscuring the present design.
- Link issues only for active unresolved work. When an issue is solved, delete it and move still-useful context here.
