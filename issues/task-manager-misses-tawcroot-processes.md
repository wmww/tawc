# In-app task manager shows "No Linux processes running" while tawcroot processes are alive

The Task Manager screen (`me.phie.tawc.tasks.TaskManagerActivity`) walks
`/proc` and classifies a pid as belonging to a rootfs install when any
of `/proc/<pid>/{cwd,exe,root}` resolves to a path under the install's
`<dataDir>/distros/<id>/rootfs/` prefix
(`AppUidProcfsScanner.kt:111-138`). For tawcroot-managed processes
**none of those links cooperate**, so every tawcroot process is
invisible to the scanner — even with the rootfs's `/usr/bin/bash` or
any other rootfs binary running in plain sight.

Found while debugging an orphan `sshd` on a Pixel 10 Pro Fold that
nobody could see or kill; the same set of anomalies reproduces on the
Pixel 7a / `physical` target, so this is a generic tawcroot property,
not device-specific.

## Repro

Compile a trivial binary inside an `arch`/`tawcroot` install that just
`chdir("/")`s and sleeps, double-fork-daemonized so it survives the
parent rootfs-run:

```c
/* /tmp/repro.c */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/prctl.h>
int main(void) {
    prctl(PR_SET_PDEATHSIG, 0);   /* clear tawcroot's die-with-parent */
    chdir("/");
    if (fork()) return 0;
    setsid(); prctl(PR_SET_PDEATHSIG, 0);
    pid_t p2 = fork();
    if (p2) { printf("pid=%d\n", p2); return 0; }
    prctl(PR_SET_PDEATHSIG, 0);
    int dn = open("/dev/null", O_RDWR); dup2(dn,0); dup2(dn,1); dup2(dn,2);
    FILE *f = fopen("/tmp/repro.pid","w"); fprintf(f,"%d\n",getpid()); fclose(f);
    for (;;) pause();
}
```

```
bash scripts/rootfs-run.sh 'gcc -o /tmp/repro /tmp/repro.c && /tmp/repro'
# read /tmp/repro.pid, then in the app: MainActivity → Task manager.
# Screen says "No Linux processes running." despite the daemon being
# alive (verifiable with `test -d /proc/<pid>` from another rootfs-run).
```

## Why the scanner misses it

`/proc/<pid>/` snapshot of the daemon (real readings from this repro):

| link / file | value | scanner expects |
| --- | --- | --- |
| `/proc/<pid>/comm` | `4` | the binary's basename |
| `/proc/<pid>/cmdline` | `tawcroot\0--exec-child\0 3\0` | the guest binary's argv |
| `/proc/<pid>/exe` | `/usr/bin/ls` | the guest binary's path |
| `/proc/<pid>/cwd` | `/data/data/me.phie.tawc/distros/arch/rootfs` (no trailing slash) | startsWith `…/rootfs/` |
| `/proc/<pid>/root` | `/` | startsWith install prefix |
| `/proc/<pid>/maps` (r-x entries) | `…/rootfs/tmp/repro` is there | (not consulted) |

Four independent things are off here:

1. **`comm` is "4"** — a single digit. Not the binary's basename, not
   anything resembling it, just whatever the tail of `argv` happened
   to be. `pgrep <name>` / `pkill <name>` therefore never match.

2. **`cmdline` is the tawcroot supervisor argv** (`tawcroot
   --exec-child 3`), not the guest binary's argv. So `pgrep -f
   <name>` (cmdline match) also misses.

3. **`exe` points to `/usr/bin/ls`** — totally unrelated to the
   binary actually executing. Strongly suggests tawcroot bootstraps a
   child by `execve`ing some throwaway binary (here, `ls`) and then
   manually loads the real ELF into the address space (since the
   real binary does show up in `/proc/<pid>/maps` as an `r-xp`
   mapping). The kernel only records the `execve` target in `exe`.

4. **`cwd` is exactly the rootfs root**, no trailing slash. The
   scanner builds the prefix as `path.trimEnd("/") + "/"`
   (`AppUidProcfsScanner.kt:60`) and then does `path.startsWith(prefix)`,
   so an exact-rootfs-root cwd (which `readlink` returns without a
   trailing slash) is rejected. This is a separate bug from 1-3 and
   would fire even on non-tawcroot guests whose cwd happens to be
   exactly the rootfs root at scan time.

5. (Bonus) `root` is always `/` because we don't have CAP_SYS_CHROOT
   and never call `chroot(2)`. That link is permanently useless to
   the scanner under tawcroot/proot.

So the scanner's three signals are: `cwd` (rejected by the
trailing-slash bug for exact match, also "/" or `/var/empty`-style
post-chdir reads), `exe` (tawcroot loader artifact), `root` (always
`/`). Zero useful signal out of three.

## Suggested fix

The single signal that **does** survive tawcroot's loader stack is
`/proc/<pid>/maps`: the actual ELF binary is mmap'd from
`<install>/rootfs/...` and appears as an `r-xp` file-backed mapping.
Classification by "any r-xp mapping under the install prefix" is both
necessary (covers tawcroot) and sufficient (proot guests have it too).
Suggested change to `AppUidProcfsScanner.classify`:

- Keep the existing `cwd`/`exe`/`root` checks (they still catch proot
  cleanly and are cheap).
- After they all miss, fall through to one pass over `/proc/<pid>/maps`
  looking for an `r-xp .* <prefix>...` line. Limit to file-backed
  executable mappings to avoid false positives from data files.
- Fix the trailing-slash exact-match bug independently: either compare
  with `(path + "/").startsWith(prefix)` or strip the trailing `/`
  from the prefix and add a length-or-`/`-boundary check.

Reading `/proc/<pid>/maps` is more expensive than three `readlink`s but
still cheap (a few KiB read, regex skim). Scoping it to processes that
*didn't* match on the link checks keeps the cost off the proot fast
path.

## Why fixing the upstream tawcroot anomalies isn't easy

The `comm`/`cmdline`/`exe` weirdness comes from tawcroot's loader stack
design (see `notes/tawcroot.md`): the supervisor `execve`s a small
bootstrap binary, then performs its own ELF parsing + `mmap` to bring
the real guest into the process. The kernel only sees the `execve`
target, so `/proc/<pid>/exe` is stuck on the bootstrap and
`/proc/<pid>/comm` /  `cmdline` reflect the bootstrap's argv. Fixing
this would require either an `execveat`-style "swap to real binary"
syscall at the bottom of the loader (possible per the kernel API,
but not currently done) or a different bootstrap strategy entirely.
Both are deeper than this issue wants to dictate.

The scanner-side fix (maps-based classification + trailing-slash fix)
is a single Kotlin file change, decoupled from any tawcroot work, and
makes the task manager honest immediately.

## Bonus: stop button needs to work too

Once the scanner sees the process, `ProcessScanner.stop`
(`ProcessScanner.kt:99-108`) sends SIGTERM via `Os.kill`. That's a
direct syscall from the app uid against another app-uid pid in the same
SELinux domain — should work. But verify: while debugging the
Pixel 10 case, `run-as me.phie.tawc kill -0 <pid>` from `adb shell`
returned "Permission denied", which suggests *something* in the kill
path is restricted at least for the shell→app transition. The in-app
path (kill from the app process itself) is the one to actually test;
if it also fails, we need a separate dig.
