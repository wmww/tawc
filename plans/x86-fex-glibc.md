# FEX in-distro: x86/x86_64 emulation inside arm64 glibc distros

Run x86_64 (and eventually 32-bit x86) Linux programs inside our
existing aarch64 glibc distros via [FEX-Emu](https://github.com/FEX-Emu/FEX)
"the normal way": arm64 FEX binaries installed into the rootfs, an
x86_64 RootFS providing the guest userland, tawcroot dispatching
foreign-arch execs to `FEXInterpreter`. This is the supported upstream
shape (FEX targets exactly "aarch64 glibc Linux"), and community
proot-based setups (FEXDroid, Fex-Android) prove the
FEX-inside-a-usermode-rootfs-layer pattern works on phones at all.

**Status: plan, not started. Research-stage** — FEX has never run under
tawcroot; several load-bearing unknowns below (kernel floor first).

Sibling plans: [x86-box64.md](x86-box64.md) (shares the exec-dispatch
piece; complementary tradeoffs), [x86-fex-bionic.md](x86-fex-bionic.md)
(radical variant of this plan with no glibc layer).

## Why FEX (vs box64)

- Emulates the **entire guest userland** from an x86_64 RootFS — one
  consistent world, no per-library wrapper maintenance, and the only
  credible path to "whole x86_64 distro" (see §Stretch).
- 32-bit x86 guests without armhf anything (FEX issues 64-bit host
  syscalls for them — compatible with tawcroot's 32-bit-syscall kill).
- Serious correctness investment (x86-TSO), Valve-funded, monthly
  releases, shipping in the Steam Frame.
- Costs: hard 4K-page-kernel requirement (all current targets are 4K;
  a future 16K device breaks FEX with no userspace workaround — box64
  is the hedge), and graphics is NOT free — the default path emulates
  guest Mesa (llvmpipe = software), and every acceleration option needs
  work (§Graphics).

## Hard gate to resolve first: kernel floor

FEX-2501 (Jan 2025) raised the minimum host kernel from 5.0 to 5.15,
"matching Ubuntu 22.04". Our primary aarch64 device (OnePlus 9) runs
**5.4**, and FEX cannot run on the x86_64 emulator at all (host must be
aarch64) — so first bring-up is on exactly the device the floor
excludes. Options, decide after a probe:
- Pin the last pre-floor release (FEX-2412) for bring-up; accept stale.
- Audit what 5.15 actually gates (the phrasing smells like a
  support-policy floor, not a feature need) and patch the check;
  tawcroot can shim individual missing syscalls if that's all it is.
- Bring up on a newer aarch64 device first if one is available.

Related version pressure in the other direction: FEX plans to raise its
CPU floor to ARMv8.4/FlagM (FEX issue #4120, unlanded), which would
drop Snapdragon 888 (= OnePlus 9). Whatever we ship will likely be a
pinned version + local patches; treat FEX like a vendored dep from day
one (`deps/deps.list` pattern).

## Pieces

### 1. tawcroot exec dispatch

Same mechanism as [x86-box64.md](x86-box64.md) §1 — land once, shared.
For FEX the dispatch target is `FEXInterpreter`.

### 2. FEX binaries into the rootfs

arm64 glibc FEX build (FEXLoader/FEXInterpreter/FEXCore + configs).
Packaging: same options as box64 — vendored source build packed as an
asset, or distro packages (AUR `fex-emu-git`, Ubuntu PPA; nothing for
ALARM/Debian-arm64 that we know of — *verify*). FEX is a large C++
project; build-time/toolchain cost is real. Decide later. Note we will
almost certainly carry patches (kernel floor, tawcroot quirks), which
argues for the vendored build.

### 3. x86_64 RootFS, extracted-directory mode

FEX normally mounts squashfs/erofs RootFS images via FUSE
(FEXServer + squashfuse/erofsfuse). **FUSE mounts are impossible under
tawcroot** (no mount privileges), so we must use the supported
extract-to-plain-directory mode. Open questions:
- Whether FEXServer is still required with a directory RootFS (it also
  serves logging/config duties) and whether it behaves under tawcroot.
- Which image: FEX's official Ubuntu images vs building our own from
  the x86_64 bootstraps the installer already knows (Arch/Void/Debian
  x86_64 exist in `DistroRegistry` for the emulator). Our own is
  attractive (same distro family inside and out, cache-proxy-friendly,
  no dependency on FEX's image server); their official images are
  what upstream tests against. Decide later.
- Disk cost: an extracted x86_64 userland is multi-GB-ish per install;
  where it lives (per-distro vs shared across installs) matters.

### 4. FEX ↔ tawcroot coexistence audit

The composition "FEX's userspace path overlay inside tawcroot's path
translation" mirrors proot+FEX, which works in the field — but each of
these needs a deliberate check on device:
- **Signals:** FEX leans on SIGSEGV for SMC detection and TSO
  backpatching; tawcroot owns SIGSYS and shadows guest SIGSYS state.
  Presumed disjoint; verify handler-install ordering across FEX's
  in-process world.
- **Guest seccomp:** tawcroot denies guest `seccomp()` installs. Does
  current FEX ever install real host filters (e.g. emulating guest
  seccomp)? If yes, is degradation graceful? Unknown.
- **Address space:** tawcroot is non-PIE at `0x2000000000` with guard
  reservations; FEX manages large guest address-space regions and
  (for 32-bit) needs the low 4GB plus jemalloc (upstream warns
  jemalloc-less builds exhaust address space wrapping 32-bit). Probe
  for collisions; 64-bit-only first, 32-bit later.
- **Syscall surface:** FEX makes raw host syscalls; anything Android's
  zygote filter rejects lands in tawcroot's dispatch as usual — fixups
  become ordinary tawcroot handlers (the existing legacy-syscall
  forwarding pattern). SYSV IPC does not exist in Android kernels at
  all; if an x86 workload needs shmget et al., that's a new tawcroot
  emulation (memfd-backed, like the existing /dev/shm shadows) —
  defer until something real hits it.
- **/proc fidelity:** FEX reads /proc/self/maps etc.; tawcroot
  synthesizes some of /proc. Watch for gaps.

### 5. Graphics

Explicitly conservative, per current project direction (gfxstream is
disabled in prod and NOT part of this plan):
- **Baseline (correctness): guest x86_64 llvmpipe** from the RootFS
  rendering into SHM wl_buffers. Pure emulated software rendering; no
  new integration, expected slow. This is the bring-up target.
- **Fast path candidate: FEX thunks → libhybris.** FEX's library
  forwarding redirects guest GL/Vulkan calls to native arm64 host
  libs — in our rootfs, the hybris EGL/GLES wrappers. Uncertainties to
  investigate before committing: which libraries FEX ships thunks for
  (EGL/GLES vs desktop GL/GLX/Vulkan coverage), whether thunk host
  libs tolerate hybris's non-Mesa EGL, thunk build requirements (host
  sysroot at build time), and Wayland-platform EGL through a thunked
  boundary. Treat as a separate investigation gated on core FEX
  working.
- If gfxstream is ever proven in prod, an x86_64 gfxstream-vk guest
  driver speaking the wire protocol would cross the ISA boundary for
  free — noted for the future, not planned here.

### 6. Bring-up ladder

Static x86_64 hello-world → dynamic hello-world → syscall-heavy CLI
(`git status`, `python`) → SHM Wayland client (`weston-terminal`-ish)
→ llvmpipe GL client → (separately) thunk experiments.

## Stretch: whole-x86_64-distro UX

With per-program FEX working, "an x86_64 distro on the phone" is
mostly UX: the FEX RootFS *is* a full x86_64 distro; make it the
user-visible world (default shell/launcher entries exec through
FEXInterpreter). Known field warning: FEXDroid explicitly routes
apt/dpkg through qemu because package managers misbehave under FEX in
proot setups — whether that's FEX's fault or proot jank is exactly
what tawcroot's higher fidelity would tell us. Package-manager-under-
FEX is the acceptance test before promising this mode. Not in scope
for initial landing.

## Testing

- Exec-dispatch tests shared with the box64 plan (stub emulator).
- Handler-level tests need no FEX; everything FEX-specific is
  device-integration territory (aarch64-only), so expect a
  `tests/integration` module that self-skips off-target, like the
  existing backend-pinned tests.

## Out of scope

- gfxstream-based acceleration (until gfxstream is proven in prod).
- 32-bit x86 in the first pass (design for it, don't build it).
- Wine/Windows programs (FEX's arm64ec Wine story is a different
  stack entirely).
