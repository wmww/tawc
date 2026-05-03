/* Aarch64 port of static_execve_exit42_x86_64.S — execve("/bin/static_exit42").
 *
 * Aarch64 syscall convention: x8 = nr, x0..x5 = args, svc #0.
 *   __NR_execve     = 221
 *   __NR_exit_group = 94
 *
 * Built `-static -nostdlib -no-pie`; we use the assembler literal pool
 * (`ldr xN, =label`) to load absolute addresses so we don't need PIE-style
 * adrp+lo12: relocations against .rodata.
 *
 * On exec failure, x0 holds -errno; we encode it as exit_group(90 + min(60, errno)).
 * Layout matches the x86_64 fixture's exit-code key (40..89 reserved for
 * tawcroot loader-driver self-failures, 90..150 for execve failures).
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, =path                /* path */
	ldr     x1, =argv                /* argv */
	ldr     x2, =envp                /* envp */
	mov     x8, #221                 /* __NR_execve */
	svc     #0

	/* execve returned -errno (it shouldn't, on success). Negate, cap
	 * at 60, add 90, exit_group. */
	neg     x0, x0
	cmp     x0, #60
	b.le    .Lcap_ok
	mov     x0, #60
.Lcap_ok:
	add     x0, x0, #90
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .rodata
path:
	.asciz  "/bin/static_exit42"
	.balign 8
argv:
	.quad   path
	.quad   0
envp:
	.quad   0
	.section .note.GNU-stack,"",%progbits
