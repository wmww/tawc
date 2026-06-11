# chroot — Magisk-`su` install method

Real `chroot(2)` into the rootfs via Magisk's `su`. Fastest path
(no syscall translation, no ptrace tracer), but needs root and
runs everything inside the chroot as uid 0.

**Status: dev-only, not officially supported.** [tawcroot](tawcroot.md)
is the default and only officially supported install method; release
builds ship only tawcroot. chroot stays in debug builds because it's
the syscall-translation-free baseline for perf comparison and the
fastest way to repro rootfs-side bugs against a stock Arch userland.
It is not exposed to release users — `app/build.gradle.kts` flips
`BuildConfig.METHOD_CHROOT_ENABLED` off for release. proot is in the
same boat (see [notes/proot.md](proot.md)).

## What it ships

| file                         | role |
| ---------------------------- | ---- |
| `ChrootMethod.kt`            | `startInside` — pipes [ChrootMounter.mountScript] + the in-rootfs `setsid chroot $rootfs … /bin/bash …` to a single `su -c 'exec unshare -m -- /system/bin/sh'` over stdin. One shell per entry; mounts torn down when it exits. |
| `ChrootMounter.kt`           | Builds the bind-mount shell snippet (`mountScript`) — `/apex /vendor /system /system_ext /linkerconfig /dev /sys /proc /dev/pts /dev/shm /usr/share/tawc`. Also provides defensive-cleanup `unmount` for [RootfsCleaner] to call from uninstall. |
| `Su.kt`                      | Wrapper around Magisk `su`. Pipes the script via stdin (no shell-quoting headaches), streams combined stdout/stderr line-by-line via a callback. The non-mount-master path wraps in `unshare -m`. |
| `RootfsCleaner.kt`           | The one and only delete path (all methods, see [notes/installation.md](installation.md)): kill guest processes → unmount strictly → mount gate → `find -xdev -depth -delete` via `su`. Used by uninstall; never by install. |

The shared install pipeline (download / verify / extract /
configure / pacman bootstrap) is method-agnostic and lives in
[notes/installation.md](installation.md). chroot only contributes
its `startInside` override; [RootfsCleaner] derives the chroot-only
wipe facts (kernel mounts, root-owned guests) from the method key
recorded in metadata.

## Mount lifecycle

Magisk's `su` inherits the **calling** process's mount namespace by
default — bind mounts done inside one `Su.run` would persist into the
app's namespace and pile up across calls. The recursive
`/data/data/<pkg> → <rootfs>/data/data/<pkg>` bind in particular is
the smoking gun: it makes `find -xdev` walk back into itself ("loop
detected") and the uninstall delete fails on a tree it created
moments earlier. To keep each invocation isolated, [Su.run] wraps the
non-mount-master path in `unshare -m` so every script gets its own
private mount namespace that's torn down when the script exits.

The canonical chroot entry point is [ChrootMethod.startInside], which
pipes [ChrootMounter.mountScript] + the chroot exec to `su` over stdin
as one shell — the mounts exist for the lifetime of that shell and
never leak. The mount logic is rebuilt fresh in Kotlin on every entry,
so changes pick up without reinstalling. There is no separate `MOUNT`
operation because there can't be — the mounts only exist for the
lifetime of that one shell. This avoids polluting the global mount
table with stale entries (and avoids the zygote-fork crash that
follows when `/data/data/<pkg>/...` has live bind mounts during
package fork).

The install path **never** touches mounts. It can't possibly delete
through one because it doesn't delete at all (the gate guarantees an
empty slot). Mount cleanup belongs to the uninstall path
([RootfsCleaner]); see *Uninstall pipeline* in
[notes/installation.md](installation.md).

`ChrootMounter.unmount` `realpath`s the rootfs before scanning
`/proc/mounts`, because Kotlin's `File.absolutePath` returns the
`/data/user/0/...` symlink form while `/proc/mounts` reports the
canonical `/data/data/...` form — naive substring matching misses
every entry. The match is also a strict prefix check (`==` or starts
with `r"/"`) so paths containing `.` don't over-match other mounts.

## Parked dev-only issues

chroot is debug-build-only and not on the path to a fix. The
following issues are documented here so they don't sit in `issues/`
clogging the real backlog. Re-promote to `issues/` only if chroot
stops being dev-only. proot's parked issues are at the bottom of
[notes/proot.md](proot.md).

### Every process inside the chroot runs as uid 0

`ChrootMethod.startInside` does `chroot $rootfs … /bin/bash -l`
(ChrootMethod.kt:67,69) without dropping privileges, so bash, weston,
Firefox, GTK demos, anything launched via `rootfs-run` or any future
in-app Wayland client launcher runs as root. Inertia, not a kernel
requirement — sshd / systemd-nspawn / podman all do "privileged
setup, then `setuid()` to a regular user before exec'ing the
workload."

What's wrong with everything-as-root: lots of desktop software
dislikes it (GNOME warnings, Firefox refuses to start as root,
browser sandboxing reacts oddly, Electron flakiness); `/home/...`
files end up root-owned (Firefox profile, dotfiles) and the ownership
is wrong when inspected via host `rootfs-run` or any future file
sharing; less robust against buggy Wayland clients trashing
`/etc`/`/usr`; mismatches the proot shape (proot lies via `-0` so
processes "appear to be" uid 0 but on-disk owner is the app uid).

The right model is two execution modes picked by the caller:
**as-root** for one-time setup and `pacman` (which refuses non-root
anyway), **as-user** for user-launched apps. A single regular user
inside the rootfs (`useradd -m user`, uid 1000) suffices — the
wayland socket is mode 0777, no remapping needed. Concrete changes:
a new `Distro.createUser` step (`useradd -m -s /bin/bash user`,
optional `/etc/sudoers.d/wheel`); `InstallationMethod.runInside`
grows an `asUser: Boolean` (chroot side appends
`setpriv --reuid=1000 --regid=1000 --clear-groups --` to the chroot
exec); proot mirrors with `setpriv` (or accepts that proot already
lies enough that uid doesn't matter — TBD); Wayland client launches
pass `asUser=true`; `ArchPacmanCommon.installBasePackages` keeps
`asUser=false`; `scripts/rootfs-run.sh` defaults to as-user with a
`--root` opt-in. Existing installs keep working if `asUser` defaults
to false on legacy installs without a `user` account; new installs
get the user at install time. Optional one-shot upgrade at launcher
startup (`useradd` if `/home/user` is missing). Out of scope:
multi-user chroots, UID mapping, in-chroot sudo.

### Mount setup runs on every chroot entry — no fast path

(The referenced `ChrootRunner.run` is now `ChrootMethod.startInside`,
but the behaviour is unchanged.) Each entry is a fresh `su` shell
with a private mount namespace, so `ChrootMounter.mountScript` runs
every time and pays a ~400ms cost. Earlier iterations had a fast
path (`if /sys is mounted`) that skipped setup on repeated
invocations because mounts persisted in the global namespace; the
new design is deliberate to avoid the zygote-fork crash from live
binds under `/data/data/<pkg>` during package fork (see *Mount
lifecycle* above). Not a correctness regression but a real perf hit
for many-short-commands workflows like integration tests doing one
chroot entry per assertion. Mitigations: batch commands into one
`RUN` (already supported), or a long-lived helper shell inside the
chroot fed commands over a pipe so mount setup happens once per
session. Defer until benchmarks show it matters.
