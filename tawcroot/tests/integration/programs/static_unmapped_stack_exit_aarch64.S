/* Aarch64 static guest fixture: musl's detached-thread exit.
 *
 * Runs on an mmap'd stack with no altstack, makes one trapped
 * rt_sigprocmask (as libc does around thread exit), munmaps that stack
 * and calls exit(42) with SP pointing at unmapped memory — musl's
 * __unmapself. exit(2) is trapped, so its SIGSYS frame needs a stack
 * that isn't the guest's: SIGSEGV death = no fallback altstack.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x0, #0             /* mmap(NULL, 65536, RW, PRIVATE|ANON, -1, 0) */
	mov     x1, #65536
	mov     x2, #3
	mov     x3, #0x22
	mov     x4, #-1
	mov     x5, #0
	mov     x8, #222
	svc     #0
	mov     x19, x0
	add     x0, x0, #65536
	mov     sp, x0

	mov     x0, #0             /* rt_sigprocmask(SIG_BLOCK, NULL, NULL, 8) */
	mov     x1, #0
	mov     x2, #0
	mov     x3, #8
	mov     x8, #135
	svc     #0

	mov     x0, x19            /* munmap(stack, 65536) */
	mov     x1, #65536
	mov     x8, #215
	svc     #0

	mov     x0, #42            /* exit(42) */
	mov     x8, #93
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
