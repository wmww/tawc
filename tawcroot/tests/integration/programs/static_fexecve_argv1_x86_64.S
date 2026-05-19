/* Static x86_64 fixture: open(argv[1], O_PATH|O_CLOEXEC), then
 * execveat(fd, "", argv+1, envp_empty, AT_EMPTY_PATH).
 *
 * Used to validate tawcroot's fexecve(3)-shape execveat handling.
 * On success the target binary's exit code is observed by the test.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	mov     $-100, %rdi          /* AT_FDCWD */
	mov     16(%rsp), %rsi       /* argv[1] */
	mov     $0x280000, %rdx      /* O_PATH | O_CLOEXEC */
	xor     %r10, %r10
	mov     $257, %rax           /* SYS_openat */
	syscall
	test    %rax, %rax
	js      .Lfail_open

	mov     %rax, %rdi           /* fd */
	movabs  $empty_path, %rsi
	lea     16(%rsp), %rdx       /* argv = &argv[1] */
	movabs  $envp_empty, %r10
	mov     $0x1000, %r8         /* AT_EMPTY_PATH */
	mov     $322, %rax           /* SYS_execveat */
	syscall

	neg     %rax
	cmp     $60, %rax
	jbe     1f
	mov     $60, %rax
1:
	add     $90, %rax
	mov     %rax, %rdi
	jmp     .Lexit

.Lfail_argc:
	mov     $73, %rdi
	jmp     .Lexit
.Lfail_open:
	neg     %rax
	cmp     $20, %rax
	jbe     1f
	mov     $20, %rax
1:
	add     $70, %rax
	mov     %rax, %rdi
.Lexit:
	mov     $231, %rax           /* SYS_exit_group */
	syscall
	ud2

	.size _start, .-_start

	.section .rodata
empty_path:
	.asciz  ""
	.balign 8
envp_empty:
	.quad   0

	.section .note.GNU-stack,"",@progbits
