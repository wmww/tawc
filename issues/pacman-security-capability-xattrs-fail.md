# pacman cannot restore `security.capability` xattrs under tawcroot

During Manjaro install, package extraction prints warnings on every
file that ships a `security.capability` xattr, and the gstreamer
post-install scriptlet (which runs `setcap` itself) prints an error
that pacman bubbles up as `error: command failed to execute correctly`.

Symptoms in the install log:

    installing shadow...
    warning: warning given when extracting /usr/bin/newgidmap (Cannot restore extended attributes: security.capability security.capability)
    warning: warning given when extracting /usr/bin/newuidmap (Cannot restore extended attributes: security.capability security.capability)
    ...
    installing gstreamer...
    warning: warning given when extracting /usr/lib/gstreamer-1.0/gst-ptp-helper (Cannot restore extended attributes: security.capability security.capability)
    unable to set CAP_SETFCAP effective capability: Operation not permitted
    error: command failed to execute correctly

The root cause is that `untrusted_app` on Android is denied
`CAP_SETFCAP` by SELinux, and writing `security.capability` xattrs
requires that capability. The kernel returns EPERM, libarchive
degrades to a warning, and the install proceeds **without the
capability bit set on disk**.

## Impact

- Setuid-via-capability binaries (`newuidmap`, `newgidmap`,
  `gst-ptp-helper`, `ping`, …) lose their elevation. They run, but
  operations that depend on the capability fail at runtime.
- `gst-ptp-helper`'s post-install scriptlet itself prints an error
  because `setcap` (a separate binary calling `cap_set_proc()`,
  unrelated to the xattr write) fails with EPERM on
  `CAP_SETFCAP`. That fix lives elsewhere — `cap_set_proc` doesn't
  go through `setxattr`.

## Fix path: don't lie about capability bits

The temptation is to make `handle_setxattr` lie success on
`security.capability` so the warnings disappear. **Do not do this.**

- libalpm's warning is correct: the bit *did not get set*. Suppressing
  the warning hides real information from the user.
- The xattr is meaningful — Linux applies it at exec-time to grant
  the calling process specific capabilities. Lying success doesn't
  write the xattr to disk; the kernel's exec path still reads no
  caps off the binary.
- A guest later running `getcap /usr/bin/foo` would see the truth
  (no caps) and correctly conclude the bit isn't set, but anything
  that used `setxattr` rv as ground truth (rare but possible) would
  silently misconfigure.

The fix would only be correct if either tawcroot actually applied
the capabilities (we can't — we don't have CAP_SETFCAP, and we
don't have an exec-time capability application layer to fake it
with) or capabilities were 100% irrelevant in this environment
(they aren't — `newuidmap` and friends genuinely depend on them).

## What we should do instead

Cosmetic: nothing. The warnings are accurate.

If a workload actually needs `newuidmap` or `gst-ptp-helper` to be
functional inside the chroot, the right fixes are:

- For `newuidmap`/`newgidmap`: not relevant under tawcroot (no
  user namespaces; uid mapping is handled by tawcroot's fake-root).
- For `gst-ptp-helper`: pre-time-protocol use case is niche enough
  inside a phone-bound chroot that just letting it fail (it's an
  optional GStreamer plugin) is fine.
- If a future workload genuinely needs the capability bit, the only
  honest answer is to mark that workload unsupported under tawcroot
  on rooted-via-Android-app-uid environments, or run it under
  `chroot` install method (which has CAP_SYS_ADMIN via su — the
  one install method that *can* set the xattr).

## Severity

Low — install completes, affected binaries don't run inside our
compositor anyway. Filed as an explanation of expected behavior so
the next person staring at the warnings doesn't reach for the
lying-success workaround.
