/* Static x86_64 fixture: open /proc/self/fd, getdents64, scan
 * for a d_name byte sequence "1000\0". Exit 42 if absent (the
 * dirent filter correctly hid the rootfs reserved fd), 95 if
 * present.
 *
 * Used by tests/integration/test_prod_features.c to validate the
 * dirent filter (syscalls_fd.c::handle_getdents64 +
 * dirent_filter.c). The closefrom death-spiral regression
 * (notes/tawcroot.md "Phase 5c") hinged on glibc's
 * `__closefrom_fallback` enumerating /proc/self/fd, "closing"
 * each fd via close() — and reserved fds reappearing on the next
 * pass kept the loop alive forever. Filtering them out at the
 * getdents64 layer is what fixed it.
 *
 * fd 1000 is always tawcroot's rootfs_fd in production; binds add
 * 1001, 1002, ... but rootfs_fd at 1000 is the indispensable case.
 * Looking for the byte sequence "1000\0" in the dirent buffer is
 * sufficient — d_name is NUL-terminated, fds <= 999 are at most 3
 * digits and won't exhibit "1000\0".
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib -no-pie.
 *
 * Exit code key:
 *    42 — dirent buffer does not contain "1000\0" (filter worked)
 *    91 — openat /proc/self/fd failed
 *    92 — getdents64 returned a negative errno
 *    93 — getdents64 returned 0 (empty — unexpected; we open at
 *         least the dirfd itself and inherit a few from tawcroot)
 *    95 — found "1000\0" in the dirent stream (filter regressed)
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* openat(AT_FDCWD, "/proc/self/fd", O_RDONLY|O_DIRECTORY) */
	mov     $-100, %rdi              /* AT_FDCWD                      */
	movabs  $procfd_path, %rsi
	mov     $(0 | 0200000), %rdx     /* O_RDONLY | O_DIRECTORY        */
	xor     %r10, %r10
	mov     $257, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_open
	mov     %rax, %r12               /* dirfd */

	/* getdents64(dirfd, buf, sizeof buf). buf in .bss. */
	mov     %r12, %rdi
	movabs  $dent_buf, %rsi
	mov     $4096, %rdx
	mov     $217, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_dents
	jz      .Lfail_empty

	/* Scan the buffer for the byte sequence "1000\0". buf at rsi,
	 * length in rax. Loop i = 0..rax-5: if buf[i..i+5] == "1000\0",
	 * fail. */
	movabs  $dent_buf, %rsi
	mov     %rax, %rcx               /* total bytes */
	xor     %rdi, %rdi               /* i = 0 */
.Lscan:
	mov     %rdi, %rdx
	add     $5, %rdx
	cmp     %rcx, %rdx
	ja      .Lscan_done              /* i + 5 > total → no match */
	cmpb    $'1', (%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $'0', 1(%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $'0', 2(%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $'0', 3(%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $0, 4(%rsi, %rdi)
	jne     .Lscan_next
	/* Match: filter regressed. */
	mov     $95, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lscan_next:
	inc     %rdi
	jmp     .Lscan
.Lscan_done:

	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_open:
	mov     $91, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lfail_dents:
	mov     $92, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lfail_empty:
	mov     $93, %rdi
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start

	.section .rodata
procfd_path:
	.asciz  "/proc/self/fd"

	.bss
dent_buf:
	.zero   4096

	.section .note.GNU-stack,"",@progbits
