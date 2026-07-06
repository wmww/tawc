/* `--exec-child` re-entry point.
 *
 * The handler's `execveat`-into-self lands here. Our seccomp filter is
 * inherited (kernel state); the SIGSYS handler is reset to SIG_DFL by
 * the kernel across `execve`, so we must reinstall it before any TRAPed
 * syscall fires. Until then we issue every syscall through
 * `tawcroot_raw_syscall()`, which the inherited filter ALLOWs by IP
 * (this works because we link non-PIE -- the stub is at the same
 * link-time-fixed address in the new image).
 *
 * Foundation-smoke child: read the smoke state-fd, reinstall handler, re-run the
 * trap-contract assertion, exit.
 *
 * Production `--exec-child` uses the loader path in main.c; this file is
 * the testhost-only smoke child.
 *
 * See notes/tawcroot/seccomp-filter.md "Approach A: re-exec into ourselves first" and
 * "Why non-PIE".
 */

#include <stddef.h>
#include <stdint.h>

#include "child.h"
#include "smoke.h"
#include "io.h"
#include "raw_sys.h"
#include "handler.h"
#include "filter.h"

extern char tawcroot_raw_syscall_ret[];

int tawcroot_child_main(int argc, char **argv)
{
	int fails = 0;

	int state_fd = -1;
	for (int i = 1; i < argc; i++) {
		if (tawc_starts_with(argv[i], "--state-fd=")) {
			state_fd = (int)tawc_parse_long(argv[i] + 11);
		}
	}

	long inst = tawcroot_install_handler();

	tawc_io_str("\ntawcroot foundation smoke (--exec-child)\n");
	tawc_io_kv_hex("stub_ret_addr",
		       (unsigned long)(uintptr_t)&tawcroot_raw_syscall_ret[0]);
	tawc_io_kv_dec("state_fd", state_fd);
	fails += tawc_io_step("reinstall SIGSYS handler", inst == 0);

	struct tawcroot_smoke_state st;
	long r = tawc_read(state_fd, &st, sizeof st);
	fails += tawc_io_step("read exec-state from inherited fd",
			      r == (long)sizeof st);
	if (r == (long)sizeof st) {
		fails += tawc_io_step("magic matches",
				      st.magic == TAWC_SMOKE_MAGIC);
		tawc_io_kv_hex("    magic",                (unsigned long)st.magic);
		tawc_io_kv_dec("    parent_pid",           (long)st.parent_pid);
		tawc_io_kv_dec("    parent_handler_calls", (long)st.parent_handler_calls);
		tawc_io_kv_dec("    round",                (long)st.round);
	}
	tawc_close(state_fd);

	fails += tawcroot_smoke_trap_contract("[child] ");

	long my_pid = tawc_getpid();
	fails += tawc_io_step("self pid == parent_pid (execveat preserves pid)",
			      my_pid == (long)st.parent_pid);
	tawc_io_kv_dec("    my_pid (stub)", my_pid);

	if (fails == 0) {
		tawc_io_str("CHILD SMOKE: PASS\n");
	} else {
		tawc_io_str("CHILD SMOKE: FAIL (");
		tawc_io_dec(fails);
		tawc_io_str(" failure(s))\n");
	}
	return fails == 0 ? 0 : 1;
}
