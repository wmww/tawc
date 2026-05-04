/* Initial-stack synthesizer. See include/loader_stack.h.
 *
 * Pure data construction — no syscalls. Compile into both
 * production and PROD_C_FOR_TESTS so the unit tests exercise the
 * same code that runs in the guest exec path. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "loader_stack.h"
#include "tawc_string.h"

#define MAX_AUXV     32       /* generous upper bound on entries we emit */

/* Round a uintptr_t down to a power-of-two boundary. */
static inline uintptr_t align_down(uintptr_t v, uintptr_t a)
{ return v & ~(a - 1); }

long tawc_loader_build_stack(void *region_low, size_t region_size,
                             const struct tawc_loader_stack_input *in,
                             struct tawc_loader_stack_out *out)
{
	if (!region_low || !in || !out) return TAWC_EINVAL;
	if (in->argc < 0 || !in->argv || !in->envp) return TAWC_EINVAL;
	if (!in->at_random16 || !in->at_execfn || !in->at_platform)
		return TAWC_EINVAL;
	for (int i = 0; i < in->argc; i++)
		if (!in->argv[i]) return TAWC_EINVAL;

	int envc = 0;
	while (in->envp[envc]) envc++;

	uintptr_t lo = (uintptr_t)region_low;
	uintptr_t hi = lo + region_size;

	/* ---- Step 1: lay out strings at the high end. ----
	 *
	 * Walk down from `hi`, writing strings; record each pointer so we
	 * can store it in the matching argv[]/envp[]/auxv slot below.
	 * Strings are written in reverse logical order so that argv[0]
	 * is at the lowest address among argv strings, matching kernel
	 * convention (not actually required by glibc, but keeps the
	 * memory layout intuitive when debugging /proc/<pid>/maps). */

	uintptr_t cur = hi;

	uintptr_t addr_random;
	{
		size_t n = 16;
		if (cur - lo < n) return TAWC_ENOMEM;
		cur -= n;
		memcpy((void *)cur, in->at_random16, n);
		addr_random = cur;
	}

	uintptr_t addr_platform;
	{
		size_t n = strlen(in->at_platform) + 1;
		if (cur - lo < n) return TAWC_ENOMEM;
		cur -= n;
		memcpy((void *)cur, in->at_platform, n);
		addr_platform = cur;
	}

	uintptr_t addr_execfn;
	{
		size_t n = strlen(in->at_execfn) + 1;
		if (cur - lo < n) return TAWC_ENOMEM;
		cur -= n;
		memcpy((void *)cur, in->at_execfn, n);
		addr_execfn = cur;
	}

	/* envp/argv strings: walked in reverse logical order so that
	 * argv[0] ends up at the lowest string address. We store each
	 * string's eventual pointer in a fixed-size on-stack array
	 * (bounded by MAX_ARGS so the stack frame is bounded; real
	 * callers pass at most a few hundred). */
#define MAX_ARGS 1024
	if (in->argc > MAX_ARGS || envc > MAX_ARGS) return TAWC_EINVAL;
	uintptr_t argv_buf[MAX_ARGS];
	uintptr_t envp_buf[MAX_ARGS];

	for (int i = envc - 1; i >= 0; i--) {
		size_t n = strlen(in->envp[i]) + 1;
		if (cur - lo < n) return TAWC_ENOMEM;
		cur -= n;
		memcpy((void *)cur, in->envp[i], n);
		envp_buf[i] = cur;
	}
	for (int i = in->argc - 1; i >= 0; i--) {
		size_t n = strlen(in->argv[i]) + 1;
		if (cur - lo < n) return TAWC_ENOMEM;
		cur -= n;
		memcpy((void *)cur, in->argv[i], n);
		argv_buf[i] = cur;
	}

	uintptr_t strings_lo = cur;

	/* ---- Step 2: build the auxv entries in a temporary array. ----
	 * Order doesn't matter to ld.so/glibc except AT_NULL must be
	 * last. Emit a fixed set: required entries first, optional
	 * ones (CLKTCK, HWCAP, SYSINFO_EHDR) only if the caller
	 * provided non-zero values. */
	struct tawc_loader_aux_entry aux[MAX_AUXV];
	unsigned na = 0;

	#define AUX(t, v) do { \
		if (na >= MAX_AUXV - 1) return TAWC_ENOMEM; \
		aux[na].a_type = (t); aux[na].a_val = (v); na++; \
	} while (0)

	AUX(TAWC_AT_PHDR,    (uint64_t)in->at_phdr);
	AUX(TAWC_AT_PHENT,   (uint64_t)in->at_phent);
	AUX(TAWC_AT_PHNUM,   (uint64_t)in->at_phnum);
	AUX(TAWC_AT_PAGESZ,  in->at_pagesz ? in->at_pagesz : 4096);
	AUX(TAWC_AT_BASE,    (uint64_t)in->at_base);
	AUX(TAWC_AT_FLAGS,   in->at_flags);
	AUX(TAWC_AT_ENTRY,   (uint64_t)in->at_entry);
	/* Fake-root identity: every guest sees uid/gid 0 to match the
	 * `getuid` handler. Without this, glibc startup may differ
	 * subtly between exec-path and getuid-syscall reads. */
	AUX(TAWC_AT_UID,     0);
	AUX(TAWC_AT_EUID,    0);
	AUX(TAWC_AT_GID,     0);
	AUX(TAWC_AT_EGID,    0);
	AUX(TAWC_AT_SECURE,  0);
	AUX(TAWC_AT_RANDOM,  (uint64_t)addr_random);
	AUX(TAWC_AT_PLATFORM,(uint64_t)addr_platform);
	AUX(TAWC_AT_EXECFN,  (uint64_t)addr_execfn);
	if (in->at_clktck)        AUX(TAWC_AT_CLKTCK, in->at_clktck);
	if (in->at_hwcap)         AUX(TAWC_AT_HWCAP,  in->at_hwcap);
	if (in->at_hwcap2)        AUX(TAWC_AT_HWCAP2, in->at_hwcap2);
	if (in->at_sysinfo_ehdr)  AUX(TAWC_AT_SYSINFO_EHDR, (uint64_t)in->at_sysinfo_ehdr);
	AUX(TAWC_AT_NULL,    0);
	#undef AUX

	/* ---- Step 3: compute SP so that argc lands 16-byte aligned. ----
	 * Below the strings_lo cursor we'll write (high → low):
	 *   auxv (na * 16 bytes)
	 *   envp ptrs ((envc + 1) * 8)
	 *   argv ptrs ((argc + 1) * 8)
	 *   argc (8)
	 * SP = bottom of argc. Pad above auxv with zero bytes if needed
	 * so SP is 16-aligned. */
	size_t aux_bytes = (size_t)na * 16;
	size_t envp_bytes = (size_t)(envc + 1) * sizeof(uintptr_t);
	size_t argv_bytes = (size_t)(in->argc + 1) * sizeof(uintptr_t);
	size_t vec_bytes = sizeof(uintptr_t) /* argc */
	                 + argv_bytes + envp_bytes + aux_bytes;

	uintptr_t sp = align_down(strings_lo - vec_bytes, 16);
	if (sp < lo) return TAWC_ENOMEM;

	/* ---- Step 4: write the vectors at SP. ---- */
	uintptr_t w = sp;

	/* argc */
	*(uint64_t *)w = (uint64_t)in->argc;
	w += sizeof(uint64_t);

	/* argv ptrs + NULL */
	for (int i = 0; i < in->argc; i++) {
		*(uint64_t *)w = (uint64_t)argv_buf[i];
		w += sizeof(uint64_t);
	}
	*(uint64_t *)w = 0;
	w += sizeof(uint64_t);

	/* envp ptrs + NULL */
	for (int i = 0; i < envc; i++) {
		*(uint64_t *)w = (uint64_t)envp_buf[i];
		w += sizeof(uint64_t);
	}
	*(uint64_t *)w = 0;
	w += sizeof(uint64_t);

	/* auxv (already includes AT_NULL terminator) */
	for (unsigned i = 0; i < na; i++) {
		*(uint64_t *)w = aux[i].a_type;
		w += sizeof(uint64_t);
		*(uint64_t *)w = aux[i].a_val;
		w += sizeof(uint64_t);
	}

	out->sp      = sp;
	out->data_lo = sp;
	out->data_hi = hi;
	return 0;
}

void tawc_loader_walk_stack(uintptr_t sp,
                            int *out_argc,
                            char *const **out_argv,
                            char *const **out_envp,
                            const struct tawc_loader_aux_entry **out_auxv)
{
	uintptr_t w = sp;
	int argc = (int)*(uint64_t *)w;
	w += sizeof(uint64_t);

	char *const *argv = (char *const *)w;
	w += (size_t)(argc + 1) * sizeof(uintptr_t);

	char *const *envp = (char *const *)w;
	while (*(uint64_t *)w) w += sizeof(uint64_t);
	w += sizeof(uint64_t);  /* skip NULL */

	const struct tawc_loader_aux_entry *auxv =
	    (const struct tawc_loader_aux_entry *)w;

	if (out_argc)  *out_argc  = argc;
	if (out_argv)  *out_argv  = argv;
	if (out_envp)  *out_envp  = envp;
	if (out_auxv)  *out_auxv  = auxv;
}
