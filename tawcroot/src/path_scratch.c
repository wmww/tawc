#include "path_scratch.h"

#define TAWCROOT_PATH_SCRATCH_SLOTS 128

static struct tawcroot_path_scratch
	g_path_scratch[TAWCROOT_PATH_SCRATCH_SLOTS];

struct tawcroot_path_scratch *tawcroot_path_scratch_acquire(void)
{
#ifdef TAWCROOT_SCRATCH_DEBUG_SPIN_TRAP
	unsigned spins = 0;
#endif
	for (;;) {
		for (int i = 0; i < TAWCROOT_PATH_SCRATCH_SLOTS; i++) {
			int expected = 0;
			if (__atomic_compare_exchange_n(&g_path_scratch[i].in_use,
							&expected, 1, 0,
							__ATOMIC_ACQUIRE,
							__ATOMIC_RELAXED))
				return &g_path_scratch[i];
		}
#ifdef TAWCROOT_SCRATCH_DEBUG_SPIN_TRAP
		if (++spins == TAWCROOT_SCRATCH_DEBUG_SPIN_TRAP)
			__builtin_trap();
#endif
	}
}

void tawcroot_path_scratch_release(struct tawcroot_path_scratch *scratch)
{
	if (!scratch) return;
	__atomic_store_n(&scratch->in_use, 0, __ATOMIC_RELEASE);
}
