/* Static x86_64 fixture: device-node mknod degradation under fake
 * root. S_IFCHR needs CAP_MKNOD, which untrusted_app never has, so in
 * prod-env the host refuses and the handler must degrade to an empty
 * regular-file placeholder (rootfs paths) or swallow to bare success
 * (host-/dev bind, where even the placeholder is refused). Requires a
 * `/dev` bind (production always has one).
 *
 * Sequence:
 *   1. mknodat(/mknod-chr, S_IFCHR|0644, 1:3)  -> 0 (placeholder)
 *   2. fstatat(/mknod-chr)                     -> 0, S_IFREG
 *   3. mknodat(/mknod-chr) again               -> -EEXIST (honest)
 *   4. unlinkat(/mknod-chr)                    -> 0
 *   5. mknodat(/dev/tawc-prodenv-mknod, chr)   -> 0 (swallow tier)
 *   6. fstatat(/dev/tawc-prodenv-mknod)        -> -ENOENT (nothing made)
 *   7. setresgid/setresuid(994)                -> 0 (virtual drop)
 *   8. mknodat(/mknod-chr-dropped, chr)        -> real -EPERM/-EACCES
 *
 * Exit code key:
 *    42 — all steps held
 *   220 — mknodat(chr) in rootfs was not degraded to success
 *   221 — fstatat on the placeholder failed
 *   222 — placeholder is not a regular file
 *   223 — re-mknodat didn't return EEXIST
 *   224 — unlinkat failed
 *   225 — mknodat(chr) in /dev was not swallowed to success
 *   226 — swallowed mknod left something at the /dev name
 *   227 — setresgid failed
 *   228 — setresuid failed
 *   229 — dropped mknodat didn't return EPERM/EACCES
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	sub     $160, %rsp                /* struct stat buffer (144)      */

	/* mknodat(AT_FDCWD, "/mknod-chr", S_IFCHR|0644, 1:3) -> 0 */
	mov     $-100, %rdi
	lea     path_chr(%rip), %rsi
	mov     $0x21a4, %rdx             /* S_IFCHR | 0644                */
	mov     $0x103, %r10              /* dev 1:3                       */
	mov     $259, %rax                /* SYS_mknodat                   */
	syscall
	test    %rax, %rax
	jnz     .Lfail220

	/* fstatat(AT_FDCWD, "/mknod-chr", buf, 0) -> 0 */
	mov     $-100, %rdi
	lea     path_chr(%rip), %rsi
	mov     %rsp, %rdx
	xor     %r10, %r10
	mov     $262, %rax                /* SYS_newfstatat                */
	syscall
	test    %rax, %rax
	jnz     .Lfail221

	/* (st_mode & S_IFMT) == S_IFREG */
	mov     24(%rsp), %eax            /* st_mode                       */
	and     $0xf000, %eax
	cmp     $0x8000, %eax             /* S_IFREG                       */
	jne     .Lfail222

	/* re-mknodat on the existing name -> -EEXIST */
	mov     $-100, %rdi
	lea     path_chr(%rip), %rsi
	mov     $0x21a4, %rdx
	mov     $0x103, %r10
	mov     $259, %rax                /* SYS_mknodat                   */
	syscall
	cmp     $-17, %rax                /* -EEXIST                       */
	jne     .Lfail223

	/* unlinkat(AT_FDCWD, "/mknod-chr", 0) -> 0 */
	mov     $-100, %rdi
	lea     path_chr(%rip), %rsi
	xor     %rdx, %rdx
	mov     $263, %rax                /* SYS_unlinkat                  */
	syscall
	test    %rax, %rax
	jnz     .Lfail224

	/* mknodat in the host-/dev bind: even the placeholder is refused
	 * there — swallow tier reports bare success. */
	mov     $-100, %rdi
	lea     path_dev(%rip), %rsi
	mov     $0x21a4, %rdx
	mov     $0x103, %r10
	mov     $259, %rax                /* SYS_mknodat                   */
	syscall
	test    %rax, %rax
	jnz     .Lfail225

	/* ... and nothing was actually created. */
	mov     $-100, %rdi
	lea     path_dev(%rip), %rsi
	mov     %rsp, %rdx
	xor     %r10, %r10
	mov     $262, %rax                /* SYS_newfstatat                */
	syscall
	cmp     $-2, %rax                 /* -ENOENT                       */
	jne     .Lfail226

	/* setresgid(994, 994, 994) -> 0 */
	mov     $994, %rdi
	mov     $994, %rsi
	mov     $994, %rdx
	mov     $119, %rax                /* SYS_setresgid                 */
	syscall
	test    %rax, %rax
	jnz     .Lfail227

	/* setresuid(994, 994, 994) -> 0 */
	mov     $994, %rdi
	mov     $994, %rsi
	mov     $994, %rdx
	mov     $117, %rax                /* SYS_setresuid                 */
	syscall
	test    %rax, %rax
	jnz     .Lfail228

	/* dropped mknodat -> real -EPERM / -EACCES, no placeholder */
	mov     $-100, %rdi
	lea     path_dropped(%rip), %rsi
	mov     $0x21a4, %rdx
	mov     $0x103, %r10
	mov     $259, %rax                /* SYS_mknodat                   */
	syscall
	cmp     $-1, %rax                 /* -EPERM                        */
	je      1f
	cmp     $-13, %rax                /* -EACCES                       */
	jne     .Lfail229
1:
	mov     $42, %rdi
	jmp     .Lexit

.Lfail220:
	mov     $220, %rdi
	jmp     .Lexit
.Lfail221:
	mov     $221, %rdi
	jmp     .Lexit
.Lfail222:
	mov     $222, %rdi
	jmp     .Lexit
.Lfail223:
	mov     $223, %rdi
	jmp     .Lexit
.Lfail224:
	mov     $224, %rdi
	jmp     .Lexit
.Lfail225:
	mov     $225, %rdi
	jmp     .Lexit
.Lfail226:
	mov     $226, %rdi
	jmp     .Lexit
.Lfail227:
	mov     $227, %rdi
	jmp     .Lexit
.Lfail228:
	mov     $228, %rdi
	jmp     .Lexit
.Lfail229:
	mov     $229, %rdi
.Lexit:
	mov     $231, %rax                /* SYS_exit_group                */
	syscall
	ud2
	.size _start, .-_start

	.section .rodata
path_chr:
	.asciz  "/mknod-chr"
path_dev:
	.asciz  "/dev/tawc-prodenv-mknod"
path_dropped:
	.asciz  "/mknod-chr-dropped"

	.section .note.GNU-stack,"",@progbits
