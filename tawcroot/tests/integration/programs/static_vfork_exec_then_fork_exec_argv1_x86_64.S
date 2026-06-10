/* Static x86_64 fixture: posix_spawn-shaped clone(CLONE_VM|CLONE_VFORK|
 * SIGCHLD) whose child execve's argv[1], then a plain clone(SIGCHLD)
 * whose child execve's argv[1] again. Parent forwards the second
 * child's exit code.
 *
 * Regression surface: syscalls_exec.c's exec_lock leaking through a
 * CLONE_VM child. The vfork child shares the parent's address space,
 * so an exec_lock still held at the execveat commit point stays SET in
 * the parent's memory after the child execs away — and in every later
 * fork snapshot. The stage-2 exec then spins forever inside the SIGSYS
 * handler. This is how Firefox's startup-crash relaunch hung after it
 * posix_spawn'd glxtest. The lock must be dropped before commit
 * (exec_handler.h prepare/commit split).
 *
 * The stage-2 parent polls wait4(WNOHANG) for ~10 s instead of
 * blocking, so a regression exits 80 rather than hanging the suite
 * (the wedged child spins with all signals blocked; only SIGKILL
 * gets through).
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib.
 *
 * argv: [self_path, child_exec_path]   (point argv[1] at a fixture
 *        that exits 42, e.g. /bin/static_exit42)
 *
 * Exit code key:
 *    42  — success (stage-2 child's forwarded exit code)
 *    70  — stage-1 clone(2) returned -errno
 *    71  — stage-1 wait4(2) returned -errno
 *    72  — stage-1 child died on signal
 *    73  — argc < 2
 *    74  — stage-1 child exited but not with 42
 *    75  — stage-2 clone(2) returned -errno
 *    80  — stage-2 child never exited within ~10 s (leaked-lock spin)
 *    81  — stage-2 wait4(2) returned -errno
 *    82  — stage-2 child died on signal
 *  90..150 — a child's execve failed: 90 + min(errno, 60)
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	/* Stage 1: clone(CLONE_VM|CLONE_VFORK|SIGCHLD). NULL child
	 * stack — CLONE_VFORK suspends the parent until the child
	 * execs, so the child borrows the parent's sp, same as raw
	 * vfork(2). */
	mov     $0x4111, %edi      /* CLONE_VM|CLONE_VFORK|SIGCHLD */
	xor     %esi, %esi
	xor     %edx, %edx
	xor     %r10d, %r10d
	xor     %r8d,  %r8d
	mov     $56, %eax          /* SYS_clone */
	syscall
	test    %rax, %rax
	js      .Lfail_clone_a
	jz      .Lexec_child

	/* Parent: wait4(pidA, &status, 0, NULL); require exit 42. */
	mov     %rax, %rdi
	movabs  $status, %rsi
	xor     %edx, %edx
	xor     %r10d, %r10d
	mov     $61, %eax          /* SYS_wait4 */
	syscall
	test    %rax, %rax
	js      .Lfail_wait_a
	mov     status(%rip), %eax
	test    $0x7f, %al
	jnz     .Lfail_sig_a
	shr     $8, %eax
	and     $0xff, %eax
	cmp     $42, %eax
	jne     .Lfail_code_a

	/* Stage 2: plain clone(SIGCHLD) + execve. With the leaked lock
	 * this child inherits exec_lock=1 and spins in the handler. */
	mov     $17, %edi          /* SIGCHLD */
	xor     %esi, %esi
	xor     %edx, %edx
	xor     %r10d, %r10d
	xor     %r8d,  %r8d
	mov     $56, %eax          /* SYS_clone */
	syscall
	test    %rax, %rax
	js      .Lfail_clone_b
	jz      .Lexec_child
	mov     %rax, %rbx         /* pidB */
	mov     $100, %ebp         /* 100 x 100 ms polls = ~10 s */

.Lpoll:
	mov     %rbx, %rdi
	movabs  $status, %rsi
	mov     $1, %edx           /* WNOHANG */
	xor     %r10d, %r10d
	mov     $61, %eax          /* SYS_wait4 */
	syscall
	cmp     %rbx, %rax
	je      .Lreaped_b
	test    %rax, %rax
	js      .Lfail_wait_b
	movabs  $ts_100ms, %rdi
	xor     %esi, %esi
	mov     $35, %eax          /* SYS_nanosleep */
	syscall
	dec     %ebp
	jnz     .Lpoll

	/* Timed out — kill the wedged child, report the regression. */
	mov     %rbx, %rdi
	mov     $9, %esi           /* SIGKILL */
	mov     $62, %eax          /* SYS_kill */
	syscall
	mov     $80, %edi
	jmp     .Lexit

.Lreaped_b:
	mov     status(%rip), %eax
	test    $0x7f, %al
	jnz     .Lfail_sig_b
	shr     $8, %eax
	and     $0xff, %eax
	mov     %eax, %edi          /* forward (42 on success) */
	jmp     .Lexit

.Lexec_child:
	/* execve(argv[1], &argv[1], envp_empty) — see
	 * static_fork_exec_argv1_x86_64.S for the stack-layout note. */
	mov     16(%rsp), %rdi
	lea     16(%rsp), %rsi
	movabs  $envp_empty, %rdx
	mov     $59, %eax           /* SYS_execve */
	syscall

	/* execve returned -errno. Encode 90..150. */
	neg     %rax
	cmp     $60, %rax
	jbe     1f
	mov     $60, %rax
1:
	add     $90, %rax
	mov     %rax, %rdi
	mov     $231, %eax
	syscall
	ud2

.Lfail_clone_a:
	mov     $70, %rdi
	jmp     .Lexit
.Lfail_wait_a:
	mov     $71, %rdi
	jmp     .Lexit
.Lfail_sig_a:
	mov     $72, %rdi
	jmp     .Lexit
.Lfail_argc:
	mov     $73, %rdi
	jmp     .Lexit
.Lfail_code_a:
	mov     $74, %rdi
	jmp     .Lexit
.Lfail_clone_b:
	mov     $75, %rdi
	jmp     .Lexit
.Lfail_wait_b:
	mov     $81, %rdi
	jmp     .Lexit
.Lfail_sig_b:
	mov     $82, %rdi
.Lexit:
	mov     $231, %eax
	syscall
	ud2

	.size _start, .-_start

	.section .rodata
envp_empty:
	.quad   0
ts_100ms:
	.quad   0                  /* tv_sec  */
	.quad   100000000          /* tv_nsec */

	.bss
status:
	.quad   0

	.section .note.GNU-stack,"",@progbits
