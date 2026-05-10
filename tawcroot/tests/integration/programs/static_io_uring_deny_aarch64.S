/* Aarch64 port of static_io_uring_deny_x86_64.S — see that file's
 * header for rationale and exit-code key.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x0, #0
	mov     x1, #0
	mov     x8, #425                 /* io_uring_setup                */
	svc     #0
	cmp     x0, #-38
	b.ne    .Lfail_setup

	mov     x0, #99999
	mov     x1, #0
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x5, #0
	mov     x8, #426                 /* io_uring_enter                */
	svc     #0
	cmp     x0, #-38
	b.ne    .Lfail_enter

	mov     x0, #99999
	mov     x1, #0
	mov     x2, #0
	mov     x3, #0
	mov     x8, #427                 /* io_uring_register             */
	svc     #0
	cmp     x0, #-38
	b.ne    .Lfail_register

	mov     x0, #42
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_setup:
	mov     x0, #91
	b       .Lexit
.Lfail_enter:
	mov     x0, #92
	b       .Lexit
.Lfail_register:
	mov     x0, #93
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start
	.section .note.GNU-stack,"",%progbits
