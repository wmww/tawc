/* Aarch64 port of static_expect_erofs_argv1_x86_64.S — see that file's
 * header. Same exit-code key:
 *   42  — openat returned -EROFS (success)
 *   200 — argc < 2
 *   202 — openat unexpectedly SUCCEEDED (write-through — the bug)
 *   203 — openat failed with some other errno
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	mov     x0, #-100               /* AT_FDCWD                      */
	ldr     x1, [sp, #16]           /* argv[1]                       */
	mov     x2, #(1 | 0100 | 01000) /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     x3, #0x180              /* mode 0600                     */
	mov     x8, #56                 /* __NR_openat                   */
	svc     #0
	cmn     x0, #30                 /* x0 == -EROFS ?                */
	b.eq    .Lpass
	cmp     x0, #0
	b.ge    .Lfail_succeeded
	mov     x0, #203
	b       .Lexit

.Lpass:
	mov     x0, #42
	b       .Lexit
.Lfail_argc:
	mov     x0, #200
	b       .Lexit
.Lfail_succeeded:
	mov     x0, #202
.Lexit:
	mov     x8, #94                 /* __NR_exit_group               */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
