/* Dispatch table — one function pointer per syscall number.
 *
 * The SIGSYS handler reads the trapped syscall number, looks up the
 * corresponding handler, calls it with `(args, uc)`, and writes the
 * return value back into ucontext. Empty slots return -ENOSYS — see
 * notes/tawcroot.md "SIGSYS handler" (we deliberately do NOT abort on
 * unexpected TRAP because Android's stacked filter can hand us
 * syscalls we didn't ask for, and that should soft-fail to ENOSYS).
 *
 * Table size is one larger than the highest trapped syscall number;
 * we size it conservatively to cover both arches' top numbers we care
 * about. Storage is a flat C array — no malloc, no STC, async-signal
 * safe to read from the handler.
 *
 * Handlers must themselves stay async-signal-safe: no malloc, no
 * stdio, no libc with hidden mutable state. Local stack-only state.
 */

#pragma once

#include <stddef.h>
#include <ucontext.h>

#include "arch.h"

/* Sized to fit the largest syscall number we'll touch on either arch.
 * Today's max is x86_64 faccessat2 (439); aarch64 close_range is 436.
 * Bumped to 1024 (review finding D5) to cover Linux's ongoing additions
 * in the 500-600 range without silent install-failure when a future
 * handler picks a high number. tawcroot_dispatch_install asserts that
 * `nr` fits — see review finding D6. */
#define TAWCROOT_DISPATCH_MAX 1024

typedef long (*tawcroot_handler_fn)(const tawcroot_syscall_args *args,
				    ucontext_t *uc);

void tawcroot_dispatch_install(int nr, tawcroot_handler_fn fn);
tawcroot_handler_fn tawcroot_dispatch_get(int nr);

/* Phase-1 init: register every handler we ship today. Single source of
 * truth for "what's in the trap set"; the BPF filter generator reads
 * the same list. */
void tawcroot_dispatch_init(void);

/* Returns the trap-syscall list for the BPF filter generator. The caller
 * passes a buffer; we fill it with syscall numbers (in the same order
 * the dispatch table was built) and return the count. The list is the
 * union of "every syscall with a non-NULL dispatch slot". */
size_t tawcroot_dispatch_trap_list(int *out, size_t out_cap);
