/* exec_state writer + reader. See include/exec_state.h.
 *
 * Pure data construction — no syscalls. Can be unit-tested from
 * cleat by feeding any bag of bytes through the round trip.
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "exec_state.h"
#include "tawc_string.h"

static int count_envp(const char *const *envp)
{
	int n = 0;
	while (envp[n]) n++;
	return n;
}

/* Sentinel offset for "absent string". 0 is a valid offset (the path
 * goes there), so we reserve UINT32_MAX. The header is zero-initialized
 * to mean "absent" but we explicitly set ABSENT_OFF to avoid relying on
 * 0 == path_off ambiguity.
 *
 * Convention: writer sets the field to 0 if the input pointer is NULL,
 * and reader treats 0 as "present at offset 0" only for path_off (which
 * is always present); for the optional fields (rootfs_host_off,
 * guest_exe_off, bind_*_off when n_binds > 0) we encode "absent" as 0
 * because the writer ensures path is the FIRST string at offset 0, so
 * any non-zero offset distinguishes optional fields from path. */

size_t tawcroot_exec_state_estimate_bytes(const char *path,
                                          int argc, const char *const *argv,
                                          const char *const *envp,
                                          const tawcroot_exec_state_extras *ex)
{
	size_t s = strlen(path) + 1;
	for (int i = 0; i < argc; i++) {
		if (!argv[i]) break;
		s += strlen(argv[i]) + 1;
	}
	int envc = count_envp(envp);
	for (int i = 0; i < envc; i++) {
		s += strlen(envp[i]) + 1;
	}
	if (ex) {
		if (ex->rootfs_host) s += strlen(ex->rootfs_host) + 1;
		if (ex->guest_exe)   s += strlen(ex->guest_exe) + 1;
		for (uint32_t i = 0; i < ex->n_binds; i++) {
			if (ex->bind_src && ex->bind_src[i])
				s += strlen(ex->bind_src[i]) + 1;
			if (ex->bind_dst && ex->bind_dst[i])
				s += strlen(ex->bind_dst[i]) + 1;
		}
		for (uint32_t i = 0; i < ex->n_shm; i++) {
			if (ex->shm_name && ex->shm_name[i])
				s += strlen(ex->shm_name[i]) + 1;
		}
	}
	return tawcroot_exec_state_total_bytes((uint32_t)s);
}

long tawcroot_exec_state_write(void *buf, size_t buf_cap,
                               const char *path,
                               int argc, const char *const *argv,
                               const char *const *envp,
                               const tawcroot_exec_state_extras *ex)
{
	if (!buf || !path || !argv || !envp) return TAWC_EINVAL;
	if (argc < 0 || argc > TAWCROOT_EXEC_STATE_MAX_ARGS) return TAWC_E2BIG;

	int envc = count_envp(envp);
	if (envc > TAWCROOT_EXEC_STATE_MAX_ENV) return TAWC_E2BIG;

	if (ex && ex->n_binds > TAWCROOT_EXEC_STATE_MAX_BINDS) return TAWC_E2BIG;
	if (ex && ex->n_shm   > TAWCROOT_EXEC_STATE_MAX_SHM)   return TAWC_E2BIG;
	/* shm names are required (no absent sentinel — reader uses
	 * CHECK_REQUIRED). Reject NULL up front to surface bad callers
	 * rather than silently aliasing to the path string at offset 0. */
	if (ex) {
		for (uint32_t i = 0; i < ex->n_shm; i++) {
			if (!ex->shm_name || !ex->shm_name[i]) return TAWC_EINVAL;
		}
	}

	size_t need = tawcroot_exec_state_estimate_bytes(path, argc, argv, envp, ex);
	if (need > buf_cap) return TAWC_ENOSPC;

	uint8_t *base = (uint8_t *)buf;
	tawcroot_exec_state_header *h = (tawcroot_exec_state_header *)base;
	memset(h, 0, sizeof *h);
	h->magic   = TAWCROOT_EXEC_STATE_MAGIC;
	h->version = TAWCROOT_EXEC_STATE_VERSION;
	h->argc    = (uint32_t)argc;
	h->envc    = (uint32_t)envc;

	uint8_t *strings = base + sizeof(*h);
	uint32_t off = 0;

	/* path first — convention, so a debug dump finds it easily. */
	{
		size_t n = strlen(path) + 1;
		h->path_off = off;
		memcpy(strings + off, path, n);
		off += (uint32_t)n;
	}

	for (int i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;
		h->argv_off[i] = off;
		memcpy(strings + off, argv[i], n);
		off += (uint32_t)n;
	}
	for (int i = 0; i < envc; i++) {
		size_t n = strlen(envp[i]) + 1;
		h->envp_off[i] = off;
		memcpy(strings + off, envp[i], n);
		off += (uint32_t)n;
	}

	/* Optional per-process state. 0-offset = absent (path is at offset 0
	 * so any subsequent string has off > 0 — unambiguous). */
	if (ex) {
		if (ex->rootfs_host) {
			size_t n = strlen(ex->rootfs_host) + 1;
			h->rootfs_host_off = off;
			memcpy(strings + off, ex->rootfs_host, n);
			off += (uint32_t)n;
		}
		if (ex->guest_exe) {
			size_t n = strlen(ex->guest_exe) + 1;
			h->guest_exe_off = off;
			memcpy(strings + off, ex->guest_exe, n);
			off += (uint32_t)n;
		}
		h->n_binds = ex->n_binds;
		for (uint32_t i = 0; i < ex->n_binds; i++) {
			if (ex->bind_src && ex->bind_src[i]) {
				size_t n = strlen(ex->bind_src[i]) + 1;
				h->bind_src_off[i] = off;
				memcpy(strings + off, ex->bind_src[i], n);
				off += (uint32_t)n;
			}
			if (ex->bind_dst && ex->bind_dst[i]) {
				size_t n = strlen(ex->bind_dst[i]) + 1;
				h->bind_dst_off[i] = off;
				memcpy(strings + off, ex->bind_dst[i], n);
				off += (uint32_t)n;
			}
		}
		h->n_shm = ex->n_shm;
		for (uint32_t i = 0; i < ex->n_shm; i++) {
			size_t n = strlen(ex->shm_name[i]) + 1;
			h->shm_name_off[i] = off;
			memcpy(strings + off, ex->shm_name[i], n);
			off += (uint32_t)n;
			h->shm_fd[i] = (uint32_t)ex->shm_fd[i];
		}
	}

	h->string_bytes = off;
	return (long)(sizeof(*h) + off);
}

long tawcroot_exec_state_read(const void *buf, size_t buf_size,
                              const char **argv_buf,
                              const char **envp_buf,
                              tawcroot_exec_state *out)
{
	if (!buf || !argv_buf || !envp_buf || !out)
		return TAWC_EINVAL;
	if (buf_size < sizeof(tawcroot_exec_state_header))
		return TAWC_EINVAL;

	const tawcroot_exec_state_header *h =
	    (const tawcroot_exec_state_header *)buf;
	if (h->magic != TAWCROOT_EXEC_STATE_MAGIC) return TAWC_EINVAL;
	if (h->version != TAWCROOT_EXEC_STATE_VERSION) return TAWC_EINVAL;
	if (h->argc > TAWCROOT_EXEC_STATE_MAX_ARGS) return TAWC_EINVAL;
	if (h->envc > TAWCROOT_EXEC_STATE_MAX_ENV) return TAWC_EINVAL;
	if (h->n_binds > TAWCROOT_EXEC_STATE_MAX_BINDS) return TAWC_EINVAL;
	if (h->n_shm   > TAWCROOT_EXEC_STATE_MAX_SHM)   return TAWC_EINVAL;

	size_t need = sizeof(*h) + h->string_bytes;
	if (buf_size < need) return TAWC_EINVAL;
	if (h->path_off >= h->string_bytes) return TAWC_EINVAL;

	const char *strings = (const char *)buf + sizeof(*h);

	/* Validate every offset is within strings AND the string is
	 * NUL-terminated within bounds. The ABSENT sentinel for optional
	 * strings is `off == 0`: since path_off is always 0, any other
	 * offset matching 0 indicates "field absent" — because no other
	 * string can be at offset 0. */
	#define CHECK_REQUIRED(off) do { \
		uint32_t _o = (off); \
		if (_o >= h->string_bytes) return TAWC_EINVAL; \
		const char *_p = strings + _o; \
		const char *_end = strings + h->string_bytes; \
		while (_p < _end && *_p) _p++; \
		if (_p == _end) return TAWC_EINVAL; \
	} while (0)
	#define CHECK_OPTIONAL(off) do { \
		uint32_t _opt = (off); \
		if (_opt == 0) break; /* absent */ \
		CHECK_REQUIRED(_opt); \
	} while (0)

	CHECK_REQUIRED(h->path_off);
	for (uint32_t i = 0; i < h->argc; i++) CHECK_REQUIRED(h->argv_off[i]);
	for (uint32_t i = 0; i < h->envc; i++) CHECK_REQUIRED(h->envp_off[i]);
	CHECK_OPTIONAL(h->rootfs_host_off);
	CHECK_OPTIONAL(h->guest_exe_off);
	for (uint32_t i = 0; i < h->n_binds; i++) {
		CHECK_OPTIONAL(h->bind_src_off[i]);
		CHECK_OPTIONAL(h->bind_dst_off[i]);
	}
	for (uint32_t i = 0; i < h->n_shm; i++) {
		CHECK_REQUIRED(h->shm_name_off[i]);
	}
	#undef CHECK_REQUIRED
	#undef CHECK_OPTIONAL

	out->path = strings + h->path_off;
	out->argc = h->argc;
	out->envc = h->envc;
	for (uint32_t i = 0; i < h->argc; i++)
		argv_buf[i] = strings + h->argv_off[i];
	argv_buf[h->argc] = (const char *)0;
	for (uint32_t i = 0; i < h->envc; i++)
		envp_buf[i] = strings + h->envp_off[i];
	envp_buf[h->envc] = (const char *)0;
	out->argv = argv_buf;
	out->envp = envp_buf;

	out->rootfs_host = h->rootfs_host_off ? strings + h->rootfs_host_off : (const char *)0;
	out->guest_exe   = h->guest_exe_off   ? strings + h->guest_exe_off   : (const char *)0;
	out->n_binds     = h->n_binds;
	for (uint32_t i = 0; i < h->n_binds; i++) {
		out->bind_src[i] = h->bind_src_off[i] ? strings + h->bind_src_off[i] : (const char *)0;
		out->bind_dst[i] = h->bind_dst_off[i] ? strings + h->bind_dst_off[i] : (const char *)0;
	}
	out->n_shm = h->n_shm;
	for (uint32_t i = 0; i < h->n_shm; i++) {
		out->shm_name[i] = strings + h->shm_name_off[i];
		out->shm_fd[i]   = (int)h->shm_fd[i];
	}
	return 0;
}
