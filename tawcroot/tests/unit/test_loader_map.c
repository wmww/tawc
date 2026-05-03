/* Mapper integration tests. The mapper itself is pluggable I/O — we
 * give it a libc-forwarding `tawc_loader_io` and have it map a real
 * /proc/self/exe into the test process's address space. Then we walk
 * /proc/self/maps to confirm the exact vaddrs / prots we expected.
 *
 * This is the highest-fidelity unit test we can do without forking
 * a child: the kernel itself is the oracle for "did the mmap have
 * the effect you wanted." Anything the kernel accepts here, our
 * production raw-syscall I/O impl will also accept.
 *
 * Static-binary smoke (forking + jumping into the loaded image) lives
 * in test_loader_smoke.c.
 */

#define _GNU_SOURCE
#include <cleat/test.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "loader_elf.h"
#include "loader_map.h"

/* ------------- libc-forwarding I/O impl ------------- */

static uintptr_t io_mmap(void *ctx, void *addr, size_t len, int prot,
                         int flags, int fd, uint64_t offset)
{
	(void)ctx;
	void *r = mmap(addr, len, prot, flags, fd, (off_t)offset);
	if (r == MAP_FAILED) return (uintptr_t)-(intptr_t)errno;
	return (uintptr_t)r;
}
static long io_mprotect(void *ctx, void *addr, size_t len, int prot)
{
	(void)ctx;
	if (mprotect(addr, len, prot) < 0) return -errno;
	return 0;
}
static long io_munmap(void *ctx, void *addr, size_t len)
{
	(void)ctx;
	if (munmap(addr, len) < 0) return -errno;
	return 0;
}
static long io_pread(void *ctx, int fd, void *buf, size_t n, uint64_t off)
{
	(void)ctx;
	ssize_t r = pread(fd, buf, n, (off_t)off);
	if (r < 0) return -errno;
	return r;
}

static struct tawc_loader_io libc_io = {
	.ctx = nullptr,
	.mmap = io_mmap,
	.mprotect = io_mprotect,
	.munmap = io_munmap,
	.pread = io_pread,
};

/* ------------- helpers ------------- */

/* Open + parse a binary from `path` into `img`. Returns the open fd
 * on success (caller closes), -1 on failure. */
static int open_and_parse(const char *path, struct tawc_loader_image *img)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	uint8_t ebuf[sizeof(tawc_elf64_ehdr)];
	if (pread(fd, ebuf, sizeof ebuf, 0) != (ssize_t)sizeof ebuf) {
		close(fd); return -1;
	}
	if (tawc_loader_parse_ehdr(ebuf, sizeof ebuf, img) != 0) {
		close(fd); return -1;
	}
	uint8_t pbuf[64 * 64]; /* phdrs almost never exceed this */
	size_t pbytes = (size_t)img->e_phnum * img->e_phentsize;
	if (pbytes > sizeof pbuf) { close(fd); return -1; }
	if (pread(fd, pbuf, pbytes, (off_t)img->e_phoff) != (ssize_t)pbytes) {
		close(fd); return -1;
	}
	if (tawc_loader_parse_phdrs(pbuf, sizeof pbuf, 4096, img) != 0) {
		close(fd); return -1;
	}
	return fd;
}

/* Walk /proc/self/maps and check that an [addr,end) range exists with
 * the given r/w/x perms. Returns 1 on found, 0 on not-found. */
static int maps_check_range(uintptr_t addr, uintptr_t end, int want_r,
                            int want_w, int want_x)
{
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return 0;
	char line[1024];
	int hit = 0;
	while (fgets(line, sizeof line, f)) {
		uintptr_t a, b;
		char r, w, x, p;
		if (sscanf(line, "%lx-%lx %c%c%c%c",
		           &a, &b, &r, &w, &x, &p) != 6) continue;
		if (a > addr || b < end) continue;
		int gr = (r == 'r'), gw = (w == 'w'), gx = (x == 'x');
		if (gr == want_r && gw == want_w && gx == want_x) {
			hit = 1;
			break;
		}
	}
	fclose(f);
	return hit;
}

/* ------------- tests ------------- */

/* Recording I/O vtable: doesn't actually map, just acknowledges every
 * mmap by returning the requested address (or a fake non-zero base for
 * the ET_DYN reservation when hint=NULL). Used to drive the mapper
 * against a synthetic image without polluting the test process's address
 * space — we only care about the placement struct it produces.
 *
 * The fake-base value is chosen so addresses don't collide with any
 * real mapping a sanitizer might want to inspect; nothing dereferences
 * the returned pointers. */
#define MOCK_DYN_BASE ((uintptr_t)0x70000000UL)

static uintptr_t mock_mmap_record(void *ctx, void *addr, size_t len,
                                  int prot, int flags, int fd,
                                  uint64_t offset)
{
	(void)ctx; (void)len; (void)prot; (void)flags;
	(void)fd; (void)offset;
	if (addr) return (uintptr_t)addr;
	return MOCK_DYN_BASE;
}
static long mock_mprotect_ok(void *ctx, void *addr, size_t len, int prot)
{ (void)ctx; (void)addr; (void)len; (void)prot; return 0; }
static long mock_munmap_ok(void *ctx, void *addr, size_t len)
{ (void)ctx; (void)addr; (void)len; return 0; }
static long mock_pread_ok(void *ctx, int fd, void *buf, size_t n, uint64_t off)
{ (void)ctx; (void)fd; (void)buf; (void)off; return (long)n; }

static struct tawc_loader_io mock_io = {
	.ctx = nullptr,
	.mmap = mock_mmap_record,
	.mprotect = mock_mprotect_ok,
	.munmap = mock_munmap_ok,
	.pread = mock_pread_ok,
};

/* Regression: tawc_loader_unmap on an ET_EXEC placement used to do
 * `munmap(0, addr_max)` because we recorded base=0/span=addr_max — that
 * would unmap libtawcroot itself. The fix: record the actual loaded
 * range, [addr_min, addr_max). This test exercises the bookkeeping
 * with a recording mock io so we don't need a real ET_EXEC fixture. */
test(placement_et_exec_covers_only_loaded_range)
{
	/* Two PT_LOADs with filesz/memsz aligned to page size — no BSS,
	 * so the mapper never calls zero_range on the (un-backed) addresses
	 * the recording mock io hands out. */
	tawc_elf64_phdr ph[2];
	memset(ph, 0, sizeof ph);
	ph[0].p_type = TAWC_PT_LOAD; ph[0].p_flags = 0x4 | 0x1;
	ph[0].p_offset = 0x1000; ph[0].p_vaddr = 0x401000; ph[0].p_paddr = 0x401000;
	ph[0].p_filesz = 0x1000; ph[0].p_memsz = 0x1000; ph[0].p_align = 0x1000;
	ph[1].p_type = TAWC_PT_LOAD; ph[1].p_flags = 0x4 | 0x2;
	ph[1].p_offset = 0x2000; ph[1].p_vaddr = 0x402000; ph[1].p_paddr = 0x402000;
	ph[1].p_filesz = 0x1000; ph[1].p_memsz = 0x1000; ph[1].p_align = 0x1000;

	struct tawc_loader_image img;
	memset(&img, 0, sizeof img);
	img.e_type      = TAWC_ET_EXEC;
	img.e_machine   = TAWC_EM_X86_64;
	img.e_phentsize = sizeof(tawc_elf64_phdr);
	img.e_phnum     = 2;
	img.e_entry     = 0x401234;
	test_int_eq(tawc_loader_parse_phdrs(ph, sizeof ph, 4096, &img), 0);
	test_int_eq((int)img.addr_min, 0x401000);
	test_int_eq((int)img.addr_max, 0x403000);

	struct tawc_loader_placement pl;
	test_int_eq(tawc_loader_map(&img, /*fd*/-1, 0, 4096, &mock_io, &pl), 0);
	test_int_eq((int)pl.base, 0x401000);
	test_int_eq((int)pl.span, 0x2000);   /* addr_max - addr_min */
	/* entry uses the local base (0 for ET_EXEC) — absolute. */
	test_int_eq((int)pl.entry, 0x401234);
}

test(map_real_self_exe_dyn)
{
	struct tawc_loader_image img;
	int fd = open_and_parse("/proc/self/exe", &img);
	test_true(fd >= 0);
	test_int_eq(img.e_type, TAWC_ET_DYN);

	struct tawc_loader_placement pl;
	long rc = tawc_loader_map(&img, fd, /*requested_base*/ 0, 4096,
	                          &libc_io, &pl);
	test_int_eq(rc, 0);
	test_true(pl.base != 0);
	test_int_eq((int)pl.span, (int)img.addr_max);
	test_int_eq((int)pl.entry, (int)(pl.base + img.e_entry));
	test_true(pl.phdr_addr != 0);
	test_int_eq(pl.phnum, img.e_phnum);
	test_int_eq(pl.phentsize, img.e_phentsize);

	/* Each PT_LOAD must have the right perms in /proc/self/maps. */
	for (unsigned i = 0; i < img.n_loads; i++) {
		uintptr_t a = pl.base + img.loads[i].vaddr_lo;
		uintptr_t b = pl.base + img.loads[i].vaddr_memhi;
		int want_r = !!(img.loads[i].prot & TAWC_LOADER_PROT_R);
		int want_w = !!(img.loads[i].prot & TAWC_LOADER_PROT_W);
		int want_x = !!(img.loads[i].prot & TAWC_LOADER_PROT_X);
		test_true(maps_check_range(a, b, want_r, want_w, want_x));
	}

	/* The first byte of the first PT_LOAD must equal "\x7fELF" — i.e.
	 * the file is actually mapped, not just reserved. */
	const uint8_t *p = (const uint8_t *)(pl.base + img.loads[0].vaddr_lo);
	test_int_eq(p[0], 0x7f);
	test_int_eq(p[1], 'E');
	test_int_eq(p[2], 'L');
	test_int_eq(p[3], 'F');

	/* AT_PHDR-pointed memory must contain the program-header table.
	 * The first phdr should be a PT_PHDR, PT_INTERP, or PT_LOAD —
	 * binutils ordering varies but it's always one of those. We just
	 * sanity-check the readability + a plausible p_type. */
	const tawc_elf64_phdr *phdrs = (const tawc_elf64_phdr *)pl.phdr_addr;
	uint32_t t0 = phdrs[0].p_type;
	test_true(t0 == TAWC_PT_PHDR || t0 == TAWC_PT_INTERP ||
	          t0 == TAWC_PT_LOAD);

	test_int_eq((int)tawc_loader_unmap(&pl, &libc_io), 0);
	close(fd);
}

test(map_real_self_exe_at_specific_base)
{
	struct tawc_loader_image img;
	int fd = open_and_parse("/proc/self/exe", &img);
	test_true(fd >= 0);

	/* Pick a base far above the test process's address space. We
	 * reserve a probe region first to find an address that's free,
	 * then ask the mapper to land there. */
	void *probe = mmap(nullptr, img.addr_max, PROT_NONE,
	                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_true(probe != MAP_FAILED);
	uintptr_t want_base = (uintptr_t)probe;
	munmap(probe, img.addr_max);

	struct tawc_loader_placement pl;
	long rc = tawc_loader_map(&img, fd, want_base, 4096, &libc_io, &pl);
	test_int_eq(rc, 0);
	test_int_eq((int)pl.base, (int)want_base);

	tawc_loader_unmap(&pl, &libc_io);
	close(fd);
}

test(map_read_interp)
{
	struct tawc_loader_image img;
	int fd = open_and_parse("/proc/self/exe", &img);
	test_true(fd >= 0);
	test_int_eq(img.interp_present, 1);

	char interp[256];
	long rc = tawc_loader_read_interp(fd, &img, interp, sizeof interp,
	                                  &libc_io);
	test_int_eq(rc, 0);
	/* The interp path should start with '/' and look like a dynamic
	 * linker. glibc: ld-2.x.so / ld-linux-x86-64.so.2; musl: ld-musl-*;
	 * Android bionic: /system/bin/linker[64]. */
	test_int_eq(interp[0], '/');
	test_true(strstr(interp, "ld-")    != nullptr ||
	          strstr(interp, "ld.so")  != nullptr ||
	          strstr(interp, "ld64")   != nullptr ||
	          strstr(interp, "linker") != nullptr);

	close(fd);
}

test(map_read_interp_static_returns_enoent)
{
	/* Synthesize an image with no PT_INTERP. */
	struct tawc_loader_image img;
	memset(&img, 0, sizeof img);
	img.interp_present = 0;

	char buf[64];
	test_int_eq((int)tawc_loader_read_interp(/*fd*/-1, &img, buf, sizeof buf,
	                                         &libc_io), -2 /* ENOENT */);
}

test(map_rejects_bad_args)
{
	struct tawc_loader_image img;
	memset(&img, 0, sizeof img);
	struct tawc_loader_placement pl;
	/* n_loads == 0 */
	test_int_eq((int)tawc_loader_map(&img, -1, 0, 4096, &libc_io, &pl),
	            -22);
	/* page_size = 0 */
	img.n_loads = 1;
	test_int_eq((int)tawc_loader_map(&img, -1, 0, 0, &libc_io, &pl),
	            -22);
	/* page_size not pow2 */
	test_int_eq((int)tawc_loader_map(&img, -1, 0, 0x1500, &libc_io, &pl),
	            -22);
	/* null io */
	test_int_eq((int)tawc_loader_map(&img, -1, 0, 4096, nullptr, &pl),
	            -22);
}

test(map_dyn_at_occupied_base_fails_clean)
{
	struct tawc_loader_image img;
	int fd = open_and_parse("/proc/self/exe", &img);
	test_true(fd >= 0);

	/* Reserve some pages and ask the mapper to land on top — should
	 * get -EEXIST (17). */
	void *blocker = mmap(nullptr, img.addr_max, PROT_READ,
	                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_true(blocker != MAP_FAILED);

	struct tawc_loader_placement pl;
	long rc = tawc_loader_map(&img, fd, (uintptr_t)blocker, 4096,
	                          &libc_io, &pl);
	test_int_eq((int)rc, -17);

	munmap(blocker, img.addr_max);
	close(fd);
}
