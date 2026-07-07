/* PT_LOAD mapper. See include/loader_map.h.
 *
 * No syscalls live here — every kernel touch goes through the io
 * vtable. That makes the mapper testable from cleat (under hosted
 * glibc) by passing a libc-forwarding io impl, against the same
 * binary the kernel itself would map on `execve`.
 *
 * Reference reading (no code copied):
 *   musl ldso/dynlink.c::map_library — addr_min/max + BSS partial-page
 *     dance.
 *   Linux fs/binfmt_elf.c::elf_map / load_elf_binary — kernel side.
 */

#include <stddef.h>
#include <stdint.h>

#include "errno_neg.h"
#include "loader_map.h"
#include "tawc_string.h"

static int loader_prot_to_mmap(unsigned p)
{
	int r = 0;
	if (p & TAWC_LOADER_PROT_R) r |= TAWC_MM_PROT_READ;
	if (p & TAWC_LOADER_PROT_W) r |= TAWC_MM_PROT_WRITE;
	if (p & TAWC_LOADER_PROT_X) r |= TAWC_MM_PROT_EXEC;
	return r;
}

/* Issue an mmap that the caller has already tagged with
 * `MAP_FIXED_NOREPLACE` and verify the kernel actually placed the
 * mapping at `addr`. Older kernels (Linux < 4.17, e.g. Pixel 4a's
 * 4.14) silently ignore the flag — without `MAP_FIXED` the call
 * degrades to a hint, so an occupied hint produces a successful
 * placement at *some other* address. That's worse than failing: the
 * caller would proceed at the wrong base. We detect this by
 * comparing the returned address to the request and synthesise
 * `-EEXIST` (with an undo munmap) when they disagree, restoring the
 * modern-kernel error contract. */
static uintptr_t mmap_fixed_noreplace_checked(
	const struct tawc_loader_io *io, void *addr, size_t len,
	int prot, int flags, int fd, uint64_t offset)
{
	uintptr_t rv = io->mmap(io->ctx, addr, len, prot, flags, fd, offset);
	if (tawc_loader_mmap_is_err(rv)) return rv;
	if (rv != (uintptr_t)addr) {
		(void)io->munmap(io->ctx, (void *)rv, len);
		return (uintptr_t)(intptr_t)TAWC_EEXIST;
	}
	return rv;
}

/* Compute AT_PHDR address for the synthesized auxv. The result is the
 * runtime address (already biased by `base` for ET_DYN). Strategy:
 *   - If PT_PHDR was present, use phdr_vaddr (image-relative) + base.
 *   - Otherwise fall back to e_phoff under the first PT_LOAD: the
 *     program headers live in the file at e_phoff, and if the first
 *     PT_LOAD covers e_phoff (which it does in every real binary —
 *     binutils always lays out the headers inside the first
 *     readable load), the in-memory address is
 *         loads[0].vaddr_lo + (e_phoff - loads[0].file_offset) + base
 *
 * Returns 0 if neither path can produce a sensible address (pathological
 * binary), and the caller should refuse to load. */
static uintptr_t compute_phdr_addr(const struct tawc_loader_image *img,
                                   uintptr_t base)
{
	if (img->phdr_present)
		return base + (uintptr_t)img->phdr_vaddr;

	if (img->n_loads == 0) return 0;
	const struct tawc_loader_seg *s = &img->loads[0];
	uint64_t phoff = img->e_phoff;
	if (phoff < s->file_offset) return 0;
	uint64_t off_in_load = phoff - s->file_offset;
	if (off_in_load >= s->file_size) return 0;
	/* The phdr table itself must live entirely inside the mapped
	 * file_size of the first load — otherwise AT_PHDR points at
	 * mapped+unmapped boundary and ld.so SEGVs reading it. */
	uint64_t phsize = (uint64_t)img->e_phnum * (uint64_t)img->e_phentsize;
	if (phsize > s->file_size - off_in_load) return 0;
	return base + s->vaddr_lo + (uintptr_t)off_in_load;
}

long tawc_loader_map(const struct tawc_loader_image *img,
                     int fd, uintptr_t requested_base, size_t page_size,
                     const struct tawc_loader_io *io,
                     struct tawc_loader_placement *out)
{
	if (!img || !io || !out || !io->mmap || !io->mprotect || !io->munmap)
		return TAWC_EINVAL;
	if (img->n_loads == 0)
		return TAWC_EINVAL;
	if (page_size < 4096 || (page_size & (page_size - 1)))
		return TAWC_EINVAL;

	const int is_dyn = (img->e_type == TAWC_ET_DYN);
	uintptr_t base = 0;
	uintptr_t span = (uintptr_t)img->addr_max;

	/* Step 1: reservation (ET_DYN only). For ET_EXEC the per-segment
	 * MAP_FIXED_NOREPLACE handles overlap detection. */
	if (is_dyn) {
		int flags = TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_ANON;
		uintptr_t rv;
		if (requested_base != 0) {
			rv = mmap_fixed_noreplace_checked(
				io, (void *)requested_base, span,
				TAWC_MM_PROT_NONE,
				flags | TAWC_MM_MAP_FIXED_NOREPLACE, -1, 0);
		} else {
			rv = io->mmap(io->ctx, (void *)0, span,
			              TAWC_MM_PROT_NONE, flags, -1, 0);
		}
		if (tawc_loader_mmap_is_err(rv))
			return tawc_loader_mmap_errno(rv);
		base = rv;
	} else {
		/* ET_EXEC: addresses are absolute; ignore requested_base. */
		base = 0;
	}

	/* Step 2: per-segment mapping. */
	for (unsigned i = 0; i < img->n_loads; i++) {
		const struct tawc_loader_seg *s = &img->loads[i];

		/* Need write while we memset the BSS partial-page slice.
		 * ORDER IS LOAD-BEARING under SELinux (untrusted_app): the
		 * initial mmap must carry PROT_EXEC (checked as app_data_file
		 * `execute`, allowed) so the later mprotect only DROPS write.
		 * Mapping rw- first and mprotecting r-x after the memset
		 * would instead trigger the `execmod` check (exec on a
		 * modified private mapping) — denied for app domains,
		 * EACCES on every guest text segment, production only. */
		int has_partial = s->bss_partial_hi > s->bss_partial_lo;
		int needs_temp_w = has_partial && !(s->prot & TAWC_LOADER_PROT_W);
		int seg_prot = loader_prot_to_mmap(s->prot);
		int load_prot = needs_temp_w ? (seg_prot | TAWC_MM_PROT_WRITE) : seg_prot;

		uintptr_t seg_va = base + s->vaddr_lo;

		/* (a) File-backed mapping. Skip if file_size == 0 (rare:
		 * pure-BSS PT_LOAD). */
		if (s->file_size > 0) {
			uintptr_t rv;
			if (is_dyn) {
				/* Replace part of the reservation. MAP_FIXED is
				 * required because the kernel's MAP_FIXED_NOREPLACE
				 * would refuse to overwrite our PROT_NONE
				 * reservation. */
				int flags = TAWC_MM_MAP_PRIVATE | TAWC_MM_MAP_FIXED;
				rv = io->mmap(io->ctx, (void *)seg_va,
				              s->file_size, load_prot, flags,
				              fd, s->file_offset);
			} else {
				int flags = TAWC_MM_MAP_PRIVATE |
				            TAWC_MM_MAP_FIXED_NOREPLACE;
				rv = mmap_fixed_noreplace_checked(
					io, (void *)seg_va, s->file_size,
					load_prot, flags, fd, s->file_offset);
			}
			if (tawc_loader_mmap_is_err(rv))
				return tawc_loader_mmap_errno(rv);
		}

		/* (b) BSS partial-page zero-fill. Only if there's something
		 * to clear AND the file mapping covered it. */
		if (has_partial && s->file_size > 0) {
			memset((void *)(base + s->bss_partial_lo), 0,
			       s->bss_partial_hi - s->bss_partial_lo);
		}

		/* (c) Drop the temporary write bit if we added one. */
		if (needs_temp_w && s->file_size > 0) {
			long rc = io->mprotect(io->ctx, (void *)seg_va,
			                       s->file_size, seg_prot);
			if (rc < 0) return rc;
		}

		/* (d) Anonymous BSS extension pages (covers the whole segment
		 * for pure-BSS PT_LOADs). ET_DYN replaces pages inside our
		 * own PROT_NONE reservation, so plain MAP_FIXED is required;
		 * ET_EXEC has no reservation, so clobbering an existing
		 * mapping (ours included) must fail loudly — same
		 * MAP_FIXED_NOREPLACE discipline as the file-backed part. */
		if (s->bss_anon_hi > s->bss_anon_lo) {
			uintptr_t alen = s->bss_anon_hi - s->bss_anon_lo;
			uintptr_t avirt = base + s->bss_anon_lo;
			uintptr_t rv;
			if (is_dyn) {
				int flags = TAWC_MM_MAP_PRIVATE |
				            TAWC_MM_MAP_ANON | TAWC_MM_MAP_FIXED;
				rv = io->mmap(io->ctx, (void *)avirt, alen,
				              seg_prot, flags, -1, 0);
			} else {
				int flags = TAWC_MM_MAP_PRIVATE |
				            TAWC_MM_MAP_ANON |
				            TAWC_MM_MAP_FIXED_NOREPLACE;
				rv = mmap_fixed_noreplace_checked(
					io, (void *)avirt, alen,
					seg_prot, flags, -1, 0);
			}
			if (tawc_loader_mmap_is_err(rv))
				return tawc_loader_mmap_errno(rv);
		}
	}

	/* placement.base/span describe the in-memory range of the loaded
	 * image so `tawc_loader_unmap(placement, ...)` can free exactly
	 * what we mapped — nothing more, nothing less.
	 *
	 *   ET_DYN: addr_min is 0 (image-relative), so the loaded range is
	 *           [chosen_base, chosen_base + addr_max). Record that.
	 *   ET_EXEC: vaddrs are absolute. Loaded range is
	 *           [addr_min, addr_max). Record that. The previous form
	 *           recorded base=0/span=addr_max, which would unmap the
	 *           whole low half of the address space (including
	 *           libtawcroot itself) on cleanup.
	 *
	 * `out->entry` and `compute_phdr_addr` still use the local `base`
	 * (which is 0 for ET_EXEC) so absolute vaddrs go through unchanged. */
	if (is_dyn) {
		out->base = base;
		out->span = (uintptr_t)img->addr_max;
	} else {
		out->base = (uintptr_t)img->addr_min;
		out->span = (uintptr_t)(img->addr_max - img->addr_min);
	}
	out->entry      = base + (uintptr_t)img->e_entry;
	out->phdr_addr  = compute_phdr_addr(img, base);
	out->phnum      = img->e_phnum;
	out->phentsize  = img->e_phentsize;
	return 0;
}

long tawc_loader_unmap(const struct tawc_loader_placement *placement,
                       const struct tawc_loader_io *io)
{
	if (!placement || !io || !io->munmap) return TAWC_EINVAL;
	if (placement->span == 0) return 0;
	return io->munmap(io->ctx, (void *)placement->base, placement->span);
}

long tawc_loader_read_interp(int fd, const struct tawc_loader_image *img,
                             char *out_path, size_t out_cap,
                             const struct tawc_loader_io *io)
{
	if (!img || !out_path || !io || !io->pread) return TAWC_EINVAL;
	if (!img->interp_present) return TAWC_ENOENT;
	if (img->interp_size == 0) return TAWC_EINVAL;
	if (img->interp_size > out_cap) return TAWC_ERANGE;

	long n = io->pread(io->ctx, fd, out_path, img->interp_size,
	                   img->interp_offset);
	if (n < 0) return n;
	if ((uint64_t)n != img->interp_size) return TAWC_EINVAL;
	/* PT_INTERP includes a trailing NUL; sanity-check that it does and
	 * also defensively NUL-terminate at out_cap-1 in case the binary
	 * lies. */
	if (out_path[img->interp_size - 1] != '\0') {
		if (img->interp_size < out_cap)
			out_path[img->interp_size] = '\0';
		else
			out_path[out_cap - 1] = '\0';
	}
	return 0;
}
