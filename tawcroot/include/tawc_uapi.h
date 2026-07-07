/* Linux uapi flag constants used across tawcroot.
 *
 * tawcroot is freestanding but the kernel uapi headers are
 * freestanding-safe (they're plain `#define`s and a few structs, no
 * libc dependencies). Pull them in one place so individual TUs don't
 * each maintain their own `#ifndef AT_FDCWD` defensive blocks (which
 * are dead code anyway: every value below is provided by the included
 * uapi header on every Linux toolchain we ship to).
 *
 * Two arch-specific values matter:
 *   - O_NOFOLLOW: 0x8000 on aarch64, 0x20000 on x86_64.
 *   - O_DIRECTORY: 0x4000 on aarch64, 0x10000 on x86_64.
 *   - O_PATH: 0x200000 on aarch64, 0x200000 on x86_64 (same).
 * Hand-pinning the x86_64 values silently breaks aarch64 — the
 * `<linux/fcntl.h>` include below resolves correctly per-arch.
 *
 * Anything kernel-uapi-shaped that's used from more than one TU
 * belongs here. Centralizing keeps the per-arch correctness in one
 * place. */

#pragma once

#include <linux/fcntl.h>   /* O_*, F_*, AT_* */
#include <linux/fs.h>      /* RENAME_* */
#include <linux/memfd.h>   /* MFD_CLOEXEC, MFD_ALLOW_SEALING */
#include <linux/stat.h>    /* S_IF*, STATX_* */

/* <linux/stat.h> hides the S_IF / S_IS / S_IRWXU macro families when
 * __GLIBC__ is defined (the hosted tests build); glibc's <sys/stat.h>
 * provides them there. Freestanding builds never define __GLIBC__, so
 * they keep getting the uapi definitions above. */
#if __STDC_HOSTED__
#include <sys/stat.h>
#endif

/* O_TMPFILE as userspace passes it: __O_TMPFILE plus O_DIRECTORY (both
 * glibc and bionic define the composite; the kernel requires both bits
 * and EINVALs O_CREAT alongside). Defined here because pre-6.4 uapi
 * headers spell the composite while newer ones dropped O_DIRECTORY
 * from it — pin the userspace-visible shape, arch-correct via
 * O_DIRECTORY. */
#define TAWC_O_TMPFILE (020000000 | O_DIRECTORY)
