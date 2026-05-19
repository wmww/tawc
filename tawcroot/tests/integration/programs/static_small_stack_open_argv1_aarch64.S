/* Aarch64 port of static_small_stack_open_argv1_x86_64.S. */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc
	ldr     x9, [sp, #16]
	ldr     x10, =pathptr
	str     x9, [x10]

	/* mmap(NULL, 16 KiB, PROT_READ|PROT_WRITE,
	 *      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
	mov     x0, #0
	mov     x1, #16384
	mov     x2, #3
	mov     x3, #0x22
	mov     x4, #-1
	mov     x5, #0
	mov     x8, #222
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_mmap

	/* clone(SIGCHLD, stack_top, 0, 0, 0) */
	add     x1, x0, #16384
	mov     x0, #17
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x8, #220
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_clone
	cbz     x0, .Lchild

	mov     x9, x0
	mov     x0, x9
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
	mov     x0, #-100
	ldr     x10, =pathptr
	ldr     x1, [x10]
	mov     x2, #(1 | 0100 | 01000)
	mov     x3, #0x180
	mov     x8, #56
	svc     #0
	cmp     x0, #0
	b.lt    .Lchild_open_fail

	mov     x8, #57
	svc     #0

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
	b       .Lexit
.Lfail_mmap:
	mov     x0, #74
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start

	.bss
status:
	.quad   0
pathptr:
	.quad   0

	.section .note.GNU-stack,"",%progbits
