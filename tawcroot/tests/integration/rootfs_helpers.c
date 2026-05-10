/* See rootfs_helpers.h. */

#define _GNU_SOURCE

#include "rootfs_helpers.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

void rh_rmrf(const char *path)
{
	char cmd[PATH_MAX + 32];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
	(void)!system(cmd);
}

bool rh_mkdir_p(const char *path, mode_t mode)
{
	return mkdir(path, mode) == 0 || errno == EEXIST;
}

bool rh_copy_file(const char *src, const char *dst, mode_t mode)
{
	int s = open(src, O_RDONLY);
	if (s < 0) {
		/* Loud about WHICH path is missing. Without this, every
		 * caller's failure surfaces as a generic
		 * `test_true(build_rootfs())` panic and the operator has
		 * to diff fixture lists by hand to find the missing one.
		 * Common cause on device: build-fixtures bailed mid-list
		 * (e.g. an aarch64 .S that doesn't assemble), so a fixture
		 * was never built / pushed. (See test_prod_features.c's
		 * shared build_rootfs — one missing fixture takes out
		 * every test in the file regardless of whether the test
		 * itself uses it.) */
		fprintf(stderr,
		        "rh_copy_file: open src=\"%s\" failed: %s\n",
		        src, strerror(errno));
		return false;
	}
	int d = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (d < 0) {
		fprintf(stderr,
		        "rh_copy_file: open dst=\"%s\" failed: %s\n",
		        dst, strerror(errno));
		close(s);
		return false;
	}
	char buf[65536];
	for (;;) {
		ssize_t n = read(s, buf, sizeof buf);
		if (n == 0) break;
		if (n < 0) {
			fprintf(stderr,
			        "rh_copy_file: read src=\"%s\" failed: %s\n",
			        src, strerror(errno));
			close(s); close(d); return false;
		}
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(d, buf + off, n - off);
			if (w <= 0) {
				fprintf(stderr,
				        "rh_copy_file: write dst=\"%s\" failed: %s\n",
				        dst, w < 0 ? strerror(errno) : "short write");
				close(s); close(d); return false;
			}
			off += w;
		}
	}
	close(s); close(d);
	(void)chmod(dst, mode);
	return true;
}

bool rh_write_text(const char *path, const char *contents)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	size_t n = strlen(contents);
	bool ok = (write(fd, contents, n) == (ssize_t)n);
	close(fd);
	return ok;
}

bool rh_make_marker_path(char *out, size_t cap, const char *tag)
{
#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int n = snprintf(out, cap,
			 TAWCROOT_TEST_TMPDIR "/tawc-test-%s-%d-%ld%09ld",
			 tag, (int)getpid(), (long)ts.tv_sec, ts.tv_nsec);
	return n > 0 && (size_t)n < cap;
}

bool rh_poll_for_path(const char *path, int ticks)
{
	for (int i = 0; i < ticks; i++) {
		if (access(path, F_OK) == 0) return true;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000 };
		nanosleep(&ts, NULL);
	}
	return false;
}
