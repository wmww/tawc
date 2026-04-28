# `testing/fake-bwrap` disables bubblewrap sandboxing — production users hit the same kernel limitation

The integration test setup script (`testing/install-test-deps.sh`)
installs a wrapper script over `/usr/bin/bwrap` in the chroot rootfs:

```
HOST_BWRAP="$ROOT_DIR/testing/fake-bwrap"
GUEST_BWRAP="/data/data/me.phie.tawc/distros/arch/rootfs/usr/bin/bwrap"
adb shell "su -c 'install -m 0755 /data/local/tmp/fake-bwrap $GUEST_BWRAP'"
```

`testing/fake-bwrap` walks bubblewrap's known argv, throws away the
sandbox-flavour flags (`--unshare-user`, `--bind`, `--ro-bind`,
`--symlink`, `--setenv`, …), and `exec`s the COMMAND that follows
with **no isolation at all** — the loader process runs in the
calling process's environment.

The reason: stock Android kernels ship without `CONFIG_USER_NS`, so
even setuid bwrap can't `clone(NEWUSER)`. Modern Arch GTK + Firefox
both pull in glycin, which in turn execs bwrap to sandbox each image
loader. Without the replacement, every gtk-app integration test
crashes on the first SVG icon.

This unblocks the test suite, but:

## Production gap

End users running tawc on the same Android device will hit the same
`CONFIG_USER_NS` missing bug as soon as they:

- launch any Adwaita-themed GTK app that loads SVG icons
- launch Firefox (which uses glycin for image decode)
- launch any app that uses `bwrap`-via-flatpak-portal patterns

Today's user-facing experience is "app crashes silently on first
icon load" — exactly the failure mode the integration tests would
have hit before we installed the fake-bwrap workaround. There is
**no production-side fix shipped today**.

## What a real fix looks like

Three options, in increasing painfulness:

1. **Ship `fake-bwrap` from the in-app install pipeline** instead of
   only the test setup script. Lowest-friction, identical
   "works-but-no-isolation" outcome the tests already validated.
   Production user gets a working browser at the cost of
   bwrap's would-be sandbox.
2. **Build a userspace bwrap shim that emulates as much as
   possible without `CLONE_NEWUSER`.** E.g. translate `--bind`
   into `mount --bind` (only works under root), `--unshare-net`
   via Linux mount namespaces (allowed without USER_NS),
   `--ro-bind` via `chmod`. Net: more isolation than fake-bwrap,
   less than real bwrap. Substantial effort.
3. **Run apps under proot's view** so glycin's `clone(NEWUSER)`
   happens inside the proot-tracee tree where proot's seccomp
   handler can rewrite it to `clone(...)` without USER_NS — but
   that's a significant proot extension and only helps proot
   installs, not chroot.

Option (1) gets us to feature parity with the test rig fastest. The
honest framing for users would be a release note explaining that
GTK image-loader sandboxing isn't enforced (because the device
kernel doesn't support it), not a silent suppression.

## Why we didn't fix it inline

The test rig doing this in the test-deps script was exactly the
right call to unblock the integration suite — but the production
gap was filed away rather than addressed.

## References

- `testing/fake-bwrap` (the script itself, with rationale comment)
- `testing/install-test-deps.sh` (where it gets installed)
- `notes/proot.md` (related: documents Firefox-under-proot but
  doesn't cover the bwrap angle yet — would be a good update target
  when the fix lands)
