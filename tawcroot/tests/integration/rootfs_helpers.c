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
	if (s < 0) return false;
	int d = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (d < 0) { close(s); return false; }
	char buf[65536];
	for (;;) {
		ssize_t n = read(s, buf, sizeof buf);
		if (n == 0) break;
		if (n < 0) { close(s); close(d); return false; }
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(d, buf + off, n - off);
			if (w <= 0) { close(s); close(d); return false; }
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
