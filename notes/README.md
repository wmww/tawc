# Notes Index

Start here when looking for durable project context. `AGENTS.md` keeps only always-needed operating rules; detailed design/build/test information belongs in these notes. Future-work plans live in [`../plans/`](../plans/).

## Build, Install, Test

- [building.md](building.md) - host packages, env vars, vendored deps, component builds, APK assembly.
- [installation.md](installation.md) - in-app installer, distro state machine, install methods, shipped rootfs files, integrity policy.
- [external-binds.md](external-binds.md) - per-install binds of host dirs (e.g. shared storage) into the rootfs; all-files-access gating.
- [testing.md](testing.md) - integration-test layout, backend pins, test deps, debug app, input injection.
- [cache-proxy.md](cache-proxy.md) - dev mirror cache behavior and safety rules.
- [emulator.md](emulator.md) - AVD setup, rooted/rootless workflows, x86_64 limitations.
- [release.md](release.md) - versioning scheme, release prep/publish steps, keystore rules.

## Runtime Architecture

- [architecture.md](architecture.md) - compositor module layout and Smithay integration.
- [android.md](android.md) - Android platform integration, sockets, SELinux, app-private state.
- [exec-broker.md](exec-broker.md) - debug broker protocol, host helper, action model, security.
- [ando.md](ando.md) - production `ando <cmd>` broker: run Android commands from inside the rootfs.
- [rendering.md](rendering.md) - window management, coordinates, SHM/AHB rendering behavior.
- [rootfs-sessions.md](rootfs-sessions.md) - session invariant for rootfs entry paths.
- [log-screen.md](log-screen.md) - shared operation/log-screen UI abstraction.
- [launcher.md](launcher.md) - distro launcher and `.desktop` scanner.
- [terminal.md](terminal.md) - in-app per-distro terminal (vendored termux terminal modules, tawcroot pty spawn path).
- [multi-activity.md](multi-activity.md) - one-Android-task-per-window design and implementation notes.
- [task-icons-window-index.md](task-icons-window-index.md) - task recents icons/labels and Kotlin open-window metadata mirror.

## Graphics Backends

- [gpu-strategy.md](gpu-strategy.md) - overall GPU strategy and buffer-sharing background; libhybris is the working production default.
- [wsi-layer.md](wsi-layer.md) - libhybris Wayland EGL/Vulkan WSI details.
- [gfxstream-bridge.md](gfxstream-bridge.md) - experimental gfxstream/kumquat backend; Vulkan via AHB works on physical hardware, but it is partial and not production-ready.
- [libhybris-zink.md](libhybris-zink.md) - implemented libhybris+Zink backend; blocked on currently available Vulkan 1.1 hardware.
- [xwayland.md](xwayland.md) - bionic-built Xwayland, TAWC-DRI, and X11 client support.

## Input And Apps

- [input.md](input.md) - touch input and Android Back behavior.
- [gtk3-broken-menus-workaround.md](gtk3-broken-menus-workaround.md) - contained compatibility workaround for GTK3 native Wayland menubars on touch-only devices.
- [text-input.md](text-input.md) - Android IME to `zwp_text_input_v3` bridge.
- [clipboard.md](clipboard.md) - Android/Wayland clipboard bridge: announce-only syncs, paste-time fetch, eager client mirror.
- [firefox.md](firefox.md) - Firefox setup, libhybris fixes, browser-specific issues.

## Rootfs Methods And Distros

- [tawcroot/](tawcroot/README.md) - systrap-based rootless runtime architecture and implementation notes (split into topic files).
- [proot.md](proot.md) - debug-only Termux/proot install method.
- [chroot.md](chroot.md) - debug-only root `chroot(2)` install method.
- [distro-abstraction.md](distro-abstraction.md) - distro definition/refactor notes.
- [distro-options.md](distro-options.md) - survey of viable glibc distros.

## Maintenance Rules

- Keep notes factual and current. Prefer correcting stale prose over adding warnings around it.
- Move long historical logs out of current-state sections when they start obscuring the present design.
- Link issues only for active unresolved work. When an issue is solved, delete it and move still-useful context here.
- Keep future-work plans in [`../plans/`](../plans/) instead of mixing them into current-state notes.
