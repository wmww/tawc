# tawcroot — seccomp filter

## Seccomp filter

Hand-written cBPF. The list of trapped syscalls is the runtime
dispatch table — `prod_rootfs_init` walks it via
`tawcroot_dispatch_trap_list` to collect every syscall number with a
non-NULL slot, and `filter.c` emits the BPF program from that list
and installs it. The dispatch table is the single source of truth
shared between handler routing and the allowlist. Program structure:

```
load arch from seccomp_data (offset 4)
if arch != AUDIT_ARCH_<our-arch>: KILL_PROCESS  // can't happen, defense in depth
// cBPF is 32-bit — compare instruction_pointer in two halves
load instruction_pointer[31:0] from seccomp_data (offset 8)
if lo32 != lo32(&tawcroot_raw_syscall_ret): goto not_stub
load instruction_pointer[63:32] from seccomp_data (offset 12)
if hi32 == hi32(&tawcroot_raw_syscall_ret): ALLOW  // our stub
not_stub:
load syscall_nr from seccomp_data (offset 0)
switch (syscall_nr):
  case openat: TRAP
  case stat: TRAP
  ...
  default: ALLOW
```

**Implementation note:** seccomp cBPF operates on 32-bit words.
`seccomp_data.instruction_pointer` is a `__u64` at offset 8 (after
`int nr` at 0 and `__u32 arch` at 4); you must `BPF_LD_ABS_W` both
halves (offsets 8 and 12) and compare each. On aarch64 with base 0x2000000000 the high 32 bits
are non-zero — a filter that only checks the low half would
mis-ALLOW guest code at a coincidentally matching low address.

That's ~45 BPF instructions, well within the kernel's filter
limit.

Two filters are active: Android's existing `untrusted_app` filter
(already loaded by zygote before we got control), plus our filter
(installed on top). The kernel evaluates all filters on every
syscall; the result with the highest precedence wins. Precedence is
the kernel-defined seccomp action order, not a rule we invent in
tawcroot:

```
KILL_PROCESS (0x80000000) > KILL_THREAD (0x00000000) > TRAP (0x00030000) >
ERRNO (0x00050000) > USER_NOTIF (0x7fc00000) > TRACE (0x7ff00000) >
LOG (0x7ffc0000) > ALLOW (0x7fff0000)
```

Do not reimplement this as an arbitrary numeric sort. The constants
are encoded so the documented order works today, but the maintenance
contract is "follow Linux's action precedence"; use the constants and
keep the behavior tests below as the source of truth.

Key interactions with Android's filter:
- Android says ERRNO, we say TRAP → **TRAP wins**. This is exactly
  what we want for the lp64-`access`-on-x86_64 case documented in
  `notes/proot.md`.
- Both filters say TRAP for the same syscall → **one SIGSYS**, with
  `RET_DATA` from the most recently installed filter (ours). No
  double delivery.
- We say ALLOW, Android says TRAP → **TRAP wins**. We cannot
  un-TRAP anything a prior filter TRAPs. This is why
  `--exec-child` must not install a new filter — see §"Why
  non-PIE". **This also means we can receive TRAPs for syscalls
  we didn't ask to trap** — the handler must return `-ENOSYS`
  for NULL dispatch entries, not abort (see §"SIGSYS handler").
  Future Android versions may TRAP additional syscalls.
- If Android ever KILL_PROCESSes a syscall, we lose — but that's
  true for any sandbox technique on Android and isn't unique to us.

### Filter installation as non-root

`PR_SET_NO_NEW_PRIVS` is the magic that lets unprivileged processes
install seccomp filters. Set it before `seccomp(SECCOMP_SET_MODE_FILTER, …)`.
No `CAP_SYS_ADMIN` needed, no SELinux restriction — confirmed
working from Android's `untrusted_app` domain.

Note: bionic does **not** provide a `seccomp()` libc wrapper. Use
`syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog)`
directly.

The filter is installed **once**, during the initial tawcroot
invocation. It is inherited across `fork` and `execve` as kernel
state. Children of the guest will be filtered too, which is what we
want — a `bash` that spawns `pacman` should still translate paths.
**`--exec-child` does NOT install a new filter** — the inherited
filter is correct because the stub address is a link-time constant
(non-PIE). See §"Why non-PIE".

The SIGSYS handler, by contrast, is process-local (memory, not
kernel state). It is duplicated across `fork` but **reset to
`SIG_DFL` across `execve`** — the post-execve guest has the filter
(kernel state, survived) but no handler. The default action for
SIGSYS is process termination. This is the central thing we have to
solve. Two known approaches:

#### Approach A: re-exec into ourselves first

Make the guest's `execve` go through us. Specifically: rewrite
every `execve("/bin/foo", argv, envp)` so it re-invokes our binary
with a state fd:
`execveat(our_binary_fd, "", ["tawcroot", "--exec-child",
"--state-fd=NN"], envp, AT_EMPTY_PATH)`. The state fd contains the
original target path, guest argv strings, current-root state, rootfs
path, bind specs, and tawcroot flags. envp is the *guest's* envp,
passed through unchanged — `--exec-child` must not splice
anything of its own into the environment. Our binary, on detecting
`--exec-child`, reads and closes the state fd, reinstalls the SIGSYS
handler (reset by `execve`) and re-opens the rootfs fd / bind table.
It does **NOT** install a new seccomp filter — the inherited filter
is correct because the stub is at the same address (non-PIE, see
§"Why non-PIE"). It then proceeds to launch the real target via the
PT_INTERP dance below.

**Critical bootstrap constraint:** between the `execveat` that
launches `--exec-child` and the point where the handler + rootfs
fd are fully initialized, the inherited seccomp filter is live but
there is no handler. The default SIGSYS action is process
termination. Therefore **`--exec-child`'s entire init path —
state-fd read, handler install, rootfs fd open, bind-table setup,
ELF load — must issue all its own syscalls through
`tawcroot_raw_syscall()`**, which the inherited filter ALLOWs by
IP. No libc wrappers for TRAPped syscalls (`openat`, `stat`,
`readlink`, `mmap` with path args, etc.) until after the jump to
the guest. `sigaction` is safe (not TRAPped). Static bionic's
`memcpy`, `memset`, string functions are safe (no syscalls). This
is the same discipline as the handler itself — the handler uses
the stub to avoid recursion; `--exec-child` init uses it to
survive the no-handler window and the no-rootfs-fd window.

Pros: clean conceptual model, every guest process is a fresh
tawcroot init.

Cons: every guest exec becomes one host re-exec into tawcroot plus a
manual ELF load/jump for the real target. Per-exec overhead is small
individually but stacks up on fork-heavy workloads (`pacman -Qkk`,
configure scripts, Firefox content-process spawn). Worth measuring
once the MVP runs.

This is the approach we start with. It's morally what proot's
loader does, except our "loader" is the same binary as our entry
point — **but only if `libtawcroot.so` is statically linked.**
Otherwise bionic linker opens libtawcroot's `DT_NEEDED` libs
under the inherited filter with no handler installed, and the
process dies before `--exec-child` runs. See "Known gaps" #1.

#### Approach B: in-process trampoline preserved across execve

Use `mmap(... MAP_FIXED ...)` to put our handler + globals at a
fixed address that we then claim is "owned" via something like
preloading. Doesn't actually work — `execve` clears mappings
unconditionally. The only way to survive `execve` is to be in the
new program's text or be re-loaded.

Verdict: Approach A plus manual guest load. Each guest `execve`
becomes a re-exec of tawcroot. The `--exec-child` process
reinstalls the handler, then manual-loads the real target and jumps
to it without another `execve`. The seccomp filter is inherited so
syscalls keep getting trapped; the handler stays live because the
final transfer to guest code is an in-process jump, not an exec.

This means **the binary must be exec'able** from the guest's
context. In Android terms, that's the `nativeLibraryDir`
(`/data/app/~~<hash>/me.phie.tawc-<hash>/lib/<abi>/libtawcroot.so`)
which has the `apk_data_file` SELinux context, same trick proot
uses for `libproot-loader.so`. So we ship as `libtawcroot.so` and
the path stays in `nativeLibraryDir`.

`libtawcroot.so` is therefore a directly-`execve`d non-PIE static
binary *named* as a shared object — same pattern proot's
`libproot-loader.so` uses (also an ET_EXEC static binary renamed
to `.so` for jniLib extraction). Android's jniLib extractor only
matches on `lib*.so` filenames — it doesn't validate ELF type.
Build with `-static -no-pie` and an explicit `_start`; the build
script links it as an executable, with
`-Wl,-Ttext-segment=<base-addr>` per arch (see §"Why non-PIE"
for address choices) and
`-Wl,-soname,libtawcroot.so` so the jniLib packaging step is
happy. The APK **must** set `extractNativeLibs="true"` in the
manifest (already the case — proot's `libproot-loader.so` has the
same constraint). Without extraction, jniLibs are mmap'd directly
from the APK and don't exist as files the kernel can `execve`.

The explicit `_start` matters. Do not rely on opaque static-bionic
startup code before tawcroot has installed or reinstalled the `SIGSYS`
handler, because the inherited seccomp filter is already live in
`--exec-child`. The startup contract is:

- `_start` parses the raw initial stack enough to find `argc`, `argv`,
  `envp`, and auxv without libc.
- If this is `--exec-child`, all syscalls before the handler is live
  go through `tawcroot_raw_syscall()` and only through the raw stub.
- Any bionic/libc initialization we choose to call must be audited for
  syscalls under the inherited filter. If we skip bionic init for the
  runtime path, document which libc facilities are therefore forbidden
  before the manual guest jump.
- Handler/runtime code does not depend on bionic TLS, `errno`, stdio,
  malloc, pthread state, or property-service initialization. Init code
  may use higher-level helpers only before the first filter install, or
  after the `--exec-child` handler is installed.

Side effect: the binary needs to know its own path (to re-exec
itself). Either bake it in via a `-D` at compile time (brittle —
APK paths change on reinstall) or read `/proc/self/exe` at startup
and stash it. **Two separate stashed paths, two separate uses:**

- **`our_binary_path`** — the real on-disk path of
  `libtawcroot.so` (read via `readlinkat` through the raw stub
  against `/proc/self/exe` at initial startup and in each
  `--exec-child` init, before the handler is installed or before
  the guest exe stash is set — so the kernel returns the real
  path, not the synthesized one). Used by the handler to re-exec
  ourselves on guest `execve`.
- **`guest_exe_path`** — the path the guest *asked* us to exec,
  set by `--exec-child` after it translates the guest's requested
  binary path. Used by the `/proc/self/exe` synthesis handler
  (§"`/proc/self/exe`") to return what the guest expects.

These are distinct static globals. Confusing them is an easy bug:
`our_binary_path` is a host path into `nativeLibraryDir`;
`guest_exe_path` is a rootfs-relative path like `/usr/bin/bash`.

The Kotlin install side rebuilds the argv on every entry, the same
way the bind table is rebuilt for proot/chroot (see
`notes/rootfs-sessions.md` for the single-entry-point invariant).

