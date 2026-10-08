/* x86_64 static guest fixture: musl's detached-thread exit.
 *
 * Runs on an mmap'd stack with no altstack, makes one trapped syscall
 * (an openat; which one must not matter), munmaps that stack and calls
 * exit(42) with SP pointing at unmapped memory — musl's __unmapself.
 * exit(2) is trapped, so its SIGSYS frame needs a stack that isn't the
 * guest's: SIGSEGV death = no fallback altstack.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     $9, %eax           /* mmap(NULL, 65536, RW, PRIVATE|ANON, -1, 0) */
	xor     %edi, %edi
	mov     $65536, %esi
	mov     $3, %edx
	mov     $0x22, %r10d
	mov     $-1, %r8
	xor     %r9d, %r9d
	syscall
	mov     %rax, %rbx
	lea     65536(%rax), %rsp

	mov     $257, %eax         /* openat(AT_FDCWD, "/", O_RDONLY) */
	mov     $-100, %edi
	lea     root(%rip), %rsi
	xor     %edx, %edx
	syscall

	mov     $11, %eax          /* munmap(stack, 65536) */
	mov     %rbx, %rdi
	mov     $65536, %esi
	syscall

	mov     $60, %eax          /* exit(42) */
	mov     $42, %edi
	syscall
	ud2
	.size _start, .-_start

	.section .rodata
root:	.asciz "/"

	.section .note.GNU-stack,"",@progbits
