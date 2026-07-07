/* Hardlink-emulation kill matrix (hosted layer).
 *
 * The crash-safety proof from notes/tawcroot/link-emulation.md:
 * for every mutation (ADD / DEL / NEW / CLOBBER), run the op in a
 * forked child whose raw-syscall hook _exit()s the process just
 * before the k-th syscall — for every k until the op completes — then
 * repair in the parent and assert the store invariants:
 *
 *   - the intent slot is empty after recovery;
 *   - no guest name dangles (a token symlink whose object is gone);
 *   - every sidecar count is >= the number of live referrers
 *     (monotone invariant: leaks allowed, deficits never);
 *   - content is intact through every surviving name.
 *
 * Recovery itself is also killed at every syscall j and re-run (it
 * must be idempotent), and the store-keyed discriminators are checked
 * against lock-free guest renames landing between crash and repair —
 * in particular the fully-published-NEW crash followed by a rename of
 * the destination (recovery must never recreate a name from a
 * recorded path: that would mint a phantom referrer). */

#include <cleat/test.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hosted.h"
#include "../integration/rootfs_helpers.h"

#include "dispatch.h"
#include "errno_neg.h"
#include "linkstore.h"
#include "path.h"
#include "sysnr.h"

#include "linkstore_fixture.h"

/* Dispatch a syscall through the real handler, no cleat asserts (used
 * inside forked children where a failed assert can't propagate). */
static long sys(long nr, long a, long b, long c, long d, long e, long f)
{
	tawcroot_handler_fn fn = tawcroot_dispatch_get((int)nr);
	if (!fn) return TAWC_ENOSYS;
	tawcroot_syscall_args args = {
		.nr = nr, .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
	};
	return fn(&args, NULL);
}

/* Child-side hook: EPERM the host linkat (the SELinux denial,
 * uncounted), _exit(9) just before the kill_at-th other raw syscall. */
static int g_kill_at;
static int g_seen;
static bool kill_hook(long nr, const long args[6], long *ret)
{
	(void)args;
	if (nr == TAWC_SYS_linkat) { *ret = TAWC_EPERM; return true; }
	if (g_kill_at > 0 && ++g_seen >= g_kill_at) _exit(9);
	return false;
}

/* Fork `fn(arg)`; exit code 0 = completed, 9 = killed at k, 7 = the
 * op returned an unexpected error. Returns the child's exit code. */
static int run_killed(TestCtx *test_ctx, int kill_at,
		      long (*fn)(void *), void *arg)
{
	pid_t pid = fork();
	test_true(pid >= 0);
	if (pid == 0) {
		g_kill_at = kill_at;
		g_seen = 0;
		tawcroot_test_raw_hook = kill_hook;
		long rv = fn(arg);
		_exit(rv == 0 ? 0 : 7);
	}
	int st = 0;
	test_true(waitpid(pid, &st, 0) == pid);
	test_true(WIFEXITED(st));
	return WEXITSTATUS(st);
}

/* ------------------------------------------------------------------ */
/* Invariant checker (parent side, libc introspection)                 */

/* Read the token out of a host-path symlink; returns 1 + fills tok. */
static int host_token(const char *hpath, char *tok, size_t cap)
{
	char t[64];
	ssize_t n = readlink(hpath, t, sizeof t - 1);
	if (n <= 0) return 0;
	t[n] = 0;
	const char *p;
	if (!tawcroot_link_target_is_token(t, &p)) return 0;
	snprintf(tok, cap, "%s", p);
	return 1;
}

/* Assert the store + guest-tree invariants for a set of candidate
 * guest names. `content` is what every live cluster member must read
 * back through the handlers. */
static void assert_invariants_ex(TestCtx *test_ctx, th_view *v,
				 const char *content,
				 const char *const *names, int n_names,
				 int allow_strands)
{
	char p[4600];

	/* Intent slot empty. */
	snprintf(p, sizeof p, "%s/intent", g_store);
	test_int_eq(access(p, F_OK), -1);

	for (int i = 0; i < n_names; i++) {
		snprintf(p, sizeof p, "%s%s", v->root, names[i]);
		char tok[64];
		if (!host_token(p, tok, sizeof tok)) continue;

		/* Token name → object must exist (no dangling names).
		 * Exception, only when guest renames raced the crash
		 * window: a moved rollback symlink is a content-safe
		 * strand the plan accepts — the data never left the
		 * source. */
		char op[4600];
		snprintf(op, sizeof op, "%s/link/%s", g_store, tok);
		struct stat ost;
		if (stat(op, &ost) != 0) {
			test_true(allow_strands);
			continue;
		}

		/* Referrer count for this token across the candidates. */
		int refs = 0;
		for (int j = 0; j < n_names; j++) {
			char q[4600], tk2[64];
			snprintf(q, sizeof q, "%s%s", v->root, names[j]);
			if (host_token(q, tk2, sizeof tk2) &&
			    strcmp(tk2, tok) == 0)
				refs++;
		}

		/* Sidecar count (if parseable) >= live referrers. */
		char cp[4600];
		snprintf(cp, sizeof cp, "%s/link/%s.cnt", g_store, tok);
		FILE *fp = fopen(cp, "r");
		if (fp) {
			unsigned long cnt = 0;
			if (fscanf(fp, "%lu", &cnt) == 1)
				test_true(cnt >= (unsigned long)refs);
			fclose(fp);
		}

		/* Content intact through the guest name. */
		long fd = sys(TAWC_SYS_openat, AT_FDCWD, (long)names[i],
			      O_RDONLY, 0, 0, 0);
		test_true(fd >= 0);
		char buf[128] = {0};
		test_true(read((int)fd, buf, sizeof buf - 1) >= 0);
		close((int)fd);
		test_str_eq(buf, content);
	}
}

static void assert_invariants(TestCtx *test_ctx, th_view *v,
			      const char *content,
			      const char *const *names, int n_names)
{
	assert_invariants_ex(test_ctx, v, content, names, n_names, 0);
}

/* Kill recovery at syscall j, repeatedly, until a run completes —
 * verifies recovery is idempotent under its own crashes. */
static long recover_op(void *arg)
{
	(void)arg;
	return tawcroot_linkstore_recover_now();
}

static void recover_through_kills(TestCtx *test_ctx)
{
	/* A crash before the store was even created leaves nothing to
	 * recover (and nothing recoverable). */
	if (access(g_store, F_OK) != 0) return;
	for (int j = 1; j < 200; j++) {
		int rc = run_killed(test_ctx, j, recover_op, NULL);
		if (rc == 0) return;
		test_int_eq(rc, 9);
	}
	test_true(0 && "recovery never completed");
}

/* The killed child may have CREATED the store (the parent was LATENT
 * with no fds); refresh the parent's view so token detection works
 * for the invariant checks. Ordinary session restart. */
static void reopen_store(void)
{
	tawcroot_linkstore_configure(NULL);
	tawcroot_linkstore_configure(g_store);
}

/* ------------------------------------------------------------------ */
/* Ops under test                                                      */

struct two_names { const char *a, *b; };

static long op_link(void *arg)
{
	struct two_names *tn = arg;
	return sys(TAWC_SYS_linkat, AT_FDCWD, (long)tn->a,
		   AT_FDCWD, (long)tn->b, 0, 0);
}

static long op_unlink(void *arg)
{
	return sys(TAWC_SYS_unlinkat, AT_FDCWD, (long)arg, 0, 0, 0, 0);
}

static long op_rename(void *arg)
{
	struct two_names *tn = arg;
	return sys(TAWC_SYS_renameat2, AT_FDCWD, (long)tn->a,
		   AT_FDCWD, (long)tn->b, 0, 0);
}

static void write_guest(TestCtx *test_ctx, const char *gpath,
			const char *content)
{
	long fd = sys(TAWC_SYS_openat, AT_FDCWD, (long)gpath,
		      O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
	test_true(fd >= 0);
	test_true(write((int)fd, content, strlen(content)) ==
		  (ssize_t)strlen(content));
	close((int)fd);
}

static void make_pair(TestCtx *test_ctx, const char *src, const char *dst)
{
	tawcroot_test_raw_hook = kill_hook;  /* EPERM linkat, no kill */
	g_kill_at = 0;
	g_seen = 0;
	struct two_names tn = { src, dst };
	test_int_eq(op_link(&tn), 0);
	tawcroot_test_raw_hook = NULL;
}

/* ------------------------------------------------------------------ */
/* Matrices                                                            */

test(linkstore_kill_matrix_new)
{
	static const char *names[] = { "/run/f", "/run/l1" };
	for (int k = 1; k < 300; k++) {
		th_view v;
		th_setup(&v, "lsk-new");
		store_setup(&v);
		write_guest(test_ctx, "/run/f", "newdata\n");

		struct two_names tn = { "/run/f", "/run/l1" };
		int rc = run_killed(test_ctx, k, op_link, &tn);
		test_true(rc == 0 || rc == 9);

		recover_through_kills(test_ctx);
		reopen_store();
		assert_invariants(test_ctx, &v, "newdata\n", names, 2);

		/* Whatever survived, the DATA must be reachable through
		 * at least one of the two names (NEW's one-syscall
		 * vacancy plus a crash can strand the src name — the
		 * object is then reachable via dst). */
		int reachable = 0;
		for (int i = 0; i < 2; i++) {
			long fd = sys(TAWC_SYS_openat, AT_FDCWD,
				      (long)names[i], O_RDONLY, 0, 0, 0);
			if (fd >= 0) { reachable = 1; close((int)fd); }
		}
		test_true(reachable);

		store_teardown();
		th_teardown(&v);
		if (rc == 0) return;  /* op ran to completion: matrix done */
	}
	test_true(0 && "NEW never completed");
}

test(linkstore_kill_matrix_add)
{
	static const char *names[] = { "/run/f", "/run/l1", "/run/l2" };
	for (int k = 1; k < 300; k++) {
		th_view v;
		th_setup(&v, "lsk-add");
		store_setup(&v);
		write_guest(test_ctx, "/run/f", "adddata\n");
		make_pair(test_ctx, "/run/f", "/run/l1");

		struct two_names tn = { "/run/l1", "/run/l2" };
		int rc = run_killed(test_ctx, k, op_link, &tn);
		test_true(rc == 0 || rc == 9);

		recover_through_kills(test_ctx);
		reopen_store();
		assert_invariants(test_ctx, &v, "adddata\n", names, 3);

		/* The pre-existing pair must never be damaged by a
		 * crashed ADD. */
		for (int i = 0; i < 2; i++) {
			long fd = sys(TAWC_SYS_openat, AT_FDCWD,
				      (long)names[i], O_RDONLY, 0, 0, 0);
			test_true(fd >= 0);
			close((int)fd);
		}

		store_teardown();
		th_teardown(&v);
		if (rc == 0) return;
	}
	test_true(0 && "ADD never completed");
}

test(linkstore_kill_matrix_del)
{
	static const char *names[] = { "/run/f", "/run/l1" };
	for (int k = 1; k < 300; k++) {
		th_view v;
		th_setup(&v, "lsk-del");
		store_setup(&v);
		write_guest(test_ctx, "/run/f", "deldata\n");
		make_pair(test_ctx, "/run/f", "/run/l1");

		int rc = run_killed(test_ctx, k, op_unlink,
				    (void *)"/run/l1");
		test_true(rc == 0 || rc == 9);

		recover_through_kills(test_ctx);
		reopen_store();
		assert_invariants(test_ctx, &v, "deldata\n", names, 2);

		/* The other name survives a crashed DEL of its sibling,
		 * whatever the window. */
		long fd = sys(TAWC_SYS_openat, AT_FDCWD, (long)"/run/f",
			      O_RDONLY, 0, 0, 0);
		test_true(fd >= 0);
		close((int)fd);

		store_teardown();
		th_teardown(&v);
		if (rc == 0) return;
	}
	test_true(0 && "DEL never completed");
}

test(linkstore_kill_matrix_del_last_name_reclaims)
{
	/* DEL of the LAST name exercises the object-unlink branch. */
	static const char *names[] = { "/run/f" };
	for (int k = 1; k < 300; k++) {
		th_view v;
		th_setup(&v, "lsk-dell");
		store_setup(&v);
		write_guest(test_ctx, "/run/f", "lastdata\n");
		make_pair(test_ctx, "/run/f", "/run/l1");
		test_int_eq(sys(TAWC_SYS_unlinkat, AT_FDCWD,
				(long)"/run/l1", 0, 0, 0, 0), 0);

		int rc = run_killed(test_ctx, k, op_unlink,
				    (void *)"/run/f");
		test_true(rc == 0 || rc == 9);

		recover_through_kills(test_ctx);
		reopen_store();
		assert_invariants(test_ctx, &v, "lastdata\n", names, 1);

		store_teardown();
		th_teardown(&v);
		if (rc == 0) return;
	}
	test_true(0 && "DEL-last never completed");
}

test(linkstore_kill_matrix_clobber)
{
	/* CLOBBER writes no intent; crash windows may overcount but must
	 * never lose either file's bytes or leave a dangling name. */
	static const char *names[] = { "/run/f", "/run/l1" };
	for (int k = 1; k < 300; k++) {
		th_view v;
		th_setup(&v, "lsk-clob");
		store_setup(&v);
		write_guest(test_ctx, "/run/f", "kept\n");
		make_pair(test_ctx, "/run/f", "/run/l1");
		write_guest(test_ctx, "/run/incoming", "incoming\n");

		struct two_names tn = { "/run/incoming", "/run/l1" };
		int rc = run_killed(test_ctx, k, op_rename, &tn);
		test_true(rc == 0 || rc == 9);

		recover_through_kills(test_ctx);
		reopen_store();
		assert_invariants(test_ctx, &v, "kept\n", names, 2);

		/* /run/f keeps its bytes in every window; l1 reads either
		 * the old cluster bytes (rename not yet applied) or the
		 * incoming bytes (applied). */
		long fd = sys(TAWC_SYS_openat, AT_FDCWD, (long)"/run/f",
			      O_RDONLY, 0, 0, 0);
		test_true(fd >= 0);
		char buf[64] = {0};
		test_true(read((int)fd, buf, sizeof buf - 1) > 0);
		close((int)fd);
		test_str_eq(buf, "kept\n");

		fd = sys(TAWC_SYS_openat, AT_FDCWD, (long)"/run/l1",
			 O_RDONLY, 0, 0, 0);
		test_true(fd >= 0);
		memset(buf, 0, sizeof buf);
		test_true(read((int)fd, buf, sizeof buf - 1) > 0);
		close((int)fd);
		test_true(strcmp(buf, "kept\n") == 0 ||
			  strcmp(buf, "incoming\n") == 0);

		store_teardown();
		th_teardown(&v);
		if (rc == 0) return;
	}
	test_true(0 && "CLOBBER never completed");
}

test(linkstore_kill_then_guest_rename_interleaving)
{
	/* For every NEW crash window: move the (possibly published) dst
	 * with a lock-free guest rename BEFORE recovery runs, then
	 * verify recovery never mints an entry back at the recorded dst
	 * path — staged-absent is its witness that the publish already
	 * happened (phantom-referrer guard). */
	for (int k = 1; k < 300; k++) {
		th_view v;
		th_setup(&v, "lsk-ilv");
		store_setup(&v);
		write_guest(test_ctx, "/run/f", "ilv\n");

		struct two_names tn = { "/run/f", "/run/l1" };
		int rc = run_killed(test_ctx, k, op_link, &tn);
		test_true(rc == 0 || rc == 9);

		/* Lock-free guest rename of the dst, if it exists. */
		int moved = sys(TAWC_SYS_renameat2, AT_FDCWD,
				(long)"/run/l1", AT_FDCWD,
				(long)"/etc/sub/moved",
				RENAME_NOREPLACE, 0) == 0;

		recover_through_kills(test_ctx);
		reopen_store();

		static const char *names[] = {
			"/run/f", "/run/l1", "/etc/sub/moved",
		};
		assert_invariants_ex(test_ctx, &v, "ilv\n", names, 3, 1);

		/* Whatever the window and wherever the names went, the
		 * DATA stays reachable through at least one candidate. */
		int reachable = 0;
		for (int i = 0; i < 3 && !reachable; i++) {
			long fd = sys(TAWC_SYS_openat, AT_FDCWD,
				      (long)names[i], O_RDONLY, 0, 0, 0);
			if (fd >= 0) { reachable = 1; close((int)fd); }
		}
		test_true(reachable);

		if (moved) {
			/* Recovery must NOT have recreated /run/l1: the
			 * staged dst entry was consumed by the original
			 * publish, and recreating from the recorded path
			 * would mint a phantom referrer. */
			char p[4600];
			snprintf(p, sizeof p, "%s/run/l1", v.root);
			struct stat st;
			test_int_eq(lstat(p, &st), -1);
		}

		store_teardown();
		th_teardown(&v);
		if (rc == 0) return;
	}
	test_true(0 && "interleaved NEW never completed");
}

test(linkstore_kill_matrix_session_start_recovery)
{
	/* The configure-time path: crash a NEW, then re-configure the
	 * store from scratch (session start) — the intent check must
	 * repair without an explicit recover call. Use a mid-flight k
	 * (after the intent write, around the staging steps). */
	th_view v;
	th_setup(&v, "lsk-sess");
	store_setup(&v);
	write_guest(test_ctx, "/run/f", "sess\n");

	struct two_names tn = { "/run/f", "/run/l1" };
	int rc = run_killed(test_ctx, 12, op_link, &tn);
	test_true(rc == 0 || rc == 9);

	/* Session restart. */
	tawcroot_linkstore_configure(NULL);
	tawcroot_linkstore_configure(g_store);
	test_int_eq((long)tawcroot_linkstore_state(),
		    (long)TAWCROOT_STORE_READY);

	char p[4600];
	snprintf(p, sizeof p, "%s/intent", g_store);
	test_int_eq(access(p, F_OK), -1);

	static const char *names[] = { "/run/f", "/run/l1" };
	assert_invariants(test_ctx, &v, "sess\n", names, 2);

	store_teardown();
	th_teardown(&v);
}
