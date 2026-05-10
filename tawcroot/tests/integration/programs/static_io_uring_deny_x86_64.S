/* Static x86_64 fixture: probe io_uring_setup, io_uring_register, and
 * io_uring_enter. Each must return -ENOSYS. Exit 42 if all three deny.
 *
 * Validates the defense-in-depth deny on
 * `issues/tawcroot-io-uring-not-fully-intercepted.md`. Without the
 * register/enter handlers, a guest with an inherited ring fd could
 * submit path-bearing SQEs that bypass tawcroot's translation. With
 * the handlers, every io_uring syscall returns -ENOSYS regardless of
 * ring-fd provenance.
 *
 * io_uring NRs are arch-independent on aarch64/x86_64:
 *   io_uring_setup    = 425
 *   io_uring_enter    = 426
 *   io_uring_register = 427
 *
 * Exit code key:
 *    42 — all three returned -ENOSYS (-38)
 *    91 — io_uring_setup returned something else
 *    92 — io_uring_enter returned something else
 *    93 — io_uring_register returned something else
 *
 * Args to each call are bogus on purpose. The expected return is
 * -ENOSYS regardless of arg validity, because the handler short-
 * circuits before any kernel-side validation.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* io_uring_setup(0, NULL): 0 entries, no params */
	xor     %rdi, %rdi
	xor     %rsi, %rsi
	mov     $425, %rax
	syscall
	cmp     $-38, %rax
	jne     .Lfail_setup

	/* io_uring_enter(99999, 0, 0, 0, NULL, 0) — bogus fd; handler
	 * returns -ENOSYS before the kernel checks fd validity. */
	mov     $99999, %rdi
	xor     %rsi, %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	xor     %r8,  %r8
	xor     %r9,  %r9
	mov     $426, %rax
	syscall
	cmp     $-38, %rax
	jne     .Lfail_enter

	/* io_uring_register(99999, 0, NULL, 0) — same shape. */
	mov     $99999, %rdi
	xor     %rsi, %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	mov     $427, %rax
	syscall
	cmp     $-38, %rax
	jne     .Lfail_register

	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_setup:
	mov     $91, %rdi
	jmp     .Lexit
.Lfail_enter:
	mov     $92, %rdi
	jmp     .Lexit
.Lfail_register:
	mov     $93, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
