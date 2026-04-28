# `ProotMethod.wipe`'s su-retry path is probably legacy — and the
# bind-mount safety story for any su-capable wipe needs an audit

`ProotMethod.wipe` falls back to `Su.run("find … -xdev -depth -delete")`
when the app-uid delete fails. The kdoc rationale (`ProotMethod.kt`
~lines 236-249) is that integration tests on rooted devices used to
exercise `client/tawc-chroot-run` against a proot install via a
`su -c '<enter.sh>'` path, leaving pacman/package files root-owned
on disk. Plain app-uid `chmod -R u+rwX` then couldn't make those
files writable, and unlink failed with EACCES.

`client/tawc-chroot-run` now reads `metadata.json#method` and
dispatches:

- proot install → `run-as me.phie.tawc <enter.sh>` (no root)
- chroot install → `su -c <enter.sh>` (root, real bind mounts)

So a freshly-installed proot rootfs running through the current test
rig should never accumulate root-owned files. The only remaining
justification for the su-retry is **stale installs from before that
dispatch landed** — installs that exist on a device but were created
when `tawc-chroot-run` always took the `su -c` path.

## The action

1. **Decide whether the legacy case still matters.** If we believe no
   such installs exist in the wild (it's an early-development project,
   nobody's relying on a months-old install), drop the su-retry
   entirely and let the wipe surface a clean FAILED. Users can recover
   manually with `adb shell su -c 'rm -rf …'`.

2. **Or keep it, with mount-aware semantics.** The current code does
   use `find -xdev -depth -delete` which already refuses to cross
   filesystem boundaries — so a stray bind mount in the rootfs would
   block recursion into the source rather than letting `rm` eat
   `/apex` or `/system`. That's the safe baseline.

   Note for any future "let su clean up app-uid leftovers" path
   added elsewhere (or if this one is rewritten with `rm -rf` for
   any reason): **bind-mounted device nodes inside the rootfs are
   a recurring footgun on this project.** A privileged delete that
   isn't `-xdev`-equivalent risks deleting the live `/dev/binder`,
   `/dev/null`, and friends, which is system-trashing in a way that
   "I deleted my install dir" is not. The chroot path's
   `RootfsCleaner.wipe` is the reference — match it or beat it,
   never weaken it.

## Why we didn't fix it inline

Came up during a code review of the proot work. Worth handling on
its own so we can either confidently delete the path or harden it
without rolling other unrelated cleanup in.

## References

- `server/app/src/main/java/me/phie/tawc/install/ProotMethod.kt`
  — `wipe` and its kdoc
- `server/app/src/main/java/me/phie/tawc/install/RootfsCleaner.kt`
  — chroot-side reference for mount-aware deletion
- `client/tawc-chroot-run` — the now-method-aware dispatch that made
  the legacy case stop accumulating
