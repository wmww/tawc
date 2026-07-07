# tawcroot: AF_UNIX path rewrite blows the 108-byte sun_path budget

`tawcroot/src/syscalls_socket.c` rewrites guest AF_UNIX socket paths
to the HOST-absolute path before bind/connect/sendto. sun_path is
capped at 108 bytes including NUL; the production rootfs prefix
(`/data/data/me.phie.tawc/distros/<id>/rootfs`, ~45+ bytes, longer
with a long distro id) is spent on every socket path. Guest paths
that are fine natively (~60+ bytes — deep `$XDG_RUNTIME_DIR`, dbus
session sockets, wayland/pipewire sockets under nested runtimes)
fail bind/connect with ENAMETOOLONG only under long install
prefixes, i.e. only on device, and worse for long distro ids.

Possible fix: anchor through a short spelling instead of the full
host path — open the parent dir O_PATH and use
`/proc/self/fd/<n>/<leaf>` (always short) as sun_path, closing the
fd after the call. Needs care for sendto/sendmsg (per-call fd churn)
and for the reverse translation of getsockname/getpeername results.

Host-testable by configuring a rootfs under an artificially deep
tmpdir; verify on device afterwards.
