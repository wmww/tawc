/* See steps.h. */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <cleat/string_helpers.h>

#include <stc/cstr.h>
#include <stc/csview.h>

#include <stdlib.h>
#include <string.h>

#include "steps.h"

#ifndef TAWCROOT_TESTHOST_BIN
# error "TAWCROOT_TESTHOST_BIN must be passed via -D from tawcroot/Makefile"
#endif

/* Pass cases use a no-op impl (cleat treats a test that pushes zero
 * failures as passing). Fail cases use a custom impl that calls
 * test_fail_message with a strdup'd message. We use plain `char*` rather
 * than cleat's cstr for the test_data, so step ownership is a single
 * malloc / free boundary and we don't have to thread cstr move semantics
 * through cleat's TestData clone/drop machinery. */
static void noop_pass_impl(TestCtx *test_ctx, void const *test_data)
{
	(void)test_ctx;
	(void)test_data;
}

static void fail_with_message_impl(TestCtx *test_ctx, void const *test_data)
{
	const char *msg = test_data;
	test_fail_message("%s", msg);
}

static void free_str_drop(void *test_data)
{
	free(test_data);
}

/* Find the byte offset of `needle` in `sv`, or -1 if not found. */
static long find_in_sv(csview sv, const char *needle)
{
	isize n = csview_find(sv, needle);
	return n == c_NPOS ? -1 : (long)n;
}

/* Sanitize a step label into a valid cleat test name: collapse `::` (cleat
 * forbids it), strip leading/trailing whitespace. Returns owned cstr. */
static cstr sanitize_name(csview raw)
{
	csview trimmed = csview_trim_whitespace(raw);
	cstr out = cstr_init();
	for (size_t i = 0; i < trimmed.size; i++) {
		char c = trimmed.buf[i];
		if (c == ':' && i + 1 < trimmed.size && trimmed.buf[i + 1] == ':') {
			cstr_append_sv(&out, c_sv("__"));
			i++;
		} else {
			cstr_append_sv(&out, csview_from_n(&trimmed.buf[i], 1));
		}
	}
	return out;
}

/* Append `extra` to a malloc'd C string, growing as needed. *out is freed
 * and replaced with a fresh malloc on each call. Returns the new pointer.
 * Initial *out should be NULL or a malloc'd string. */
static char *append_str(char *out, csview extra)
{
	size_t old = out ? strlen(out) : 0;
	char *next = malloc(old + extra.size + 1);
	if (old) memcpy(next, out, old);
	memcpy(next + old, extra.buf, extra.size);
	next[old + extra.size] = '\0';
	free(out);
	return next;
}

void steps_register_from_testhost(csview module, const char *const *extra_args)
{
	steps_register_from_testhost_prefixed(module, NULL, extra_args);
}

void steps_register_from_testhost_prefixed(csview module,
                                           const char *const *prefix_argv,
                                           const char *const *extra_args)
{
	/* `VecStr` stores `const char*` (cstr_raw). Pushed C strings must
	 * outlive `run_subproc`, which they do: TAWCROOT_TESTHOST_BIN is a
	 * `-D` literal, prefix_argv / extra_args come from the caller's
	 * static / stack lifetime. No allocations needed. */
	VecStr cmd = c_init(vec_str, {});
	if (prefix_argv) {
		for (const char *const *a = prefix_argv; *a; a++) {
			vec_str_push(&cmd, *a);
		}
	}
	vec_str_push(&cmd, TAWCROOT_TESTHOST_BIN);
	if (extra_args) {
		for (const char *const *a = extra_args; *a; a++) {
			vec_str_push(&cmd, *a);
		}
	}

	cstr out, err;
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){
		.vec_cmd = cmd,
		.stdout = &out,
		.stderr = &err,
		.exit_code = &rc,
	});
	failable_result_drop(&res);

	int n_steps = 0;

	/* Walk lines once, finding every `[ok ]` / `[FAIL]` step. For each
	 * fail, accumulate the step line + the indented kv-context lines that
	 * follow it as a single failure message; flush by registering once we
	 * hit the next step or end of input. Passes register immediately and
	 * carry no message.
	 *
	 * Step + kv lines come on stderr — `tawc_io_str` / `tawc_io_dec` /
	 * `tawc_io_hex` all write to fd 2, so a single key/value pair lands
	 * on one stream. Stdout is unused by the testhost. */
	csview body = cstr_sv(&err);

	cstr      pending_name = cstr_init();   /* name for the in-progress fail */
	char     *pending_msg  = NULL;          /* malloc'd; ownership transfers
	                                         * to register_test on flush */
	bool      pending_active = false;

	#define FLUSH_PENDING() do { \
		if (pending_active) { \
			register_test(module, cstr_sv(&pending_name), \
			              fail_with_message_impl, free_str_drop, \
			              pending_msg); \
			pending_msg = NULL; \
			cstr_drop(&pending_name); \
			pending_name = cstr_init(); \
			pending_active = false; \
		} \
	} while (0)

	for (lines_sv_iter it = lines_sv_begin(body); it.ref; lines_sv_next(&it)) {
		csview line = it.line;

		long ok_at   = find_in_sv(line, "[ok ] ");
		long fail_at = find_in_sv(line, "[FAIL] ");
		long skip_at = find_in_sv(line, "[skip] ");

		if (ok_at >= 0 || fail_at >= 0 || skip_at >= 0) {
			FLUSH_PENDING();

			/* [skip] from the testhost (e.g. faccessat2 / close_range
			 * on kernel <5.8/5.9) means "this case is N/A on this
			 * platform." cleat doesn't expose a per-test skip API, so
			 * we register it as a passing test -- the suffix in the
			 * label keeps the reason visible in the test name. */
			bool passed = (ok_at >= 0 || skip_at >= 0);
			long marker;
			size_t skip;
			if (ok_at >= 0)        { marker = ok_at;   skip = sizeof("[ok ] ")   - 1; }
			else if (skip_at >= 0) { marker = skip_at; skip = sizeof("[skip] ")  - 1; }
			else                   { marker = fail_at; skip = sizeof("[FAIL] ")  - 1; }
			csview label = csview_slice(line, (size_t)marker + skip,
			                            line.size);

			/* Test name = `<prefix> <label>` -- prefix is whatever
			 * appeared on the line before the marker (e.g.
			 * `[parent]   ` / `  `). */
			csview prefix = csview_slice(line, 0, (size_t)marker);
			prefix = csview_trim_whitespace(prefix);

			cstr name = cstr_init();
			if (prefix.size) {
				cstr_append_sv(&name, prefix);
				cstr_append_sv(&name, c_sv(" "));
			}
			cstr_append_sv(&name, label);
			cstr safe = sanitize_name(cstr_sv(&name));
			cstr_drop(&name);

			if (passed) {
				register_test(module, cstr_sv(&safe),
				              noop_pass_impl, NULL, NULL);
				cstr_drop(&safe);
			} else {
				/* Defer registration so we can collect the
				 * trailing kv-context as part of the failure
				 * message. */
				pending_name = safe;
				pending_msg = append_str(NULL, line);
				pending_active = true;
			}
			n_steps++;
			continue;
		}

		/* Non-step line: if a fail is pending and this line is
		 * indented (kv-format), append it to the pending message.
		 * Otherwise the step's context ends here. */
		if (pending_active) {
			bool indented = (line.size > 0 &&
			                 (line.buf[0] == ' ' || line.buf[0] == '\t'));
			if (!indented) {
				FLUSH_PENDING();
			} else {
				pending_msg = append_str(pending_msg, c_sv("\n"));
				pending_msg = append_str(pending_msg, line);
			}
		}
	}
	FLUSH_PENDING();
	#undef FLUSH_PENDING

	/* Process-level gates. Run regardless of whether per-step lines were
	 * parsed -- we want a clean failure if the testhost crashed (e.g.,
	 * SIGSEGV under -fsanitize, or build-mismatch). */
	if (n_steps == 0) {
		register_test_problem(
			module, c_sv("testhost_produced_no_steps"),
			cstr_from_fmt(
				"testhost ran (rc=%d) but produced zero "
				"[ok ]/[FAIL] lines.\nstderr:\n%s\nstdout:\n%s\n",
				rc, cstr_str(&err), cstr_str(&out)));
	} else if (rc != 0) {
		register_test_problem(
			module, c_sv("testhost_exit_status"),
			cstr_from_fmt(
				"testhost exited with status %d (per-step "
				"results may still be accurate).\nstderr:\n%s\n",
				rc, cstr_str(&err)));
	}
	if (cstr_size(&out) > 0) {
		register_test_problem(
			module, c_sv("testhost_stdout_was_nonempty"),
			cstr_from_fmt(
				"testhost wrote %zu bytes to stdout (smokes "
				"route trace + values to stderr; stdout should "
				"be empty):\n%s",
				cstr_size(&out), cstr_str(&out)));
	}

	cstr_drop(&out);
	cstr_drop(&err);
	cstr_drop(&pending_name);
	free(pending_msg);
	/* `cmd` is intentionally NOT dropped -- `run_subproc` takes ownership
	 * and drops it internally (cleat/src/subproc.c:193). Likewise the
	 * `out`/`err` cstrs were *assigned into* via the SubprocArgs pointers,
	 * so we own and drop them above. */
}
