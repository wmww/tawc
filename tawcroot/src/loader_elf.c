/* ELF parser + PT_LOAD geometry. See include/loader_elf.h. Pure: no
 * syscalls, no globals. Drop-in for both the freestanding tawcroot
 * production build and the hosted-glibc cleat test orchestrator. */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "loader_elf.h"

/* ELF identification bytes */
#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define ELFCLASS64     2
#define ELFDATA2LSB    1   /* little-endian — both archs we care about */
#define EV_CURRENT     1

/* Helpers — explicit because we don't pull in libc / kernel headers. */
static inline int is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

static inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static inline uint64_t align_up  (uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static void zero_image_tail(struct tawc_loader_image *img)
{
	img->n_loads       = 0;
	img->addr_min      = 0;
	img->addr_max      = 0;
	img->interp_present = 0;
	img->interp_offset = 0;
	img->interp_size   = 0;
	img->phdr_present  = 0;
	img->phdr_vaddr    = 0;
	img->phdr_filesz   = 0;
	for (unsigned i = 0; i < TAWC_LOADER_MAX_LOADS; i++) {
		struct tawc_loader_seg z = { 0 };
		img->loads[i] = z;
	}
}

long tawc_loader_parse_ehdr(const void *buf, size_t buf_len,
                            struct tawc_loader_image *out)
{
	if (!buf || !out || buf_len < sizeof(tawc_elf64_ehdr))
		return TAWC_EINVAL;

	const tawc_elf64_ehdr *eh = (const tawc_elf64_ehdr *)buf;

	if (eh->e_ident[EI_MAG0] != 0x7f ||
	    eh->e_ident[EI_MAG1] != 'E'  ||
	    eh->e_ident[EI_MAG2] != 'L'  ||
	    eh->e_ident[EI_MAG3] != 'F')
		return TAWC_EINVAL;

	if (eh->e_ident[EI_CLASS] != ELFCLASS64)
		return TAWC_ENOEXEC;
	if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
		return TAWC_ENOEXEC;
	if (eh->e_ident[EI_VERSION] != EV_CURRENT || eh->e_version != EV_CURRENT)
		return TAWC_EINVAL;

	if (eh->e_type != TAWC_ET_EXEC && eh->e_type != TAWC_ET_DYN)
		return TAWC_ENOEXEC;

	if (eh->e_machine != TAWC_EM_X86_64 && eh->e_machine != TAWC_EM_AARCH64)
		return TAWC_ENOEXEC;

	if (eh->e_phentsize != sizeof(tawc_elf64_phdr))
		return TAWC_EINVAL;
	if (eh->e_phnum == 0)
		return TAWC_EINVAL;
	/* PHDRs must fit somewhere reasonable; the caller is responsible
	 * for reading them out of the file, but we sanity-check that
	 * phoff is plausible. */
	if (eh->e_phoff < sizeof(tawc_elf64_ehdr))
		return TAWC_EINVAL;

	out->e_type      = eh->e_type;
	out->e_machine   = eh->e_machine;
	out->e_entry     = eh->e_entry;
	out->e_phoff     = eh->e_phoff;
	out->e_phentsize = eh->e_phentsize;
	out->e_phnum     = eh->e_phnum;
	zero_image_tail(out);
	return 0;
}

void tawc_loader_seg_layout(uint64_t p_vaddr, uint64_t p_filesz,
                            uint64_t p_memsz, uint64_t p_offset,
                            uint32_t p_flags,
                            size_t page_size,
                            struct tawc_loader_seg *seg)
{
	uint64_t end_file = p_vaddr + p_filesz;
	uint64_t end_mem  = p_vaddr + p_memsz;

	uint64_t lo       = align_down(p_vaddr,  page_size);
	uint64_t fhi      = align_up  (end_file, page_size);
	uint64_t mhi      = align_up  (end_mem,  page_size);

	uint64_t off_lo   = align_down(p_offset, page_size);
	/* p_offset must have the same in-page offset as p_vaddr (the
	 * parser validated this). So `p_offset - off_lo == p_vaddr - lo`,
	 * which means the file_size derived from the page-aligned span
	 * is correct. */
	uint64_t file_size = fhi - lo;

	seg->vaddr_lo       = (uintptr_t)lo;
	seg->vaddr_filehi   = (uintptr_t)fhi;
	seg->vaddr_memhi    = (uintptr_t)mhi;
	seg->file_offset    = off_lo;
	seg->file_size      = file_size;

	/* BSS partial-page slice: bytes from end_file up to fhi.
	 * If end_file == fhi (file ends exactly on a page boundary)
	 * the slice is empty. */
	seg->bss_partial_lo = (uintptr_t)end_file;
	seg->bss_partial_hi = (uintptr_t)fhi;

	/* Anonymous BSS extension: from fhi up to mhi.
	 * Empty if memsz == filesz or if the file end's page already
	 * contains the entire BSS. */
	seg->bss_anon_lo    = (uintptr_t)fhi;
	seg->bss_anon_hi    = (uintptr_t)mhi;

	/* PF_R/W/X are bits 2/1/0 in p_flags (per ELF spec — note R/W/X
	 * are 0x4/0x2/0x1, opposite of our TAWC_LOADER_PROT_* values).
	 * Translate explicitly. */
	unsigned prot = 0;
	if (p_flags & 0x4) prot |= TAWC_LOADER_PROT_R;
	if (p_flags & 0x2) prot |= TAWC_LOADER_PROT_W;
	if (p_flags & 0x1) prot |= TAWC_LOADER_PROT_X;
	seg->prot = prot;

	seg->p_vaddr   = p_vaddr;
	seg->p_filesz  = p_filesz;
	seg->p_memsz   = p_memsz;
	seg->p_offset  = p_offset;
	seg->p_flags   = p_flags;
}

long tawc_loader_parse_phdrs(const void *phdr_buf, size_t phdr_buf_len,
                             size_t page_size,
                             struct tawc_loader_image *img)
{
	if (!phdr_buf || !img)
		return TAWC_EINVAL;
	if (!is_pow2(page_size) || page_size < 4096)
		return TAWC_EINVAL;
	if (phdr_buf_len < (size_t)img->e_phnum * img->e_phentsize)
		return TAWC_EINVAL;

	zero_image_tail(img);

	const uint8_t *base = (const uint8_t *)phdr_buf;
	unsigned n_loads = 0;

	for (uint16_t i = 0; i < img->e_phnum; i++) {
		const tawc_elf64_phdr *ph =
		    (const tawc_elf64_phdr *)(base + (size_t)i * img->e_phentsize);

		switch (ph->p_type) {
		case TAWC_PT_LOAD: {
			if (ph->p_filesz > ph->p_memsz)
				return TAWC_EINVAL;
			if (ph->p_memsz == 0)
				continue; /* empty PT_LOAD: ignore (linkers occasionally emit) */
			if (!is_pow2(ph->p_align) || ph->p_align < page_size)
				return TAWC_EINVAL;
			if ((ph->p_offset & (page_size - 1)) !=
			    (ph->p_vaddr  & (page_size - 1)))
				return TAWC_EINVAL;
			if (n_loads >= TAWC_LOADER_MAX_LOADS)
				return TAWC_E2BIG;

			tawc_loader_seg_layout(ph->p_vaddr, ph->p_filesz,
			                       ph->p_memsz, ph->p_offset,
			                       ph->p_flags,
			                       page_size,
			                       &img->loads[n_loads]);
			n_loads++;
			break;
		}
		case TAWC_PT_INTERP:
			if (ph->p_filesz == 0 || ph->p_filesz > 4096)
				return TAWC_EINVAL;
			img->interp_present = 1;
			img->interp_offset  = ph->p_offset;
			img->interp_size    = ph->p_filesz;
			break;
		case TAWC_PT_PHDR:
			img->phdr_present = 1;
			img->phdr_vaddr   = ph->p_vaddr;
			img->phdr_filesz  = ph->p_filesz;
			break;
		default:
			/* PT_DYNAMIC / PT_TLS / PT_GNU_STACK / etc — irrelevant
			 * to the loader. ld.so reads PT_DYNAMIC + PT_TLS via
			 * the AT_PHDR pointer. PT_GNU_STACK influences stack
			 * exec; we always make our synthesized stack non-exec. */
			break;
		}
	}

	if (n_loads == 0)
		return TAWC_EINVAL;

	/* Insertion-sort PT_LOADs by p_vaddr. n_loads ≤ 16, so O(n²) is
	 * trivial; this lets the mapper walk in address order without
	 * caring about the file's PT_LOAD ordering. Real linkers usually
	 * emit them sorted but the spec doesn't require it. */
	for (unsigned i = 1; i < n_loads; i++) {
		struct tawc_loader_seg key = img->loads[i];
		unsigned j = i;
		while (j > 0 && img->loads[j - 1].p_vaddr > key.p_vaddr) {
			img->loads[j] = img->loads[j - 1];
			j--;
		}
		img->loads[j] = key;
	}

	/* Detect overlapping PT_LOADs after sorting — adjacent pages-after-
	 * alignment must not overlap. */
	for (unsigned i = 1; i < n_loads; i++) {
		if (img->loads[i].vaddr_lo < img->loads[i - 1].vaddr_memhi)
			return TAWC_EINVAL;
	}

	img->n_loads = n_loads;

	uintptr_t lo = img->loads[0].vaddr_lo;
	uintptr_t hi = img->loads[n_loads - 1].vaddr_memhi;

	if (img->e_type == TAWC_ET_DYN) {
		/* Image-relative span: rebase to 0 so the mapper can drop
		 * the reservation wherever the kernel hands us memory. We
		 * keep the per-segment vaddrs as offsets from `lo` (typically
		 * 0 already, but PIE binaries occasionally have `lo > 0` for
		 * a deliberate gap). */
		if (lo != 0) {
			for (unsigned i = 0; i < n_loads; i++) {
				img->loads[i].vaddr_lo     -= lo;
				img->loads[i].vaddr_filehi -= lo;
				img->loads[i].vaddr_memhi  -= lo;
				img->loads[i].bss_partial_lo -= lo;
				img->loads[i].bss_partial_hi -= lo;
				img->loads[i].bss_anon_lo  -= lo;
				img->loads[i].bss_anon_hi  -= lo;
				img->loads[i].p_vaddr      -= lo;
			}
			if (img->phdr_present) img->phdr_vaddr -= lo;
			img->e_entry -= lo;
			hi -= lo;
			lo = 0;
		}
	}

	img->addr_min = lo;
	img->addr_max = hi;
	return 0;
}
