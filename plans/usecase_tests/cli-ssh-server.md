# Usecase test: run an SSH server in the rootfs

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user runs sshd in the rootfs and logs into their phone's Linux from a laptop.

This is a marquee usecase the maintainer explicitly expects people to try.
It is also the test most likely to hit undocumented tawcroot gaps: sshd
privilege separation calls `setresuid`/`setgroups` (we run as fake root —
uid 0 is faked, real uid is the app uid), and sshd's seccomp sandbox will
get EPERM (tawcroot denies guest seccomp installs; Firefox tolerates that,
sshd may not). Finding out is the point.

## Prerequisites

- Cache proxy up (README step 6).

## Steps

1. `pacman -S --noconfirm openssh`.
2. `ssh-keygen -A` (host keys), `ssh-keygen -t ed25519 -N '' -f /root/.ssh/usecase_key`,
   append the pubkey to `/root/.ssh/authorized_keys` (mode 600, dir 700).
3. Start on an unprivileged port with logging:
   `/usr/sbin/sshd -D -e -p 2222` (background it; capture stderr to a file).
   If privsep aborts it, retry with `-o UsePrivilegeSeparation=no` if the
   version still accepts it, and record which configs work.
4. Loopback login from a second `rootfs-run.sh` session:
   `ssh -p 2222 -i /root/.ssh/usecase_key -o StrictHostKeyChecking=no root@127.0.0.1 'echo over-ssh; uname -a'`.
5. From the **host**: `adb forward tcp:2222 tcp:2222`, then
   `ssh -p 2222 -i <pulled key> root@127.0.0.1 true` from the host machine
   (pull the private key to your scratchpad). This proves inbound
   reachability from outside the app.
6. Negative check (document, don't fix): binding port 22 should fail —
   no real `CAP_NET_BIND_SERVICE`.
7. If openssh cannot be made to serve a session, try `dropbear`
   (`pacman -S dropbear`, `dropbear -F -E -p 2222 -R`) to separate
   "openssh's sandbox/privsep vs tawcroot" from "sockets are broken".

## Expected results

- At least one server (openssh preferred) accepts a key-authenticated
  session with a working remote command, both loopback and via
  `adb forward`. If only dropbear works, that's a partial pass — file an
  issue for the openssh gap.
- Port 22 bind fails with a permission error (expected, not a bug).

## Known issues / caveats

- Guest seccomp → EPERM, `setuid`-family behavior under fake root is
  unverified (notes/tawcroot/status.md, notes/tawcroot/overview.md). If
  sshd dies on these, capture the exact `-e` log lines for the issue.
- No `systemctl` — daemons must be started by hand; a `System has not been
  booted with systemd` error from `systemctl` is expected behavior.

## Cleanup

Kill sshd/dropbear, `adb forward --remove tcp:2222`, delete
`/root/.ssh/usecase_key*` and the authorized_keys entry, remove the host
copy of the key, `pacman -Rns openssh` (and `dropbear` if installed).
