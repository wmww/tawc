# tawcroot: device-node mknod fails with raw EPERM only in production

`handle_mknodat` is a blind translate-and-forward
(`DECLARE_AT_PASS(mknodat, ...)` in `tawcroot/src/syscalls_fs.c`).
FIFOs and sockets work everywhere, but S_IFCHR/S_IFBLK needs
CAP_MKNOD, which the untrusted_app domain never has — so a fake-root
guest gets a raw EPERM from mknod. On rooted test environments
(rooted emulator, `su` shell) the same call succeeds, so this is
invisible to most of the device suite: production-only breakage.

Concrete exposure: distro package postinst scripts and makedev-style
tooling that create `/dev` nodes; debootstrap/second-stage flows.
The module's own posture elsewhere (fchmodat/fchownat/fchmod swallow
EPERM/EACCES under virtual euid 0) says root shouldn't see permission
errors; proot's `-0` degrades device mknod to creating a plain
placeholder file so scripts proceed.

Options (needs a decision + device verification):
- proot-style: on EPERM with virtual euid 0 and S_IFCHR/S_IFBLK,
  create an empty regular file (or FIFO) at the name and return 0.
  Opens of the fake node then fail at use time, like proot.
- fake success without creating anything (worse: later stat/open
  ENOENTs look inconsistent).
- keep EPERM but document.

Whatever is chosen, verify on the prod-env suite (broker,
untrusted_app) where the denial actually fires; the adb-root paths
can't see it.
