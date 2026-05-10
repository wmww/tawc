/* Static x86_64 fixture: socket(AF_UNIX); bind to sun_path = argv[1];
 * close; exit_group(42).
 *
 * Used by tests/integration/test_prod_features.c to validate AF_UNIX
 * bind() sun_path translation (syscalls_socket.c). The gpg-agent
 * regression — `bind(/run/.gnupg/S.gpg-agent)` looking for `/run/`
 * on the host filesystem instead of inside the rootfs — is the canonical
 * shape this tests against. Documented in notes/tawcroot.md "More
 * phase-5b bugs" but had no test surface before this.
 *
 * argv: [self, sun_path]
 *
 * sockaddr_un layout (matches Linux's struct sockaddr_un):
 *   u16  sun_family = AF_UNIX (1)
 *   char sun_path[108]    (NUL-terminated; we zero the rest)
 *
 * We construct the struct on the stack, copy argv[1] byte-by-byte into
 * sun_path, NUL-padding via stosb. addrlen passed to bind is the full
 * 110-byte struct size — Linux ignores trailing padding past the NUL.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib -no-pie.
 *
 * Exit code key:
 *    42 — socket+bind+close all succeeded
 *  90+errno (cap 60) — socket failed (codes 90..150)
 * 160+errno (cap 60) — bind failed   (codes 160..220)
 *   200 — argc < 2
 *   201 — sun_path too long (≥ 108 chars)
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check */
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	/* Reserve 128 bytes on the stack for sockaddr_un (110 + slack).
	 * Zero it via 16 quad stores so sun_path tail is clean. */
	sub     $128, %rsp
	mov     %rsp, %rbx               /* &addr base */
	xor     %rax, %rax
	mov     $16, %rcx
	mov     %rbx, %rdi
	rep stosq                        /* zero 128 bytes */

	/* sun_family = AF_UNIX = 1 (u16 at offset 0) */
	movw    $1, (%rbx)

	/* Copy argv[1] bytes to (rbx+2). Bound at 107 bytes (sun_path
	 * is 108; we leave one for NUL — which is already there from
	 * the zero fill above). */
	mov     144(%rsp), %rsi          /* argv[1] (rsp-pre-sub was rsp;
	                                     argv[1] at rsp+16; we sub'd
	                                     128; so 16+128 = 144) */
	lea     2(%rbx), %rdi            /* &addr.sun_path[0] */
	xor     %rcx, %rcx               /* count */
.Lcpy:
	cmp     $107, %rcx
	jge     .Lpath_too_long_post     /* path overrun */
	movzbl  (%rsi, %rcx), %eax
	test    %al, %al
	jz      .Lcpy_done
	mov     %al, (%rdi, %rcx)
	inc     %rcx
	jmp     .Lcpy
.Lcpy_done:

	/* socket(AF_UNIX=1, SOCK_STREAM=1, 0). nr=41. */
	mov     $1, %rdi
	mov     $1, %rsi
	xor     %rdx, %rdx
	mov     $41, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_socket
	mov     %rax, %r12               /* sock_fd */

	/* bind(sock_fd, &addr, 110). nr=49. */
	mov     %r12, %rdi
	mov     %rbx, %rsi
	mov     $110, %rdx               /* sizeof(sockaddr_un) */
	mov     $49, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_bind

	/* close(sock_fd). nr=3. */
	mov     %r12, %rdi
	mov     $3, %rax
	syscall

	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_socket:
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

.Lfail_bind:
	neg     %rax
	cmp     $60, %rax
	jbe     1f
	mov     $60, %rax
1:
	add     $160, %rax
	mov     %rax, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lpath_too_long_post:
	mov     $201, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_argc:
	mov     $200, %rdi
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
