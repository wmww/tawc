/* Aarch64 fixture: open(argv[1], O_PATH|O_CLOEXEC), then
 * execveat(fd, "", argv+1, envp_empty, AT_EMPTY_PATH).
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x9, [sp]                 /* argc */
	cmp     x9, #2
	b.lt    .Lfail_argc

	mov     x0, #-100                /* AT_FDCWD */
	ldr     x1, [sp, #16]            /* argv[1] */
	movz    x2, #0x28, lsl #16       /* O_PATH | O_CLOEXEC */
	mov     x3, #0
	mov     x8, #56                  /* __NR_openat */
	svc     #0
	tbnz    x0, #63, .Lfail_open
	mov     x19, x0

	mov     x0, x19                  /* fd */
	ldr     x1, =empty_path
	add     x2, sp, #16              /* argv = &argv[1] */
	ldr     x3, =envp_empty
	mov     x4, #0x1000              /* AT_EMPTY_PATH */
	mov     x8, #281                 /* __NR_execveat */
	svc     #0

	neg     x0, x0
	cmp     x0, #60
	b.le    1f
	mov     x0, #60
1:
	add     x0, x0, #90
	b       .Lexit

.Lfail_argc:
	mov     x0, #73
	b       .Lexit
.Lfail_open:
	neg     x0, x0
	cmp     x0, #20
	b.le    1f
	mov     x0, #20
1:
	add     x0, x0, #70
.Lexit:
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0
	brk     #0

	.size _start, .-_start

	.section .rodata
empty_path:
	.asciz  ""
	.balign 8
envp_empty:
	.quad   0

	.section .note.GNU-stack,"",%progbits
