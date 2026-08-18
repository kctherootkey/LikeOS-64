// LikeOS-64 ELF64 Loader Implementation
// Supports static (ET_EXEC) and dynamic/PIE (ET_DYN) executables.
// When PT_INTERP is present, loads the dynamic linker (ld-likeos.so) and
// passes control to it with an auxiliary vector on the stack.
#include <kernel/ke/elf.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h> /* PROT_* for lazy BSS regions */
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/pipe.h>
#include <kernel/net/net.h>
#include <kernel/ke/smp.h>
#include <kernel/dev/rand/random.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// VALIDATION
// ============================================================================

int elf_validate(const void *data, size_t size)
{
	BUILD_BUG_ON(sizeof(Elf64_Ehdr) != 64);
	BUILD_BUG_ON(sizeof(Elf64_Phdr) != 56);
	if (!data || size < sizeof(Elf64_Ehdr))
		return -1;

	const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

	if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3)
		return -2;

	if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
		return -3;
	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
		return -4;

	// Accept both ET_EXEC (static) and ET_DYN (PIE / shared object)
	if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
		return -5;
	if (ehdr->e_machine != EM_X86_64)
		return -6;
	if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
		return -7;

	uint64_t ph_end =
		ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
	if (ph_end > size)
		return -8;

	return 0;
}

// ============================================================================
// SEGMENT LOADER  (common for main binary & interpreter)
// ============================================================================

/* Read `len` bytes of the ELF image at `file_off` into `dst`.  Buffer mode
 * (whole != NULL) copies from the in-memory image; file mode seeks + reads
 * from the open backing file.  Caller has bounds-checked against elf_size. */
static int elf_image_read(const uint8_t *whole, vfs_file_t *file,
			  uint64_t file_off, void *dst, uint64_t len)
{
	if (whole) {
		mm_memcpy(dst, whole + file_off, len);
		return 0;
	}
	if (vfs_seek(file, (long)file_off, SEEK_SET) < 0)
		return -1;
	if (vfs_read(file, dst, (long)len) != (long)len)
		return -1;
	return 0;
}

// Load PT_LOAD segments of an ELF into *pml4* at the given base offset.
// For ET_EXEC base_offset = 0 (absolute addresses in ELF).
// For ET_DYN  base_offset shifts every p_vaddr.
//
// Two source modes:
//   whole != NULL — classic path: the entire image is in memory.
//   file  != NULL — demand-paged path: only ehdr+phdrs are in memory;
//     non-writable segments with page-congruent offsets are NOT loaded at
//     all — they are recorded as file-backed lazy regions (tagged file_idx)
//     and paged in from `file` on first touch.  Writable segments (small
//     .data) are read eagerly; BSS stays lazy-anonymous as before.
static int elf_load_segments_ex(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdrs,
				const uint8_t *whole, vfs_file_t *file,
				size_t elf_size, uint64_t *pml4,
				uint64_t base_offset, int file_idx,
				elf_load_result_t *result)
{
	BUG_ON(ehdr == NULL || phdrs == NULL || pml4 == NULL || result == NULL);
	BUG_ON(whole == NULL && file == NULL);

	result->load_base = ~0ULL;
	result->load_end = 0;
	result->brk_start = 0;
	result->phdr_addr = 0;
	result->has_interp = 0;
	result->is_dynamic = (ehdr->e_type == ET_DYN);
	result->interp_base = 0;
	result->interp_entry = 0;
	result->interp_path[0] = '\0';
	result->phnum = ehdr->e_phnum;
	result->phentsize = ehdr->e_phentsize;

	// ---- First pass: PT_INTERP ----
	for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
		const Elf64_Phdr *ph =
			(const Elf64_Phdr *)((const uint8_t *)phdrs +
					     (size_t)i * ehdr->e_phentsize);
		if (ph->p_type == PT_INTERP && ph->p_filesz > 0 &&
		    ph->p_filesz < 256 &&
		    ph->p_offset + ph->p_filesz <= elf_size) {
			size_t len = ph->p_filesz;
			if (len > 255)
				len = 255;
			if (elf_image_read(whole, file, ph->p_offset,
					   result->interp_path, len) != 0)
				continue;
			result->interp_path[len] = '\0';
			// Trim trailing NUL that the ELF may include in p_filesz
			while (len > 0 && result->interp_path[len - 1] == '\0')
				len--;
			result->has_interp = 1;
		}
	}

	// ---- Second pass: PT_LOAD + PT_PHDR ----
	for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
		const Elf64_Phdr *ph =
			(const Elf64_Phdr *)((const uint8_t *)phdrs +
					     (size_t)i * ehdr->e_phentsize);

		if (ph->p_type == PT_PHDR) {
			result->phdr_addr = ph->p_vaddr + base_offset;
			continue;
		}
		if (ph->p_type != PT_LOAD)
			continue;

		// File-bounds check
		if (ph->p_offset + ph->p_filesz > elf_size)
			return -9;

		WARN_RATELIMIT(
			ph->p_filesz > ph->p_memsz,
			"ELF: p_filesz (%lu) > p_memsz (%lu) in segment %u",
			(unsigned long)ph->p_filesz, (unsigned long)ph->p_memsz,
			i);
		WARN_ON(ph->p_align != 0 &&
			(ph->p_align & (ph->p_align - 1)) !=
				0); /* p_align must be power-of-2 */

		uint64_t seg_vaddr = ph->p_vaddr + base_offset;
		uint64_t seg_end = seg_vaddr + ph->p_memsz;

		if (seg_vaddr < USER_SPACE_START || seg_end > USER_SPACE_END)
			return -10;

		if (seg_vaddr < result->load_base)
			result->load_base = seg_vaddr;
		if (seg_end > result->load_end)
			result->load_end = seg_end;

		// Page flags
		uint64_t flags = PAGE_PRESENT | PAGE_USER;
		if (ph->p_flags & PF_W)
			flags |= PAGE_WRITABLE;
		if (!(ph->p_flags & PF_X))
			flags |= PAGE_NO_EXECUTE;

		uint64_t vaddr_start = seg_vaddr & ~0xFFFULL;
		uint64_t vaddr_end = (seg_end + 0xFFF) & ~0xFFFULL;

		/* Demand-paged executable text/rodata: in file mode, a
		 * non-writable segment whose file offset is page-congruent
		 * with its vaddr is not loaded here at all — the pages
		 * containing its file bytes become a file-backed lazy region
		 * and are paged in from disk on first touch.  Writable
		 * segments (small .data) and non-congruent oddballs stay
		 * eager.  Fall back to eager if the first page is already
		 * mapped (an overlapping earlier segment owns it — the fault
		 * handler would never fire for a present page). */
		int lazy_file_seg = 0;
		if (file && !(ph->p_flags & PF_W) && ph->p_filesz > 0 &&
		    ((ph->p_offset & 0xFFFULL) == (ph->p_vaddr & 0xFFFULL)) &&
		    result->num_lazy_regions < ELF_MAX_LAZY_REGIONS &&
		    mm_get_physical_address_from_pml4(pml4, vaddr_start) == 0) {
			uint64_t fend = (seg_vaddr + ph->p_filesz + 0xFFFULL) &
					~0xFFFULL;
			uint64_t rprot = PROT_READ;
			if (ph->p_flags & PF_X)
				rprot |= PROT_EXEC;
			int n = result->num_lazy_regions++;
			result->lazy_regions[n].start = vaddr_start;
			result->lazy_regions[n].length = fend - vaddr_start;
			result->lazy_regions[n].prot = rprot;
			result->lazy_regions[n].file_off =
				ph->p_offset - (ph->p_vaddr & 0xFFFULL);
			result->lazy_regions[n].file_idx = (uint8_t)file_idx;
			lazy_file_seg = 1;
		}

		for (uint64_t va = vaddr_start;
		     lazy_file_seg == 0 && va < vaddr_end; va += PAGE_SIZE) {
			// Copy window of file data that falls within this page.
			// The segment occupies user addresses [seg_vaddr, seg_vaddr+p_filesz)
			// for file-backed data and [seg_vaddr+p_filesz, seg_end) for BSS (zeros).
			uint64_t page_lo = va;
			uint64_t page_hi = va + PAGE_SIZE;

			uint64_t data_lo = seg_vaddr;
			uint64_t data_hi = seg_vaddr + ph->p_filesz;

			uint64_t cpy_lo =
				(page_lo > data_lo) ? page_lo : data_lo;
			uint64_t cpy_hi =
				(page_hi < data_hi) ? page_hi : data_hi;

			// Check if page already mapped (overlapping segments)
			uint64_t existing =
				mm_get_physical_address_from_pml4(pml4, va);
			uint8_t *page_ptr;

			if (existing) {
				page_ptr = (uint8_t *)phys_to_virt(existing);
			} else {
				/* Demand paging: a page with NO file bytes is
				 * pure BSS — leave it unmapped.  The caller
				 * registers the range (recorded below) as a
				 * lazy region and the fault handler zero-
				 * fills on first touch.  A 67 MB BSS then
				 * costs nothing until actually used. */
				if (cpy_lo >= cpy_hi)
					continue;
				uint64_t phys = mm_allocate_physical_page();
				if (!phys)
					return -11;
				mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
				if (!mm_map_page_in_address_space(
					    pml4, va, phys, flags)) {
					mm_free_physical_page(phys);
					return -12;
				}
				page_ptr = (uint8_t *)phys_to_virt(phys);
			}

			if (cpy_lo < cpy_hi) {
				uint64_t dst_off = cpy_lo - va;
				uint64_t file_off =
					ph->p_offset + (cpy_lo - seg_vaddr);
				uint64_t len = cpy_hi - cpy_lo;
				if (file_off + len <= elf_size &&
				    elf_image_read(whole, file, file_off,
						   page_ptr + dst_off,
						   len) != 0)
					return -13;
			}
		}

		// Record the pure-BSS page range (if any) for lazy zero-fill.
		{
			uint64_t lazy_lo =
				(seg_vaddr + ph->p_filesz + 0xFFF) & ~0xFFFULL;
			if (vaddr_end > lazy_lo &&
			    result->num_lazy_regions < ELF_MAX_LAZY_REGIONS) {
				uint64_t rprot = PROT_READ;
				if (ph->p_flags & PF_W)
					rprot |= PROT_WRITE;
				if (ph->p_flags & PF_X)
					rprot |= PROT_EXEC;
				int n = result->num_lazy_regions++;
				result->lazy_regions[n].start = lazy_lo;
				result->lazy_regions[n].length =
					vaddr_end - lazy_lo;
				result->lazy_regions[n].prot = rprot;
			} else {
				WARN_ON(vaddr_end > lazy_lo &&
					result->num_lazy_regions >=
						ELF_MAX_LAZY_REGIONS);
			}
		}
	}

	// phdr_addr fallback
	if (result->phdr_addr == 0 && result->load_base != ~0ULL)
		result->phdr_addr = result->load_base + ehdr->e_phoff;

	result->entry_point = ehdr->e_entry + base_offset;
	WARN_ON(result->entry_point < USER_SPACE_START ||
		result->entry_point >=
			USER_SPACE_END); /* entry point outside user range */
	result->brk_start = (result->load_end + 0xFFF) & ~0xFFFULL;
	return 0;
}

/* Classic whole-buffer wrapper (image fully in memory, nothing file-lazy). */
static int elf_load_segments(const void *elf_data, size_t elf_size,
			     uint64_t *pml4, uint64_t base_offset,
			     elf_load_result_t *result)
{
	const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
	const Elf64_Phdr *phdrs =
		(const Elf64_Phdr *)((const uint8_t *)elf_data + ehdr->e_phoff);
	return elf_load_segments_ex(ehdr, phdrs, (const uint8_t *)elf_data,
				    NULL, elf_size, pml4, base_offset, 0,
				    result);
}

/* Demand-paged ELF load: reads ONLY the ehdr + program headers from the
 * file.  Non-writable segments become file-backed lazy regions (recorded
 * in result->lazy_regions with the given file_idx tag) and are paged in
 * on first touch; writable .data is read eagerly; BSS stays lazy-anon.
 * The caller owns `file` and must keep it open until the lazy regions
 * have been registered on the task (registration takes its own refs).
 * base_override: ~0ULL = automatic (ET_DYN → 0x400000), else explicit. */
int elf_load_user_file(vfs_file_t *file, uint64_t *pml4, uint64_t base_override,
		       int file_idx, elf_load_result_t *result)
{
	if (!file || !pml4 || !result)
		return -1;
	size_t sz = vfs_size(file);
	if (sz < sizeof(Elf64_Ehdr) || sz > 64 * 1024 * 1024)
		return -2;

	Elf64_Ehdr ehdr;
	if (elf_image_read(NULL, file, 0, &ehdr, sizeof(ehdr)) != 0)
		return -3;
	int rc = elf_validate(&ehdr, sz); /* dereferences ehdr fields only */
	if (rc != 0)
		return rc;
	if (ehdr.e_phentsize != sizeof(Elf64_Phdr) || ehdr.e_phnum > 64)
		return -4;

	size_t phbytes = (size_t)ehdr.e_phnum * ehdr.e_phentsize;
	Elf64_Phdr *phdrs = (Elf64_Phdr *)kalloc(phbytes);
	if (!phdrs)
		return -5;
	if (elf_image_read(NULL, file, ehdr.e_phoff, phdrs, phbytes) != 0) {
		kfree(phdrs);
		return -6;
	}

	uint64_t base = 0;
	if (ehdr.e_type == ET_DYN)
		base = (base_override != ~0ULL) ? base_override : 0x400000ULL;
	else if (base_override != ~0ULL)
		base = base_override;

	rc = elf_load_segments_ex(&ehdr, phdrs, NULL, file, sz, pml4, base,
				  file_idx, result);
	kfree(phdrs);
	return rc;
}

// ============================================================================
// PUBLIC: elf_load_user
// ============================================================================

int elf_load_user(const void *elf_data, size_t elf_size, uint64_t *pml4,
		  elf_load_result_t *result)
{
	if (!elf_data || !pml4 || !result)
		return -1;

	int rc = elf_validate(elf_data, elf_size);
	if (rc != 0)
		return rc;

	const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
	uint64_t base = (ehdr->e_type == ET_DYN) ? 0x400000ULL : 0;

	return elf_load_segments(elf_data, elf_size, pml4, base, result);
}

// ============================================================================
// INTERPRETER LOADING
// ============================================================================

static int elf_load_interp(const char *path, uint64_t *pml4,
			   uint64_t *out_entry, uint64_t *out_base,
			   elf_load_result_t *main_result)
{
	vfs_file_t *file = NULL;
	int ret = vfs_open(path, 0, &file);
	if (ret != 0 || !file) {
		kprintf("elf: cannot open interp '%s' (err %d)\n", path, ret);
		return -1;
	}

	uint64_t interp_base = 0x7F0000000000ULL; // High address, clear of app
	elf_load_result_t ir;
	mm_memset(&ir, 0, sizeof(ir));
	/* Demand-paged: only ehdr+phdrs are read; the interpreter's text is
	 * registered as file-backed lazy regions tagged file_idx 2. */
	ret = elf_load_user_file(file, pml4, interp_base, 2, &ir);
	if (ret != 0) {
		vfs_close(file);
		return ret;
	}
	if (!ir.is_dynamic) {
		vfs_close(file);
		return -5;
	}

	/* Propagate the interpreter's lazy regions (file-backed text +
	 * anon BSS) into the main result.  Ownership of the open interp
	 * file transfers to main_result->backing[1]; the caller closes it
	 * after registering the regions (registration takes its own refs). */
	if (main_result) {
		for (int i = 0; i < ir.num_lazy_regions; i++) {
			if (main_result->num_lazy_regions >=
			    ELF_MAX_LAZY_REGIONS) {
				/* Dropping a FILE region would leave interp
				 * text unmapped and unfixable — fail hard. */
				WARN_ON(1);
				vfs_close(file);
				return -6;
			}
			main_result->lazy_regions
				[main_result->num_lazy_regions++] =
				ir.lazy_regions[i];
		}
		main_result->backing[1] = file;
	} else {
		/* No result to carry the lazy regions — the interp's demand-
		 * paged text could never be registered or paged in. */
		vfs_close(file);
		return -7;
	}

	*out_entry = ir.entry_point;
	*out_base = interp_base;
	return 0;
}

// ============================================================================
// STACK BUILDER  (argc, argv, envp, auxv)
// ============================================================================

static size_t elf_strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

/* `newcred' is the credential set the image being loaded will actually run
 * with.  It is passed in rather than read from the current task because a
 * set-id exec has not committed its transition yet at this point: the ids are
 * only applied once the image has been replaced successfully, so that a failed
 * exec cannot leave a process privileged in its old image.  Reading the task
 * here would therefore describe the CALLER, not the program -- which is how
 * AT_SECURE came to be reported as 0 for every setuid exec.  NULL means "no
 * transition pending", i.e. the current task's credentials are the answer. */
static uint64_t elf_setup_stack(uint64_t *pml4, uint64_t stack_top,
				uint64_t stack_size, char *const argv[],
				char *const envp[], elf_load_result_t *mr,
				uint64_t interp_base, const cred_t *newcred)
{
	BUG_ON(pml4 == NULL || mr == NULL);
	if (!mm_map_user_stack(pml4, stack_top, stack_size))
		return 0;

	int argc = 0;
	if (argv)
		while (argv[argc])
			argc++;
	int envc = 0;
	if (envp)
		while (envp[envc])
			envc++;
	WARN_ON_ONCE(
		argc > 127 ||
		envc > 127); /* argc/envc exceeds av[128]/ev[128] stack buffer: execve with pathologically many args */

	// Auxiliary vector entries
	typedef struct {
		uint64_t t, v;
	} ax_t;
	ax_t ax[16];
	int ac = 0;
	ax[ac].t = AT_PHDR;
	ax[ac].v = mr->phdr_addr;
	ac++;
	ax[ac].t = AT_PHENT;
	ax[ac].v = mr->phentsize;
	ac++;
	ax[ac].t = AT_PHNUM;
	ax[ac].v = mr->phnum;
	ac++;
	ax[ac].t = AT_PAGESZ;
	ax[ac].v = PAGE_SIZE;
	ac++;
	ax[ac].t = AT_ENTRY;
	ax[ac].v = mr->entry_point;
	ac++;
	ax[ac].t = AT_BASE;
	ax[ac].v = interp_base;
	ac++;
	/* The credentials the new image starts with.  Reported truthfully rather
	 * than as zeroes: AT_SECURE is how a loader is told that this exec
	 * crossed a privilege boundary and that it must ignore anything the
	 * caller could have planted in the environment.  Our own loader takes
	 * no library path from the environment at all, so nothing depends on
	 * this today -- but a loader that grew one and trusted a hardcoded
	 * "not secure" here would hand root to any user with a setuid binary
	 * to point it at, and nothing about that would be visible from the
	 * loader's side. */
	{
		task_t *cur = sched_current();
		const cred_t *cc = newcred ? newcred : (cur ? &cur->cred : NULL);
		uint32_t uid = cc ? cc->uid : 0;
		uint32_t euid = cc ? cc->euid : 0;
		uint32_t gid = cc ? cc->gid : 0;
		uint32_t egid = cc ? cc->egid : 0;

		ax[ac].t = AT_UID;
		ax[ac].v = uid;
		ac++;
		ax[ac].t = AT_EUID;
		ax[ac].v = euid;
		ac++;
		ax[ac].t = AT_GID;
		ax[ac].v = gid;
		ac++;
		ax[ac].t = AT_EGID;
		ax[ac].v = egid;
		ac++;
		ax[ac].t = AT_SECURE;
		ax[ac].v = (uid != euid || gid != egid) ? 1 : 0;
		ac++;
	}
	ax[ac].t = AT_NULL;
	ax[ac].v = 0;
	ac++;

	// Total string space
	size_t str_total = 0;
	for (int i = 0; i < argc; i++)
		str_total += elf_strlen(argv[i]) + 1;
	for (int i = 0; i < envc; i++)
		str_total += elf_strlen(envp[i]) + 1;

	// Pointers area: argc + argv[] + NULL + envp[] + NULL + auxv
	size_t ptrs = 8 + (argc + 1) * 8 + (envc + 1) * 8 + ac * 16;
	size_t total = ptrs + ((str_total + 15) & ~15ULL);
	total = (total + 15) & ~15ULL;

	if (total > PAGE_SIZE)
		return 0; // Won't fit in one-page model

	// Temp buffer for the top stack page
	uint8_t *buf = (uint8_t *)kalloc(PAGE_SIZE);
	if (!buf)
		return 0;
	mm_memset(buf, 0, PAGE_SIZE);

	uint64_t top_page = stack_top - PAGE_SIZE;

	// Strings: place from end of buffer downward
	uint8_t *sw = buf + PAGE_SIZE;
	uint64_t sv = stack_top;
	uint64_t av[128], ev[128];

	for (int i = argc - 1; i >= 0; i--) {
		size_t l = elf_strlen(argv[i]) + 1;
		sw -= l;
		sv -= l;
		mm_memcpy(sw, argv[i], l);
		av[i] = sv;
	}
	for (int i = envc - 1; i >= 0; i--) {
		size_t l = elf_strlen(envp[i]) + 1;
		sw -= l;
		sv -= l;
		mm_memcpy(sw, envp[i], l);
		ev[i] = sv;
	}

	uint64_t sp_va = (sv - ptrs) & ~15ULL;
	if (sp_va < top_page) {
		kfree(buf);
		return 0;
	}

	uint64_t *sp = (uint64_t *)(buf + (sp_va - top_page));
	*sp++ = (uint64_t)argc;
	for (int i = 0; i < argc; i++)
		*sp++ = av[i];
	*sp++ = 0;
	for (int i = 0; i < envc; i++)
		*sp++ = ev[i];
	*sp++ = 0;
	/* Remember where the auxiliary vector landed.
	 *
	 * A debugger needs it, and needs it before it can read anything else
	 * usefully.  The executable is position-independent, so every address
	 * in its symbol table is an offset until the debugger knows the bias it
	 * was loaded at; the conventional way to learn that is to compare
	 * AT_ENTRY here against e_entry in the file on disk.  Nothing else in
	 * the process reveals it -- reading the dynamic section to find the
	 * loader's rendezvous would already require knowing the bias.
	 *
	 * Recorded as an address and a length rather than copied: the block
	 * belongs to the process, and ptrace hands it out from here. */
	{
		uint64_t auxv_va = sp_va + 8 + (uint64_t)(argc + 1) * 8 +
				   (uint64_t)(envc + 1) * 8;
		task_t *cur = sched_current();
		if (cur) {
			cur->auxv_addr = auxv_va;
			cur->auxv_len = (uint64_t)ac * 16;
		}
	}

	for (int i = 0; i < ac; i++) {
		*sp++ = ax[i].t;
		*sp++ = ax[i].v;
	}

	// Flush buffer to physical page
	uint64_t phys = mm_get_physical_address_from_pml4(pml4, top_page);
	if (phys)
		mm_memcpy(phys_to_virt(phys), buf, PAGE_SIZE);
	kfree(buf);

	return sp_va;
}

// ============================================================================
// Per-process initial TLS block with hardware-random stack canary
// ============================================================================
//
// GCC -fstack-protector-strong emits `mov %fs:0x28,%rax` to read the canary.
// We map one page at USER_INITIAL_TLS_VA into every new process's address
// space and write a hardware-random 64-bit value at offset 0x28 (= fs:0x28).
// The self-pointer at offset 0 satisfies the glibc/musl TCB convention.
// The dynamic linker / libc may later replace this via arch_prctl(ARCH_SET_FS).
//
#define USER_INITIAL_TLS_VA \
	0x7FFFE0000000ULL /* well clear of stack and interp */

static void setup_user_tls_canary(uint64_t *pml4, task_t *task)
{
	uint64_t phys = mm_allocate_physical_page();
	if (!phys)
		return;

	uint8_t *kp = (uint8_t *)phys_to_virt(phys);
	mm_memset(kp, 0, PAGE_SIZE);

	/* TCB self-pointer at offset 0 (required by glibc / musl). */
	*(uint64_t *)(kp + 0x00) = USER_INITIAL_TLS_VA;

	/* Generate a per-process stack canary (ChaCha20 CSPRNG backed). */
	*(uint64_t *)(kp + 0x28) = generate_stack_canary(); /* fs:0x28 */

	uint64_t flags =
		PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NO_EXECUTE;
	if (!mm_map_page_in_address_space(pml4, USER_INITIAL_TLS_VA, phys,
					  flags)) {
		mm_free_physical_page(phys);
		return;
	}
	task->fs_base = USER_INITIAL_TLS_VA;
}

// ============================================================================
// PUBLIC: elf_exec  (launch new task)
// ============================================================================

int elf_exec(const char *path, char *const argv[], char *const envp[],
	     task_t **out_task)
{
	might_sleep();
	BUG_ON(out_task == NULL);
	if (!path)
		return -1;

	vfs_file_t *file = NULL;
	int ret = vfs_open(path, 0, &file);
	if (ret || !file) {
		kprintf("elf_exec: open '%s' err %d\n", path, ret);
		return -1;
	}

	uint64_t *pml4 = mm_create_user_address_space();
	if (!pml4) {
		vfs_close(file);
		return -5;
	}

	elf_load_result_t lr;
	mm_memset(&lr, 0, sizeof(lr));
	/* Demand-paged exec: only ehdr+phdrs are read here.  Text/rodata
	 * page in from the file on first touch; the file stays open, owned
	 * by lr.backing[0] until the regions are registered on the task. */
	ret = elf_load_user_file(file, pml4, ~0ULL, 1, &lr);
	if (ret) {
		vfs_close(file);
		mm_destroy_address_space(pml4);
		return -6;
	}
	lr.backing[0] = file;

	uint64_t entry = lr.entry_point;
	uint64_t ib = 0;

	if (lr.has_interp) {
		uint64_t ie = 0;
		ret = elf_load_interp(lr.interp_path, pml4, &ie, &ib, &lr);
		if (ret) {
			kprintf("elf_exec: interp '%s' err %d\n",
				lr.interp_path, ret);
			vfs_close(lr.backing[0]);
			mm_destroy_address_space(pml4);
			return -6;
		}
		entry = ie;
		lr.interp_base = ib;
		lr.interp_entry = ie;
	}

#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_SIZE \
	(2 * 1024 * 1024) /* 2MB — matches memory.h. Smaller stacks (64K)
                                                 * cause ports/lib/* (libevent, ncurses, tmux's
                                                 * deeply-recursive parser/log paths) to overflow
                                                 * silently and SIGSEGV on the guard region. */

	/* No set-id transition happens on this path -- it spawns a fresh task
	 * rather than replacing an image -- so the current credentials are
	 * already the ones the program will run with. */
	uint64_t sp = elf_setup_stack(pml4, USER_STACK_TOP, USER_STACK_SIZE,
				      argv, envp, &lr, ib, NULL);
	if (!sp) {
		for (int b = 0; b < 2; b++)
			if (lr.backing[b])
				vfs_close(lr.backing[b]);
		mm_destroy_address_space(pml4);
		return -7;
	}

	task_t *t = sched_add_user_task((task_entry_t)entry, NULL, pml4, sp, 0);
	if (!t) {
		for (int b = 0; b < 2; b++)
			if (lr.backing[b])
				vfs_close(lr.backing[b]);
		mm_destroy_address_space(pml4);
		return -10;
	}

	t->brk_start = lr.brk_start;
	t->brk = lr.brk_start;
	t->user_stack_top = USER_STACK_TOP;
	t->mmap_base = USER_STACK_TOP - (4 * 1024 * 1024);

	/* Register the loader's lazy ranges: anonymous BSS (zero-filled on
	 * first touch) and demand-paged executable/interpreter segments
	 * (paged in from their backing file).  Registration takes its own
	 * vfs references, so release the loader's after. */
	for (int i = 0; i < lr.num_lazy_regions; i++) {
		vfs_file_t *bf =
			lr.lazy_regions[i].file_idx ?
				lr.backing[lr.lazy_regions[i].file_idx - 1] :
				NULL;
		task_register_lazy_region(t, lr.lazy_regions[i].start,
					  lr.lazy_regions[i].length,
					  lr.lazy_regions[i].prot, bf,
					  lr.lazy_regions[i].file_off);
	}
	for (int b = 0; b < 2; b++)
		if (lr.backing[b])
			vfs_close(lr.backing[b]);

	/* Allocate per-process TLS page with random canary at fs:0x28.
     * This MUST happen before sched_enqueue_ready: on SMP an AP could
     * pick up the task immediately after enqueue, run arch_prctl to set
     * fs_base = &_bootstrap_tls, and then we would overwrite it here
     * with USER_INITIAL_TLS_VA — causing a false stack-protector mismatch
     * on the next context switch. */
	setup_user_tls_canary(pml4, t);

	/* Now safe to make the task runnable: fs_base is fully initialised. */
	sched_enqueue_ready(t);

	task_t *cur = sched_current();
	if (cur) {
		t->parent = cur;
		sched_add_child(cur, t);
		// Inherit session/group from parent, but only override ctty
		// if parent actually has one (otherwise keep the default from
		// sched_add_user_task which sets tty_get_console)
		t->pgid = cur->pgid;
		t->sid = cur->sid;
		if (cur->ctty)
			t->ctty = cur->ctty;
		mm_memset(t->cwd, 0, sizeof(t->cwd));
		if (cur->cwd[0]) {
			size_t i = 0;
			for (; cur->cwd[i] && i < sizeof(t->cwd) - 1; i++)
				t->cwd[i] = cur->cwd[i];
			t->cwd[i] = '\0';
		} else {
			t->cwd[0] = '/';
			t->cwd[1] = '\0';
		}
	}
	// Set comm from basename of path
	{
		const char *src = path;
		const char *p2 = src;
		while (*p2) {
			if (*p2 == '/')
				src = p2 + 1;
			p2++;
		}
		int ci;
		for (ci = 0; ci < 255 && src[ci]; ci++)
			t->comm[ci] = src[ci];
		t->comm[ci] = '\0';
	}
	// Build cmdline from argv (space-separated)
	{
		int pos = 0;
		if (argv) {
			for (int a = 0; argv[a] && pos < 1023; a++) {
				if (a > 0 && pos < 1023)
					t->cmdline[pos++] = ' ';
				for (int c = 0; argv[a][c] && pos < 1023; c++)
					t->cmdline[pos++] = argv[a][c];
			}
		}
		t->cmdline[pos] = '\0';
	}
	// Build environ from envp (space-separated)
	{
		int pos = 0;
		if (envp) {
			for (int a = 0; envp[a] && pos < 2047; a++) {
				if (a > 0 && pos < 2047)
					t->environ[pos++] = ' ';
				for (int c = 0; envp[a][c] && pos < 2047; c++)
					t->environ[pos++] = envp[a][c];
			}
		}
		t->environ[pos] = '\0';
	}

	if (out_task)
		*out_task = t;
	return 0;
}

// ============================================================================
// PUBLIC: elf_exec_replace  (execve semantics)
// ============================================================================

uint64_t elf_exec_replace(const char *path, char *const argv[],
			  char *const envp[], uint64_t *out_stack_ptr,
			  const cred_t *newcred)
{
	if (!path || !out_stack_ptr)
		return 0;
	task_t *cur = sched_current();
	if (!cur)
		return 0;

	vfs_file_t *file = NULL;
	int ret = vfs_open(path, 0, &file);
	if (ret || !file)
		return 0;

	uint64_t *old = cur->pml4;
	uint64_t *pml4 = mm_create_user_address_space();
	if (!pml4) {
		vfs_close(file);
		return 0;
	}

	elf_load_result_t lr;
	mm_memset(&lr, 0, sizeof(lr));
	/* Demand-paged execve: only ehdr+phdrs are read; text/rodata page
	 * in on first touch (see elf_exec). */
	ret = elf_load_user_file(file, pml4, ~0ULL, 1, &lr);
	if (ret) {
		vfs_close(file);
		mm_destroy_address_space(pml4);
		return 0;
	}
	lr.backing[0] = file;

	uint64_t entry = lr.entry_point;
	uint64_t ib = 0;

	if (lr.has_interp) {
		uint64_t ie = 0;
		ret = elf_load_interp(lr.interp_path, pml4, &ie, &ib, &lr);
		if (ret) {
			vfs_close(lr.backing[0]);
			mm_destroy_address_space(pml4);
			return 0;
		}
		entry = ie;
		lr.interp_base = ib;
		lr.interp_entry = ie;
	}

#define USER_STACK_TOP_EXEC 0x00007FFFFFF00000ULL
#define USER_STACK_SIZE_EXEC \
	(2 * 1024 * 1024) /* See note in elf_load_and_run. */

	uint64_t sp =
		elf_setup_stack(pml4, USER_STACK_TOP_EXEC, USER_STACK_SIZE_EXEC,
				argv, envp, &lr, ib, newcred);
	if (!sp) {
		for (int b = 0; b < 2; b++)
			if (lr.backing[b])
				vfs_close(lr.backing[b]);
		mm_destroy_address_space(pml4);
		return 0;
	}

	/* Allocate a fresh per-process TLS page with a new random canary.
     * Must be done before mm_destroy_address_space(old) frees the old TLS. */
	setup_user_tls_canary(pml4, cur);

	cur->pml4 = pml4;
	/* And the shared record, if this process has one.
	 *
	 * Any process that has ever created a thread carries an mm_struct, and
	 * it keeps its own copy of the page-table root.  Updating only the task
	 * left that copy pointing at the address space destroyed a few lines
	 * below -- so the next release of the mm_struct handed a long-dead page
	 * table to be taken apart a second time, feeding the page-table pool
	 * memory it had already been given.  Harmless only for as long as
	 * nothing routed an ordinary exit through the mm_struct; the teardown
	 * path does. */
	if (cur->mm)
		cur->mm->pml4 = pml4;
	cur->brk_start = lr.brk_start;
	cur->brk = lr.brk_start;
	cur->user_stack_top = USER_STACK_TOP_EXEC;
	/* Clear stale mmap_region slots inherited from parent via fork+exec,
	 * releasing any file references pinned for demand paging. */
	for (uint32_t i = 0; i < cur->mmap_capacity; i++) {
		if (cur->mmap_regions[i].in_use && cur->mmap_regions[i].file)
			vfs_close(cur->mmap_regions[i].file);
		cur->mmap_regions[i].file = NULL;
		cur->mmap_regions[i].lazy = false;
		cur->mmap_regions[i].in_use = false;
	}
	cur->mmap_base = USER_STACK_TOP_EXEC - (4 * 1024 * 1024);

	/* Register the new image's lazy ranges (anon BSS + demand-paged
	 * executable/interpreter segments), then drop the loader's file
	 * references — registration took its own. */
	for (int i = 0; i < lr.num_lazy_regions; i++) {
		vfs_file_t *bf =
			lr.lazy_regions[i].file_idx ?
				lr.backing[lr.lazy_regions[i].file_idx - 1] :
				NULL;
		task_register_lazy_region(cur, lr.lazy_regions[i].start,
					  lr.lazy_regions[i].length,
					  lr.lazy_regions[i].prot, bf,
					  lr.lazy_regions[i].file_off);
	}
	for (int b = 0; b < 2; b++)
		if (lr.backing[b])
			vfs_close(lr.backing[b]);

	/* Close ONLY the descriptors marked close-on-exec.  POSIX keeps every
	 * other descriptor open across exec, and programs depend on it: a
	 * shell hands a child an already-open pipe end (process substitution
	 * passes it as /dev/fd/N), a service manager passes listening sockets
	 * down, and so on.  Closing everything >= 3 unconditionally broke all
	 * of that - the child found the descriptor gone (EBADF). */
	for (int i = 3; i < TASK_MAX_FDS; i++) {
		if (!(task_get_fd_flags(cur, (unsigned)i) & FD_CLOEXEC))
			continue;
		task_set_fd_flags(cur, (unsigned)i, 0);
		if (task_fds(cur)[i]) {
			uint64_t marker = (uint64_t)task_fds(cur)[i];
			if (marker >= 1 && marker <= 3) {
				task_fds(cur)[i] = NULL;
			} else if (IS_SOCKET_FD(task_fds(cur)[i])) {
				int idx = SOCKET_FD_IDX(task_fds(cur)[i]);
				task_fds(cur)[i] = NULL;
				sock_close(idx);
			} else if (unix_sock_is(task_fds(cur)[i])) {
				unix_socket_t *ufd =
					(unix_socket_t *)task_fds(cur)[i];
				task_fds(cur)[i] = NULL;
				unix_close(ufd);
			} else if (IS_EPOLL_FD(task_fds(cur)[i])) {
				/* Release this descriptor's reference; the
				 * instance stays alive for anyone else holding
				 * one.  Closing it outright here destroyed the
				 * epoll set of the process that forked us. */
				int idx = EPOLL_FD_IDX(task_fds(cur)[i]);

				task_fds(cur)[i] = NULL;
				epoll_put(idx);
			} else if (pipe_is_end(task_fds(cur)[i])) {
				pipe_close_end((pipe_end_t *)task_fds(cur)[i]);
				task_fds(cur)[i] = NULL;
			} else {
				vfs_close(task_fds(cur)[i]);
				task_fds(cur)[i] = NULL;
			}
		}
	}

	mm_switch_address_space(pml4);
	/* The address space being discarded is the one to invalidate, not the
	 * one just loaded -- and only on CPUs that still have it.  This CPU has
	 * already left it on the line above. */
	if (smp_is_enabled() && old)
		smp_tlb_shootdown_mm_sync(virt_to_phys(old));
	else if (smp_is_enabled())
		smp_tlb_shootdown_sync();
	if (old) {
		MM_LEAK_INC(g_as_destroy_exec);
		mm_destroy_address_space(old);
	}

	*out_stack_ptr = sp;
	return entry;
}
