/* x86_64 raw syscall stub + program entry.
 *
 * The exported symbols and what each one is for:
 *   tawcroot_raw_syscall      — C-callable wrapper. Shifts SysV args into
 *                                kernel syscall registers and issues syscall.
 *   tawcroot_raw_syscall_insn — label sitting EXACTLY on the syscall
 *                                instruction. Kept symbolic so tests can
 *                                sanity-check ranges.
 *   tawcroot_raw_syscall_ret  — label immediately AFTER the syscall. This
 *                                is what the kernel reports as
 *                                seccomp_data.instruction_pointer, so the
 *                                filter's IP allowlist matches against it
 *                                (low and high 32 bits separately —
 *                                cBPF is 32-bit, see notes/tawcroot/seccomp-filter.md).
 *   _start                    — kernel-direct entry. Sets up argc/argv
 *                                from the initial stack and tail-calls
 *                                tawcroot_main. No bionic init runs.
 *
 * SysV AMD64 ABI args:  rdi rsi rdx rcx r8  r9  + stack
 * Linux x86_64 syscall: rax rdi rsi rdx r10 r8  r9
 *                        ↑   ↑   ↑   ↑   ↑   ↑   ↑
 *                       nr   a   b   c   d   e   f
 *
 * tawcroot_raw_syscall(nr, a, b, c, d, e, f) — f is on the stack at 8(%rsp)
 * because the call's return address is at (%rsp).
 *
 * The shift order is chosen so we never clobber a source before its read.
 */

	.text

	.global tawcroot_raw_syscall
	.hidden tawcroot_raw_syscall
	.type   tawcroot_raw_syscall, @function
tawcroot_raw_syscall:
	mov     %rdi, %rax           /* nr  -> rax  (rdi now free)            */
	mov     %rsi, %rdi           /* a   -> rdi  (rsi now free)            */
	mov     %rdx, %rsi           /* b   -> rsi  (rdx now free)            */
	mov     %rcx, %rdx           /* c   -> rdx  (rcx now free, syscall    */
	                             /*              would clobber it anyway) */
	mov     %r8,  %r10           /* d   -> r10  (r8 now free)             */
	mov     %r9,  %r8            /* e   -> r8   (r9 now free)             */
	mov     8(%rsp), %r9         /* f from caller's stack slot            */
	.global tawcroot_raw_syscall_insn
	.hidden tawcroot_raw_syscall_insn
tawcroot_raw_syscall_insn:
	syscall
	.global tawcroot_raw_syscall_ret
	.hidden tawcroot_raw_syscall_ret
tawcroot_raw_syscall_ret:
	ret
	.size tawcroot_raw_syscall, .-tawcroot_raw_syscall

	/* _start receives the kernel's initial stack:
	 *   [rsp+0]  argc
	 *   [rsp+8]  argv[0]
	 *   ...
	 *   [...]    NULL terminator, then envp, auxv, strings.
	 *
	 * We pull argc/argv into rdi/rsi for tawcroot_main(int, char **)
	 * and align the stack to 16 bytes before the call (SysV requires
	 * 16-byte alignment at the call site). rbp is zeroed per ABI to
	 * mark the bottom of the call stack.
	 */
	.global _start
	.hidden _start
	.type   _start, @function
_start:
	xor     %rbp, %rbp
	mov     (%rsp), %rdi         /* argc                                  */
	lea     8(%rsp), %rsi        /* argv                                  */
	and     $-16, %rsp           /* align                                 */
	call    tawcroot_main
	/* tawcroot_main is supposed to call sys_exit_group itself; if it
	 * ever returns, fall through to a hard exit so we don't run off
	 * the end of the world. */
	mov     $231, %rax           /* SYS_exit_group                        */
	xor     %rdi, %rdi
	syscall
	hlt
	.size _start, .-_start

	/* rt_sigaction(SA_RESTORER) trampoline.
	 *
	 * The x86_64 kernel rejects rt_sigaction without SA_RESTORER —
	 * see kernel commit "x86: introduce restorer for SigAction" and
	 * the SDM. The trampoline runs after the C handler returns; its
	 * job is to invoke rt_sigreturn(2) which pops the saved
	 * ucontext+siginfo off the signal frame.
	 *
	 * We can call SYSCALL directly here because seccomp inherits and
	 * rt_sigreturn is on the standard allow path of ANY sane filter
	 * (you can't deliver signals if the process can't return from
	 * them). The seccomp filter we install lets it through by
	 * default (only our explicit TRAP set matches; everything else
	 * is ALLOW).
	 */
	.global tawcroot_sigreturn_trampoline
	.hidden tawcroot_sigreturn_trampoline
	.type   tawcroot_sigreturn_trampoline, @function
tawcroot_sigreturn_trampoline:
	mov     $15, %rax            /* SYS_rt_sigreturn                       */
	syscall
	ud2                           /* unreachable                            */
	.size tawcroot_sigreturn_trampoline, .-tawcroot_sigreturn_trampoline

	.section .note.GNU-stack,"",@progbits
