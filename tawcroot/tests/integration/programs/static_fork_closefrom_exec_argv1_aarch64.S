/* Aarch64 port of static_fork_closefrom_exec_argv1_x86_64.S — see
 * that file's header for rationale and exit-code key.
 *
 * Aarch64 syscall numbers:
 *   __NR_clone        = 220
 *   __NR_close        =  57
 *   __NR_close_range  = 436
 *   __NR_execve       = 221
 *   __NR_wait4        = 260
 *   __NR_exit_group   =  94
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	mov     x0, #17
	mov     x1, #0
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x8, #220
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_clone
	cbz     x0, .Lchild

	ldr     x1, =status
	mov     x2, #0
	mov     x3, #0
	mov     x8, #260
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
	/* close(1000..1003) — exercise BPF close fastpath + handle_close
	 * lying success on reserved fds. */
	mov     x0, #1000
	mov     x8, #57
	svc     #0
	mov     x0, #1001
	mov     x8, #57
	svc     #0
	mov     x0, #1002
	mov     x8, #57
	svc     #0
	mov     x0, #1003
	mov     x8, #57
	svc     #0

	/* close_range(0, ~0u, 0). */
	mov     x0, #0
	mov     w1, #-1
	mov     x2, #0
	mov     x8, #436
	svc     #0

	/* execve(argv[1], &argv[1], envp_empty) */
	ldr     x0, [sp, #16]
	add     x1, sp, #16
	ldr     x2, =envp_empty
	mov     x8, #221
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
