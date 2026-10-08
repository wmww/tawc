/* aarch64 raw syscall stub + program entry.
 *
 * Mirror of x86_64_stub.S — see that file for the contract on each
 * exported symbol (tawcroot_raw_syscall{,_insn,_ret}, _start) and why
 * the labels are positioned exactly where they are.
 *
 * AAPCS64 args: x0..x7 + stack
 * Linux aarch64 syscall: x8 (nr), x0..x5 (args)
 *
 * tawcroot_raw_syscall(nr, a, b, c, d, e, f):
 *   x0 = nr   x1 = a    x2 = b    x3 = c
 *   x4 = d    x5 = e    x6 = f
 * Shift order: nr first (frees x0), then args 1..5, finally f from x6
 * into x5. Each move clobbers a register only after its previous use.
 */

	.text

	.global tawcroot_raw_syscall
	.hidden tawcroot_raw_syscall
	.type   tawcroot_raw_syscall, %function
tawcroot_raw_syscall:
	mov     x8, x0               /* nr  -> x8                            */
	mov     x0, x1               /* a   -> x0                            */
	mov     x1, x2               /* b   -> x1                            */
	mov     x2, x3               /* c   -> x2                            */
	mov     x3, x4               /* d   -> x3                            */
	mov     x4, x5               /* e   -> x4                            */
	mov     x5, x6               /* f   -> x5                            */
	.global tawcroot_raw_syscall_insn
	.hidden tawcroot_raw_syscall_insn
tawcroot_raw_syscall_insn:
	svc     #0
	.global tawcroot_raw_syscall_ret
	.hidden tawcroot_raw_syscall_ret
tawcroot_raw_syscall_ret:
	ret
	.size tawcroot_raw_syscall, .-tawcroot_raw_syscall

	/* tawcroot_raw_syscall_off_stack(nr, a, b, release) — see
	 * raw_sys.h and the x86_64 twin. The stub's `ret` goes through
	 * x30, so SP just has to point somewhere off the altstack; this
	 * function never touches the stack at all, so the `release` byte
	 * can be zeroed anywhere before the syscall. */
	.global tawcroot_raw_syscall_off_stack
	.hidden tawcroot_raw_syscall_off_stack
	.type   tawcroot_raw_syscall_off_stack, %function
tawcroot_raw_syscall_off_stack:
	mov     x9, sp
	mov     x10, x30
	mov     x8, x0
	mov     x0, x1
	mov     x1, x2
	cbz     x3, 1f
	stlrb   wzr, [x3]
1:	adrp    x11, .Loff_stack_sp
	add     x11, x11, :lo12:.Loff_stack_sp
	mov     sp, x11
	adr     x30, 2f
	b       tawcroot_raw_syscall_insn
2:	mov     sp, x9
	ret     x10
	.size tawcroot_raw_syscall_off_stack, .-tawcroot_raw_syscall_off_stack

	.section .rodata
	.balign 16
.Loff_stack_sp:
	.quad   0, 0
	.text

	/* Initial stack (sp on entry):
	 *   [sp+0]   argc
	 *   [sp+8]   argv[0]
	 *   ...
	 * AAPCS requires sp 16-byte aligned, which the kernel guarantees.
	 * Frame-pointer (x29) and link-register (x30) are zeroed for stack
	 * unwinder sanity.
	 */
	.global _start
	.hidden _start
	.type   _start, %function
_start:
	mov     x29, xzr
	mov     x30, xzr
	ldr     x0, [sp]             /* argc                                 */
	add     x1, sp, #8           /* argv                                 */
	bl      tawcroot_main
	/* If tawcroot_main returns, exit_group(0) hard. */
	mov     x8, #94              /* SYS_exit_group                       */
	mov     x0, xzr
	svc     #0
	brk     #0
	.size _start, .-_start

	/* aarch64 sa_restorer is unused — the kernel installs its own VDSO
	 * trampoline that calls rt_sigreturn (kernel commit "arm64: signal:
	 * use VDSO sigreturn trampoline if available" + the early
	 * compat-table fallback that always provides one). The handler
	 * still passes a non-NULL `sa_restorer`, both because some
	 * (older) kernels checked for non-zero before zeroing it and because
	 * it keeps one common code path with x86_64. The trampoline body
	 * is harmless even if never reached. */
	.global tawcroot_sigreturn_trampoline
	.hidden tawcroot_sigreturn_trampoline
	.type   tawcroot_sigreturn_trampoline, %function
tawcroot_sigreturn_trampoline:
	mov     x8, #139             /* SYS_rt_sigreturn                       */
	svc     #0
	brk     #0                    /* unreachable                            */
	.size tawcroot_sigreturn_trampoline, .-tawcroot_sigreturn_trampoline

	.section .note.GNU-stack,"",%progbits
