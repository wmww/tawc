# Android Integration

## Wayland Socket Sharing

**With root (chroot):** The compositor creates a Unix socket at a known path and the
chroot client connects directly. Root bypasses SELinux MAC checks on `connect()`.
This is the current development approach.

**Without root (proot, future goal):** SELinux blocks cross-app `connect()` between
`untrusted_app` domains on Android 9+. Two viable solutions:

1. **Binder fd passing (preferred):** Compositor creates a `socketpair()`, passes one end
   to Termux via a ContentProvider or bound Service as a `ParcelFileDescriptor`. No
   `connect()` syscall occurs, so SELinux is never triggered.

2. **Shared UID:** `sharedUserId="com.termux"` makes both apps run as same UID/SELinux
   domain. Deprecated since API 33 but still functional. Limits distribution flexibility.

## Chroot Setup

Install (once, via the dev exec broker; progress streams to your TTY
and the in-app log screen opens automatically):
```bash
scripts/tawc-exec.sh --foreground-app --action install \
    --arg id=arch \
    --arg mirrorProxy=http://127.0.0.1:8080/proxy/
```

Then drive the chroot from the host with:
```bash
scripts/rootfs-run.sh                    # interactive shell
scripts/rootfs-run.sh '<command>'        # run a command and exit
```

`rootfs-run` routes through the dev exec broker's `RUNINSIDE` request
(`tawc-exec --in-rootfs <id>`). The broker reads the install's
recorded method from `metadata.json` and dispatches to the matching
[InstallationMethod.startInside], which builds the bind table and
chroot exec fresh in Kotlin on every call. There is no on-disk
wrapper script and no `adb shell su` in this path — chroot installs
fork `su` from inside the JVM. Generic tawc Wayland env vars come
from `RootfsEnv.kt` via a `/usr/bin/env -i KEY=VAL …` wrapper around
the in-rootfs `bash -lc`, so nothing inside the rootfs needs to be
on disk between calls.

### Shell quoting

Commands sent through `tawc-exec --in-rootfs` are framed in the
broker wire protocol (length-prefixed argv), so quoting is not an
issue end-to-end. If you ever bypass the broker and use raw
`adb shell su -c '…'` directly, you'll need to handle the layered
quoting yourself:

**Critical quoting rule for `&&` / `||` in `su -c`:** When running compound
commands via adb, the outer shell (mksh) parses `&&` and `||` BEFORE `su` sees
them. This silently runs the second command as shell (uid 2000), not root:

```bash
# BROKEN: mksh splits at &&. cp runs as root, build runs as shell user.
adb shell su -c "cp /tmp/foo /chroot/tmp/ && /chroot/build.sh"

# CORRECT: inner quotes protect && from mksh.
adb shell "su -c 'cp /tmp/foo /chroot/tmp/ && /chroot/build.sh'"
```

Variable expansion like `$0` or `$KSH_VERSION` at any intermediate layer can
give misleading results. The `su` shell on Android is mksh (`/system/bin/sh`),
easily confused with the chroot's GNU bash.

## EGL Context and Surfaces

- An EGL context CAN move between threads (release on old, bind on new), but expensive
- One thread can render to multiple EGLSurfaces via `eglMakeCurrent` switches
- Each switch flushes the pipeline -- overhead per switch
- Recommended: single render thread, one context, switch surfaces per window
- `ASurfaceTransaction` + AHB avoids `eglMakeCurrent` overhead entirely (future opt)

## Multiple Activities

See [multi-activity.md](multi-activity.md) for the full per-window-task plan.
Background facts that informed it:

- All Activities in one app share the same process (single heap, static state, threads)
- One SurfaceView per Activity avoids Z-ordering issues
- Single background render thread maintains list of active surfaces
- Activity launch creates visual transitions -- suppress with
  `overridePendingTransition(0, 0)`
- Activities may be killed under memory pressure -- handle surface loss gracefully

## Kotlin App Structure

The Android app code (`app/src/main/java/me/phie/tawc/`) is split so that
everything talking to the Rust compositor lives in its own package, separate from
the rest of the app's UI/management features.

- `MainActivity.kt` — home screen. Plain Android UI (no fullscreen, no Wayland).
  Hosts buttons that launch the compositor and the installation manager.
- `compositor/` — everything that interacts with the Rust compositor:
  - `CompositorActivity.kt` — fullscreen immersive Activity that owns the
    `SurfaceView`, dispatches touch/IME, and registers the test broadcast
    receiver. Started via Intent from `MainActivity`. Uses the
    `Theme.Tawc.Compositor` style.
  - `NativeBridge.kt` — JNI surface (matches Rust JNI symbols
    `Java_me_phie_tawc_compositor_NativeBridge_*` and `find_class
    "me/phie/tawc/compositor/NativeBridge"` in `compositor/src/lib.rs`).
  - `TawcInputConnection.kt` — IME bridge.
- `install/` — Kotlin implementation of the chroot install / run /
  destroy logic. The rootfs is stored under
  `/data/data/me.phie.tawc/distros/<id>/rootfs/` so uninstalling
  the app reclaims it. The host-side counterpart is
  `scripts/rootfs-run.sh`, which routes through the dev exec broker
  to the same [InstallationMethod.startInside]. See
  [installation.md](installation.md) for the package map, the
  broker `--action install/uninstall` CLI, and the Android 14 FGS
  rationale.

When adding new app features (settings, app launcher, …), put them in
their own packages under `me.phie.tawc.*` rather than mixing them into
the compositor or install packages.

## Audio

Audio forwarding from the rootfs to Android is not implemented yet. The current
plan is a PipeWire-first rootfs stack bridged through app-owned endpoints under
`/usr/share/tawc/`; see [audio.md](audio.md).
