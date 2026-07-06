/* PT_LOAD mapper: turns a parsed `tawc_loader_image` into a real
 * memory mapping by issuing mmap/mprotect/pread.
 *
 * The mapper itself is *I/O-pluggable*: it never calls syscalls
 * directly — it calls through a `tawc_loader_io` vtable. Production
 * code (loader_io_prod.c, freestanding) fills the vtable with raw-
 * syscall wrappers; tests (test_loader_map.c, hosted glibc) fill it
 * with libc forwarders. This mirrors the path-resolver oracle pattern
 * and keeps the mapper itself in `PROD_C_FOR_TESTS` — pure logic,
 * unit-testable end to end against a real /bin/true ELF.
 *
 * On both production and test sides the constants below match Linux
 * `PROT_*` / `MAP_*` semantically. Concrete numeric values match
 * Linux uapi so a "pass through" impl works without translation.
 *
 * See notes/tawcroot/path-translation.md "execve handling in detail" for the
 * surrounding architecture; this header is just the mapper API.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "loader_elf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux PROT_* — matches uapi values. */
#define TAWC_MM_PROT_NONE   0x0
#define TAWC_MM_PROT_READ   0x1
#define TAWC_MM_PROT_WRITE  0x2
#define TAWC_MM_PROT_EXEC   0x4

/* Linux MAP_* — matches uapi values. */
#define TAWC_MM_MAP_PRIVATE          0x02
#define TAWC_MM_MAP_FIXED            0x10
#define TAWC_MM_MAP_ANON             0x20
#define TAWC_MM_MAP_FIXED_NOREPLACE  0x100000

/* Pluggable I/O. All ops return the kernel's raw value: success-pos
 * for mmap (the address) or 0 for mprotect/pread/munmap; -errno on
 * failure. We use uintptr_t for mmap because addresses are unsigned
 * but error returns are negative — callers compare against
 * (uintptr_t)-1 ... -4095. */
struct tawc_loader_io {
	void *ctx;
	uintptr_t (*mmap)(void *ctx, void *addr, size_t len, int prot,
	                  int flags, int fd, uint64_t offset);
	long      (*mprotect)(void *ctx, void *addr, size_t len, int prot);
	long      (*munmap)(void *ctx, void *addr, size_t len);
	long      (*pread)(void *ctx, int fd, void *buf, size_t n,
	                   uint64_t offset);
};

/* mmap return values >= -4095 are -errno. */
static inline int tawc_loader_mmap_is_err(uintptr_t v)
{ return v >= (uintptr_t)-4095; }

static inline long tawc_loader_mmap_errno(uintptr_t v)
{ return (long)(intptr_t)v; }

/* Output of tawc_loader_map: where the image landed and the
 * derived addresses callers (the stack synth and the trampoline)
 * actually need.
 *
 * `base` and `span` describe the in-memory range of the loaded image
 * — `tawc_loader_unmap` does `munmap(base, span)` to free exactly
 * what we mapped. For ET_DYN that's [chosen_base, chosen_base+addr_max);
 * for ET_EXEC that's [addr_min, addr_max), since vaddrs are absolute.
 *
 * `entry` and `phdr_addr` are absolute virtual addresses for the guest
 * to jump to / for ld.so to read. For ET_DYN they're biased by the
 * chosen base; for ET_EXEC the bias is 0 (image vaddrs already absolute). */
struct tawc_loader_placement {
	uintptr_t base;        /* low address of the loaded image */
	uintptr_t span;        /* size in bytes (so [base, base+span) is the load range) */
	uintptr_t entry;       /* absolute jump target */
	uintptr_t phdr_addr;   /* in-memory address of the program-header table */
	uint16_t  phnum;       /* echo of img->e_phnum (for AT_PHNUM) */
	uint16_t  phentsize;   /* echo of img->e_phentsize (for AT_PHENT) */
};

/* Map all PT_LOAD segments described by `img` from `fd` into the
 * current address space.
 *
 *  ET_EXEC: every segment is placed at its absolute p_vaddr using
 *           MAP_FIXED_NOREPLACE so we fail cleanly on overlap with an
 *           existing mapping rather than silently clobbering it.
 *           `requested_base` is ignored.
 *
 *  ET_DYN:  if `requested_base == 0` the kernel chooses (we issue a
 *           single PROT_NONE reservation of `img->addr_max` bytes
 *           with `MAP_FIXED_NOREPLACE` cleared — i.e. anywhere — and
 *           then MAP_FIXED each segment within it). The reservation
 *           ensures a contiguous span and prevents another thread
 *           from racing into the BSS gap.
 *           If `requested_base != 0` the reservation uses
 *           MAP_FIXED_NOREPLACE at that base; -EEXIST on overlap.
 *
 * `page_size` must be a power of two ≥ 4096 and match what the parser
 * was given.
 *
 * On success `out` is populated. On failure, `out` is NOT written and
 * partial mappings may remain; for ET_DYN with a kernel-chosen base the
 * caller has no way to learn the reservation address, so a failed map
 * is only safely cleaned up by process exit. Production treats any
 * failure as fatal (LOADER_FAIL).
 */
long tawc_loader_map(const struct tawc_loader_image *img,
                     int fd, uintptr_t requested_base, size_t page_size,
                     const struct tawc_loader_io *io,
                     struct tawc_loader_placement *out);

/* Unmap the entire `[placement->base, placement->base + placement->span)`
 * range. Idempotent w.r.t. already-unmapped pages (Linux munmap
 * silently ignores unmapped ranges). */
long tawc_loader_unmap(const struct tawc_loader_placement *placement,
                       const struct tawc_loader_io *io);

/* Read PT_INTERP (the requested ld.so path) from `fd` into
 * `out_path`. Returns 0 on success (path NUL-terminated in out_path),
 * -ENOENT if the binary is static (no PT_INTERP), -errno on I/O or
 * buffer-size failure. */
long tawc_loader_read_interp(int fd, const struct tawc_loader_image *img,
                             char *out_path, size_t out_cap,
                             const struct tawc_loader_io *io);

#ifdef __cplusplus
}
#endif
