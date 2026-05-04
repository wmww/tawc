/* Async-signal-safety check for the SIGSYS handler hot path.
 *
 * The handler runs in signal context and must be async-signal-safe:
 * no `malloc`, no `printf`, no locale lookups, no dynamic-linker
 * re-entry. Today we keep that property by convention — handler.c,
 * dispatch.c, and the per-syscall handlers only call hand-rolled
 * `tawc_*` helpers (which issue raw syscalls and never touch libc).
 *
 * This test enforces it at build time. It runs `nm` against the
 * production-build .o files for the handler set, parses out their
 * external (undefined) symbol references, and fails if any match a
 * known-banned name.
 *
 * Production already links `-static -nostdlib -nostartfiles`, so a
 * stray `printf` would also fail the link. The value-add here is a
 * pointed error that names the offending file + symbol BEFORE the
 * cascade of "undefined reference to printf" linker errors, and that
 * also catches deliberate-but-misguided introductions like inviting
 * a libc symbol via an unintended header include.
 *
 * Skip behavior: the .o paths are baked in at host-build time; on
 * `--device` runs the host paths don't exist on-device, so we skip
 * cleanly with a passing result and a "skip" message. The host build
 * always exercises this.
 */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef TAWCROOT_PROD_OBJ_DIR
# error "TAWCROOT_PROD_OBJ_DIR must be defined by the build"
#endif

/* Banned symbols. Anything from libc that is async-signal-unsafe, plus
 * a couple of allocator and stdio entry points the compiler might emit
 * implicitly. Production link would already flag these as unresolved
 * (we ship no libc), but this list is the canonical "must not touch"
 * set so the test message is unambiguous when it triggers.
 *
 * Drawn from POSIX's "async-signal-safe functions" allowlist by
 * inversion — these are the common entry points OUTSIDE that list a
 * maintainer might plausibly grab while editing the handler set. */
static const char *const banned[] = {
	/* Allocators. */
	"malloc", "free", "calloc", "realloc", "posix_memalign",
	"aligned_alloc",
	/* stdio. */
	"printf", "fprintf", "vfprintf", "sprintf", "snprintf",
	"dprintf", "vsnprintf", "puts", "fputs", "fopen", "fclose",
	/* fork+exec. */
	"system", "popen",
	/* Termination paths that walk atexit handlers / locale. */
	"abort", "__assert_fail", "exit",
	/* Environment access — non-atomic. */
	"getenv", "setenv", "unsetenv", "putenv", "clearenv",
	/* Locale state. */
	"localeconv", "setlocale", "nl_langinfo",
	/* errno-stringifiers reach into locale. */
	"strerror", "strerror_r", "perror",
	/* Dynamic linking. */
	"dlopen", "dlsym", "dlclose",
	/* Non-local jumps that aren't ASS (only siglongjmp is). */
	"setjmp", "longjmp", "_setjmp", "_longjmp",
	/* Threads — almost nothing in the pthread surface is ASS. */
	"pthread_create", "pthread_join",
	"pthread_mutex_lock", "pthread_mutex_unlock",
	/* glibc startup / hosted entry points the linker shouldn't drag. */
	"__libc_start_main", "__libc_init",
};

/* Files that participate in the SIGSYS dispatch path. handler.c is the
 * top of the hot path; the rest are reachable from signal context via
 * the dispatch table. exec_handler.c runs from the execve interception
 * branch, also signal-context. */
static const char *const handler_objs[] = {
	"handler.c.o",
	"dispatch.c.o",
	"syscalls_fs.c.o",
	"syscalls_fd.c.o",
	"syscalls_control.c.o",
	"syscalls_socket.c.o",
	"syscalls_exec.c.o",
	"exec_handler.c.o",
	/* Reached from handle_openat → open_proc_maps_shadow. Pure (no
	 * external symbols today), but keeping it on the list catches a
	 * future regression that pulls in libc. */
	"proc_rewrite.c.o",
};

/* Run `nm -u --format=just-symbols <obj>` and capture its stdout.
 * Sets `*ok` to 1 on success, 0 on failure (caller must report). On
 * failure, returns an empty cstr but leaves enough breadcrumbs in
 * stderr for the caller to diagnose (we print it). Refusing to
 * vacuously pass when nm is missing or the obj is unreadable is the
 * point — the test's job is to detect a real regression, not to skip
 * when the toolchain is broken. */
static cstr run_nm(const char *obj_path, int *ok)
{
	VecStr cmd = c_init(vec_str, {"nm", "-u", "--format=just-symbols"});
	vec_str_push(&cmd, obj_path);
	cstr out = cstr_init();
	cstr err = cstr_init();
	int rc = run_subproc(cmd, (SubprocArgs){
		.stdout = &out, .stderr = &err
	});
	if (rc != 0) {
		fprintf(stderr, "    nm failed (rc=%d) on %s: %s\n",
			rc, obj_path, cstr_str(&err));
		*ok = 0;
	} else {
		*ok = 1;
	}
	cstr_drop(&err);
	if (rc != 0) {
		cstr_drop(&out);
		return cstr_init();
	}
	return out;
}

/* Returns 1 if the test should skip (obj dir missing — not a host run). */
static int should_skip(void)
{
	struct stat st;
	return stat(TAWCROOT_PROD_OBJ_DIR, &st) != 0;
}

/* Fail the test if `obj_basename`'s nm-output references any banned
 * symbol. Returns the count of violations, or -1 if nm itself failed
 * (caller treats that as a hard test failure too). */
static int check_obj(const char *obj_basename)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s",
		 TAWCROOT_PROD_OBJ_DIR, obj_basename);

	int ok = 0;
	cstr out = run_nm(path, &ok);
	if (!ok) { cstr_drop(&out); return -1; }
	const char *p = cstr_str(&out);
	int violations = 0;
	const char *line = p;
	while (*line) {
		const char *eol = strchr(line, '\n');
		size_t llen = eol ? (size_t)(eol - line) : strlen(line);
		for (size_t b = 0; b < sizeof banned / sizeof banned[0]; b++) {
			const char *bs = banned[b];
			size_t bl = strlen(bs);
			if (llen == bl && memcmp(line, bs, bl) == 0) {
				printf("    %s imports forbidden symbol %s\n",
				       obj_basename, bs);
				violations++;
			}
		}
		if (!eol) break;
		line = eol + 1;
	}
	cstr_drop(&out);
	return violations;
}

test(handler_set_no_banned_libc_imports)
{
	if (should_skip()) {
		/* --device runs don't have host .o files. Pass with a note. */
		printf("    skipping (host .o dir not present: %s)\n",
		       TAWCROOT_PROD_OBJ_DIR);
		return;
	}
	int total = 0;
	int nm_failed = 0;
	for (size_t i = 0; i < sizeof handler_objs / sizeof handler_objs[0]; i++) {
		int v = check_obj(handler_objs[i]);
		if (v < 0) nm_failed++;
		else       total += v;
	}
	test_int_eq(nm_failed, 0);
	test_int_eq(total, 0);
}

/* Pin handler.c's import list explicitly. The hot SIGSYS dispatch path
 * runs through here; any new external reference is a design event we
 * want surfaced loudly so it's reviewed for async-signal-safety.
 *
 * Update this whitelist deliberately when a new dependency is added —
 * with a short justification of why it's signal-safe. */
test(handler_c_pinned_import_list)
{
	if (should_skip()) {
		printf("    skipping (host .o dir not present)\n");
		return;
	}
	static const char *const allowed[] = {
		/* Linker-internal — every PIE/PIC compile lands this. */
		"_GLOBAL_OFFSET_TABLE_",
		/* Hot-path lookup of the dispatch table (pure load, ASS). */
		"tawcroot_dispatch_get",
		/* The raw-syscall stub. ASS by construction (single SYSCALL/SVC
		 * with the inline-asm contract documented in raw_sys.h). */
		"tawcroot_raw_syscall",
		/* Sigreturn trampoline (asm). ASS — it just loads ucontext and
		 * returns to the kernel via rt_sigreturn. */
		"tawcroot_sigreturn_trampoline",
	};

	char path[512];
	snprintf(path, sizeof path, "%s/handler.c.o", TAWCROOT_PROD_OBJ_DIR);
	int ok = 0;
	cstr out = run_nm(path, &ok);
	if (!ok) { cstr_drop(&out); test_int_eq(ok, 1); return; }
	const char *p = cstr_str(&out);
	int unexpected = 0;
	const char *line = p;
	while (*line) {
		const char *eol = strchr(line, '\n');
		size_t llen = eol ? (size_t)(eol - line) : strlen(line);
		if (llen > 0) {
			int ok = 0;
			for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
				size_t al = strlen(allowed[i]);
				if (llen == al && memcmp(line, allowed[i], al) == 0) {
					ok = 1; break;
				}
			}
			if (!ok) {
				printf("    handler.c imports unexpected symbol: %.*s\n",
				       (int)llen, line);
				unexpected++;
			}
		}
		if (!eol) break;
		line = eol + 1;
	}
	cstr_drop(&out);
	test_int_eq(unexpected, 0);
}
