/* Aarch64 port of static_vfork_exec_then_fork_exec_argv1_x86_64.S —
 * see that file's header for rationale and exit-code key.
 *
 * Aarch64 syscall numbers used:
 *   __NR_clone       = 220
 *   __NR_execve      = 221
 *   __NR_wait4       = 260
 *   __NR_nanosleep   = 101
 *   __NR_kill        = 129
 *   __NR_exit_group  =  94
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	/* Stage 1: clone(CLONE_VM|CLONE_VFORK|SIGCHLD). NULL child
	 * stack — CLONE_VFORK suspends the parent until the child
	 * execs, so the child borrows the parent's sp, same as raw
	 * vfork(2). */
	mov     x0, #0x4111              /* CLONE_VM|CLONE_VFORK|SIGCHLD  */
	mov     x1, #0
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x8, #220                 /* __NR_clone                    */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_clone_a
	cbz     x0, .Lexec_child

	/* Parent: wait4(pidA, &status, 0, NULL); require exit 42. */
	ldr     x1, =status
	mov     x2, #0
	mov     x3, #0
	mov     x8, #260                 /* __NR_wait4                    */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_wait_a
	ldr     x10, =status
	ldr     w11, [x10]
	and     w12, w11, #0x7f
	cbnz    w12, .Lfail_sig_a
	ubfx    w0, w11, #8, #8
	cmp     w0, #42
	b.ne    .Lfail_code_a

	/* Stage 2: plain clone(SIGCHLD) + execve. With the leaked lock
	 * this child inherits exec_lock=1 and spins in the handler. */
	mov     x0, #17                  /* SIGCHLD                       */
	mov     x1, #0
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x8, #220                 /* __NR_clone                    */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_clone_b
	cbz     x0, .Lexec_child
	mov     x21, x0                  /* pidB                          */
	mov     x20, #100                /* 100 x 100 ms polls = ~10 s    */

.Lpoll:
	mov     x0, x21
	ldr     x1, =status
	mov     x2, #1                   /* WNOHANG                       */
	mov     x3, #0
	mov     x8, #260                 /* __NR_wait4                    */
	svc     #0
	cmp     x0, x21
	b.eq    .Lreaped_b
	cmp     x0, #0
	b.lt    .Lfail_wait_b
	ldr     x0, =ts_100ms
	mov     x1, #0
	mov     x8, #101                 /* __NR_nanosleep                */
	svc     #0
	subs    x20, x20, #1
	b.ne    .Lpoll

	/* Timed out — kill the wedged child, report the regression. */
	mov     x0, x21
	mov     x1, #9                   /* SIGKILL                       */
	mov     x8, #129                 /* __NR_kill                     */
	svc     #0
	mov     x0, #80
	b       .Lexit

.Lreaped_b:
	ldr     x10, =status
	ldr     w11, [x10]
	and     w12, w11, #0x7f
	cbnz    w12, .Lfail_sig_b
	ubfx    w0, w11, #8, #8          /* forward (42 on success)       */
	b       .Lexit

.Lexec_child:
	/* execve(argv[1], &argv[1], envp_empty); on failure 90+errno.
	 * argv lives at sp+8; argv[1] at sp+16. */
	ldr     x0, [sp, #16]
	add     x1, sp, #16
	ldr     x2, =envp_empty
	mov     x8, #221                 /* __NR_execve                   */
	svc     #0
	neg     x0, x0
	cmp     x0, #60
	b.le    1f
	mov     x0, #60
1:
	add     x0, x0, #90
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_clone_a:
	mov     x0, #70
	b       .Lexit
.Lfail_wait_a:
	mov     x0, #71
	b       .Lexit
.Lfail_sig_a:
	mov     x0, #72
	b       .Lexit
.Lfail_argc:
	mov     x0, #73
	b       .Lexit
.Lfail_code_a:
	mov     x0, #74
	b       .Lexit
.Lfail_clone_b:
	mov     x0, #75
	b       .Lexit
.Lfail_wait_b:
	mov     x0, #81
	b       .Lexit
.Lfail_sig_b:
	mov     x0, #82
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start

	.section .rodata
envp_empty:
	.quad   0
ts_100ms:
	.quad   0                        /* tv_sec                        */
	.quad   100000000                /* tv_nsec                       */

	.bss
status:
	.quad   0

	.section .note.GNU-stack,"",%progbits
