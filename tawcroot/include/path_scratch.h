/* Handler-safe path scratch buffers.
 *
 * Path-bearing SIGSYS handlers run on the trapping thread's stack. Keep
 * large PATH_MAX-sized temporaries out of those frames by borrowing them
 * from this preallocated process-wide pool. No malloc, no syscalls, no
 * libc; acquisition is a bounded CAS scan with spin retry when every slot
 * is momentarily busy.
 */

#pragma once

#define TAWCROOT_PATH_SCRATCH_SIZE 4096
/* Four buffers cover the widest current call sites: linkat/renameat
 * translate old path+suffix and new path+suffix at the same time. */
#define TAWCROOT_PATH_SCRATCH_BUFS 4

struct tawcroot_path_scratch {
	int in_use;
	char buf[TAWCROOT_PATH_SCRATCH_BUFS][TAWCROOT_PATH_SCRATCH_SIZE];
};

struct tawcroot_path_scratch *tawcroot_path_scratch_acquire(void);
void tawcroot_path_scratch_release(struct tawcroot_path_scratch *scratch);

static inline void tawcroot_path_scratch_auto_release(
	struct tawcroot_path_scratch **scratch)
{
	if (scratch && *scratch)
		tawcroot_path_scratch_release(*scratch);
}

#define TAWCROOT_PATH_SCRATCH_AUTO(name) \
	__attribute__((cleanup(tawcroot_path_scratch_auto_release))) \
	struct tawcroot_path_scratch *name = tawcroot_path_scratch_acquire()
