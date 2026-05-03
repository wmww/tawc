/* Unit tests for the ELF parser + PT_LOAD geometry
 * (tawcroot/src/loader_elf.c).
 *
 * The parser is pure data math: no syscalls, no globals, no allocations.
 * We compile loader_elf.c into the cleat orchestrator under hosted glibc
 * (via $(PROD_C_FOR_TESTS)) and feed it both synthetic ELF buffers and
 * the real `/proc/self/exe` from the test process. Synthetic input
 * exercises corner cases that natural binaries don't hit (overlapping
 * segments, alignment-mismatch traps, the ET_DYN-with-nonzero-base
 * rebase). The real-binary check is the smoke that confirms our struct
 * layouts and field offsets match what gcc/binutils actually produce.
 *
 * The integration smoke that *runs* an ELF lands in phase 2.4 — this
 * file is just structural correctness.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "loader_elf.h"

/* Build a syntactically valid ELF header in `buf`. Caller can mutate
 * specific fields after the fact to test rejection paths. */
static void make_ehdr(uint8_t *buf, size_t cap, uint16_t e_type,
                      uint16_t e_machine, uint16_t e_phnum)
{
	memset(buf, 0, cap);
	tawc_elf64_ehdr *eh = (tawc_elf64_ehdr *)buf;
	eh->e_ident[0] = 0x7f;
	eh->e_ident[1] = 'E';
	eh->e_ident[2] = 'L';
	eh->e_ident[3] = 'F';
	eh->e_ident[4] = 2;   /* ELFCLASS64 */
	eh->e_ident[5] = 1;   /* ELFDATA2LSB */
	eh->e_ident[6] = 1;   /* EV_CURRENT */
	eh->e_type    = e_type;
	eh->e_machine = e_machine;
	eh->e_version = 1;
	eh->e_entry   = 0x1000;
	eh->e_phoff   = sizeof(tawc_elf64_ehdr);
	eh->e_phentsize = sizeof(tawc_elf64_phdr);
	eh->e_phnum   = e_phnum;
}

/* ============================================================ */
/*  ehdr validation                                             */
/* ============================================================ */

test(ehdr_accepts_x86_64_exec)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), 0);
	test_int_eq(img.e_type, TAWC_ET_EXEC);
	test_int_eq(img.e_machine, TAWC_EM_X86_64);
	test_int_eq((int)img.e_entry, 0x1000);
	test_int_eq((int)img.e_phnum, 1);
	test_int_eq(img.n_loads, 0);              /* tail must be zeroed */
	test_int_eq(img.interp_present, 0);
}

test(ehdr_accepts_aarch64_dyn)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_DYN, TAWC_EM_AARCH64, 5);
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), 0);
	test_int_eq(img.e_type, TAWC_ET_DYN);
	test_int_eq(img.e_machine, TAWC_EM_AARCH64);
}

test(ehdr_rejects_bad_magic)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	buf[1] = 'X';
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -22);
}

test(ehdr_rejects_short_buffer)
{
	uint8_t buf[16] = { 0 };
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -22);
}

test(ehdr_rejects_elf32)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	buf[4] = 1;  /* ELFCLASS32 */
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -8);
}

test(ehdr_rejects_big_endian)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	buf[5] = 2;  /* ELFDATA2MSB */
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -8);
}

test(ehdr_rejects_bad_ident_version)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	buf[6] = 0;
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -22);
}

test(ehdr_rejects_et_rel)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, 1 /* ET_REL */, TAWC_EM_X86_64, 1);
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -8);
}

test(ehdr_rejects_et_core)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, 4 /* ET_CORE */, TAWC_EM_X86_64, 1);
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -8);
}

test(ehdr_rejects_unknown_machine)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, 40 /* EM_ARM (32-bit) */, 1);
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -8);
}

test(ehdr_rejects_bad_phentsize)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	tawc_elf64_ehdr *eh = (tawc_elf64_ehdr *)buf;
	eh->e_phentsize = 32;
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -22);
}

test(ehdr_rejects_zero_phnum)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 0);
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -22);
}

test(ehdr_rejects_phoff_inside_ehdr)
{
	uint8_t buf[64];
	make_ehdr(buf, sizeof buf, TAWC_ET_EXEC, TAWC_EM_X86_64, 1);
	tawc_elf64_ehdr *eh = (tawc_elf64_ehdr *)buf;
	eh->e_phoff = 8;
	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(buf, sizeof buf, &img), -22);
}

/* ============================================================ */
/*  seg_layout — pure geometry math                             */
/* ============================================================ */

test(seg_layout_no_bss_single_page)
{
	struct tawc_loader_seg s;
	tawc_loader_seg_layout(/*p_vaddr*/ 0x1000,
	                       /*filesz*/  0x500,
	                       /*memsz*/   0x500,
	                       /*offset*/  0x1000,
	                       /*flags*/   0x4 /* PF_R */,
	                       /*page*/    4096,
	                       &s);
	test_int_eq((int)s.vaddr_lo,        0x1000);
	test_int_eq((int)s.vaddr_filehi,    0x2000);
	test_int_eq((int)s.vaddr_memhi,     0x2000);
	test_int_eq((int)s.file_offset,     0x1000);
	test_int_eq((int)s.file_size,       0x1000);
	/* BSS partial: from end_file (0x1500) to next page (0x2000) — even
	 * though there's no actual BSS, our slice describes "tail bytes of
	 * the file-mapped page that don't belong to the file region"; the
	 * mapper still memsets these. This is correct because the kernel
	 * `mmap` doesn't zero past p_filesz on its own. */
	test_int_eq((int)s.bss_partial_lo,  0x1500);
	test_int_eq((int)s.bss_partial_hi,  0x2000);
	test_int_eq((int)s.bss_anon_lo,     0x2000);
	test_int_eq((int)s.bss_anon_hi,     0x2000);
	test_int_eq(s.prot, TAWC_LOADER_PROT_R);
}

test(seg_layout_filesz_on_page_boundary_with_anon_bss)
{
	struct tawc_loader_seg s;
	/* filesz lands exactly at 0x2000; partial slice is empty;
	 * memsz extends one more page → one anon page. */
	tawc_loader_seg_layout(0x1000, 0x1000, 0x2000, 0x1000,
	                       0x4 | 0x2 /* PF_R | PF_W */, 4096, &s);
	test_int_eq((int)s.vaddr_filehi, 0x2000);
	test_int_eq((int)s.vaddr_memhi,  0x3000);
	test_int_eq((int)s.bss_partial_lo, 0x2000);
	test_int_eq((int)s.bss_partial_hi, 0x2000);   /* empty */
	test_int_eq((int)s.bss_anon_lo,    0x2000);
	test_int_eq((int)s.bss_anon_hi,    0x3000);   /* one page */
	test_int_eq(s.prot, (TAWC_LOADER_PROT_R | TAWC_LOADER_PROT_W));
}

test(seg_layout_partial_and_anon_bss)
{
	struct tawc_loader_seg s;
	/* filesz mid-page, memsz spans into a new page. */
	tawc_loader_seg_layout(0x1000, 0x800, 0x3500, 0x1000,
	                       0x4 | 0x2, 4096, &s);
	test_int_eq((int)s.vaddr_filehi,  0x2000);
	test_int_eq((int)s.vaddr_memhi,   0x5000);  /* 0x1000+0x3500=0x4500 → up to 0x5000 */
	test_int_eq((int)s.bss_partial_lo, 0x1800);
	test_int_eq((int)s.bss_partial_hi, 0x2000);
	test_int_eq((int)s.bss_anon_lo,    0x2000);
	test_int_eq((int)s.bss_anon_hi,    0x5000);
}

test(seg_layout_executable_prot)
{
	struct tawc_loader_seg s;
	tawc_loader_seg_layout(0x400000, 0x1234, 0x1234, 0,
	                       0x4 | 0x1 /* PF_R | PF_X */, 4096, &s);
	test_int_eq(s.prot, (TAWC_LOADER_PROT_R | TAWC_LOADER_PROT_X));
}

test(seg_layout_p_offset_aligned_down)
{
	struct tawc_loader_seg s;
	/* p_vaddr/p_offset both have in-page offset 0x100. file_offset
	 * must round down to the nearest page; file_size accounts for it. */
	tawc_loader_seg_layout(0x1100, 0x800, 0x800, 0x3100,
	                       0x4, 4096, &s);
	test_int_eq((int)s.vaddr_lo,    0x1000);
	test_int_eq((int)s.file_offset, 0x3000);
	test_int_eq((int)s.file_size,   0x1000);
}

test(seg_layout_16k_page)
{
	/* aarch64 binaries built with --max-page-size=0x4000 — same math
	 * just bigger pages. */
	struct tawc_loader_seg s;
	tawc_loader_seg_layout(0x4000, 0x3000, 0x5000, 0x4000,
	                       0x4 | 0x2, 0x4000, &s);
	test_int_eq((int)s.vaddr_lo,     0x4000);
	test_int_eq((int)s.vaddr_filehi, 0x8000);
	test_int_eq((int)s.vaddr_memhi,  0xC000);
	test_int_eq((int)s.bss_anon_lo,  0x8000);
	test_int_eq((int)s.bss_anon_hi,  0xC000);
}

/* ============================================================ */
/*  phdr parser                                                 */
/* ============================================================ */

/* Small phdr-array builder. Takes `n` entries and pre-zeroes the
 * tawc_loader_image (including e_phnum/e_phentsize) so callers can
 * just hand the resulting buffer to tawc_loader_parse_phdrs(). */
static void mk_img_phnum(struct tawc_loader_image *img, uint16_t phnum)
{
	memset(img, 0, sizeof *img);
	img->e_type      = TAWC_ET_EXEC;
	img->e_machine   = TAWC_EM_X86_64;
	img->e_phentsize = sizeof(tawc_elf64_phdr);
	img->e_phnum     = phnum;
}

static void mk_load(tawc_elf64_phdr *p, uint64_t vaddr, uint64_t filesz,
                    uint64_t memsz, uint64_t offset, uint32_t flags)
{
	memset(p, 0, sizeof *p);
	p->p_type   = TAWC_PT_LOAD;
	p->p_flags  = flags;
	p->p_offset = offset;
	p->p_vaddr  = vaddr;
	p->p_paddr  = vaddr;
	p->p_filesz = filesz;
	p->p_memsz  = memsz;
	p->p_align  = 0x1000;
}

test(phdrs_two_loads_happy_path)
{
	tawc_elf64_phdr ph[2];
	mk_load(&ph[0], 0x401000, 0x1000, 0x1000, 0x1000, 0x4 | 0x1);
	mk_load(&ph[1], 0x402000, 0x500,  0x800,  0x2000, 0x4 | 0x2);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 2);

	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq(img.n_loads, 2);
	test_int_eq((int)img.addr_min, 0x401000);
	test_int_eq((int)img.addr_max, 0x403000);
	test_int_eq(img.interp_present, 0);
	test_int_eq(img.phdr_present, 0);
}

test(phdrs_pt_interp_recorded)
{
	tawc_elf64_phdr ph[2];
	mk_load(&ph[0], 0x401000, 0x1000, 0x1000, 0x1000, 0x4 | 0x1);
	memset(&ph[1], 0, sizeof ph[1]);
	ph[1].p_type   = TAWC_PT_INTERP;
	ph[1].p_offset = 0x300;
	ph[1].p_filesz = 28;     /* "/lib64/ld-linux-x86-64.so.2\0" */
	ph[1].p_memsz  = 28;
	struct tawc_loader_image img;
	mk_img_phnum(&img, 2);

	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq(img.interp_present, 1);
	test_int_eq((int)img.interp_offset, 0x300);
	test_int_eq((int)img.interp_size, 28);
}

test(phdrs_pt_phdr_recorded)
{
	tawc_elf64_phdr ph[2];
	memset(&ph[0], 0, sizeof ph[0]);
	ph[0].p_type   = TAWC_PT_PHDR;
	ph[0].p_offset = sizeof(tawc_elf64_ehdr);
	ph[0].p_vaddr  = 0x400040;
	ph[0].p_filesz = 2 * sizeof(tawc_elf64_phdr);
	mk_load(&ph[1], 0x400000, 0x1000, 0x1000, 0, 0x4 | 0x1);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 2);

	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq(img.phdr_present, 1);
	test_int_eq((int)img.phdr_vaddr, 0x400040);
}

test(phdrs_filesz_gt_memsz_rejected)
{
	tawc_elf64_phdr ph[1];
	mk_load(&ph[0], 0x1000, 0x2000, 0x1000, 0x1000, 0x4); /* invalid */
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), -22);
}

test(phdrs_alignment_mismatch_rejected)
{
	tawc_elf64_phdr ph[1];
	mk_load(&ph[0], 0x1100, 0x500, 0x500, 0x1200, 0x4);
	/* p_vaddr%4096 = 0x100, p_offset%4096 = 0x200 — must fail. */
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), -22);
}

test(phdrs_p_align_too_small_rejected)
{
	tawc_elf64_phdr ph[1];
	mk_load(&ph[0], 0x1000, 0x500, 0x500, 0x1000, 0x4);
	ph[0].p_align = 0x800; /* < page_size */
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), -22);
}

test(phdrs_p_align_not_pow2_rejected)
{
	tawc_elf64_phdr ph[1];
	mk_load(&ph[0], 0x1000, 0x500, 0x500, 0x1000, 0x4);
	ph[0].p_align = 0x3000;
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), -22);
}

test(phdrs_overlapping_loads_rejected)
{
	tawc_elf64_phdr ph[2];
	mk_load(&ph[0], 0x1000, 0x2000, 0x2000, 0x1000, 0x4 | 0x1);
	mk_load(&ph[1], 0x2000, 0x500,  0x500,  0x3000, 0x4 | 0x2); /* overlaps */
	struct tawc_loader_image img;
	mk_img_phnum(&img, 2);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), -22);
}

test(phdrs_too_many_loads_rejected)
{
	tawc_elf64_phdr ph[TAWC_LOADER_MAX_LOADS + 2];
	for (int i = 0; i < TAWC_LOADER_MAX_LOADS + 2; i++) {
		mk_load(&ph[i], (uint64_t)(0x10000 * (i + 1)),
		        0x1000, 0x1000, (uint64_t)(0x10000 * (i + 1)),
		        0x4);
	}
	struct tawc_loader_image img;
	mk_img_phnum(&img, TAWC_LOADER_MAX_LOADS + 2);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), -7);
}

test(phdrs_unsorted_input_gets_sorted)
{
	tawc_elf64_phdr ph[3];
	mk_load(&ph[0], 0x3000, 0x500, 0x500, 0x3000, 0x4);
	mk_load(&ph[1], 0x1000, 0x500, 0x500, 0x1000, 0x4 | 0x1);
	mk_load(&ph[2], 0x2000, 0x500, 0x500, 0x2000, 0x4 | 0x2);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 3);

	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq(img.n_loads, 3);
	test_int_eq((int)img.loads[0].p_vaddr, 0x1000);
	test_int_eq((int)img.loads[1].p_vaddr, 0x2000);
	test_int_eq((int)img.loads[2].p_vaddr, 0x3000);
	test_int_eq((int)img.addr_min, 0x1000);
	test_int_eq((int)img.addr_max, 0x4000);
}

test(phdrs_dyn_with_nonzero_first_load_gets_rebased)
{
	/* PIE binaries can have a non-zero first PT_LOAD vaddr (rare —
	 * usually 0). When they do, we rebase to 0 so the mapper can
	 * place the reservation freely. */
	tawc_elf64_phdr ph[2];
	mk_load(&ph[0], 0x10000, 0x500, 0x500, 0x10000, 0x4 | 0x1);
	mk_load(&ph[1], 0x11000, 0x500, 0x500, 0x11000, 0x4 | 0x2);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 2);
	img.e_type  = TAWC_ET_DYN;
	img.e_entry = 0x10500;

	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq(img.n_loads, 2);
	test_int_eq((int)img.addr_min, 0);
	test_int_eq((int)img.addr_max, 0x2000);
	test_int_eq((int)img.loads[0].p_vaddr, 0);
	test_int_eq((int)img.loads[1].p_vaddr, 0x1000);
	test_int_eq((int)img.e_entry, 0x500);
}

test(phdrs_exec_with_nonzero_load_kept_absolute)
{
	tawc_elf64_phdr ph[1];
	mk_load(&ph[0], 0x400000, 0x1000, 0x1000, 0, 0x4 | 0x1);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	img.e_type  = TAWC_ET_EXEC;
	img.e_entry = 0x401234;

	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq((int)img.addr_min,         0x400000);
	test_int_eq((int)img.addr_max,         0x401000);
	test_int_eq((int)img.loads[0].p_vaddr, 0x400000);
	test_int_eq((int)img.e_entry,          0x401234);
}

test(phdrs_zero_phnum_buffer_rejected)
{
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	uint8_t empty[1] = { 0 };
	/* phdr_buf_len < phnum*phentsize */
	test_int_eq(tawc_loader_parse_phdrs(empty, 1, 4096, &img), -22);
}

test(phdrs_bad_page_size_rejected)
{
	tawc_elf64_phdr ph[1];
	mk_load(&ph[0], 0x1000, 0x500, 0x500, 0x1000, 0x4);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 1);
	/* not a power of two */
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 0x1500, &img), -22);
	/* below 4096 minimum */
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 0x800, &img), -22);
}

test(phdrs_empty_pt_load_skipped)
{
	tawc_elf64_phdr ph[2];
	mk_load(&ph[0], 0x1000, 0x500, 0x500, 0x1000, 0x4);
	mk_load(&ph[1], 0x2000, 0,     0,     0x2000, 0x4 | 0x2);
	struct tawc_loader_image img;
	mk_img_phnum(&img, 2);
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq(img.n_loads, 1);
	test_int_eq((int)img.loads[0].p_vaddr, 0x1000);
}

/* ============================================================ */
/*  Real /proc/self/exe — sanity smoke                          */
/* ============================================================ */

test(real_exe_parses_and_has_loads)
{
	/* The cleat orchestrator itself is a glibc-linked ET_DYN binary,
	 * so we can read its own ELF and confirm the parser produces a
	 * sane image. This catches the "I got the struct field offsets
	 * wrong" class of bug — synthetic tests above don't cover that. */
	FILE *f = fopen("/proc/self/exe", "rb");
	test_nonnull(f);
	if (!f) return;

	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	test_int_eq((int)fread(ebuf, 1, sizeof ebuf, f), (int)sizeof ebuf);

	struct tawc_loader_image img;
	test_int_eq(tawc_loader_parse_ehdr(ebuf, sizeof ebuf, &img), 0);

	/* glibc/bionic ET_DYN, x86_64 or aarch64 host. */
	test_int_eq(img.e_type, TAWC_ET_DYN);
#if defined(__aarch64__)
	test_int_eq(img.e_machine, TAWC_EM_AARCH64);
#else
	test_int_eq(img.e_machine, TAWC_EM_X86_64);
#endif
	test_true(img.e_phnum > 0 && img.e_phnum < TAWC_LOADER_MAX_LOADS + 8);

	uint8_t pbuf[sizeof(tawc_elf64_phdr) * (TAWC_LOADER_MAX_LOADS + 8)];
	size_t pbytes = (size_t)img.e_phnum * img.e_phentsize;
	test_true(pbytes <= sizeof pbuf);
	test_int_eq(fseek(f, (long)img.e_phoff, SEEK_SET), 0);
	test_int_eq((int)fread(pbuf, 1, pbytes, f), (int)pbytes);
	fclose(f);

	test_int_eq(tawc_loader_parse_phdrs(pbuf, sizeof pbuf, 4096, &img), 0);
	test_true(img.n_loads >= 1);
	test_true(img.n_loads <= TAWC_LOADER_MAX_LOADS);
	/* glibc dynamically links with ld.so → must have PT_INTERP. */
	test_int_eq(img.interp_present, 1);
	test_true(img.interp_size > 0 && img.interp_size < 256);
	/* All segments are within the rebased span. */
	test_int_eq((int)img.addr_min, 0);
	test_true(img.addr_max > 0);
	test_true(img.addr_max < (uintptr_t)1 << 40);  /* sanity */
	/* Segments are sorted ascending. */
	for (unsigned i = 1; i < img.n_loads; i++) {
		test_true(img.loads[i].p_vaddr > img.loads[i - 1].p_vaddr);
	}
	/* Each segment's vaddr range fits within the image span. */
	for (unsigned i = 0; i < img.n_loads; i++) {
		test_true(img.loads[i].vaddr_memhi <= img.addr_max);
		test_true(img.loads[i].vaddr_lo < img.loads[i].vaddr_memhi);
		test_true(img.loads[i].file_size > 0);
		/* Each load page-aligned. */
		test_int_eq((int)(img.loads[i].vaddr_lo & 4095), 0);
		test_int_eq((int)(img.loads[i].file_offset & 4095), 0);
	}
}
