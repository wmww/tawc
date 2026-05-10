/* Aarch64 port of static_check_proc_self_fd_x86_64.S — see that
 * file's header for rationale and exit-code key.
 *
 * Aarch64 syscall numbers:
 *   __NR_openat       =  56
 *   __NR_getdents64   =  61
 *   __NR_exit_group   =  94
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	/* openat(AT_FDCWD, "/proc/self/fd", O_RDONLY|O_DIRECTORY).
	 * arm64 overrides asm-generic: O_DIRECTORY = 040000 (0x4000),
	 * O_DIRECT = 0200000. (x86_64 keeps the asm-generic 0200000 for
	 * O_DIRECTORY.) */
	mov     x0, #-100
	ldr     x1, =procfd_path
	mov     x2, #040000              /* O_DIRECTORY (arm64)           */
	mov     x3, #0
	mov     x8, #56
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_open
	mov     x12, x0                  /* dirfd                         */

	ldr     x1, =dent_buf
	mov     x2, #4096
	mov     x0, x12
	mov     x8, #61                  /* __NR_getdents64               */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_dents
	cbz     x0, .Lfail_empty

	/* Scan dent_buf for "1000\0". Total bytes in x0. */
	ldr     x9, =dent_buf
	mov     x10, x0                  /* total                         */
	mov     x11, #0                  /* i                             */
.Lscan:
	add     x13, x11, #5
	cmp     x13, x10
	b.gt    .Lscan_done
	ldrb    w14, [x9, x11]
	cmp     w14, #'1'
	b.ne    .Lscan_next
	add     x15, x11, #1
	ldrb    w14, [x9, x15]
	cmp     w14, #'0'
	b.ne    .Lscan_next
	add     x15, x11, #2
	ldrb    w14, [x9, x15]
	cmp     w14, #'0'
	b.ne    .Lscan_next
	add     x15, x11, #3
	ldrb    w14, [x9, x15]
	cmp     w14, #'0'
	b.ne    .Lscan_next
	add     x15, x11, #4
	ldrb    w14, [x9, x15]
	cmp     w14, #0
	b.ne    .Lscan_next
	/* Match. */
	mov     x0, #95
	mov     x8, #94
	svc     #0
	brk     #0
.Lscan_next:
	add     x11, x11, #1
	b       .Lscan
.Lscan_done:

	mov     x0, #42
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_open:
	mov     x0, #91
	mov     x8, #94
	svc     #0
	brk     #0
.Lfail_dents:
	mov     x0, #92
	mov     x8, #94
	svc     #0
	brk     #0
.Lfail_empty:
	mov     x0, #93
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start

	.section .rodata
procfd_path:
	.asciz  "/proc/self/fd"

	.bss
dent_buf:
	.zero   4096

	.section .note.GNU-stack,"",%progbits
