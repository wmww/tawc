/* Static x86_64 fixture: clone a fork-like child onto a 16 KiB stack,
 * have that child openat-create argv[1], parent waits and forwards the
 * child's exit status.
 *
 * This specifically catches SIGSYS handler stack regressions: the
 * child's openat trap runs the handler on the tiny child stack. A deep
 * handler frame normally surfaces as the child dying with SIGSEGV before
 * it can create the marker.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc
	mov     16(%rsp), %rax
	mov     %rax, pathptr(%rip)

	/* mmap(NULL, 16 KiB, PROT_READ|PROT_WRITE,
	 *      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
	xor     %rdi, %rdi
	mov     $16384, %rsi
	mov     $3, %rdx
	mov     $0x22, %r10
	mov     $-1, %r8
	xor     %r9, %r9
	mov     $9, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_mmap

	/* clone(SIGCHLD, stack_top, 0, 0, 0) */
	mov     $17, %rdi
	lea     16384(%rax), %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	xor     %r8, %r8
	mov     $56, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_clone
	jz      .Lchild

	mov     %rax, %rdi
	movabs  $status, %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	mov     $61, %rax
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
	mov     $-100, %rdi
	mov     pathptr(%rip), %rsi
	mov     $(1 | 0100 | 01000), %rdx
	mov     $0x180, %r10
	mov     $257, %rax
	syscall
	test    %rax, %rax
	js      .Lchild_open_fail

	mov     %rax, %rdi
	mov     $3, %rax
	syscall

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
	jmp     .Lexit
.Lfail_mmap:
	mov     $74, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start

	.bss
status:
	.quad   0
pathptr:
	.quad   0

	.section .note.GNU-stack,"",@progbits
