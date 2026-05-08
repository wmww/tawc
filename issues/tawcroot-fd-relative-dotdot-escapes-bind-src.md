# Fd-relative `..` from a bind-src dirfd still escapes into the host

The fd-relative `..` lift in `fetch_and_translate_at` only triggers
for dirfds inside the **main rootfs view** — `dirfd_to_guest_abs`
returns -ENOENT when /proc/self/fd's host path doesn't sit under
`tawcroot_rootfs_host_path`, and the caller falls through to
kernel-resolved passthrough.

A dirfd opened through a bind dst (e.g. `/system` → bind src
`/system` on the host) gets a /proc/self/fd link rooted at the bind
src on the host, fails the prefix check, and the kernel then walks
`..` past the bind src freely:

    int fd = openat(AT_FDCWD, "/system/lib/somedir", O_PATH);
    int up = openat(fd, "../../../etc/passwd", O_RDONLY);
    /* up now points at the host's /etc/passwd, which the
     * guest's view doesn't even contain. */

This is a pre-existing tawcroot gap (the `..` lift didn't exist
before either; the kernel always handled fd-relative paths
verbatim) but the new comment in `fetch_and_translate_at` —
"mimicking real chroot(2) semantics" — overclaims for bind-src
dirfds. Real chroot(2) wouldn't allow this either.

## Severity

Low under our current threat model: tawcroot already deliberately
exposes the bind-src directory tree to the guest, so `..`-walking
within an opened bind src is mostly noise (the worst it gets the
guest is access to siblings of the registered src paths). The
attack surface is the host area *between* a bind src and the
filesystem root.

## Repro

No clean repro yet — needs a tawcroot binary built with a verbose
filter logger, or a hand-rolled C program issuing the openats above
inside a tawcroot session.

## Possible fixes

1. **Reverse-translate bind src in `dirfd_to_guest_abs`.** Walk
   `tawcroot_binds[]`, longest-prefix-match against `bind.src`,
   return guest-side `/<bind.dst>/<remainder>`. `path_translate`
   then re-routes through the bind on the second pass and the lift
   clamps `..` at `bind.src` (per RESOLVE_IN_ROOT semantics for the
   bind sub-view).
2. **Canonicalize `bind.src` at `tawcroot_path_add_bind` time** so
   the longest-prefix-match in (1) matches whatever
   /proc/self/fd reports. Without this, a user-supplied src that
   traverses a symlink fails the match.

(1) without (2) catches the typical Android bind set
(`/system`, `/vendor`, `/apex`, `/dev` …) which sit on real
directories. Worth doing in one change.
