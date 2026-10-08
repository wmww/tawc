# chroot method: broker only relays the guest's first stdout line

Running a multi-command line through the chroot method relays only the
output of the first command; the rest is silently dropped even though
the guest runs it all:

    scripts/rootfs-run.sh 'echo A; echo B; echo C'
    → A                          (exit 0)

Redirecting the same commands to a file inside the rootfs and reading
it back afterwards shows `B`/`C` were produced normally, so this is the
host-side relay (ChrootMethod's `su` pipe / `MethodRunHelper` stream),
not the guest.

Suspect: `ChrootMethod.startInside` pipes the mount+chroot script to
`su -c 'exec unshare -m -- /system/bin/sh'` over stdin and deliberately
leaves the pipe open; the `exec setsid chroot …` line then replaces the
shell. Something in that handover appears to close or steal the stdout
pipe after the first write.

Caveat on the repro: observed on the OnePlus 9 (Magisk su, Android 14)
by temporarily flipping a *tawcroot*-created install's `metadata.json`
`method` to `chroot` — not by installing with `method=chroot` from
scratch. Worth confirming with a real chroot install before digging.

chroot is a debug-only method (`notes/chroot.md`), so this is low
priority; noted while spot-checking the `/linkerconfig` copy change
across all three methods, 2026-08-10. That check itself passed under
chroot (no `/linkerconfig` in `/`, config copy present) — it just had
to be read back from a file.
