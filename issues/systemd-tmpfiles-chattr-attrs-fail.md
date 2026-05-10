# systemd-tmpfiles can't set chattr-style file attributes on /var/log/journal

During post-install of `systemd` under tawcroot, `systemd-tmpfiles
--create` emits:

    Cannot set file attributes for '/var/log/journal', maybe due to incompatibility in specified attributes, previous=0x10001800, current=0x10001800, expected=0x10801800, ignoring.
    Cannot set file attributes for '/var/log/journal/remote', maybe due to incompatibility in specified attributes, previous=0x10001800, current=0x10001800, expected=0x10801800, ignoring.

The tmpfile rule for these dirs requests `FS_NOCOW_FL` (`+C`); the
underlying filesystem (the app's data dir, ext4 on Android) doesn't
support setting that flag via `FS_IOC_SETFLAGS`, so the
`ioctl(FS_IOC_SETFLAGS, …)` returns EOPNOTSUPP. systemd already says
`ignoring`, but it surfaces as `error: command failed to execute
correctly` in pacman because tmpfiles exits non-zero overall.

## Repro

    bash scripts/uninstall-distro.sh manjaro
    bash scripts/install-distro.sh manjaro tawcroot \
        mirrorProxy=http://127.0.0.1:8080/proxy/ distro=manjaro

Search for `Cannot set file attributes for '/var/log/journal'`.

## Don't lie success

The temptation is `handle_ioctl(FS_IOC_SETFLAGS) → return 0`. Don't.
The flag genuinely isn't set on disk afterward; lying about it would
hide a real failure from systemd and from the user. NOCOW on ext4
(and on the Android app data partition specifically) isn't supported
— that's the truth, and `EOPNOTSUPP` is the correct kernel response.
Suppressing it would only be correct if tawcroot actually implemented
the flag (we can't — it's a kernel-level fs feature), or the flag
were irrelevant (it isn't — journald pre-allocates `/var/log/journal`
specifically expecting NOCOW for journal-rotation perf).

## What's actually wrong

The pacman-level `error: command failed to execute correctly` is
the real noise — systemd-tmpfiles itself is being graceful (it says
`ignoring`). pacman is catching tmpfiles' non-zero exit. That's a
package-side issue, not a tawcroot one.

## Possible fixes (none in tawcroot)

- Filter the `+C` rule out of the journald tmpfile config in our
  default rootfs profile (override `/usr/lib/tmpfiles.d/journal-nocow.conf`
  with an empty `/etc/tmpfiles.d/journal-nocow.conf` at install time).
  Cleanest fix; lives in the Kotlin install profile, not in tawcroot.
- Live with the warning; users learn to ignore it.

## Severity

Low. Journal directory is created and writable; only the COW hint
is missing, which costs a small amount of journal-rotation perf.
