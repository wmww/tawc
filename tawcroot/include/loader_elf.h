/* In-process ELF loader: parser + segment geometry.
 *
 * =======================================================================
 *  WHAT THIS IS
 * =======================================================================
 *
 * Phase-2 of tawcroot needs to load an ELF binary (and its ld.so) into
 * the current address space without invoking `execve`, because doing
 * so resets our SIGSYS handler and the inherited seccomp filter then
 * SIGSYS-kills the guest's first path-bearing syscall. See
 * notes/tawcroot/path-translation.md §"execve handling in detail".
 *
 * This header is the *parser + geometry* half: it consumes raw ELF
 * bytes and produces a `tawc_loader_image` describing what the mapper
 * (loader_map.c, separate file) needs to do. No syscalls live here —
 * the parser is pure data math, fully unit-testable from cleat.
 *
 * =======================================================================
 *  WHAT IT IS NOT
 * =======================================================================
 *
 * - Not a full ELF reader: we ignore section headers, symbol tables,
 *   dynamic relocations, debug info. ld.so does relocations; we just
 *   place PT_LOAD segments and (later) jump to ld.so's entry.
 * - Not a relocator: ET_DYN load addresses come from the *mapper*
 *   choosing a free range; the parser only reports the size of the
 *   span and the per-segment offsets within it.
 * - Not multi-arch tolerant: each tawcroot binary loads guests for its
 *   own architecture only (an aarch64 tawcroot does not load x86_64
 *   guests). The parser reports `e_machine` and the caller checks it.
 *
 * =======================================================================
 *  REFERENCE READING (no code is copied)
 * =======================================================================
 *
 *  - musl `ldso/dynlink.c::map_library` — canonical layout walk; we
 *    derive the same `addr_min`/`addr_max` discipline and the BSS
 *    partial-page split from there.
 *  - Linux `fs/binfmt_elf.c::load_elf_binary` — what the kernel does
 *    on `execve`; the file we are partly reproducing.
 *  - System V ABI ELF spec (`man 5 elf` and the ABI document) — the
 *    binary format itself.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ELF binary-format types (we don't include <elf.h>) ----
 *
 * Defining these ourselves keeps the parser identical across glibc /
 * bionic / freestanding builds and makes the field offsets visible at
 * the call site. The layout is fixed by the SysV ABI; it does not
 * vary. We only do 64-bit ELF — ELFCLASS32 is rejected at parse time.
 */

typedef struct {
	uint8_t   e_ident[16];     /* magic + class/data/version/osabi */
	uint16_t  e_type;
	uint16_t  e_machine;
	uint32_t  e_version;
	uint64_t  e_entry;
	uint64_t  e_phoff;
	uint64_t  e_shoff;
	uint32_t  e_flags;
	uint16_t  e_ehsize;
	uint16_t  e_phentsize;
	uint16_t  e_phnum;
	uint16_t  e_shentsize;
	uint16_t  e_shnum;
	uint16_t  e_shstrndx;
} tawc_elf64_ehdr;

typedef struct {
	uint32_t  p_type;
	uint32_t  p_flags;
	uint64_t  p_offset;
	uint64_t  p_vaddr;
	uint64_t  p_paddr;
	uint64_t  p_filesz;
	uint64_t  p_memsz;
	uint64_t  p_align;
} tawc_elf64_phdr;

/* e_type */
#define TAWC_ET_EXEC   2
#define TAWC_ET_DYN    3

/* e_machine — only the two we support */
#define TAWC_EM_X86_64   62
#define TAWC_EM_AARCH64  183

/* The machine this build runs guests for. The parser stays arch-
 * agnostic (unit tests parse both machines on one host); every RUN
 * decision must compare against this, or a cross-arch ELF sails past
 * the exec probe and dies post-commit in the loader. */
#if defined(__x86_64__)
# define TAWC_EM_HOST TAWC_EM_X86_64
#elif defined(__aarch64__)
# define TAWC_EM_HOST TAWC_EM_AARCH64
#endif

/* p_type */
#define TAWC_PT_LOAD     1
#define TAWC_PT_DYNAMIC  2
#define TAWC_PT_INTERP   3
#define TAWC_PT_PHDR     6
#define TAWC_PT_TLS      7
#define TAWC_PT_GNU_STACK 0x6474e551

/* p_flags → mapper protection bits. These match Linux PROT_* on every
 * arch we care about, but we re-define them so the parser doesn't need
 * <sys/mman.h>. The mapper (loader_map.c) translates to the kernel
 * constants if/when they ever diverge. */
#define TAWC_LOADER_PROT_R   0x1
#define TAWC_LOADER_PROT_W   0x2
#define TAWC_LOADER_PROT_X   0x4

/* Per-PT_LOAD layout: page-aligned addresses + the BSS split. All
 * vaddrs are *image-relative* for ET_DYN — the mapper adds a base on
 * placement — and absolute for ET_EXEC. Geometry is identical either
 * way; only interpretation of the base differs.
 *
 * Visual:
 *
 *   p_vaddr           p_vaddr+p_filesz       p_vaddr+p_memsz
 *      |                     |                      |
 *      v                     v                      v
 *      +--- file-backed -----+----- BSS ------------+
 *      |                     |    partial    | full |
 *      |                     |    page       | anon |
 *      |                     |    zero-fill  | pages|
 *      +-----+---------+-----+---------+-----+------+
 *      ^               ^               ^            ^
 *   vaddr_lo     bss_partial_lo   bss_anon_lo   vaddr_memhi
 *   (==vaddr_lo) (==vaddr_filehi-page)            (==bss_anon_hi)
 *
 *      ^===============^                              <-- mmap from file
 *                      ^=======^                      <-- memset 0 (RW temp)
 *                              ^=======^              <-- mmap anon RW
 *
 * `bss_partial_lo` and `bss_partial_hi` are always the half-open range
 * `[p_vaddr+p_filesz .. page_align_up(p_vaddr+p_filesz))`. Even when
 * `p_filesz == p_memsz` (no BSS), this range is non-empty whenever
 * `p_filesz` doesn't land on a page boundary — and the mapper will zero
 * those trailing bytes anyway. The kernel's mmap leaves them as
 * file-backed; for typical ELFs that's just zero-padding emitted by the
 * linker, but the spec doesn't promise it. The empty-slice case
 * (`bss_partial_lo == bss_partial_hi`) only happens when filesz lands on
 * a page boundary. `bss_anon_lo == bss_anon_hi` whenever
 * `p_memsz` doesn't extend past the file-backed page. (Review B12.)
 */
struct tawc_loader_seg {
	uintptr_t vaddr_lo;        /* page-aligned floor of p_vaddr */
	uintptr_t vaddr_filehi;    /* page-aligned ceiling of p_vaddr+p_filesz */
	uintptr_t vaddr_memhi;     /* page-aligned ceiling of p_vaddr+p_memsz */

	uint64_t  file_offset;     /* aligned p_offset (matches vaddr_lo) */
	uint64_t  file_size;       /* vaddr_filehi - vaddr_lo */

	uintptr_t bss_partial_lo;  /* p_vaddr+p_filesz */
	uintptr_t bss_partial_hi;  /* page-aligned ceiling of bss_partial_lo */

	uintptr_t bss_anon_lo;     /* == bss_partial_hi */
	uintptr_t bss_anon_hi;     /* == vaddr_memhi */

	unsigned  prot;            /* TAWC_LOADER_PROT_* bitmask */

	/* Echoes of raw header fields; useful for debug + the mapper
	 * for sanity asserts. */
	uint64_t  p_vaddr;
	uint64_t  p_filesz;
	uint64_t  p_memsz;
	uint64_t  p_offset;
	uint32_t  p_flags;
};

/* Ceiling on PT_LOAD count we'll accept. Real binaries have 4–7;
 * static glibc /bin/true on x86_64 has 7 (R-X / R / RW for the binary
 * itself, plus a few for ld.so when we load it as a separate image).
 * 16 leaves 2× headroom and bounds the tawc_loader_image size. */
#define TAWC_LOADER_MAX_LOADS 16

struct tawc_loader_image {
	int        e_type;            /* TAWC_ET_EXEC | TAWC_ET_DYN */
	int        e_machine;         /* TAWC_EM_X86_64 | TAWC_EM_AARCH64 */
	uint64_t   e_entry;
	uint64_t   e_phoff;
	uint16_t   e_phentsize;
	uint16_t   e_phnum;

	struct tawc_loader_seg loads[TAWC_LOADER_MAX_LOADS];
	unsigned   n_loads;

	/* Span covering all PT_LOADs.
	 *   ET_DYN: addr_min is always 0 (image-relative); addr_max is the
	 *           reservation size the mapper must obtain.
	 *   ET_EXEC: addr_min/addr_max are absolute virtual addresses the
	 *           mapper must place at exactly. */
	uintptr_t  addr_min;
	uintptr_t  addr_max;

	/* PT_INTERP, if present: byte range within the source ELF that
	 * holds the (NUL-terminated) interpreter path. The caller (mapper /
	 * exec orchestrator) reads these bytes from the fd to find ld.so.
	 * `interp_present == 0` means static binary — ld.so step is
	 * skipped, AT_BASE = 0, and we jump to e_entry directly. */
	int        interp_present;
	uint64_t   interp_offset;
	uint64_t   interp_size;       /* including NUL */

	/* PT_PHDR vaddr if the binary has one. Used to compute AT_PHDR for
	 * the synthesized auxv: AT_PHDR = base + phdr_vaddr (ET_DYN) or
	 * just phdr_vaddr (ET_EXEC). For binaries lacking PT_PHDR we fall
	 * back to e_phoff under the first PT_LOAD. */
	int        phdr_present;
	uint64_t   phdr_vaddr;
	uint64_t   phdr_filesz;
};

/* Parse the ELF header out of `buf` (which must contain at least
 * `sizeof(tawc_elf64_ehdr)` bytes). On success, `out` is partially
 * populated with `e_type`, `e_machine`, `e_entry`, `e_phoff`,
 * `e_phentsize`, `e_phnum`. The `loads` / `n_loads` / `addr_*` /
 * `interp_*` / `phdr_*` fields are zeroed; populate them by following
 * up with `tawc_loader_parse_phdrs`.
 *
 * Returns 0 on success, -errno on failure:
 *   -EINVAL  malformed ELF (bad magic, bad class/data/version, weird sizes)
 *   -ENOEXEC unsupported binary type (ET_REL/ET_CORE), unsupported
 *            machine (must be TAWC_EM_X86_64 or TAWC_EM_AARCH64),
 *            ELFCLASS32 (we don't load 32-bit guests).
 */
long tawc_loader_parse_ehdr(const void *buf, size_t buf_len,
                            struct tawc_loader_image *out);

/* Walk program headers from `phdr_buf` (which must contain at least
 * `img->e_phnum * img->e_phentsize` bytes) and fill in `img->loads[]`,
 * `img->n_loads`, `img->addr_min`/`addr_max`, and `img->interp_*` /
 * `phdr_*`. `page_size` is used for the per-segment alignment math
 * and must be a power of two ≥ 4096.
 *
 * Validation performed:
 *   - p_filesz <= p_memsz for every PT_LOAD
 *   - p_align is a power of two and >= page_size for PT_LOAD
 *   - p_offset and p_vaddr have matching alignment modulo page_size
 *     (required so a single mmap of the file region gives us the
 *     correct in-page byte offsets — kernel `execve` enforces the
 *     same)
 *   - PT_LOADs are non-overlapping and sorted by p_vaddr (kernels
 *     accept either order; we sort during parse so the mapper can
 *     assume sorted)
 *   - PT_INTERP fits in the source buffer at runtime — we just record
 *     offset+size; the caller does the actual read
 *   - n_loads <= TAWC_LOADER_MAX_LOADS
 *
 * Returns 0 on success, -errno on failure:
 *   -EINVAL  any of the validations above failed
 *   -E2BIG   too many PT_LOAD segments
 */
long tawc_loader_parse_phdrs(const void *phdr_buf, size_t phdr_buf_len,
                             size_t page_size,
                             struct tawc_loader_image *img);

/* Pure geometry: derive a `tawc_loader_seg` from raw program-header
 * fields. Exposed because the unit tests want to exercise it directly
 * without building a whole ELF. The parser uses it internally via the
 * same code path.
 *
 * Caller has already validated:
 *   - p_filesz <= p_memsz
 *   - p_align is a power of two and >= page_size (parser-side; the
 *     layout math itself uses page_size, so p_align is not consulted
 *     here)
 *   - (p_offset & (page_size-1)) == (p_vaddr & (page_size-1))
 *
 * `seg->prot` is computed from `p_flags` (PF_X/W/R bits 0/1/2).
 */
void tawc_loader_seg_layout(uint64_t p_vaddr, uint64_t p_filesz,
                            uint64_t p_memsz, uint64_t p_offset,
                            uint32_t p_flags,
                            size_t page_size,
                            struct tawc_loader_seg *seg);

#ifdef __cplusplus
}
#endif
