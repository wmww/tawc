# Firefox-specific `MOZ_DISABLE_*_SANDBOX` env vars baked into the universal proot profile

`ProotMethod.renderEnterScript` writes seven Firefox-specific env vars
into `/etc/profile.d/01-tawc.sh` for every proot install:

```
export MOZ_DISABLE_CONTENT_SANDBOX=1
export MOZ_DISABLE_GPU_SANDBOX=1
export MOZ_DISABLE_RDD_SANDBOX=1
export MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1
export MOZ_DISABLE_UTILITY_SANDBOX=1
export MOZ_DISABLE_GMP_SANDBOX=1
export MOZ_DISABLE_VR_SANDBOX=1
```

These exist because Firefox's per-subprocess seccomp/namespace
sandboxes SIGSEGV under proot — proot is already a ptrace tracer with
its own seccomp filter, and Firefox's nested setup blows up at exec
time. So we disable Firefox's sandbox.

The hack works for Firefox, but:

1. **It's app-specific code in a universal codepath.** Every shell
   command run inside any proot install (whether the user ever opens
   Firefox or not) gets these vars set. A user installing Chromium /
   Thunderbird / Electron-based apps next will need a similar set of
   env knobs (Chromium has `--no-sandbox`, Electron `--no-sandbox`).
   We'll either keep growing the universal profile.d for every
   sandboxed app we encounter, or we never make those apps work.

2. **Disabling Firefox's sandbox really does reduce isolation** even
   if the rootfs is itself an app-uid sandbox. A renderer compromise
   that previously couldn't reach the parent process can now reach
   it directly.

3. **Discoverability is poor** — a user inspecting why their Firefox
   has different security posture under tawc would have to find the
   compositor-rendered profile.d/01-tawc.sh and reason about why
   tawc's authors decided to disable mutually-exclusive sandboxes.

## What a universal fix looks like

The right shape is "let nested seccomp work" or "proot doesn't set
seccomp on tracees that ask to install their own filter." Concretely:

- **Option A: PR_SET_NO_NEW_PRIVS aware proot.** Termux's proot fork
  could grow a `--allow-tracee-seccomp` flag that skips its own
  filter installation when the tracee tries to call
  `prctl(PR_SET_SECCOMP, ...)`. Tracee's filter then runs natively
  on top of Android's filter (since `PR_SET_NO_NEW_PRIVS` already
  composes filters). Some apps (Firefox subprocess, Chromium
  renderer) might still hit the underlying Android filter and need
  separate handling, but this gets us out of the universal disable.
- **Option B: per-app launcher wrappers.** Move the `MOZ_DISABLE_*`
  env vars out of profile.d/01-tawc.sh and into a Firefox-specific
  wrapper script (e.g. `/usr/local/bin/firefox-tawc-wrapper`) that
  the user invokes when they want Firefox under proot. The system
  profile stays generic. Costs: launcher discoverability, the user
  has to know to run the wrapper.
- **Option C: detect Firefox at runtime and inject env in our
  process spawner.** When the in-app launcher (future
  per-app spawner) detects an executable named `firefox`, set the
  env vars there. Doesn't help command-line `tawc-chroot-run firefox`
  though.

## Why we didn't fix it inline

The proot install just landed; getting Firefox working under proot
was the gating goal. The nested-seccomp work is its own project and
needs proot fork changes.

## Out of scope

- Solving Chromium / Thunderbird / etc. in advance. We don't have
  test apps for those today.
- Re-enabling Firefox's sandbox under chroot (where it already
  works). That path doesn't pay this cost; the issue is purely
  proot-mode.

## References

- `server/app/src/main/java/me/phie/tawc/install/ProotMethod.kt`
  (`renderEnterScript` body, the `MOZ_DISABLE_*` exports inside
  `/etc/profile.d/01-tawc.sh`)
- `notes/proot.md` "Firefox under proot" — documents the why.
