/* Static x86_64 fixture: clone(SIGCHLD), child close_range(0, ~0u, 0)
 * then execve(argv[1]); parent waits and forwards child's exit code.
 *
 * Mirrors the gpgme/glibc fork-then-closefrom-then-exec pattern that
 * found the reserved-fd survival regressions documented in
 * notes/tawcroot.md "Phase 5c — full integration suite":
 *
 *   - close-loop death-spiral: glibc's __closefrom_fallback enumerates
 *     /proc/self/fd via getdents64 and close()s each fd. Pre-fix,
 *     handle_close lied success on reserved fds but they kept showing
 *     up in the dirent listing → infinite loop.
 *   - bind src host paths must survive: --exec-child re-establishes
 *     the bind table from `bind.src[256]` after the closefrom loses
 *     the original bind src_fds.
 *   - close_range trim: handle_close_range must trim `last` to
 *     RESERVED_FD_BASE-1 so the kernel-side close_range never sees
 *     reserved fds.
 *
 * If any of those bugs regress, the child either spins forever (no
 * exit) or the new tawcroot from --exec-child fails to set up rootfs
 * and the child dies with a tawcroot loader code (60..89).
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib -no-pie.
 *
 * argv: [self_path, child_exec_path]
 *
 * Exit code key (same as static_fork_exec_argv1):
 *    Child's exit code on success
 *    70  — clone(2) failed
 *    71  — wait4(2) failed
 *    72  — child died on signal
 *    73  — argc < 2
 *  90..150 — child's execve failed
 *
 * Note: close_range is trapped by tawcroot, so even if the kernel is
 * too old to support it directly, the SIGSYS handler routes it. We
 * also try plain close() of a few well-known reserved fd numbers
 * (1000..1003) to exercise the BPF close fast-path.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	mov     $17, %rdi
	xor     %rsi, %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	xor     %r8,  %r8
	mov     $56, %rax           /* SYS_clone */
	syscall
	test    %rax, %rax
	js      .Lfail_clone
	jz      .Lchild

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
	/* close(1000), close(1001), close(1002), close(1003) — exercise
	 * the BPF close-fastpath + handle_close lie-success on reserved
	 * fds. We don't check return values; we only care that the
	 * subsequent execve succeeds. (handle_close returns 0 for
	 * reserved fds without forwarding to the kernel; -EBADF is fine
	 * if the slot wasn't reserved on this run.)
	 *
	 * NB: tawcroot reserves rootfs_fd + bind src_fds starting at
	 * 1000 in order of registration. The single-bind test rootfs
	 * with no -b flags has rootfs_fd = 1000 and that's it; with -b
	 * flags the bind src_fds follow at 1001, 1002, …. We hit a
	 * conservative range. */
	mov     $1000, %rdi
	mov     $3, %rax            /* SYS_close */
	syscall
	mov     $1001, %rdi
	mov     $3, %rax
	syscall
	mov     $1002, %rdi
	mov     $3, %rax
	syscall
	mov     $1003, %rdi
	mov     $3, %rax
	syscall

	/* close_range(0, ~0u, 0) — close everything from stdin onwards.
	 * handle_close_range trims `last` to RESERVED_FD_BASE-1 so the
	 * kernel never sees our internal slots; reserved fds survive
	 * untouched. After this returns the guest has no open fds in
	 * the [0, 999] range; the upcoming execve will pick stdin/out/
	 * err back up via /dev/null only if we cared (we don't — the
	 * child binary doesn't print). */
	xor     %rdi, %rdi
	mov     $-1, %esi
	xor     %rdx, %rdx
	mov     $436, %rax          /* SYS_close_range */
	syscall

	/* Now execve(argv[1], &argv[1], envp_empty). Same shape as
	 * static_fork_exec_argv1's child path. */
	mov     16(%rsp), %rdi
	lea     16(%rsp), %rsi
	movabs  $envp_empty, %rdx
	mov     $59, %rax           /* SYS_execve */
	syscall

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
