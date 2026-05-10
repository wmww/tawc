# tawcroot: execveat with dirfd != AT_FDCWD returns -ENOSYS

`tawcroot/src/syscalls_exec.c:155-164`:

```c
static long handle_execveat(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	int dirfd = (int)args->a;
	if (dirfd != AT_FDCWD) return TAWC_ENOSYS;
	return do_exec((const void *)args->b, (char *const *)args->c,
	               (char *const *)args->d);
}
```

The header comment is honest about it: *"Phase-2 minimum: AT_FDCWD only. Anything else returns -ENOSYS for now."*

## Affected callers

The classic shape is `fexecve(3)`, glibc's wrapper for "exec this open
fd". Glibc implements it as

    execveat(fd, "", argv, envp, AT_EMPTY_PATH)

— `dirfd` is a real fd, path is empty, flag is set. Hits the
`-ENOSYS` branch unconditionally. Anything that passes a real dirfd
plus a relative path (the `execveat(dirfd, "subdir/cmd", …)` shape)
also hits it.

Guests likely to trip this:

- libcs that prefer fd-based exec for sandboxing (bionic-derived
  paths, some Go runtime variants).
- `runit`, `s6`, container runtimes that execveat by fd.
- Programs that open the binary, do something with it (verify
  signature, lock against unlink, etc.), then exec the same fd.

Glibc itself uses `fexecve` internally in a few corners (e.g.
`posix_spawn` paths in some configurations), but the main fork+exec
chain on Android/aarch64 goes through plain `execve` (NR 221), so the
common pacman/bash workloads don't trip this.

## What -ENOSYS means here

The kernel has `execveat`. Returning -ENOSYS from our handler tells
the guest "this syscall doesn't exist on this kernel." Most callers
have *no fallback* for this — they just propagate -ENOSYS up as a
fexecve failure. Those that do fall back (some glibc versions detect
ENOSYS and then call execve via /proc/self/fd/N) hit our path
translation correctly because /proc/self/fd is procfs.

Net effect: silent breakage of any guest that relies on fd-based exec.

## Fix

Translate to the AT_FDCWD form by reading the dirfd's path.

For the `(dirfd, "", AT_EMPTY_PATH)` shape — by far the common case:

1. `readlinkat(AT_FDCWD, "/proc/self/fd/<dirfd>", buf)` to recover the
   guest path the fd points at.
2. Call into `do_exec(path_buf, argv, envp)`.

The same pattern handles the relative-path form
(`execveat(dirfd, "subdir/cmd", argv, envp, 0)`): join dirfd's path
with `"subdir/cmd"` and forward.

Care needed for the `dirfd` lookup: dirfd may itself be one of our
reserved fds (rootfs O_PATH or a bind src). The reverse-translate path
already exists in `path.c`'s `dirfd_to_guest_abs`; reuse it instead of
hand-rolling a new readlinkat.

AT_SYMLINK_NOFOLLOW is also a flag execveat can carry — it just refuses
to follow a leaf symlink. Easy to honor by passing through to
`tawcroot_path_translate` with the equivalent mode.

## Test plan

A new fixture and integration test under
`tawcroot/tests/integration/test_prod_features.c`:

- `static_fexecve_argv1.S`: open `argv[1]` with `O_PATH | O_CLOEXEC`,
  then execveat(fd, "", argv+1, envp, AT_EMPTY_PATH). On
  -ENOSYS-from-handler this fails immediately and we exit non-zero.
- Test runs it under `tawcroot -r ROOTFS -- /bin/static_fexecve_argv1
  /bin/static_exit42` and expects exit 42 (the inner binary's exit).

Should be runnable on host x86_64 + aarch64 phone the same way the
existing fork tests are.

## Severity

Medium. Correctness, not performance — guests that use fexecve are
silently broken and there's no fallback to translate around it.
Not blocking the current Arch/pacman/Firefox workloads (none of those
use fexecve in their hot paths), so latent rather than active.
