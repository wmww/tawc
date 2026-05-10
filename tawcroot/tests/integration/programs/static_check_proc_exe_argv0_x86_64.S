/* Static x86_64 fixture: readlink("/proc/self/exe", buf, ...);
 * compare buf with argv[0]; exit_group(42) on match.
 *
 * Used by tests/integration/test_prod_fork.c to validate that
 * /proc/self/exe resolves to the *current* exec target, not the
 * original guest binary, after a fork+execve. The Firefox libxul.so
 * regression (notes/tawcroot.md "Phase 5c"; exec_handler.c:91-101
 * comment) was: fork-and-exec'd descendants saw the *parent's*
 * stashed `tawcroot_guest_exe_path` ("/bin/bash") through
 * /proc/self/exe instead of their own exec target ("/usr/bin/firefox"),
 * so the firefox stub couldn't $ORIGIN-relative dlopen libxul.
 *
 * The fix in exec_handler.c sets `extras.guest_exe = NULL` when
 * exporting state for --exec-child, so loader_exec_child falls back
 * to `st.path` (the actual exec target). This test runs from a
 * fork-child of static_fork_exec_argv1 — the call site must be a
 * fork-child execve, not a top-level execve, because:
 *   - top-level: prod_rootfs_init seeds guest_exe from argv[cmd_start]
 *     (the only exec we ever asked for); /proc/self/exe is correct.
 *   - fork-child execve: handler dispatches to exec_handler_perform,
 *     which writes a NEW exec_state with guest_exe=NULL; the
 *     re-init in loader_exec_child then sets guest_exe from st.path.
 *     If that NULL handoff regresses, this test fails.
 *
 * argv: [self_path] (after the fork+exec parent strips its own argv0,
 * the new process's argv[0] IS the path the fork-child execve'd to,
 * which is what we expect /proc/self/exe to read back as).
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib -no-pie.
 *
 * Exit code key:
 *    42 — readlink == argv[0]
 *    91 — readlink failed
 *    92 — readlink returned 0 bytes
 *    93 — argc < 1 (no argv[0] to compare against)
 *    95 — readlink result != argv[0]
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax
	cmp     $1, %rax
	jl      .Lfail_argc

	/* readlinkat(AT_FDCWD, "/proc/self/exe", buf, sizeof buf).
	 * nr=267. */
	mov     $-100, %rdi
	movabs  $exe_path, %rsi
	movabs  $link_buf, %rdx
	mov     $1024, %r10
	mov     $267, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_readlink
	jz      .Lfail_empty

	/* NUL-terminate the link buffer at offset rax. */
	movabs  $link_buf, %rdi
	movb    $0, (%rdi, %rax)

	/* strcmp(link_buf, argv[0]). */
	mov     8(%rsp), %rsi            /* argv[0] */
.Lcmp:
	movzbl  (%rdi), %eax
	movzbl  (%rsi), %ecx
	cmp     %eax, %ecx
	jne     .Lfail_mismatch
	test    %al, %al
	jz      .Lmatch                  /* both NUL → equal */
	inc     %rdi
	inc     %rsi
	jmp     .Lcmp

.Lmatch:
	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_readlink:
	mov     $91, %rdi
	jmp     .Lexit
.Lfail_empty:
	mov     $92, %rdi
	jmp     .Lexit
.Lfail_argc:
	mov     $93, %rdi
	jmp     .Lexit
.Lfail_mismatch:
	mov     $95, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start

	.section .rodata
exe_path:
	.asciz  "/proc/self/exe"

	.bss
link_buf:
	.zero   1025

	.section .note.GNU-stack,"",@progbits
