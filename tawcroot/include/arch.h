/* Arch dispatch header.
 *
 * Selects between x86_64 and aarch64 inline helpers and exposes a small,
 * ucontext-shaped surface the handler uses to read syscall args and write
 * the return value. Per notes/tawcroot/sigsys-handler.md "Reading and writing the saved
 * registers". Header-only on purpose — every helper is tiny and inlines
 * cleanly into the SIGSYS handler.
 */

#pragma once

#include <signal.h>
#include <ucontext.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/audit.h>

/* Six args is the kernel syscall ABI maximum on both arches; the seventh
 * "no_op" slot keeps the helpers' signatures aligned with our raw stub. */
typedef struct {
	long nr;
	long a, b, c, d, e, f;
} tawcroot_syscall_args;

#if defined(__aarch64__)
# include "arch/aarch64.h"
#elif defined(__x86_64__)
# include "arch/x86_64.h"
#else
# error "unsupported arch"
#endif
