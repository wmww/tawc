/* Static x86_64 fixture: open(argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600)
 * then exit_group(0).
 *
 * Used by the orphan-bypass regression test in test_exec_child.c.
 * That test orphans a grandchild onto init (gpgme posix_spawn pattern)
 * and exec's tawcroot --exec-child of THIS binary, with argv[1] set to
 * a unique tmp path. If tawcroot's exit_if_orphan check fires (the
 * pre-fix bug), the loader never reaches us and the file is never
 * created; the test polls and asserts it appeared. Marker-on-disk is
 * the cheapest observable side-effect that doesn't require waiting on
 * an unrelated process (`waitid`/`pidfd` still need parentage).
 *
 * Built `-static -nostdlib -no-pie` so it loads at the default base
 * with no PT_INTERP — same shape as static_exit42 etc.
 *
 * Exit code encoding (intentionally distinct from any tawcroot loader
 * code in 60..89, the static_argc_random codes 96..99/100..227, and
 * the static_execve_exit42 codes 90..150):
 *   0   — open succeeded, file created (success)
 *   200 — argc < 2 (argv plumbing broken)
 *   201 — open(2) returned an error
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check: must be >= 2 (program path + marker path) */
	mov     (%rsp), %rax              /* argc                          */
	cmp     $2, %rax
	jl      .Lfail_argc

	/* argv[1] is the marker path. argv lives at rsp+8;
	 * argv[1] = *(rsp + 16). */
	mov     16(%rsp), %rdi            /* path                          */
	mov     $(1 | 0100 | 01000), %rsi /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     $0600, %rdx               /* mode                          */
	mov     $2, %rax                  /* SYS_open                      */
	syscall
	cmp     $0, %rax
	jl      .Lfail_open

	xor     %rdi, %rdi                /* exit_group(0)                 */
	mov     $231, %rax
	syscall
	ud2

.Lfail_argc:
	mov     $200, %rdi
	jmp     .Lexit
.Lfail_open:
	mov     $201, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
