/* Foundation-smoke routines extracted from main.c.
 *
 * These exercise the BPF IP allowlist + handler dispatch contract without
 * ever loading a guest. We keep them around as regression tests
 * (the IP-allowlist contract is the spine of the whole architecture, and
 * the smoke is cheap to keep running on every binary).
 *
 * See notes/tawcroot/sigsys-handler.md "Issuing host syscalls from the handler" and
 * "Why the handler is async-signal-safe".
 */

#include <stddef.h>
#include <stdint.h>

#include "smoke.h"
#include "io.h"
#include "raw_sys.h"
#include "handler.h"

extern char tawcroot_raw_syscall_insn[];
extern char tawcroot_raw_syscall_ret[];

static long getpid_via_stub(void)
{
	return tawc_getpid();
}

static long getpid_via_inline(void)
{
	long rv;
#if defined(__x86_64__)
	__asm__ __volatile__ (
		"mov $39, %%rax    \n\t"   /* SYS_getpid */
		"syscall           \n\t"
		: "=a"(rv)
		: : "rcx", "r11", "memory");
#elif defined(__aarch64__)
	register long x8 __asm__("x8") = TAWC_SYS_getpid;
	register long x0 __asm__("x0");
	__asm__ __volatile__ (
		"svc #0            \n\t"
		: "=r"(x0)
		: "r"(x8)
		: "memory");
	rv = x0;
#else
# error "unsupported arch"
#endif
	return rv;
}

int tawcroot_smoke_trap_contract(const char *label_prefix)
{
	int fails = 0;
	tawcroot_handler_obs before, after;

	tawcroot_handler_observe(&before);
	long pid_stub = getpid_via_stub();
	tawcroot_handler_observe(&after);

	int stub_ok = (pid_stub > 0) && (after.calls == before.calls);
	tawc_io_str(label_prefix);
	fails += tawc_io_step("stub call: ALLOW (no trap)", stub_ok);
	tawc_io_kv_dec("    pid_stub", pid_stub);
	tawc_io_kv_dec("    handler_calls_delta",
		       (long)(after.calls - before.calls));

	tawcroot_handler_observe(&before);
	long pid_inline = getpid_via_inline();
	tawcroot_handler_observe(&after);

	int inline_ok =
		(pid_inline == -38) &&
		(after.calls == before.calls + 1) &&
		(after.last_nr == TAWC_SYS_getpid);
	tawc_io_str(label_prefix);
	fails += tawc_io_step("inline call: TRAP -> handler -> -ENOSYS",
			      inline_ok);
	tawc_io_kv_dec("    pid_inline",          pid_inline);
	tawc_io_kv_dec("    handler_calls_delta",
		       (long)(after.calls - before.calls));
	tawc_io_kv_dec("    last_nr",             after.last_nr);
	tawc_io_kv_hex("    last_call_addr",
		       (unsigned long)after.last_call_addr);

	return fails;
}

static int exercise_one(const char *name, long rv,
			tawcroot_handler_obs *snap)
{
	tawcroot_handler_obs after;
	tawcroot_handler_observe(&after);
	int trapped = (after.calls != snap->calls);
	int ok = !trapped;
	/* Use tawc_io_step + a separate kv-line for `rv` so the [ok ]/[FAIL]
	 * label is stable across runs (no embedded runtime value) -- the
	 * cleat-side runner uses that line as the test name. */
	int fails = tawc_io_step(name, ok);
	tawc_io_kv_dec("    rv", rv);
	if (trapped) tawc_io_str("    (handler caught it!)\n");
	*snap = after;
	return fails;
}

int tawcroot_smoke_exercise_raw(void)
{
	tawc_io_str("[exercise] raw syscalls via stub\n");
	int fails = 0;
	tawcroot_handler_obs snap;
	tawcroot_handler_observe(&snap);

	long rv;

	rv = tawc_getuid();
	fails += exercise_one("getuid",         rv, &snap);

	{
		char buf[256];
		rv = tawc_readlinkat(-100, "/proc/self/exe", buf, sizeof buf);
		fails += exercise_one("readlinkat(/proc/self/exe)", rv, &snap);
	}

	{
		long fd = tawc_openat(-100, "/proc/self/exe", 0, 0);
		fails += exercise_one("openat(/proc/self/exe)", fd, &snap);
		if (fd >= 0) {
			rv = tawc_close((int)fd);
			fails += exercise_one("close",          rv, &snap);
		}
	}

	{
		char buf[256];
		rv = TAWC_RAW(TAWC_SYS_getcwd, (long)buf, (long)sizeof buf,
			      0, 0, 0, 0);
		fails += exercise_one("getcwd",         rv, &snap);
	}

	rv = tawc_fcntl(1, 1 /*F_GETFD*/, 0);
	fails += exercise_one("fcntl(F_GETFD,1)",  rv, &snap);

	{
		const long PROT_RW = 0x1 | 0x2;
		const long MAP_ANON_PRIV = 0x20 | 0x02;
		long p = TAWC_RAW(TAWC_SYS_mmap, 0, 4096, PROT_RW,
				  MAP_ANON_PRIV, -1, 0);
		fails += exercise_one("mmap(anon, 4K)", p, &snap);
		if (p > 0) {
			((volatile char *)p)[0] = 'x';
			rv = TAWC_RAW(TAWC_SYS_mprotect, p, 4096,
				      0x1 /*PROT_READ*/, 0, 0, 0);
			fails += exercise_one("mprotect(R)", rv, &snap);
			rv = TAWC_RAW(TAWC_SYS_munmap, p, 4096, 0, 0, 0, 0);
			fails += exercise_one("munmap",     rv, &snap);
		}
	}

	{
		uint8_t stbuf[256];
		rv = TAWC_RAW(TAWC_SYS_fstatat, -100, (long)"/proc/self/exe",
			      (long)stbuf, 0, 0, 0);
		fails += exercise_one("fstatat(/proc/self/exe)", rv, &snap);
	}

	{
		uint8_t sxbuf[256];
		rv = TAWC_RAW(TAWC_SYS_statx, -100, (long)"/proc/self/exe",
			      0, 0x7ff /*STATX_BASIC_STATS*/, (long)sxbuf, 0);
		fails += exercise_one("statx(/proc/self/exe)", rv, &snap);
	}

	{
		long mfd = tawc_memfd_create("tawcroot-exercise", 0x0001);
		fails += exercise_one("memfd_create", mfd, &snap);
		if (mfd >= 0) {
			rv = tawc_close((int)mfd);
			fails += exercise_one("close(memfd)", rv, &snap);
		}
	}

	{
		uint8_t rnd[16];
		rv = TAWC_RAW(TAWC_SYS_getrandom, (long)rnd, sizeof rnd,
			      0, 0, 0, 0);
		fails += exercise_one("getrandom(16)", rv, &snap);
	}

	rv = TAWC_RAW(TAWC_SYS_brk, 0, 0, 0, 0, 0, 0);
	fails += exercise_one("brk(0)",         rv, &snap);

	rv = tawc_lseek(1, 0, 1 /*SEEK_CUR*/);
	fails += exercise_one("lseek(stdout)",  rv, &snap);

	return fails;
}
