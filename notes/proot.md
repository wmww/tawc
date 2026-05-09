# proot — rootless install method

The proot install method runs an Arch chroot without root by using the
[Termux fork](https://github.com/termux/proot) of `proot` as a
ptrace-based fake chroot. This note covers what makes it work on
Android, the non-obvious bits we hit during integration, and the
maintenance contract for the vendored sources.

**Status: dev-only.** [tawcroot](tawcroot.md) is the default and only
officially supported install method; release builds ship only tawcroot
(see `notes/installation.md` "Install methods"). proot remains in
debug builds for performance comparisons and as a fallback during
tawcroot bring-up — it's not exposed to release users.

libhybris-driven GPU acceleration works under proot too: `ProotMethod`
binds `/apex /vendor /system /system_ext /linkerconfig` into the
rootfs view (mirroring `ChrootMounter.mountScript`'s mount set),
and `TawcInstaller`/`LibhybrisInstallProvider` copies the libhybris
tree into both methods' rootfses (real files at `/usr/lib/hybris/`,
not symlinks). EGL, GLES, and
Vulkan all reach the Android driver under proot. Firefox works too,
modulo two extra setup steps the proot path applies automatically
(see "Firefox under proot" below).

## What we ship

`scripts/build-proot.sh` clones two upstream sources into project-local
gitignored dirs and produces two jniLibs per ABI:

| jniLib               | source                                   | role |
| -------------------- | ---------------------------------------- | ---- |
| `libproot.so`        | `github.com/termux/proot` (Termux's fork)| the proot binary |
| `libproot-loader.so` | same — built as `loader/loader`          | per-tracee loader stub |

talloc 2.4.4 is downloaded as a tarball into `proot-deps/` and built
as a static lib (no waf — we hand-roll a minimal `config.h`).

The Termux fork is what makes glibc-on-Android work. Three things
upstream `proot-me/proot` lacks:

1. **`src/tracee/seccomp.c`** — a SIGSYS-from-stacked-seccomp
   handler. When the kernel raises SIGSYS at the tracee because of
   Android's `untrusted_app` filter (which `RET_TRAP`s deprecated
   syscalls like `access`, `open`, `chmod`, `chown`, `mkdir`,
   `rmdir`, `unlink`, `symlink`, `link`, `rename`), proot catches the
   signal-stop, reads the registers, and rewrites the syscall to its
   `*at` equivalent before resuming. Without this, ld-linux on
   x86_64 dies on its very first `access()` call during dynamic
   linker startup.
2. Android-specific tweaks in `arch.h` (different `LOADER_ADDRESS`,
   `SYSCALL_AVOIDER` values).
3. Additional filter rules (notably `ioctl` in seccomp.c) and a
   couple of Android-specific extensions like `ashmem_memfd`.

## Why upstream proot doesn't work on Android x86_64

Android's bionic policy
([`SECCOMP_PRIVATE_ALLOWLIST_APP.TXT`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/SECCOMP_PRIVATE_ALLOWLIST_APP.TXT))
only allowlists `access(2)` for **lp32** (32-bit) processes. On
lp64 the syscall falls through to the default `RET_TRAP`, which
raises SIGSYS at the tracee. arm64 dodges the issue by accident —
the architecture itself has no `access` syscall, so glibc routes
through `faccessat(2)` natively.

We confirmed empirically:

- A statically-linked `access(2)` test running under our app's
  process tree (`Seccomp:2`, `untrusted_app:s0`) dies with exit 159
  (128+31 = SIGSYS).
- The same test, but run-as'd into a `runas_app` context
  (`Seccomp:0`, no filter), succeeds.
- Termux's `proot-distro install archlinux` succeeds on the same
  emulator. Their proot translates the syscall in the SIGSYS
  handler.

`proot-me/proot` (the canonical proot repo) does *not* have this
handler. It used to be the upstream, but the Termux fork has been
the de facto Android upstream for years and `proot-me` is mostly
abandoned for Android purposes.

## Performance cost

Pure ptrace + per-syscall stop. Roughly 5-10× slower than chroot
for syscall-heavy work. Concretely a fresh proot install of Arch
takes ~7 minutes wall time on the x86_64 emulator (~3-4 min for
download, the rest pacman). Most of that is `pacman -Syu` extracting
package contents, which is exactly the syscall-heavy case. Once
installed, runtime cost is much smaller — long-running CPU/GPU work
in the Wayland clients themselves doesn't trip the tracer.

A friend's idea for a faster proot replacement is in the air. If/when
that lands, this whole story collapses to "swap one ptrace tool for
another." The `InstallationMethod` abstraction was designed with that
in mind.

## Firefox under proot

Two extras are needed to make Firefox start AND render through the
libhybris/AHB path under the `runas_app` SELinux domain (without
these the app either crashes during init or paints every chrome
frame through cairo/SHM, which the compositor magenta-tints to
make obvious):

1. **`/dev/shm` bind**. Android has no `/dev/shm` and `runas_app`
   can't create one in the host `/dev` (which proot bind-passes
   through unmodified). Firefox's parent process uses
   `shm_open(3)` for its main IPC shared-memory segment and hits
   `MOZ_RELEASE_ASSERT(mHandle.IsValid() && mMapping.IsValid())`
   when it fails — instant SIGSEGV before the URL bar paints.
   `ProotMethod.devShmDir` allocates a per-app cache dir and
   binds it at `/dev/shm` (must come *after* `-b /dev` so proot's
   later-bind-wins rule applies).

2. **`MOZ_DISABLE_*_SANDBOX=1`** as a proot-only branch in
   [RootfsEnv] (content / GPU / RDD / utility / GMP / socket / VR).
   Firefox's per-subprocess sandboxes set up their
   own seccomp filters and PID/user namespaces during startup.
   Under proot every process is already a ptrace tracee with
   proot's own seccomp filter active, and Firefox's sandbox setup
   then SIGSEGVs every subprocess immediately
   (`[Parent] WARNING: process N exited on signal 11` repeating).
   The chroot path doesn't need these env vars because there's
   no ptrace tracer there. Disabling the sandbox is "less secure"
   in the abstract, but the whole rootfs is already an app-uid
   sandbox, so the marginal exposure is small.

The third Firefox-specific hazard documented in earlier revisions
of this doc — bionic `__cfi_slowpath` patching faulting under
`runas_app:s0`'s missing `system_file:execmod` — is gone now that
libhybris hooks `__cfi_slowpath{,_diag}` at symbol-resolution
time instead of patching libdl in place. See the CFI section of
`libhybris/TAWC_FORK.md` for details.

## Android quirks worked around

The full list of things Android does differently that we paper over:

1. **Loader exec** — Android 10+ blocks `execve` of files in the
   app's home directory (the `untrusted_app` SELinux domain has no
   `execute_no_trans` for `app_data_file` in enforcing mode; in
   permissive mode it's logged but other restrictions still bite).
   proot's default behaviour is to extract its loader stub to
   `$PROOT_TMP_DIR` (which by default is `/tmp`, then we override
   to `cacheDir`) and execve it. We avoid the issue by setting
   `PROOT_LOADER` to a vendored loader at `nativeLibraryDir/`,
   which has the friendlier `apk_data_file` SELinux context.

2. **`PROOT_TMP_DIR`** — `/tmp` doesn't exist as a writable dir for
   apps on Android. Always export it to a path inside the app's
   own `cacheDir` before invoking proot.

3. **mksh's here-doc temp files** — historical: when host launchers
   shelled into a wrapper script via `run-as me.phie.tawc`, the
   default cwd was `/data/local` (uid `shell`'s default), where app
   uid has no write access, and mksh's here-doc machinery fell over
   trying to create a temp file there. The current broker-mediated
   path runs entirely inside the JVM with `TMPDIR=$cacheDir/proot-tmp`
   exported by [ProotMethod.startInside], so this is no longer a
   concern. Documented for archaeology only.

4. **Inherited seccomp filter** — the filter follows fork+exec via
   `PR_SET_NO_NEW_PRIVS` and can't be relaxed from userland. This is
   what (1) above is really about.

5. **App data dir tar quirks** — the Arch bootstrap contains
   `/etc/ca-certificates/extracted/cadir/` with mode `0500`; toybox
   `tar` honours that immediately and the next file inside fails to
   write under app uid. We do the bootstrap extract in pure Kotlin
   via `ProotArchiveExtractor` (commons-compress), deferring dir
   mode application until after the children are written.

   The same extractor also has a **hardlink → symlink fallback**:
   stock Android's `untrusted_app` SELinux policy doesn't grant `link`
   on `app_data_file` on every device (observed: OnePlus 9, Android
   14 — `avc: denied { link } scontext=untrusted_app`). When `Os.link`
   fails with EACCES/EPERM the extractor falls back to a relative
   symlink, which is the same trick proot's `--link2symlink` runtime
   flag does. ALARM's `/usr/lib/getconf/{XBS5,POSIX_V7}_*` are the
   tarball entries that hit this in practice. Behaviourally identical
   for everything in the chroot — the only observable difference is
   `stat().st_nlink`, and nothing in our pipeline relies on that.

6. **Bootstrap-bundled pacman DB is mtime-poisoned** — the bootstrap
   ships its own `/var/lib/pacman/sync/{core,extra,alarm,aur}.db`
   snapshot. After extract, those files have `mtime=now`. Plain
   `pacman -Sy` then issues a conditional GET (`If-Modified-Since:
   <mtime>`) and the mirror — whose actual `Last-Modified` is older
   than now — answers 304. pacman keeps the bootstrap's possibly
   stale DB and 404s on packages the mirror has rolled past
   (observed: `mesa-1:26.0.4-1` no longer at any ALARM mirror after
   it rolled to `26.0.5`). `ArchPacmanCommon.installBasePackages`
   uses `-Syyu` to short-circuit the conditional GET and download
   the current DB unconditionally.

7. **`nativeLibraryDir` freshness across APK upgrades** —
   `applicationInfo.nativeLibraryDir` is a `/data/app/~~<hash>/...`
   path that changes on every APK re-install. We don't persist it:
   [ProotMethod] is constructed per-context and resolves the path
   fresh each time, so `adb install -r` and Play Store auto-updates
   work without any cold-start fix-up.

## Maintenance contract

`scripts/build-proot.sh` pins to a specific Termux/proot commit
(`PROOT_REV`). Bump deliberately when you want changes from
upstream.

We apply one tiny source patch automatically (`apply_local_patches`
in `scripts/build-proot.sh`, run after each fetch/checkout):

- `deps/proot/src/extension/ashmem_memfd/ashmem_memfd.c` needs `#include
  <string.h>` to compile under NDK clang. The patch is idempotent
  via a grep guard, so reclones are safe.

If the Termux fork ever stops compiling under our NDK toolchain, the
options in roughly increasing pain:

1. Pin to the prior known-good commit until the issue is resolved
   upstream.
2. Patch `arch.h` / extensions to suit the new compiler.
3. Switch back to `proot-me/proot` — but you'll need to backport
   the SIGSYS handler from Termux's `src/tracee/seccomp.c` or
   write your own. Don't do this unless you have a reason.

## Reproducing the failure with upstream

If you're curious why we don't use upstream:

```diff
-PROOT_GIT="https://github.com/termux/proot.git"
+PROOT_GIT="https://github.com/proot-me/proot.git"
+PROOT_REV="bd5a5f6"  # tag: v5.4.0
```

Rebuild + reinstall. The proot install reaches PKG_KEYRING then
fails with `proot exit=255` and (with `proot -v 2`) `vpid 1:
terminated with signal 31` on the first `access(2)` syscall.
