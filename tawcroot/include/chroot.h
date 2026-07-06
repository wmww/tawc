/* chroot(2) emulation.
 *
 * Real `chroot(2)` requires CAP_SYS_CHROOT, which we don't have inside
 * the Android app sandbox. We emulate by swapping tawcroot's root view
 * (the `tawcroot_rootfs_fd`, the canonical host-path string, the bind
 * table's anchoring, and the well-known-symlink memo cache) so that
 * subsequent path-bearing syscalls resolve as if the kernel had
 * actually chrooted. The kernel's real root is unchanged.
 *
 * Why not -EPERM (the previous behaviour): pacman 6.x calls
 * `chroot(handle->root)` unconditionally inside `_alpm_run_chroot`,
 * even when the handle root is "/". On a Manjaro install every
 * post-install scriptlet and post-transaction hook fails (≈80 errors
 * per `pacman -Syu`); the rootfs is left with un-generated icon
 * caches, GSettings schemas, etc. Faking success without
 * doing the bookkeeping would be just as broken: the next path
 * translation would resolve against the wrong root (or worse, leak
 * outside the rootfs).
 *
 * See notes/tawcroot/path-translation.md §"chroot emulation" for the full state model
 * and edge-case behaviour.
 */

#pragma once

/* Register the chroot dispatch entry. Called from
 * tawcroot_dispatch_init alongside the other registration entry
 * points. (pivot_root continues to deny with -EPERM via
 * `fake_eperm` in syscalls_control.c — different syscall, different
 * registration site.) */
void tawcroot_chroot_register(void);
