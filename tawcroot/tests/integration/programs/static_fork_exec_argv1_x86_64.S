/* Static x86_64 fixture: clone(SIGCHLD), child execve(argv[1]),
 * parent waits and forwards child's exit code.
 *
 * Used by tests/integration/test_prod_fork.c to exercise the full
 * bash-style fork+execve chain end-to-end through tawcroot:
 *
 *   guest fork → child execve → SIGSYS handler → exec_handler_perform
 *   → execveat(/proc/self/exe, --exec-child <fd>) → new tawcroot
 *   re-establishes rootfs view + handler from exec_state, manual-
 *   loads the requested guest binary → that binary runs.
 *
 * This is the path notes/tawcroot/phasing.md "phase-5b" calls out as the
 * regression surface where (a) NR 221 execve was missing from the
 * aarch64 dispatch, (b) AT_EXECFN incorrectly stuck to the original
 * guest binary across exec, (c) the inherited SIGSYS-blocked sigmask
 * killed the new tawcroot before its handler ran. None of those bugs
 * surface from a non-fork execve(2) of the original guest.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib.
 *
 * argv: [self_path, child_exec_path]
 *
 * Exit code key:
 *    Anything 0..89 — child's exit code, forwarded by parent (so the
 *                     callsite expects e.g. 42 if child exec'd a
 *                     /bin/static_exit42 fixture)
 *    70  — clone(2) returned -errno
 *    71  — wait4(2) returned -errno
 *    72  — child died on signal (parent saw !WIFEXITED)
 *    73  — argc < 2 in parent
 *  90..150 — child's execve failed: 90 + min(errno, 60)
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	mov     $17, %rdi          /* SIGCHLD */
	xor     %rsi, %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	xor     %r8,  %r8
	mov     $56, %rax          /* SYS_clone */
	syscall
	test    %rax, %rax
	js      .Lfail_clone
	jz      .Lchild

	/* Parent: wait4(pid, &status, 0, NULL) */
	mov     %rax, %rdi
	movabs  $status, %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	mov     $61, %rax           /* SYS_wait4 */
	syscall
	test    %rax, %rax
	js      .Lfail_wait

	mov     status(%rip), %eax
	test    $0x7f, %al
	jnz     .Lchild_signaled
	shr     $8, %eax
	and     $0xff, %eax
	mov     %eax, %edi
	jmp     .Lexit

.Lchild:
	/* execve(argv[1], argv+1, envp_empty). argv+1 still points at the
	 * inherited stack layout; we just hand the kernel a pointer to
	 * argv[1] for the array, and a NULL-only envp.
	 *
	 * Stack: rsp=argc; argv[0] at rsp+8; argv[1] at rsp+16; ...;
	 * argv[argc] = NULL is at rsp+8+8*argc — already there because
	 * the kernel laid it out for us. So the kernel-built argv
	 * starting at rsp+16 is "{argv[1], argv[2], ..., NULL}", which
	 * is exactly what execve wants for "child's argv0 = argv[1]". */
	mov     16(%rsp), %rdi          /* path = argv[1] */
	lea     16(%rsp), %rsi          /* argv = &argv[1] */
	movabs  $envp_empty, %rdx
	mov     $59, %rax               /* SYS_execve */
	syscall

	/* execve returned -errno. Encode 90..150. */
	neg     %rax
	cmp     $60, %rax
	jbe     1f
	mov     $60, %rax
1:
	add     $90, %rax
	mov     %rax, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_clone:
	mov     $70, %rdi
	jmp     .Lexit
.Lfail_wait:
	mov     $71, %rdi
	jmp     .Lexit
.Lchild_signaled:
	mov     $72, %rdi
	jmp     .Lexit
.Lfail_argc:
	mov     $73, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start

	.section .rodata
envp_empty:
	.quad   0

	.bss
status:
	.quad   0

	.section .note.GNU-stack,"",@progbits
