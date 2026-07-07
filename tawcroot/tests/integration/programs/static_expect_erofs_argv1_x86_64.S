/* Static x86_64 fixture: open(argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600)
 * and expect -EROFS.
 *
 * Used by the read-only-bind integration tests in test_prod_rootfs.c:
 * argv[1] points into a `-b src:dst:ro` bind, so tawcroot's central
 * RO check must refuse the write-mode open with EROFS. Run both
 * directly and as the execve target of static_fork_exec_argv1 (the
 * exec_state v6 bind_ro ferry).
 *
 * Exit code encoding (distinct from loader codes 60..89 and the other
 * fixtures' 90..227 bands — 42 is the shared success value):
 *   42  — open returned -EROFS (success)
 *   200 — argc < 2 (argv plumbing broken)
 *   202 — open unexpectedly SUCCEEDED (write-through — the bug)
 *   203 — open failed with some other errno
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax              /* argc                          */
	cmp     $2, %rax
	jl      .Lfail_argc

	mov     16(%rsp), %rdi            /* path = argv[1]                */
	mov     $(1 | 0100 | 01000), %rsi /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     $0x180, %rdx              /* mode 0600                     */
	mov     $2, %rax                  /* SYS_open                      */
	syscall
	cmp     $-30, %rax                /* -EROFS?                       */
	je      .Lpass
	test    %rax, %rax
	jns     .Lfail_succeeded
	mov     $203, %rdi
	jmp     .Lexit

.Lpass:
	mov     $42, %rdi
	jmp     .Lexit
.Lfail_argc:
	mov     $200, %rdi
	jmp     .Lexit
.Lfail_succeeded:
	mov     $202, %rdi
.Lexit:
	mov     $231, %rax                /* SYS_exit_group                */
	syscall
	hlt
	.size _start, .-_start

	.section .note.GNU-stack,"",@progbits
