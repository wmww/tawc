/* Synthesized Android-`untrusted_app`-shape seccomp prefilter wrapper.
 *
 * Reproduces the parts of Android's stacked seccomp filter that have caused
 * production tawcroot bugs (see notes/tawcroot/testing.md "Bugs found and fixed
 * during phase-5b" and similar). On plain host Linux this lets us run the
 * existing handler-layer test surface against the same filter shape we'd
 * see under run-as on a real device, without needing adb / a phone /
 * emulator.
 *
 * Usage:
 *
 *   wrap [--include-legacy-x86_64] [--xperm-tcgets2] -- <child> [args...]
 *
 * Installs PR_SET_NO_NEW_PRIVS + a seccomp BPF filter, then execve()s the
 * child with the original args. The filter inherits across exec, so the
 * child runs under it.
 *
 * Filter shape (default = ALLOW, RET_TRAP on the listed syscall numbers):
 *
 *   - openat2     (437)  // RET_TRAPed on Android 16; tawcroot probe must
 *                        // route to handler -> -ENOSYS fallback.
 *   - faccessat2  (439)  // Likewise; handlers must NOT issue NR 439
 *                        // recursively (the bug fixed in handle_access).
 *   - clone3      (435)  // tawcroot must explicitly trap and -ENOSYS so
 *                        // glibc 2.34+'s probe falls back to clone(2).
 *   - With --include-legacy-x86_64 (only meaningful on x86_64): also TRAP
 *     access (21), open (2), chmod (90), chown (92), mkdir (83),
 *     rmdir (84), unlink (87), symlink (88), link (86), rename (82),
 *     readlink (89), stat (4), lstat (6), epoll_wait (232). These are
 *     the lp64 legacy syscalls Android RET_TRAPs because bionic's
 *     allowlist only grants them to lp32 — see notes/proot.md "Why
 *     upstream proot doesn't work on Android x86_64". tawcroot routes
 *     them all through *at variants (or for epoll_wait, through
 *     epoll_pwait) in the handler. (No-op on aarch64; those NRs aren't
 *     allocated.)
 *   - With --xperm-tcgets2: simulate Android's untrusted_app_all_devpts
 *     ioctl xperm whitelist gap by RET_ERRNO|EACCES'ing
 *     ioctl(_, TCGETS2|TCSETS2|TCSETSW2|TCSETSF2, _). This is the
 *     real-world bug shape that breaks bash/lxterminal/wezterm — the
 *     SELinux ioctl xperm check returns EACCES instead of the
 *     ENOTTY/EINVAL glibc would fall back from. tawcroot's ioctl
 *     handler must intercept these four cmds and route them through
 *     the legacy TCGETS family. Unlike the RET_TRAP rules above this
 *     is a RET_ERRNO action, applied unconditionally — there's no
 *     handler in the wrapper, the child just sees -EACCES from
 *     userspace's POV until tawcroot's handler intervenes.
 *
 * Deliberately NOT trapped:
 *
 *   - execve / execveat: the wrapper itself uses execve to launch the
 *     child. Trapping would SIGSYS the wrapper (no handler installed) and
 *     kill it before exec. tawcroot's handler internally uses execveat
 *     to re-exec self for the --exec-child handoff; trapping that would
 *     re-enter the SIGSYS handler, which the design explicitly avoids
 *     (see "faccessat2 recursive SIGSYS" in notes/tawcroot/phasing.md).
 *   - Any syscall the wrapper itself uses during init (read, write,
 *     mmap, prctl, seccomp). Same reason — kills the wrapper.
 *
 * The wrapper does NOT install a SIGSYS handler. SIGSYS for any trapped
 * syscall delivers default-action process termination — exactly Android's
 * behavior pre-tawcroot. Once the child execs and installs its own
 * SIGSYS handler, the inherited filter will route trapped syscalls
 * through that handler instead of killing.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

/* Match notes/tawcroot/testing.md naming. */
#if defined(__aarch64__)
# define WRAP_AUDIT_ARCH      AUDIT_ARCH_AARCH64
# define WRAP_NR_openat2      437
# define WRAP_NR_faccessat2   439
# define WRAP_NR_clone3       435
# define WRAP_NR_ioctl         29
#elif defined(__x86_64__)
# define WRAP_AUDIT_ARCH      AUDIT_ARCH_X86_64
# define WRAP_NR_openat2      437
# define WRAP_NR_faccessat2   439
# define WRAP_NR_clone3       435
# define WRAP_NR_ioctl         16
/* Legacy lp64-x86_64 RET_TRAP set (Android allows these only for lp32). */
# define WRAP_NR_access       21
# define WRAP_NR_open         2
# define WRAP_NR_chmod        90
# define WRAP_NR_chown        92
# define WRAP_NR_mkdir        83
# define WRAP_NR_rmdir        84
# define WRAP_NR_unlink       87
# define WRAP_NR_symlink      88
# define WRAP_NR_link         86
# define WRAP_NR_rename       82
# define WRAP_NR_readlink     89
# define WRAP_NR_stat         4
# define WRAP_NR_lstat        6
# define WRAP_NR_epoll_wait   232
#else
# error "unsupported arch"
#endif

#define BPF_S(_code, _k)            ((struct sock_filter){ (_code), 0, 0, (_k) })
#define BPF_J(_code, _k, _jt, _jf)  ((struct sock_filter){ (_code), (_jt), (_jf), (_k) })

static long sys_seccomp(unsigned int op, unsigned int flags, void *args)
{
	return syscall(SYS_seccomp, op, flags, args);
}

/* termios2 ioctl numbers (asm-generic, identical on x86_64 and aarch64). */
#define WRAP_TCGETS2   0x802C542AU
#define WRAP_TCSETS2   0x402C542BU
#define WRAP_TCSETSW2  0x402C542CU
#define WRAP_TCSETSF2  0x402C542DU

static bool install_filter(bool include_legacy_x86_64,
                           bool xperm_tcgets2)
{
	/* Cap mirrors the production trap_nrs[256] in main.c. The current
	 * trap set fits comfortably in 64 slots, but the array+guard pair
	 * is the cheap defense against silent truncation as the set
	 * grows — see the same pattern in main.c and rootfs_smoke.c. */
	int trap_nrs[64];
	const size_t trap_cap = sizeof trap_nrs / sizeof trap_nrs[0];
	size_t n = 0;

#define WRAP_PUSH(_nr) do {                                              \
		if (n >= trap_cap) {                                     \
			fprintf(stderr,                                  \
				"wrap: trap_nrs[%zu] overflow at %s\n",  \
				trap_cap, #_nr);                         \
			return false;                                    \
		}                                                        \
		trap_nrs[n++] = (_nr);                                   \
	} while (0)

	WRAP_PUSH(WRAP_NR_openat2);
	WRAP_PUSH(WRAP_NR_faccessat2);
	WRAP_PUSH(WRAP_NR_clone3);

#if defined(__x86_64__)
	if (include_legacy_x86_64) {
		WRAP_PUSH(WRAP_NR_access);
		WRAP_PUSH(WRAP_NR_open);
		WRAP_PUSH(WRAP_NR_chmod);
		WRAP_PUSH(WRAP_NR_chown);
		WRAP_PUSH(WRAP_NR_mkdir);
		WRAP_PUSH(WRAP_NR_rmdir);
		WRAP_PUSH(WRAP_NR_unlink);
		WRAP_PUSH(WRAP_NR_symlink);
		WRAP_PUSH(WRAP_NR_link);
		WRAP_PUSH(WRAP_NR_rename);
		WRAP_PUSH(WRAP_NR_readlink);
		WRAP_PUSH(WRAP_NR_stat);
		WRAP_PUSH(WRAP_NR_lstat);
		WRAP_PUSH(WRAP_NR_epoll_wait);
	}
#else
	(void)include_legacy_x86_64;
#endif
#undef WRAP_PUSH

	/* Prologue: arch != ours -> KILL_PROCESS. Then load nr at offset 0
	 * once and run a linear JEQ chain. This mimics Android's bionic
	 * allowlist generator (see SECCOMP_PRIVATE_ALLOWLIST_APP.TXT) but
	 * only for the syscalls we want to model.
	 *
	 *   load arch
	 *   jeq <ours>, +1, 0     ; if matches skip kill
	 *   ret KILL_PROCESS
	 *   load nr
	 *   [optional --xperm-tcgets2 block: ioctl + cmd in {TCGETS2,
	 *    TCSETS2, TCSETSW2, TCSETSF2} -> RET_ERRNO|EACCES, mimicking
	 *    Android's untrusted_app_all_devpts:chr_file ioctl xperm
	 *    whitelist that allows the legacy TCGETS family but not the
	 *    termios2 variants. Without this block tests run on the host
	 *    can't exercise the EACCES path our handler is supposed to
	 *    bypass — the kernel just lets TCGETS2 through.]
	 *   for each trap_nr:
	 *     jeq nr, 0, +1
	 *     ret TRAP
	 *   ret ALLOW
	 *
	 * Kernel BPF instruction limit is 4096; with trap_cap=64 plus
	 * the xperm block (~10 instructions) the worst case is ~145.
	 * prog[] is sized comfortably above that.
	 */
	struct sock_filter prog[256];
	size_t i = 0;
	prog[i++] = BPF_S(BPF_LD | BPF_W | BPF_ABS, 4);
	prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K, WRAP_AUDIT_ARCH, 1, 0);
	prog[i++] = BPF_S(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
	prog[i++] = BPF_S(BPF_LD | BPF_W | BPF_ABS, 0);

	if (xperm_tcgets2) {
		/* If nr != ioctl, skip the whole xperm block. The block is:
		 *   LD args[1] low half (offset 24)
		 *   JEQ TCGETS2,  jt -> RET_ERRNO   ; computed below
		 *   JEQ TCSETS2,  jt -> RET_ERRNO
		 *   JEQ TCSETSW2, jt -> RET_ERRNO
		 *   JEQ TCSETSF2, jt -> RET_ERRNO
		 *   LD nr (reload — trap loop expects A == nr)
		 *   ; fall through to trap loop
		 *   RET_ERRNO|EACCES        <-- target of the JEQ-true branches
		 *
		 * The fall-through reload-nr is needed because A is clobbered
		 * by the LD args[1] above; the trap loop reads A as the nr.
		 *
		 * Layout (relative offsets from the JEQ TCGETS2 instruction):
		 *   +0  JEQ TCGETS2   (instr A)
		 *   +1  JEQ TCSETS2
		 *   +2  JEQ TCSETSW2
		 *   +3  JEQ TCSETSF2
		 *   +4  LD nr (reload)
		 *   +5  RET ERRNO|EACCES   <-- jt target
		 * So jt offsets from each JEQ are 4, 3, 2, 1 respectively.
		 *
		 * The "if not ioctl, skip 7" jump skips: LD args, 4*JEQ,
		 * LD nr, RET_ERRNO = 7 instructions. Wait — we WANT to
		 * skip the LD nr too if not ioctl (since A still holds nr
		 * from before). So skip everything up to and including the
		 * RET_ERRNO. After the skip, we're at the start of the
		 * trap loop with A still holding nr. */
		/* Internal numbering of the 9-instruction xperm block:
		 *   0  JEQ ioctl, jt=0, jf=8       ; not-ioctl -> trap loop
		 *   1  LD args[1] low (offset 24)
		 *   2  JEQ TCGETS2,  jt=5, jf=0    ; match -> RET ERRNO at +8
		 *   3  JEQ TCSETS2,  jt=4
		 *   4  JEQ TCSETSW2, jt=3
		 *   5  JEQ TCSETSF2, jt=2
		 *   6  LD nr (reload — A was clobbered by LD args)
		 *   7  JA +1                       ; skip RET ERRNO on no-match
		 *   8  RET ERRNO|EACCES            ; only reached via xperm jt
		 *   9  (trap loop continues here)
		 *
		 * The JA at slot 7 is the difference-maker: without it, every
		 * non-matching ioctl falls into RET ERRNO and the wrapper
		 * EACCES'es every ioctl, not just the four termios2 cmds. */
		uint32_t eacces = SECCOMP_RET_ERRNO | (13U & SECCOMP_RET_DATA);
		prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
		                  (uint32_t)WRAP_NR_ioctl, 0, 8);
		prog[i++] = BPF_S(BPF_LD | BPF_W | BPF_ABS, 24);
		prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
		                  WRAP_TCGETS2,  5, 0);
		prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
		                  WRAP_TCSETS2,  4, 0);
		prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
		                  WRAP_TCSETSW2, 3, 0);
		prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
		                  WRAP_TCSETSF2, 2, 0);
		prog[i++] = BPF_S(BPF_LD | BPF_W | BPF_ABS, 0); /* reload nr */
		prog[i++] = BPF_S(BPF_JMP | BPF_JA, 1);
		prog[i++] = BPF_S(BPF_RET | BPF_K, eacces);
	}

	for (size_t t = 0; t < n; t++) {
		prog[i++] = BPF_J(BPF_JMP | BPF_JEQ | BPF_K,
		                  (uint32_t)trap_nrs[t], 0, 1);
		prog[i++] = BPF_S(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
	}
	prog[i++] = BPF_S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

	struct sock_fprog fprog = {
		.len = (unsigned short)i,
		.filter = prog,
	};
	if (sys_seccomp(SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
		fprintf(stderr, "wrap: seccomp(SET_MODE_FILTER) failed: %s\n",
		        strerror(errno));
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	bool include_legacy = false;
	bool xperm_tcgets2 = false;
	int i = 1;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "--include-legacy-x86_64") == 0) {
			include_legacy = true;
		} else if (strcmp(argv[i], "--xperm-tcgets2") == 0) {
			xperm_tcgets2 = true;
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else {
			break;
		}
	}
	if (i >= argc) {
		fprintf(stderr,
		        "usage: wrap [--include-legacy-x86_64] "
		        "[--xperm-tcgets2] [--] <child> [args...]\n");
		return 2;
	}

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		fprintf(stderr, "wrap: PR_SET_NO_NEW_PRIVS failed: %s\n",
		        strerror(errno));
		return 2;
	}
	if (!install_filter(include_legacy, xperm_tcgets2)) return 2;

	execv(argv[i], &argv[i]);
	/* execv only returns on failure. */
	fprintf(stderr, "wrap: execv(%s): %s\n", argv[i], strerror(errno));
	return 2;
}
