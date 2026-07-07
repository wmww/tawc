/* Aarch64 port of static_mknod_chr_fake_x86_64.S — see that file's
 * header for the sequence and the exit-code key (42 pass, 220..229
 * per-step failures).
 *
 * Syscall numbers (asm-generic):
 *   mknodat=33 unlinkat=35 newfstatat=79 setresuid=147 setresgid=149
 *   exit_group=94
 * AT_FDCWD = -100. st_mode is at offset 16 in the aarch64 struct stat.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	sub     sp, sp, #144            /* struct stat buffer (128)      */

	/* mknodat(AT_FDCWD, "/mknod-chr", S_IFCHR|0644, 1:3) -> 0 */
	mov     x0, #-100
	ldr     x1, =path_chr
	mov     x2, #0x21a4             /* S_IFCHR | 0644                */
	mov     x3, #0x103              /* dev 1:3                       */
	mov     x8, #33                 /* __NR_mknodat                  */
	svc     #0
	cbnz    x0, .Lfail220

	/* fstatat(AT_FDCWD, "/mknod-chr", buf, 0) -> 0 */
	mov     x0, #-100
	ldr     x1, =path_chr
	mov     x2, sp
	mov     x3, #0
	mov     x8, #79                 /* __NR_newfstatat               */
	svc     #0
	cbnz    x0, .Lfail221

	/* (st_mode & S_IFMT) == S_IFREG */
	ldr     w0, [sp, #16]           /* st_mode                       */
	and     w0, w0, #0xf000
	cmp     w0, #0x8000             /* S_IFREG                       */
	b.ne    .Lfail222

	/* re-mknodat on the existing name -> -EEXIST */
	mov     x0, #-100
	ldr     x1, =path_chr
	mov     x2, #0x21a4
	mov     x3, #0x103
	mov     x8, #33                 /* __NR_mknodat                  */
	svc     #0
	cmn     x0, #17                 /* -EEXIST                       */
	b.ne    .Lfail223

	/* unlinkat(AT_FDCWD, "/mknod-chr", 0) -> 0 */
	mov     x0, #-100
	ldr     x1, =path_chr
	mov     x2, #0
	mov     x8, #35                 /* __NR_unlinkat                 */
	svc     #0
	cbnz    x0, .Lfail224

	/* mknodat in the host-/dev bind: even the placeholder is refused
	 * there — swallow tier reports bare success. */
	mov     x0, #-100
	ldr     x1, =path_dev
	mov     x2, #0x21a4
	mov     x3, #0x103
	mov     x8, #33                 /* __NR_mknodat                  */
	svc     #0
	cbnz    x0, .Lfail225

	/* ... and nothing was actually created. */
	mov     x0, #-100
	ldr     x1, =path_dev
	mov     x2, sp
	mov     x3, #0
	mov     x8, #79                 /* __NR_newfstatat               */
	svc     #0
	cmn     x0, #2                  /* -ENOENT                       */
	b.ne    .Lfail226

	/* setresgid(994, 994, 994) -> 0 */
	mov     x0, #994
	mov     x1, #994
	mov     x2, #994
	mov     x8, #149                /* __NR_setresgid                */
	svc     #0
	cbnz    x0, .Lfail227

	/* setresuid(994, 994, 994) -> 0 */
	mov     x0, #994
	mov     x1, #994
	mov     x2, #994
	mov     x8, #147                /* __NR_setresuid                */
	svc     #0
	cbnz    x0, .Lfail228

	/* dropped mknodat -> real -EPERM / -EACCES, no placeholder */
	mov     x0, #-100
	ldr     x1, =path_dropped
	mov     x2, #0x21a4
	mov     x3, #0x103
	mov     x8, #33                 /* __NR_mknodat                  */
	svc     #0
	cmn     x0, #1                  /* -EPERM                        */
	b.eq    1f
	cmn     x0, #13                 /* -EACCES                       */
	b.ne    .Lfail229
1:
	mov     x0, #42
	b       .Lexit

.Lfail220:
	mov     x0, #220
	b       .Lexit
.Lfail221:
	mov     x0, #221
	b       .Lexit
.Lfail222:
	mov     x0, #222
	b       .Lexit
.Lfail223:
	mov     x0, #223
	b       .Lexit
.Lfail224:
	mov     x0, #224
	b       .Lexit
.Lfail225:
	mov     x0, #225
	b       .Lexit
.Lfail226:
	mov     x0, #226
	b       .Lexit
.Lfail227:
	mov     x0, #227
	b       .Lexit
.Lfail228:
	mov     x0, #228
	b       .Lexit
.Lfail229:
	mov     x0, #229
.Lexit:
	mov     x8, #94                 /* __NR_exit_group               */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .rodata
path_chr:
	.asciz  "/mknod-chr"
path_dev:
	.asciz  "/dev/tawc-prodenv-mknod"
path_dropped:
	.asciz  "/mknod-chr-dropped"

	.section .note.GNU-stack,"",%progbits
