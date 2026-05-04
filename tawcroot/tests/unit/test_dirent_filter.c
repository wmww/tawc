/* Unit tests for the getdents64 reserved-fd filter
 * (tawcroot/src/dirent_filter.c).
 *
 * Background: the in-handler getdents64 trap exists because glibc's
 * __closefrom_fallback opens /proc/self/fd, getdents64-iterates, and
 * close()s every fd >= start_fd, retrying via lseek(0)+getdents64
 * any pass that closed at least one fd. tawcroot's handle_close lies
 * about closing reserved fds (1000+) so they survive the guest's
 * closefrom — but if the getdents64 stream still mentions them, the
 * "close, retry, close, retry" loop never terminates. The filter
 * compacts those entries out of the dirent buffer when the dirfd
 * resolves to /proc/self/fd or /proc/<pid>/fd. Pacman/gpgme under
 * the in-app installer hangs at 100% CPU without this filter.
 *
 * Pure-function helpers tested here:
 *   - tawcroot_dirent_filter_is_proc_fd_link: gate predicate
 *   - tawcroot_dirent_filter_dname_is_reserved: per-entry predicate
 *   - tawcroot_dirent_filter_compact: in-place buffer compaction
 *
 * The handler-side glue (readlinkat probe, real getdents64 issue)
 * lives in syscalls_fd.c and isn't tested here — it's just the
 * connect-the-pure-functions layer.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dirent_filter.h"

/* --- is_proc_fd_link ------------------------------------------------ */

test(is_proc_fd_link_self_fd_matches)
{
	const char s[] = "/proc/self/fd";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 1);
}

test(is_proc_fd_link_pid_fd_matches)
{
	const char s[] = "/proc/12345/fd";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 1);
}

test(is_proc_fd_link_single_digit_pid_matches)
{
	const char s[] = "/proc/1/fd";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 1);
}

test(is_proc_fd_link_self_alone_does_not_match)
{
	const char s[] = "/proc/self";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_self_fd_extra_path_does_not_match)
{
	/* /proc/self/fd/3 names a specific fd, not the directory. */
	const char s[] = "/proc/self/fd/3";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_pid_fdinfo_does_not_match)
{
	const char s[] = "/proc/123/fdinfo";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_self_status_does_not_match)
{
	const char s[] = "/proc/self/status";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_non_proc_does_not_match)
{
	const char s[] = "/etc/passwd";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_proc_alone_does_not_match)
{
	const char s[] = "/proc";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_truncated_does_not_match)
{
	const char s[] = "/proc/self/f";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

test(is_proc_fd_link_zero_length_does_not_match)
{
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link("", 0), 0);
}

test(is_proc_fd_link_pid_with_letters_does_not_match)
{
	/* Pids are pure digits; /proc/12a3/fd shouldn't match. */
	const char s[] = "/proc/12a3/fd";
	test_int_eq(tawcroot_dirent_filter_is_proc_fd_link(s, sizeof s - 1), 0);
}

/* --- dname_is_reserved ---------------------------------------------- */

test(dname_is_reserved_match)
{
	int reserved[] = {1000, 1001, 1002};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("1001", reserved, 3), 1);
}

test(dname_is_reserved_no_match)
{
	int reserved[] = {1000, 1001, 1002};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("3", reserved, 3), 0);
}

test(dname_is_reserved_empty_name)
{
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("", reserved, 1), 0);
}

test(dname_is_reserved_non_digit_rejected)
{
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved(".", reserved, 1), 0);
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("..", reserved, 1), 0);
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("1000a", reserved, 1), 0);
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("a1000", reserved, 1), 0);
}

test(dname_is_reserved_no_leading_sign)
{
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("+1000", reserved, 1), 0);
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("-1000", reserved, 1), 0);
}

test(dname_is_reserved_empty_set)
{
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved("1000", NULL, 0), 0);
}

test(dname_is_reserved_overflow_safe)
{
	/* Astronomical numeric name shouldn't crash; should return 0.
	 * Regression for signed-overflow UB in the accumulator: the
	 * walk must cap before any *10 wraps. */
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved(
	    "999999999999999999999", reserved, 1), 0);
}

test(dname_is_reserved_just_above_int_max)
{
	/* INT_MAX = 2147483647. "2147483648" is one past — must not
	 * match, must not crash, and the multiply leading up to it
	 * (214748364 * 10) must not have wrapped int. */
	int reserved[] = {2147483647};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved(
	    "2147483648", reserved, 1), 0);
}

test(dname_is_reserved_int_max_matches)
{
	int reserved[] = {0x7fffffff};
	test_int_eq(tawcroot_dirent_filter_dname_is_reserved(
	    "2147483647", reserved, 1), 1);
}

/* --- compact: build a synthetic linux_dirent64 buffer and verify ---- */

/* Match the layout in dirent_filter.c. */
#define DIRENT64_NAME_OFF    19

/* Reclen must be a multiple of 8 (kernel guarantees). Emit a single
 * dirent into `buf+off`, return new offset. */
static long emit_dirent(unsigned char *buf, long off, const char *name)
{
	size_t name_len = strlen(name);
	/* d_ino(8) + d_off(8) + d_reclen(2) + d_type(1) + name + NUL,
	 * rounded up to multiple of 8. */
	size_t needed = DIRENT64_NAME_OFF + name_len + 1;
	size_t reclen = (needed + 7) & ~(size_t)7;
	memset(buf + off, 0, reclen);
	uint64_t ino = 1; uint64_t doff = (uint64_t)off + reclen;
	memcpy(buf + off + 0, &ino, 8);
	memcpy(buf + off + 8, &doff, 8);
	uint16_t r = (uint16_t)reclen;
	memcpy(buf + off + 16, &r, 2);
	buf[off + 18] = 4 /*DT_DIR*/;
	memcpy(buf + off + DIRENT64_NAME_OFF, name, name_len);
	/* terminating NUL already from memset */
	return off + (long)reclen;
}

/* Walk a linux_dirent64 buffer of length n, copy d_names into out
 * (NUL-separated, double-NUL terminated). Caller-allocated. */
static void collect_names(const unsigned char *buf, long n, char *out)
{
	long i = 0;
	long o = 0;
	while (i < n) {
		uint16_t reclen;
		memcpy(&reclen, buf + i + 16, 2);
		const char *name = (const char *)(buf + i + DIRENT64_NAME_OFF);
		size_t len = strlen(name);
		memcpy(out + o, name, len + 1);
		o += (long)len + 1;
		i += reclen;
	}
	out[o] = 0;
}

test(compact_drops_reserved_keeps_others)
{
	unsigned char buf[512];
	long n = 0;
	n = emit_dirent(buf, n, ".");
	n = emit_dirent(buf, n, "..");
	n = emit_dirent(buf, n, "0");
	n = emit_dirent(buf, n, "1000"); /* drop */
	n = emit_dirent(buf, n, "5");
	n = emit_dirent(buf, n, "1003"); /* drop */

	int reserved[] = {1000, 1001, 1002, 1003};
	long out_n = tawcroot_dirent_filter_compact(buf, n, reserved, 4);
	test_true(out_n < n);

	char names[256];
	collect_names(buf, out_n, names);

	/* Expect: ., .., 0, 5 (1000 and 1003 gone). */
	test_str_eq(names + 0, ".");
	test_str_eq(names + 2, "..");
	test_str_eq(names + 5, "0");
	test_str_eq(names + 7, "5");
}

test(compact_no_reserved_present_is_identity)
{
	unsigned char buf[256];
	long n = 0;
	n = emit_dirent(buf, n, ".");
	n = emit_dirent(buf, n, "..");
	n = emit_dirent(buf, n, "3");
	n = emit_dirent(buf, n, "9");
	long n_before = n;

	int reserved[] = {1000, 1001};
	long out_n = tawcroot_dirent_filter_compact(buf, n, reserved, 2);
	test_int_eq(out_n, n_before);
}

test(compact_all_reserved_returns_zero)
{
	unsigned char buf[256];
	long n = 0;
	n = emit_dirent(buf, n, "1000");
	n = emit_dirent(buf, n, "1001");

	int reserved[] = {1000, 1001};
	long out_n = tawcroot_dirent_filter_compact(buf, n, reserved, 2);
	test_int_eq(out_n, 0);
}

test(compact_empty_buffer_returns_zero)
{
	unsigned char buf[16];
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_compact(buf, 0, reserved, 1), 0);
}

test(compact_null_buffer_is_identity)
{
	/* NULL buffer with positive length is unsafe to walk; the
	 * function bails out with `n` unchanged. Defensive — the handler
	 * never calls compact() with NULL since real getdents64 returns
	 * the user's buffer. */
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_compact(NULL, 100, reserved, 1), 100);
}

test(compact_no_reserved_set_is_identity)
{
	unsigned char buf[256];
	long n = 0;
	n = emit_dirent(buf, n, "1000");
	n = emit_dirent(buf, n, "1001");
	long n_before = n;

	long out_n = tawcroot_dirent_filter_compact(buf, n, NULL, 0);
	test_int_eq(out_n, n_before);
}

test(compact_malformed_reclen_zero_bails)
{
	/* Zero reclen → infinite loop trap; function should bail and
	 * return original length unchanged. */
	unsigned char buf[64];
	memset(buf, 0, sizeof buf);
	/* d_reclen at offset 16 = 0 (already from memset). */
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_compact(buf, 32, reserved, 1), 32);
}

test(compact_malformed_reclen_overshoots_bails)
{
	unsigned char buf[64];
	memset(buf, 0, sizeof buf);
	uint16_t huge = 200;  /* > buffer length */
	memcpy(buf + 16, &huge, 2);
	int reserved[] = {1000};
	test_int_eq(tawcroot_dirent_filter_compact(buf, 32, reserved, 1), 32);
}
