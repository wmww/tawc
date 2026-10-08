/* Test-side interface to the hosted tawcroot_raw_syscall shim.
 * See raw_syscall_host.c for the contract. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* When non-NULL, called before every raw syscall the production code under
 * test issues. Return true to short-circuit: *ret becomes the syscall
 * result (-errno convention). Return false to pass through to the kernel.
 * Reset to NULL when the test is done. */
extern bool (*tawcroot_test_raw_hook)(long nr, const long args[6], long *ret);

extern long tawcroot_raw_syscall(long nr, long a, long b, long c,
				 long d, long e, long f);
extern long tawcroot_raw_syscall_off_stack(long nr, long a, long b,
					   uint8_t *release);
