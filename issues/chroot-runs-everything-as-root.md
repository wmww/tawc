# Chroot install method runs every process inside the chroot as root

The chroot install method's `enter.sh` does `chroot $ROOTFS /bin/bash
-l` after `su` and bind mounts. The chroot syscall itself needs root,
which is fine — but we never drop privileges before exec'ing bash, so
**every process running inside the chroot has uid 0**. Bash, weston,
Firefox, GTK demos, anything launched via `tawc-chroot-run` or any
future in-app Wayland client launcher.

This is just inertia, not a kernel requirement. The standard Unix
pattern (sshd, login, systemd-nspawn, lxc, podman) is "privileged
setup, then `setuid()` to a regular user before exec'ing the
workload."

## What's wrong with everything-as-root

- Lots of desktop software dislikes it. GNOME apps print warnings,
  Firefox refuses to start as root by default, browser sandboxing
  reacts oddly, Electron apps don't always work.
- Files created in `/home/...` (Firefox profile, dotfiles, the user's
  cache) end up root-owned. When the user inspects via host
  `tawc-chroot-run` or wires up file sharing in future, the
  ownership is wrong.
- Less robust against bugs/crashes in Wayland clients trashing
  arbitrary `/etc` / `/usr` files. Today they have full uid-0 powers.
- Mismatches the proot path's eventual story: under proot, in-rootfs
  processes also "appear to be" uid 0 (proot lies via `-0`) but the
  on-disk owner is the app uid. The shapes diverge here in a way
  that's annoying when comparing the two methods.

## What the right model looks like

Two execution modes inside the chroot, picked by the caller:

- **as-root** for one-time setup and package management: install
  flow's `pacman-key`, `pacman -Syu`, `pacman -S <pkg>`. pacman
  explicitly refuses to run as non-root anyway.
- **as-user** for user-launched apps: weston, Firefox, GTK clients,
  the interactive `tawc-chroot-run` shell when the user types it.

A single regular user inside the rootfs (`useradd -m user`, uid
1000) is enough. The wayland socket is mode 0777 so any uid can
`connect()` — no remapping needed.

## Concrete changes (estimate)

- `Distro.configure` (or a new `Distro.createUser` step in the
  install pipeline): `useradd -m -s /bin/bash user`, write
  `/etc/sudoers.d/wheel` if we want passwordless sudo for system
  ops.
- `InstallationMethod.runInside` grows an `asUser: Boolean`
  parameter (or a separate `runInsideAsUser`). For chroot, as-user
  mode appends `setpriv --reuid=1000 --regid=1000 --clear-groups
  --` to the chroot exec. enter.sh learns a `--user` flag.
- `ProotMethod.runInside` mirrors with `setpriv` inside the proot
  view (or accepts that proot already lies enough that uid doesn't
  matter — TBD).
- Wayland client launch path (whoever ends up calling `runInside`
  to spawn user clients): pass `asUser=true`.
- `ArchPacmanCommon.installBasePackages` etc.: keep `asUser=false`.
- `client/tawc-chroot-run`: maybe a `--root` flag for the rare
  user case who needs the privileged shell, default to as-user.

Existing rootfs ownership stays root-owned for system files. Only
`/home/user` and what the in-rootfs user creates ends up
user-owned. No bulk chown of existing installs.

## Migration

Existing installs (today's "everything-as-root") would keep working
if `asUser` defaults to false on legacy installs without a `user`
account. New installs after this change would have the user
created at install time. Optional one-shot upgrade path: at
launcher startup, if an install lacks `/home/user`, run `useradd`
in the chroot. Cheap and idempotent.

## Out of scope

- Per-user (multi-user) chroots. Just one `user` account is
  enough for the foreseeable future.
- UID mapping between host app uid and chroot user uid. They can
  be different — the wayland socket is the only cross-boundary
  thing and it's world-permissive.
- Privilege escalation inside the chroot via `sudo`. We're the
  ones running the install pipeline; we always have root via
  `su` directly. No need for in-chroot sudo unless the user types
  `sudo apt install ...` interactively.
