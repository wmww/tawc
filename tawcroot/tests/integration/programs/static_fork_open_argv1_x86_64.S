/* Static x86_64 fixture: clone(SIGCHLD), child openat-creates argv[1],
 * parent waits and forwards child's exit code.
 *
 * Used by tests/integration/test_prod_fork.c to catch the
 * "process_vm_readv must use the child's tid" class of regression
 * (notes/tawcroot.md "More phase-5b bugs"; usercopy.c:14-22). The
 * fork-child issues a path-translated openat — the SIGSYS handler
 * runs in the child's process, and path translation copies the path
 * string out of the child's VM via process_vm_readv. If the handler
 * cached the parent's tid, the child's openat would read the wrong
 * VM and surface as -EFAULT. The success path here proves
 * cross-process tid invalidation actually works.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib -no-pie.
 *
 * argv: [self_path, marker_path]
 *
 * Exit code key:
 *    42 — fork worked, child opened+created argv[1], parent reaped 42
 *    70 — clone(2) returned -errno
 *    71 — wait4(2) returned -errno
 *    72 — child died on signal (parent saw !WIFEXITED)
 *    73 — argc < 2 in parent (argv plumbing broken)
 *  90..150 — child's openat failed: 90 + min(errno, 60)
 *  Anything else — child's exit code (e.g. its own argc/path failure)
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check: argv = self, marker_path */
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	/* clone(SIGCHLD, 0, 0, 0, 0): all zero except flags. The arg order
	 * for the raw syscall is (flags, newsp, parent_tid, child_tid, tls)
	 * on x86_64, but everything past flags is 0 so order is moot. */
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

	/* Parent: wait4(child_pid, &status, 0, NULL). status lives in our
	 * .bss, CoW-shared with the child. Child exits before parent reads,
	 * so no race. */
	mov     %rax, %rdi          /* pid */
	movabs  $status, %rsi       /* &status */
	xor     %rdx, %rdx          /* options = 0 */
	xor     %r10, %r10          /* rusage = NULL */
	mov     $61, %rax           /* SYS_wait4 */
	syscall
	test    %rax, %rax
	js      .Lfail_wait

	/* WIFEXITED: low 7 bits of *status are zero. */
	mov     status(%rip), %eax
	test    $0x7f, %al
	jnz     .Lchild_signaled

	/* WEXITSTATUS = (status >> 8) & 0xff. Forward as our own. */
	shr     $8, %eax
	and     $0xff, %eax
	mov     %eax, %edi
	jmp     .Lexit

.Lchild:
	/* openat(AT_FDCWD, argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600).
	 * argv[1] sits at *(rsp + 16) — the kernel laid out the stack
	 * pre-clone, and child shares the same VA via CoW, so it's still
	 * valid here. */
	mov     $-100, %rdi               /* AT_FDCWD                      */
	mov     16(%rsp), %rsi            /* argv[1]                       */
	mov     $(1 | 0100 | 01000), %rdx /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     $0x180, %r10              /* mode 0600                     */
	mov     $257, %rax                /* SYS_openat                    */
	syscall
	test    %rax, %rax
	js      .Lchild_open_fail

	/* close(fd) — be tidy. fd in rax. */
	mov     %rax, %rdi
	mov     $3, %rax                  /* SYS_close                     */
	syscall

	/* exit_group(42) */
	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lchild_open_fail:
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

	.bss
status:
	.quad   0

	.section .note.GNU-stack,"",@progbits
