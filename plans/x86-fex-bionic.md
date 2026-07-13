# Bionic FEX: x86_64-only installs with native driver loading

The radical variant: build FEX against **bionic** so the emulator
itself is an Android-native binary, and there is no arm64 glibc distro
at all. The user picks an x86_64 distro at install time; the entire
user-visible userland is the x86_64 RootFS; FEX is invisible plumbing
owned by the app (like tawcroot is today). Because the FEX host
process is bionic, its thunk host libraries can `dlopen` Android's
NDK-stable `libEGL`/`libGLESv2`/`libvulkan` directly — native driver
loading with **no libhybris** anywhere in the x86 path.

**Status: plan, not started. The most speculative of the three x86
plans** — gated on [x86-fex-glibc.md](x86-fex-glibc.md) proving FEX
works under tawcroot at all. Do not start here.

Sibling plans: [x86-box64.md](x86-box64.md),
[x86-fex-glibc.md](x86-fex-glibc.md). All background facts (kernel
floor, 4K pages, CPU floor, version pinning) from the glibc plan apply
here unchanged.

## Why this might be worth it

- **Deletes two layers for x86 workloads:** no glibc↔bionic bridging
  (libhybris) and no arm64 glibc distro to install/maintain alongside
  the x86 one. The guest userland comes entirely from the x86_64
  RootFS, so the host libc is FEX's private business.
- **Sanctioned driver access:** thunk host libs against the NDK's
  stable EGL/GLES/Vulkan — the same libraries every Android app uses —
  rather than hybris's reverse-engineered blob loading.
- **Precedent it's not fantasy:** upstream says FEXCore builds for
  Android aarch64; Termux shipped a full bionic FEX package (since
  moved to disabled-packages — its patch stack is the best map of the
  porting friction: signal delegation, VDSO emulation, allocator
  hooks). Winlator-family apps run FEXCore JITs in bionic app
  processes in production (as arm64ec Wine DLLs — different syscall
  story, but proof the recompiler runs fine on bionic/Android).

## Why it might not be

- Permanently off upstream's support matrix ("Android is not a target
  and will never be a target") — we'd own a patch stack against a
  fast-moving C++ codebase forever.
- The x86_64 RootFS becomes load-bearing for *everything*, including
  package management under emulation (no native fallback shell) — the
  known weak point of FEX-in-proot field reports.
- Most of the deleted complexity reappears as new complexity: WSI glue
  (§below), thunk toolchain work, and a containment-layer decision
  that touches tawcroot's core assumptions.

## Central design decision: who contains what

Today: tawcroot contains a glibc distro. In this plan there are two
candidate shapes — pick after prototyping, not now:

1. **tawcroot wraps bionic FEX.** tawcroot is libc-agnostic about its
   guests; a dynamic bionic FEX needs `/system/bin/linker64` and
   /system libs reachable, which tawcroot's default binds already
   expose. tawcroot keeps doing what it does (fake root, path view,
   Android-seccomp fixups, /proc synthesis) and FEX runs as an
   ordinary guest whose own RootFS overlay serves the x86 world.
   Most continuity, three nested path layers (tawcroot view → FEX
   overlay → x86 RootFS) — audit double-translation cost/correctness.
2. **FEX replaces tawcroot for these installs.** FEX's overlay is the
   only filesystem layer; no SIGSYS machinery at all. But then nobody
   provides fake-root, Android-blocked-syscall fixups, or /proc
   synthesis — FEX would need to grow tawc-specific patches for
   exactly the things tawcroot already solved, and raw FEX on Android
   is what upstream's FAQ warns about (raw syscall usage vs zygote
   seccomp). Cleaner in theory, likely more patching in practice.

An honest prototype probably starts as (1) since it reuses everything.

## Pieces (beyond everything in x86-fex-glibc.md)

### 1. Bionic FEX build

NDK cross-build of FEXLoader/FEXCore + the Linux syscall frontend,
vendored via `deps/` with a patch stack seeded from Termux's
disabled-package patches (evaluate how stale those are against current
FEX first). Unknowns: how much of the syscall frontend assumes glibc
host conveniences; jemalloc-on-bionic for 32-bit address-space
wrapping; whether FEX's CMake tolerates the NDK toolchain without
major surgery. This piece alone is a substantial project — timebox a
build spike before committing to the plan.

### 2. Install-time integration

A new install-time choice producing an install whose `metadata` marks
it emulated. Natural fit: the existing x86_64 `Distro` impls
(Arch/Void/Debian x86_64 already in `DistroRegistry`) become
installable on arm64 hosts when the emulated-arch path is enabled —
`DistroRegistry.availableForHost()`'s ABI-match filter and
`Installation.arch` semantics need an explicit emulated-arch concept
rather than a hack. Installer package steps (`pacman`/`apt` inside the
rootfs) then run under FEX from the very first install stage — see
package-manager risk above; the installer is the first stress test.

### 3. Thunks against NDK graphics

New thunk host-lib target: FEX's thunk generator currently emits
host libs for a Linux glibc sysroot; we need NDK-built bionic host
libs. Then the actual graphics glue:
- **WSI is the real work.** Android's EGL/Vulkan present to
  `ANativeWindow`/AHardwareBuffer, not `wl_surface`. The guest speaks
  Wayland to our compositor; the thunk host side needs to back guest
  swapchains/EGLSurfaces with AHBs and hand them to the compositor —
  most likely mirroring the existing hybris WSI trick (AHB-backed
  buffers announced over our Wayland protocol) minus the glibc
  bridging. Needs a real design pass; do not assume it's small — this
  is the layer that "deleting libhybris" regrows under a new name.
- GL/GLES coverage questions from the glibc plan apply doubly (NDK
  gives EGL/GLES + Vulkan; desktop-GL guests would need a translator
  on top — existing gl-on-gles work/plan becomes relevant).

### 4. Everything-else audit

Sound, input-method, clipboard, launcher/.desktop scanning, terminal
spawn paths — all currently assume the glibc distro layout and
tawcroot entry conventions. Under shape (1) most should carry over
(the x86 RootFS is still a normal distro layout); under shape (2)
each needs rework. Enumerate during prototyping.

## Suggested gating milestones

1. x86-fex-glibc.md works end-to-end on a device (prerequisite).
2. Build spike: bionic FEX runs a static x86_64 hello-world under
   tawcroot on a device.
3. Thunk spike: one NDK-built host thunk (Vulkan or EGL/GLES) renders
   offscreen from an x86 guest.
4. WSI design note written and reviewed before any compositor work.

Abandon criteria are as valuable as milestones here: if (2) needs a
patch stack rivaling Termux's *and* the glibc-plan path already works
well through hybris, this plan's payoff may not clear its maintenance
bar. Reevaluate at each gate.

## Out of scope

- gfxstream in any form (until proven in prod).
- Replacing the glibc-distro product default — this is an additional
  install-time option, not a migration.
- Wine/arm64ec integration (separate stack, even though it's the
  best-proven bionic FEXCore deployment).
