/* Aarch64 port of static_fork_exec_argv1_x86_64.S — see that file's
 * header for rationale and exit-code key.
 *
 * Aarch64 syscall numbers used:
 *   __NR_clone       = 220
 *   __NR_execve      = 221
 *   __NR_wait4       = 260
 *   __NR_exit_group  =  94
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	mov     x0, #17                  /* SIGCHLD                       */
	mov     x1, #0
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x8, #220                 /* __NR_clone                    */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_clone
	cbz     x0, .Lchild

	/* Parent: wait4(pid, &status, 0, NULL) */
	ldr     x1, =status
	mov     x2, #0
	mov     x3, #0
	mov     x8, #260                 /* __NR_wait4                    */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_wait

	ldr     x10, =status
	ldr     w11, [x10]
	and     w12, w11, #0x7f
	cbnz    w12, .Lchild_signaled
	ubfx    w0, w11, #8, #8
	b       .Lexit

.Lchild:
	/* execve(argv[1], &argv[1], envp_empty). argv lives at sp+8;
	 * argv[1] at sp+16. */
	ldr     x0, [sp, #16]            /* path = argv[1]                */
	add     x1, sp, #16              /* argv = &argv[1]               */
	ldr     x2, =envp_empty          /* envp                          */
	mov     x8, #221                 /* __NR_execve                   */
	svc     #0

	/* execve returned -errno; encode 90..150. */
	neg     x0, x0
	cmp     x0, #60
	b.le    1f
	mov     x0, #60
1:
	add     x0, x0, #90
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_clone:
	mov     x0, #70
	b       .Lexit
.Lfail_wait:
	mov     x0, #71
	b       .Lexit
.Lchild_signaled:
	mov     x0, #72
	b       .Lexit
.Lfail_argc:
	mov     x0, #73
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start

	.section .rodata
envp_empty:
	.quad   0

	.bss
status:
	.quad   0

	.section .note.GNU-stack,"",%progbits
