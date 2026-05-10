/* Aarch64 port of static_fork_open_argv1_x86_64.S — see that file's
 * header for the full rationale and exit-code key.
 *
 * Aarch64 syscall numbers used:
 *   __NR_clone       = 220
 *   __NR_openat      =  56
 *   __NR_close       =  57
 *   __NR_wait4       = 260
 *   __NR_exit_group  =  94
 *
 * Aarch64 clone arg order is (flags, child_stack, parent_tid, tls,
 * child_tid) — note tls/child_tid swapped vs. x86_64. Doesn't matter
 * here since everything past flags is zero. AT_FDCWD = -100, same
 * as x86_64.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	/* argc check */
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	/* clone(SIGCHLD=17, 0, 0, 0, 0) */
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
	mov     x9, x0                   /* save pid                      */
	mov     x0, x9
	ldr     x1, =status
	mov     x2, #0
	mov     x3, #0
	mov     x8, #260                 /* __NR_wait4                    */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_wait

	/* Decode *status. WIFEXITED: (status & 0x7f) == 0. */
	ldr     x10, =status
	ldr     w11, [x10]
	and     w12, w11, #0x7f
	cbnz    w12, .Lchild_signaled

	/* WEXITSTATUS = (status >> 8) & 0xff */
	ubfx    w0, w11, #8, #8
	b       .Lexit

.Lchild:
	/* openat(AT_FDCWD, argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600). argv
	 * lives at sp+8 in the inherited initial-stack layout; argv[1] is
	 * at sp+16. Child shares the parent's stack VA via CoW. */
	mov     x0, #-100                /* AT_FDCWD                      */
	ldr     x1, [sp, #16]            /* argv[1]                       */
	mov     x2, #(1 | 0100 | 01000)  /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     x3, #0x180               /* mode 0600                     */
	mov     x8, #56                  /* __NR_openat                   */
	svc     #0
	cmp     x0, #0
	b.lt    .Lchild_open_fail

	/* close(fd). fd in x0. */
	mov     x8, #57                  /* __NR_close                    */
	svc     #0

	/* exit_group(42) */
	mov     x0, #42
	mov     x8, #94
	svc     #0
	brk     #0

.Lchild_open_fail:
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

	.bss
status:
	.quad   0

	.section .note.GNU-stack,"",%progbits
