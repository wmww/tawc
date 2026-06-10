/* Unit tests for the pure string / memory helpers in tawcroot/src/strings.c.
 *
 * The helpers are libc-free (the production binary links `-nostdlib`).
 * `strings.c` is compiled twice: once with the freestanding flag set for
 * the production / testhost binaries, and once with hosted glibc for this
 * test runner. Both share a single source of truth for what the helpers
 * should do; cleat asserts behavior on a concrete set of inputs.
 *
 * The mem* helpers (memcpy/memset/memmove/memcmp/strlen) live in strings.c
 * too because the compiler implicitly emits calls to them under `-O2
 * -nostdlib` for struct copies / `= 0` field stores. They're identity-named
 * with libc's versions, so under hosted (`__STDC_HOSTED__`) builds strings.c
 * compiles them out and we use glibc's. mem* coverage in the freestanding
 * build comes from the smoke driver exercising the codegen they're emitted
 * for.
 */

#include <cleat/test.h>
#include <stdint.h>

#include "io.h"

test(tawc_strlen_basics)
{
	test_int_eq(tawc_strlen(""), 0);
	test_int_eq(tawc_strlen("a"), 1);
	test_int_eq(tawc_strlen("hello"), 5);
	test_int_eq(tawc_strlen("hello world"), 11);
	/* Embedded NULs terminate. */
	test_int_eq(tawc_strlen("foo\0bar"), 3);
}

test(tawc_streq_basics)
{
	test_int_eq(tawc_streq("", ""), 1);
	test_int_eq(tawc_streq("a", "a"), 1);
	test_int_eq(tawc_streq("hello", "hello"), 1);
	test_int_eq(tawc_streq("hello", "hellp"), 0);
	test_int_eq(tawc_streq("hello", "hell"), 0);
	test_int_eq(tawc_streq("hell", "hello"), 0);
	test_int_eq(tawc_streq("a", ""), 0);
	test_int_eq(tawc_streq("", "a"), 0);
	/* Case-sensitive. */
	test_int_eq(tawc_streq("Hello", "hello"), 0);
}

test(tawc_starts_with_basics)
{
	test_int_eq(tawc_starts_with("hello", ""), 1);
	test_int_eq(tawc_starts_with("hello", "h"), 1);
	test_int_eq(tawc_starts_with("hello", "hell"), 1);
	test_int_eq(tawc_starts_with("hello", "hello"), 1);
	test_int_eq(tawc_starts_with("hello", "hellz"), 0);
	test_int_eq(tawc_starts_with("hello", "hello!"), 0);
	test_int_eq(tawc_starts_with("", ""), 1);
	test_int_eq(tawc_starts_with("", "x"), 0);
	/* Real arg-parse case from main.c / testhost_main.c. */
	test_int_eq(tawc_starts_with("--state-fd=42", "--state-fd="), 1);
	test_int_eq(tawc_starts_with("--state-fd=42", "--state-fd=42"), 1);
	test_int_eq(tawc_starts_with("--state-fd=", "--state-fd=4"), 0);
}

test(tawc_parse_long_basics)
{
	test_int_eq(tawc_parse_long("0"), 0);
	test_int_eq(tawc_parse_long("1"), 1);
	test_int_eq(tawc_parse_long("42"), 42);
	test_int_eq(tawc_parse_long("12345"), 12345);
	/* Negative. */
	test_int_eq(tawc_parse_long("-1"), -1);
	test_int_eq(tawc_parse_long("-42"), -42);
	/* Stops at first non-digit (mirrors what child.c does on `--state-fd=N`
	 * after a `tawc_starts_with` check has already validated the prefix). */
	test_int_eq(tawc_parse_long("42abc"), 42);
	test_int_eq(tawc_parse_long(""), 0);
	/* Whitespace is NOT skipped -- caller must hand us a clean number. */
	test_int_eq(tawc_parse_long("  42"), 0);
}

test(tawc_int_to_str_basics)
{
	char buf[32];
	int n;

	n = tawc_int_to_str(buf, sizeof buf, 0);
	test_int_eq(n, 1);
	test_str_eq(buf, "0");

	n = tawc_int_to_str(buf, sizeof buf, 1);
	test_int_eq(n, 1);
	test_str_eq(buf, "1");

	n = tawc_int_to_str(buf, sizeof buf, 42);
	test_int_eq(n, 2);
	test_str_eq(buf, "42");

	n = tawc_int_to_str(buf, sizeof buf, -1);
	test_int_eq(n, 2);
	test_str_eq(buf, "-1");

	n = tawc_int_to_str(buf, sizeof buf, -42);
	test_int_eq(n, 3);
	test_str_eq(buf, "-42");

	n = tawc_int_to_str(buf, sizeof buf, 2147483647);  /* INT_MAX */
	test_str_eq(buf, "2147483647");

	n = tawc_int_to_str(buf, sizeof buf, -2147483647);
	test_str_eq(buf, "-2147483647");

	n = tawc_int_to_str(buf, sizeof buf, INT32_MIN); /* negation must not overflow */
	test_str_eq(buf, "-2147483648");
}

test(tawc_int_to_str_buffer_too_small)
{
	char buf[4];

	/* buflen == 0 returns 0 without touching buf. We can't directly verify
	 * "untouched" without a sentinel, so just check the documented return. */
	int n = tawc_int_to_str(buf, 0, 42);
	test_int_eq(n, 0);

	/* Tight fit: "42" + NUL takes 3 bytes. */
	buf[0] = buf[1] = buf[2] = buf[3] = 'X';
	n = tawc_int_to_str(buf, 3, 42);
	test_str_eq(buf, "42");
	test_int_eq(n, 2);

	/* Overflow: digits get truncated rather than written past the end --
	 * the writer reserves a slot for the trailing NUL. (Documenting
	 * current behavior; callers in tawcroot give plenty of room.) */
	buf[0] = buf[1] = buf[2] = buf[3] = 'X';
	n = tawc_int_to_str(buf, 3, 12345);
	test_int_eq(buf[3], 'X');                  /* never wrote past the end */
	test_int_eq(buf[(int)tawc_strlen(buf)], 0); /* always NUL-terminated */
}

test(tawc_int_to_str_round_trip_with_parse_long)
{
	char buf[32];
	int values[] = { 0, 1, -1, 42, -42, 12345, -12345, 2147483647, -2147483647 };
	for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
		tawc_int_to_str(buf, sizeof buf, values[i]);
		long parsed = tawc_parse_long(buf);
		test_int_eq(parsed, (long)values[i]);
	}
}

test(tawc_str_append_basics)
{
	char buf[16];
	size_t pos = 0;
	test_int_eq(tawc_str_append(buf, sizeof buf, &pos, "foo"), 0);
	test_int_eq(tawc_str_append(buf, sizeof buf, &pos, "/bar"), 0);
	test_int_eq(pos, 7);
	test_true(tawc_streq(buf, "foo/bar"));
	/* Empty append is a no-op that still terminates. */
	test_int_eq(tawc_str_append(buf, sizeof buf, &pos, ""), 0);
	test_int_eq(pos, 7);
}

test(tawc_str_append_overflow_drops_whole_append)
{
	char buf[8];
	size_t pos = 0;
	test_int_eq(tawc_str_append(buf, sizeof buf, &pos, "abcd"), 0);
	long e = tawc_str_append(buf, sizeof buf, &pos, "efgh");
	test_true(e < 0);
	test_int_eq(pos, 4);                /* pos unchanged */
	test_true(tawc_streq(buf, "abcd")); /* re-terminated at old pos */
}

test(tawc_str_append_dec_basics)
{
	char buf[32];
	size_t pos = 0;
	test_int_eq(tawc_str_append_dec(buf, sizeof buf, &pos, 0), 0);
	test_int_eq(tawc_str_append_dec(buf, sizeof buf, &pos, -42), 0);
	test_true(tawc_streq(buf, "0-42"));
}

test(tawc_str_copy_basics)
{
	char buf[8];
	test_int_eq(tawc_str_copy(buf, sizeof buf, "hello"), 5);
	test_true(tawc_streq(buf, "hello"));
	test_true(tawc_str_copy(buf, sizeof buf, "too-long-for-8") < 0);
	test_true(tawc_streq(buf, ""));   /* left empty on overflow */
}

test(tawc_long_to_str_basics)
{
	char buf[32];
	test_int_eq(tawc_long_to_str(buf, sizeof buf, 0), 1);
	test_true(tawc_streq(buf, "0"));
	tawc_long_to_str(buf, sizeof buf, -9223372036854775807L - 1);
	test_true(tawc_streq(buf, "-9223372036854775808"));
	long vals[] = { 1, -1, 4096, -4096, 9223372036854775807L };
	for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
		tawc_long_to_str(buf, sizeof buf, vals[i]);
		test_int_eq(tawc_parse_long(buf), vals[i]);
	}
}

test(tawc_proc_fd_path_basics)
{
	char buf[64];
	test_int_eq(tawc_proc_fd_path(buf, sizeof buf, 17, 0), 16);
	test_true(tawc_streq(buf, "/proc/self/fd/17"));
	test_int_eq(tawc_proc_fd_path(buf, sizeof buf, 17, ""), 16);
	test_true(tawc_streq(buf, "/proc/self/fd/17"));
	test_int_eq(tawc_proc_fd_path(buf, sizeof buf, 3, "etc/hosts"), 25);
	test_true(tawc_streq(buf, "/proc/self/fd/3/etc/hosts"));
	test_true(tawc_proc_fd_path(buf, 8, 3, 0) < 0);
}
