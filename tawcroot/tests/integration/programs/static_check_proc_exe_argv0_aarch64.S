/* Aarch64 port of static_check_proc_exe_argv0_x86_64.S — see that
 * file's header for rationale and exit-code key.
 *
 * Aarch64 syscall numbers:
 *   __NR_readlinkat   =  78
 *   __NR_exit_group   =  94
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #1
	b.lt    .Lfail_argc

	/* readlinkat(AT_FDCWD, "/proc/self/exe", buf, 1024). nr=78. */
	mov     x0, #-100
	ldr     x1, =exe_path
	ldr     x2, =link_buf
	mov     x3, #1024
	mov     x8, #78
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_readlink
	cbz     x0, .Lfail_empty

	/* NUL-terminate at offset x0. */
	ldr     x9, =link_buf
	mov     w10, #0
	strb    w10, [x9, x0]

	/* strcmp(link_buf, argv[0]). */
	mov     x10, x9
	ldr     x11, [sp, #8]            /* argv[0] */
.Lcmp:
	ldrb    w12, [x10]
	ldrb    w13, [x11]
	cmp     w12, w13
	b.ne    .Lfail_mismatch
	cbz     w12, .Lmatch
	add     x10, x10, #1
	add     x11, x11, #1
	b       .Lcmp

.Lmatch:
	mov     x0, #42
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_readlink:
	mov     x0, #91
	b       .Lexit
.Lfail_empty:
	mov     x0, #92
	b       .Lexit
.Lfail_argc:
	mov     x0, #93
	b       .Lexit
.Lfail_mismatch:
	mov     x0, #95
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start

	.section .rodata
exe_path:
	.asciz  "/proc/self/exe"

	.bss
link_buf:
	.zero   1025

	.section .note.GNU-stack,"",%progbits
