# Distro options

Survey of Linux distros for the in-app chroot, with tradeoffs.
Companion to [distro-abstraction.md](distro-abstraction.md), which
documents the code-level abstraction that lets us add new families.

We currently ship **Arch Linux ARM** (aarch64), **Arch Linux**
(x86_64, for the emulator), **Manjaro ARM** (aarch64), **Void
Linux** glibc (x86_64 and aarch64), **Debian sid** (x86_64 and
aarch64), and **Ubuntu 24.04 LTS** (x86_64 and aarch64). This note exists because ALARM is
under-maintained and somewhat bloated for our needs, and we keep
getting asked "what about $distro?".

## Which distros are supported

Three, for users:

- **Arch Linux ARM** (aarch64)
- **Debian sid** (aarch64, and x86_64 for the emulator)
- **Ubuntu 24.04 LTS** (aarch64, and x86_64 for the emulator)

Everything else — Manjaro ARM, Void Linux, and Arch
Linux x86_64 — still ships in every build (release included) but is
dev / experimental: less tested, and free to break. Arch Linux x86_64 is
flagged supported in the code purely because it is the emulator's
stand-in for ALARM (there is no aarch64 emulator path), so the
emulator dev loop mirrors the phone's default; no user runs an
x86_64 Android device.

Mechanically this is the `Distro.supported` flag, surfaced only in
the UI: the install form lists supported distros directly and hides
the rest behind an "Other distros" expander (see
[distro-abstraction.md](distro-abstraction.md) → *Supported vs
other*). The install pipeline itself treats all distros identically.

Practical consequence: new work — testing, GPU/browser bring-up,
issue triage — targets ALARM and Debian sid first. A bug that only
reproduces on Void or Manjaro is not release-blocking.

**Manjaro ARM** is the most recent addition — also pacman-based, so
it reuses ~all of `ArchPacmanCommon` (mirrorlist + keyring set are
the only real differences). Bootstrap is the official
[manjaro-arm/rootfs](https://github.com/manjaro-arm/rootfs/releases)
weekly auto-build (~210 MB, much smaller than ALARM); the GitHub
Releases REST API exposes a server-computed SHA-256 in the asset's
`digest` field, which we fetch over HTTPS and verify against — see
[installation.md](installation.md) → *Bootstrap integrity*. The
rootfs is unusually clean (~93 packages: no kernel, no firmware, no
editors, no openssh) so the post-extract cruft purge has nothing to
strip on this distro.

A Manjaro **x86_64** equivalent is not currently shipped: Manjaro
publishes no clean rootfs tarball for amd64 (only ISO images and
Docker layers via `manjarolinux/base:latest` on Docker Hub). Adding
it would mean a small Docker Registry HTTP API client in the
installer (token + manifest list + content-addressed blob fetch) —
left out for now since x86_64 is the emulator-only path and Arch
Linux x86_64 already covers regression testing of the install
abstraction.

The old integrity argument for replacing ALARM is gone: upstream now
publishes a detached PGP signature for the bootstrap tarball, and we
verify it against the build-system key shipped in the APK (see
[installation.md](installation.md) → *ALARM bootstrap: from
cross-mirror MD5 to PGP*). The remaining reasons to consider a
different distro are maintenance and freshness, listed below.

## The hard constraint: glibc

We require **glibc**, not musl. The reason is libhybris.

libhybris bridges glibc programs to bionic-linked Android GPU drivers.
Our fork's headline patch rewrites `mrs TPIDR_EL0` reads inside the GPU
blobs to translate bionic's TLS slot layout to **glibc's** layout (see
`libhybris/TAWC_FORK.md`). Musl has a different TLS ABI — different
head reservation, different `pthread_t` layout, different
`__tls_get_addr` semantics — so the thunked offsets would be wrong, and
the dynamic-linker tricks libhybris relies on don't all have musl
equivalents.

There is no production-grade "libhybris for musl". Adelie and
postmarketOS have attempted ports for years; nothing usable has
shipped. So any musl distro (Alpine, Void-musl, Chimera, …) is out for
real-device builds. They could in principle work on the emulator,
where libhybris is disabled anyway, but it's not worth a second
toolchain for that.

## Viable shortlist

Criteria: glibc, well-maintained on aarch64 + x86_64, small base
suitable for a chroot, fresh-enough packages for a desktop browser.

### Debian

- arm64 is a **first-class release architecture**, not a port.
- `debootstrap --variant=minbase` produces a ~120-150 MB rootfs.
- `apt` is boring and reliable; conservative versions on stable.
- Fresh apps via `testing` or `sid` overlays, or the Mozilla repo for
  Firefox specifically.
- x86_64 path is identical tooling — same `debootstrap`, same repos.
- Largest package selection of any candidate.
- Downside: stable's packages are old by design. For a Wayland
  compositor target this is mostly fine; for Firefox you'll want
  `testing` or upstream.
- **Currently shipped as sid** for the rolling/fresh package path.
  Bootstrap comes from Debian's official debuerreotype Docker
  artifacts (`dist-amd64` / `dist-arm64v8` branches), resolving the
  OCI `rootfs.tar.gz` layer digest at install time and verifying the
  downloaded tarball with SHA-256. The apt-family code is suite-driven,
  so adding `testing`, `stable`, or Ubuntu-style variants should mostly
  be a data-object addition.

### Void Linux (glibc flavor)

- Rolling release, runit init (irrelevant in a chroot), `xbps` package
  manager — fast and pleasant.
- Official aarch64 + x86_64 prebuilt rootfs tarballs
  (`void-<arch>-ROOTFS-*.tar.xz`), ~80 MB uncompressed — **smaller
  than Debian minbase**.
- Independent (not a Debian/Arch derivative), well-maintained but
  smaller community.
- Best pick if "minimal + rolling" is the goal.
- **Currently shipped** (see top of this note +
  `install/distro/voidlinux/`).
- Bootstrap integrity: SHA-256 from upstream `sha256sum.txt`, whose
  minisign (Ed25519) signature we verify against a per-image-date
  release key published in void-packages on GitHub — a second origin,
  so this is stronger than the single-HTTPS-endpoint profile the other
  `Sha256` distros have. See notes/installation.md "Bootstrap
  integrity".
- Downside: smaller package repo than Debian/Arch. Things like Firefox,
  GTK, weston are fine; long-tail desktop apps less so.

### Manjaro ARM

- Arch derivative but with **separately maintained ARM packages** —
  notably better aarch64 upkeep than ALARM proper.
- Rolling release, `pacman` (so most of our existing
  `ArchPacmanCommon.kt` would carry over).
- Active community focused on SBCs / PinePhone.
- Best pick if "I like Arch but want better ARM maintenance" is the
  goal — minimal porting effort given our current code.
- **Currently shipped** (see top of this note + `install/distro/manjaro/ManjaroArm.kt`).
- Downside: still a smaller team than Debian/Fedora; quality is good
  but not enterprise-grade.

### Fedora

- Official aarch64, treated as primary.
- Fresh packages on a 6-month cadence; high QC.
- `dnf` is slow; base footprint is heavier than Debian.
- Real option, just chunky. Not the best fit for "minimal".

### openSUSE Tumbleweed / Leap

- Official aarch64 with full repos.
- Tumbleweed = rolling, Leap = stable.
- `zypper` is decent. Less popular than Fedora but very
  well-engineered.
- Same chunkiness criticism as Fedora.

## Picking one

| Goal                              | Pick           |
| --------------------------------- | -------------- |
| Safe default, biggest repo        | Debian         |
| Minimal + rolling                 | Void glibc     |
| Stay in pacman-land, better ARM   | Manjaro ARM    |
| Fresh + heavy + enterprise feel   | Fedora         |

Recommended stable-style option: **Debian stable**, with `testing` or
upstream Mozilla repo for Firefox. Boring, correct, biggest community
on arm64. For rolling/fresh packages, use the shipped **Debian sid** or
Void glibc.

## Other options (and why they're worse)

### Ubuntu

Shipped since 2026-10, **supported** since 2026-10-01 (first install
verified end to end on a Galaxy Tab S9: bootstrap trust chain, ports
sources, `apt-get update`, base packages, and GUI apps launched from
the in-app launcher and a pinned shortcut). The concerns below still shape how it is wired:

- It is Debian underneath — same `apt`, same arm64 story — so it adds
  no capability Debian sid lacks. It exists for familiarity:
  `ubuntu-base` is a ~30 MB non-snap rootfs, and it is the name many
  users ask for first. `Ubuntu2404` is a thin data object over
  `AptCommon`, deliberately.
- **Snap does not apply, and `snapd` is refused**. `ubuntu-base` ships
  no snapd and TAWC neither installs it nor lets apt do so: `Ubuntu2404`
  writes an apt preference pinning `snapd` to `Pin-Priority: -1`
  (`AptCommon.blockedPackagesBody`). Without the pin, a user who follows
  Ubuntu's own "install the Firefox snap" advice gets
  `unable to set CAP_SETFCAP effective capability` from snapd's postinst
  (tawcroot is fake root with `CapEff: 0`), which leaves dpkg
  half-configured and makes **every later apt transaction exit
  non-zero**. Snaps cannot work here anyway — they need systemd, loop
  mounts and apparmor — so apt answers with "no installation candidate"
  instead. The transitional `firefox` `.deb` then fails on its own
  `PreDepends: snapd (>= 2.54)` — verified on device: apt reports
  `Package 'snapd' has no installation candidate`, so the user gets a
  clear error rather than a browser stub. Real Firefox comes from the
  Mozilla PPA or the upstream tarball.
- **arm64 is on `ports`, not `archive`.** `Ubuntu2404Aarch64` points at
  `ports.ubuntu.com/ubuntu-ports` and `Ubuntu2404X86_64` at
  `archive.ubuntu.com/ubuntu`, so the two repository roots are not
  interchangeable between the two flavours.
- `systemd-standalone-sysusers` / `-tmpfiles` are **universe**-only in
  noble, so the apt source carries `main universe` (Debian's stays
  `main`).
- `supported = true`, so the install form lists it in the main group
  with ALARM and Debian sid — see *Which distros are supported* above.
- None of Ubuntu's value-adds (HWE kernels, cloud images, Pro) apply to
  us, and the LTS cadence is the only reason to prefer 24.04 over a
  rolling distro.

### Pop!_OS / Mint / Zorin / elementary

All Ubuntu derivatives, all effectively **x86_64 only**. System76 has
talked about COSMIC-on-ARM and there were experimental Pi images, but
no official Pop!_OS arm64 release. The desktop-derivative distros'
value is their DE/UX preselection, which is irrelevant when we're
running our own compositor.

### EndeavourOS / Garuda / CachyOS

Arch derivatives, x86_64 only. EndeavourOS ARM was discontinued.

### RHEL / Rocky / AlmaLinux / CentOS Stream

Enterprise aarch64, rock-solid, but old packages and you fight EPEL/
Flatpak to get a modern Firefox. Not a desktop-app target.

### Gentoo

First-class aarch64, but compilation cost in a chroot installer is a
non-starter.

### NixOS

Official aarch64. Different paradigm; would mean rewriting the
installer around Nix store semantics. Not justified by what we'd gain.

### Raspberry Pi OS / Armbian / Mobian

ARM-specialist Debian derivatives. No advantage over plain Debian once
you're not on a Pi or SBC. Armbian in particular adds SBC-specific
glue we'd just have to disable.

## "What about $X" — frequently-asked weird options

### Alpine Linux

- musl → libhybris doesn't work → no GPU on real devices → defeats the
  point.
- Could in principle be the emulator-only chroot (libhybris is disabled
  there), but maintaining two toolchains for that isn't worth it.
- postmarketOS proves Alpine works fine for desktop Wayland in
  general; the issue is purely libhybris.

### Termux

- The closest thing to a "bionic Linux distro." Huge package repo,
  built against bionic, runs as a normal Android app — no chroot, no
  root.
- Not a chroot at all. Runs as the app's UID inside Android's sandbox,
  no real `/`, no init, no system services. Packages are patched to
  cope with bionic's quirks.
- Adopting Termux means **redesigning around "Android app with package
  manager"** instead of "chroot with a real Linux userspace". That's a
  bigger architectural shift than porting libhybris to musl would be.
- Sidesteps libhybris (everything is bionic-linked), but at the cost of
  the model the rest of TAWC is built around.

### "Are there any bionic Linux distros?"

Effectively no.

- **Termux** is the only living example, with the caveats above.
- **Halium** ships a minimal Android (bionic + HALs) under a glibc
  Linux userspace — opposite of what you'd want; the bionic layer is
  an implementation detail.
- **Embedded Gentoo / OpenEmbedded with bionic** existed historically,
  all dead. Bionic intentionally omits a lot of POSIX/glibc surface
  area (NSS, locale, `getpwent`, weird pthread semantics) so every
  desktop package needs patches. Maintenance cost is enormous and
  Termux already occupies the niche.

### Halium / Waydroid / Lindroid

Full bionic-container approaches. Sidestep the libc problem by not
mixing libcs at all — you ship an Android-in-Android. Heavier and a
totally different architecture; would replace, not augment, our
current chroot model.

### Mesa native drivers (no libhybris)

Not a distro choice per se, but worth noting: if a device has
open-source GPU support (Freedreno for Adreno, Panfrost for Mali, …),
libhybris is unnecessary and any distro works, including musl ones.
But that's device-by-device and excludes anything new or locked-down,
so it's not a strategy we can rely on for "run on all modern Android
phones".

### Chimera Linux

Newer, BSD userland + LLVM toolchain, has aarch64. **musl-based**, so
ruled out for the same reason as Alpine.

### Slackware ARM, CRUX, Solus, …

Either x86_64-only, ARMv7-focused (not aarch64), or too small a team
to rely on. Mentioned only because people ask.

## Summary

For a glibc chroot on Android:

- **Debian sid is now shipped** for rolling/fresh packages with apt.
- **Default stable-style Debian** remains the conservative future pick.
- **Void glibc** if you want minimal + rolling.
- **Manjaro ARM** if you want to keep `pacman` and reuse most of the
  current Arch code.
- Anything musl-based (Alpine, Chimera, Void-musl) is blocked on
  libhybris.
- Anything bionic-based (Termux, Halium-style) is a different
  architecture, not a different distro.
