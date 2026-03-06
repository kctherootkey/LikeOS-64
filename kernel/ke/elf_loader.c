// LikeOS-64 ELF64 Loader Implementation
// Supports static (ET_EXEC) and dynamic/PIE (ET_DYN) executables.
// When PT_INTERP is present, loads the dynamic linker (ld-likeos.so) and
// passes control to it with an auxiliary vector on the stack.
#include <kernel/elf.h>
#include <kernel/memory.h>
#include <kernel/sched.h>
#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/pipe.h>
#include <kernel/smp.h>

// ============================================================================
// VALIDATION
// ============================================================================

int elf_validate(const void* data, size_t size) {
    if (!data || size < sizeof(Elf64_Ehdr)) return -1;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) return -2;

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)  return -3;
    if (ehdr->e_ident[EI_DATA]  != ELFDATA2LSB) return -4;

    // Accept both ET_EXEC (static) and ET_DYN (PIE / shared object)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return -5;
    if (ehdr->e_machine != EM_X86_64) return -6;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -7;

    uint64_t ph_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end > size) return -8;

    return 0;
}

// ============================================================================
// SEGMENT LOADER  (common for main binary & interpreter)
// ============================================================================

// Load PT_LOAD segments of an ELF into *pml4* at the given base offset.
// For ET_EXEC base_offset = 0 (absolute addresses in ELF).
// For ET_DYN  base_offset shifts every p_vaddr.
static int elf_load_segments(const void* elf_data, size_t elf_size,
                             uint64_t* pml4, uint64_t base_offset,
                             elf_load_result_t* result) {
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)elf_data;
    const uint8_t* elf_bytes = (const uint8_t*)elf_data;

    result->load_base    = ~0ULL;
    result->load_end     = 0;
    result->brk_start    = 0;
    result->phdr_addr    = 0;
    result->has_interp   = 0;
    result->is_dynamic   = (ehdr->e_type == ET_DYN);
    result->interp_base  = 0;
    result->interp_entry = 0;
    result->interp_path[0] = '\0';
    result->phnum        = ehdr->e_phnum;
    result->phentsize    = ehdr->e_phentsize;

    // ---- First pass: PT_INTERP ----
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf_bytes + ehdr->e_phoff +
                                                    i * ehdr->e_phentsize);
        if (ph->p_type == PT_INTERP && ph->p_filesz > 0 &&
            ph->p_filesz < 256 && ph->p_offset + ph->p_filesz <= elf_size) {
            size_t len = ph->p_filesz;
            if (len > 255) len = 255;
            for (size_t j = 0; j < len; j++)
                result->interp_path[j] = (char)elf_bytes[ph->p_offset + j];
            result->interp_path[len] = '\0';
            // Trim trailing NUL that the ELF may include in p_filesz
            while (len > 0 && result->interp_path[len - 1] == '\0') len--;
            result->has_interp = 1;
        }
    }

    // ---- Second pass: PT_LOAD + PT_PHDR ----
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf_bytes + ehdr->e_phoff +
                                                    i * ehdr->e_phentsize);

        if (ph->p_type == PT_PHDR) {
            result->phdr_addr = ph->p_vaddr + base_offset;
            continue;
        }
        if (ph->p_type != PT_LOAD) continue;

        // File-bounds check
        if (ph->p_offset + ph->p_filesz > elf_size) return -9;

        uint64_t seg_vaddr = ph->p_vaddr + base_offset;
        uint64_t seg_end   = seg_vaddr + ph->p_memsz;

        if (seg_vaddr < USER_SPACE_START || seg_end > USER_SPACE_END)
            return -10;

        if (seg_vaddr < result->load_base) result->load_base = seg_vaddr;
        if (seg_end   > result->load_end)  result->load_end  = seg_end;

        // Page flags
        uint64_t flags = PAGE_PRESENT | PAGE_USER;
        if (ph->p_flags & PF_W)   flags |= PAGE_WRITABLE;
        if (!(ph->p_flags & PF_X)) flags |= PAGE_NO_EXECUTE;

        uint64_t vaddr_start = seg_vaddr & ~0xFFFULL;
        uint64_t vaddr_end   = (seg_end + 0xFFF) & ~0xFFFULL;

        for (uint64_t va = vaddr_start; va < vaddr_end; va += PAGE_SIZE) {
            // Check if page already mapped (overlapping segments)
            uint64_t existing = mm_get_physical_address_from_pml4(pml4, va);
            uint8_t* page_ptr;

            if (existing) {
                page_ptr = (uint8_t*)phys_to_virt(existing);
            } else {
                uint64_t phys = mm_allocate_physical_page();
                if (!phys) return -11;
                mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
                if (!mm_map_page_in_address_space(pml4, va, phys, flags)) {
                    mm_free_physical_page(phys);
                    return -12;
                }
                page_ptr = (uint8_t*)phys_to_virt(phys);
            }

            // Copy file data that falls within this page
            // The segment occupies user addresses [seg_vaddr, seg_vaddr+p_filesz)
            // for file-backed data and [seg_vaddr+p_filesz, seg_end) for BSS (zeros).
            uint64_t page_lo = va;
            uint64_t page_hi = va + PAGE_SIZE;

            uint64_t data_lo = seg_vaddr;
            uint64_t data_hi = seg_vaddr + ph->p_filesz;

            uint64_t cpy_lo = (page_lo > data_lo) ? page_lo : data_lo;
            uint64_t cpy_hi = (page_hi < data_hi) ? page_hi : data_hi;

            if (cpy_lo < cpy_hi) {
                uint64_t dst_off  = cpy_lo - va;
                uint64_t file_off = ph->p_offset + (cpy_lo - seg_vaddr);
                uint64_t len      = cpy_hi - cpy_lo;
                if (file_off + len <= elf_size)
                    mm_memcpy(page_ptr + dst_off, elf_bytes + file_off, len);
            }
        }
    }

    // phdr_addr fallback
    if (result->phdr_addr == 0 && result->load_base != ~0ULL)
        result->phdr_addr = result->load_base + ehdr->e_phoff;

    result->entry_point = ehdr->e_entry + base_offset;
    result->brk_start   = (result->load_end + 0xFFF) & ~0xFFFULL;
    return 0;
}

// ============================================================================
// PUBLIC: elf_load_user
// ============================================================================

int elf_load_user(const void* elf_data, size_t elf_size,
                  uint64_t* pml4, elf_load_result_t* result) {
    if (!elf_data || !pml4 || !result) return -1;

    int rc = elf_validate(elf_data, elf_size);
    if (rc != 0) return rc;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)elf_data;
    uint64_t base = (ehdr->e_type == ET_DYN) ? 0x400000ULL : 0;

    return elf_load_segments(elf_data, elf_size, pml4, base, result);
}

// ============================================================================
// INTERPRETER LOADING
// ============================================================================

static int elf_load_interp(const char* path, uint64_t* pml4,
                           uint64_t* out_entry, uint64_t* out_base) {
    vfs_file_t* file = NULL;
    int ret = vfs_open(path, 0, &file);
    if (ret != 0 || !file) {
        kprintf("elf: cannot open interp '%s' (err %d)\n", path, ret);
        return -1;
    }
    size_t sz = vfs_size(file);
    if (sz == 0 || sz > 4 * 1024 * 1024) { vfs_close(file); return -2; }

    void* buf = kalloc(sz);
    if (!buf) { vfs_close(file); return -3; }

    long rd = vfs_read(file, buf, sz);
    vfs_close(file);
    if (rd != (long)sz) { kfree(buf); return -4; }

    ret = elf_validate(buf, sz);
    if (ret != 0) { kfree(buf); return ret; }

    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)buf;
    if (eh->e_type != ET_DYN) { kfree(buf); return -5; }

    uint64_t interp_base = 0x7F0000000000ULL;   // High address, clear of app
    elf_load_result_t ir;
    mm_memset(&ir, 0, sizeof(ir));
    ret = elf_load_segments(buf, sz, pml4, interp_base, &ir);
    kfree(buf);
    if (ret != 0) return ret;

    *out_entry = ir.entry_point;
    *out_base  = interp_base;
    return 0;
}

// ============================================================================
// STACK BUILDER  (argc, argv, envp, auxv)
// ============================================================================

static size_t elf_strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static uint64_t elf_setup_stack(uint64_t* pml4,
                                uint64_t stack_top, uint64_t stack_size,
                                char* const argv[], char* const envp[],
                                elf_load_result_t* mr, uint64_t interp_base) {
    if (!mm_map_user_stack(pml4, stack_top, stack_size)) return 0;

    int argc = 0;
    if (argv) while (argv[argc]) argc++;
    int envc = 0;
    if (envp) while (envp[envc]) envc++;

    // Auxiliary vector entries
    typedef struct { uint64_t t, v; } ax_t;
    ax_t ax[16];
    int ac = 0;
    ax[ac].t = AT_PHDR;   ax[ac].v = mr->phdr_addr;   ac++;
    ax[ac].t = AT_PHENT;  ax[ac].v = mr->phentsize;    ac++;
    ax[ac].t = AT_PHNUM;  ax[ac].v = mr->phnum;        ac++;
    ax[ac].t = AT_PAGESZ; ax[ac].v = PAGE_SIZE;        ac++;
    ax[ac].t = AT_ENTRY;  ax[ac].v = mr->entry_point;  ac++;
    ax[ac].t = AT_BASE;   ax[ac].v = interp_base;      ac++;
    ax[ac].t = AT_UID;    ax[ac].v = 0;  ac++;
    ax[ac].t = AT_EUID;   ax[ac].v = 0;  ac++;
    ax[ac].t = AT_GID;    ax[ac].v = 0;  ac++;
    ax[ac].t = AT_EGID;   ax[ac].v = 0;  ac++;
    ax[ac].t = AT_SECURE; ax[ac].v = 0;  ac++;
    ax[ac].t = AT_NULL;   ax[ac].v = 0;  ac++;

    // Total string space
    size_t str_total = 0;
    for (int i = 0; i < argc; i++) str_total += elf_strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) str_total += elf_strlen(envp[i]) + 1;

    // Pointers area: argc + argv[] + NULL + envp[] + NULL + auxv
    size_t ptrs = 8 + (argc + 1) * 8 + (envc + 1) * 8 + ac * 16;
    size_t total = ptrs + ((str_total + 15) & ~15ULL);
    total = (total + 15) & ~15ULL;

    if (total > PAGE_SIZE) return 0;  // Won't fit in one-page model

    // Temp buffer for the top stack page
    uint8_t* buf = (uint8_t*)kalloc(PAGE_SIZE);
    if (!buf) return 0;
    mm_memset(buf, 0, PAGE_SIZE);

    uint64_t top_page = stack_top - PAGE_SIZE;

    // Strings: place from end of buffer downward
    uint8_t* sw = buf + PAGE_SIZE;
    uint64_t sv = stack_top;
    uint64_t av[128], ev[128];

    for (int i = argc - 1; i >= 0; i--) {
        size_t l = elf_strlen(argv[i]) + 1;
        sw -= l; sv -= l;
        mm_memcpy(sw, argv[i], l);
        av[i] = sv;
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = elf_strlen(envp[i]) + 1;
        sw -= l; sv -= l;
        mm_memcpy(sw, envp[i], l);
        ev[i] = sv;
    }

    uint64_t sp_va = (sv - ptrs) & ~15ULL;
    if (sp_va < top_page) { kfree(buf); return 0; }

    uint64_t* sp = (uint64_t*)(buf + (sp_va - top_page));
    *sp++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++) *sp++ = av[i];
    *sp++ = 0;
    for (int i = 0; i < envc; i++) *sp++ = ev[i];
    *sp++ = 0;
    for (int i = 0; i < ac; i++) { *sp++ = ax[i].t; *sp++ = ax[i].v; }

    // Flush buffer to physical page
    uint64_t phys = mm_get_physical_address_from_pml4(pml4, top_page);
    if (phys) mm_memcpy(phys_to_virt(phys), buf, PAGE_SIZE);
    kfree(buf);

    return sp_va;
}

// ============================================================================
// PUBLIC: elf_exec  (launch new task)
// ============================================================================

int elf_exec(const char* path, char* const argv[], char* const envp[],
             task_t** out_task) {
    if (!path) return -1;

    vfs_file_t* file = NULL;
    int ret = vfs_open(path, 0, &file);
    if (ret || !file) { kprintf("elf_exec: open '%s' err %d\n", path, ret); return -1; }

    size_t sz = vfs_size(file);
    if (sz == 0 || sz > 16*1024*1024) { vfs_close(file); return -2; }

    void* eb = kalloc(sz);
    if (!eb) { vfs_close(file); return -3; }

    long rd = vfs_read(file, eb, sz);
    vfs_close(file);
    if (rd != (long)sz) { kfree(eb); return -4; }

    uint64_t* pml4 = mm_create_user_address_space();
    if (!pml4) { kfree(eb); return -5; }

    elf_load_result_t lr;
    mm_memset(&lr, 0, sizeof(lr));
    ret = elf_load_user(eb, sz, pml4, &lr);
    kfree(eb);
    if (ret) { mm_destroy_address_space(pml4); return -6; }

    uint64_t entry = lr.entry_point;
    uint64_t ib = 0;

    if (lr.has_interp) {
        uint64_t ie = 0;
        ret = elf_load_interp(lr.interp_path, pml4, &ie, &ib);
        if (ret) {
            kprintf("elf_exec: interp '%s' err %d\n", lr.interp_path, ret);
            mm_destroy_address_space(pml4);
            return -6;
        }
        entry = ie;
        lr.interp_base  = ib;
        lr.interp_entry = ie;
    }

    #define USER_STACK_TOP   0x00007FFFFFF00000ULL
    #define USER_STACK_SIZE  (64 * 1024)

    uint64_t sp = elf_setup_stack(pml4, USER_STACK_TOP, USER_STACK_SIZE,
                                  argv, envp, &lr, ib);
    if (!sp) { mm_destroy_address_space(pml4); return -7; }

    task_t* t = sched_add_user_task((task_entry_t)entry, NULL, pml4, sp, 0);
    if (!t) { mm_destroy_address_space(pml4); return -10; }

    t->brk_start      = lr.brk_start;
    t->brk             = lr.brk_start;
    t->user_stack_top  = USER_STACK_TOP;
    t->mmap_base       = USER_STACK_TOP - (4 * 1024 * 1024);

    task_t* cur = sched_current();
    if (cur) {
        t->parent = cur;
        sched_add_child(cur, t);
        // Inherit session/group from parent, but only override ctty
        // if parent actually has one (otherwise keep the default from
        // sched_add_user_task which sets tty_get_console)
        t->pgid = cur->pgid;
        t->sid  = cur->sid;
        if (cur->ctty)
            t->ctty = cur->ctty;
        mm_memset(t->cwd, 0, sizeof(t->cwd));
        if (cur->cwd[0]) {
            size_t i = 0;
            for (; cur->cwd[i] && i < sizeof(t->cwd) - 1; i++)
                t->cwd[i] = cur->cwd[i];
            t->cwd[i] = '\0';
        } else {
            t->cwd[0] = '/'; t->cwd[1] = '\0';
        }
    }
    // Set comm from basename of path
    {
        const char* src = path;
        const char* p2 = src;
        while (*p2) { if (*p2 == '/') src = p2 + 1; p2++; }
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
                if (a > 0 && pos < 1023) t->cmdline[pos++] = ' ';
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
                if (a > 0 && pos < 2047) t->environ[pos++] = ' ';
                for (int c = 0; envp[a][c] && pos < 2047; c++)
                    t->environ[pos++] = envp[a][c];
            }
        }
        t->environ[pos] = '\0';
    }

    if (out_task) *out_task = t;
    return 0;
}

// ============================================================================
// PUBLIC: elf_exec_replace  (execve semantics)
// ============================================================================

uint64_t elf_exec_replace(const char* path, char* const argv[],
                          char* const envp[], uint64_t* out_stack_ptr) {
    if (!path || !out_stack_ptr) return 0;
    task_t* cur = sched_current();
    if (!cur) return 0;

    vfs_file_t* file = NULL;
    int ret = vfs_open(path, 0, &file);
    if (ret || !file) return 0;

    size_t sz = vfs_size(file);
    if (sz == 0 || sz > 16*1024*1024) { vfs_close(file); return 0; }

    void* eb = kalloc(sz);
    if (!eb) { vfs_close(file); return 0; }

    long rd = vfs_read(file, eb, sz);
    vfs_close(file);
    if (rd != (long)sz) { kfree(eb); return 0; }

    uint64_t* old = cur->pml4;
    uint64_t* pml4 = mm_create_user_address_space();
    if (!pml4) { kfree(eb); return 0; }

    elf_load_result_t lr;
    mm_memset(&lr, 0, sizeof(lr));
    ret = elf_load_user(eb, sz, pml4, &lr);
    kfree(eb);
    if (ret) { mm_destroy_address_space(pml4); return 0; }

    uint64_t entry = lr.entry_point;
    uint64_t ib = 0;

    if (lr.has_interp) {
        uint64_t ie = 0;
        ret = elf_load_interp(lr.interp_path, pml4, &ie, &ib);
        if (ret) { mm_destroy_address_space(pml4); return 0; }
        entry = ie;
        lr.interp_base  = ib;
        lr.interp_entry = ie;
    }

    #define USER_STACK_TOP_EXEC   0x00007FFFFFF00000ULL
    #define USER_STACK_SIZE_EXEC  (64 * 1024)

    uint64_t sp = elf_setup_stack(pml4, USER_STACK_TOP_EXEC, USER_STACK_SIZE_EXEC,
                                  argv, envp, &lr, ib);
    if (!sp) { mm_destroy_address_space(pml4); return 0; }

    cur->pml4          = pml4;
    cur->brk_start     = lr.brk_start;
    cur->brk           = lr.brk_start;
    cur->user_stack_top = USER_STACK_TOP_EXEC;
    cur->mmap_base     = USER_STACK_TOP_EXEC - (4 * 1024 * 1024);

    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (cur->fd_table[i]) {
            uint64_t marker = (uint64_t)cur->fd_table[i];
            if (marker >= 1 && marker <= 3) {
                cur->fd_table[i] = NULL;
            } else if (pipe_is_end(cur->fd_table[i])) {
                pipe_close_end((pipe_end_t*)cur->fd_table[i]);
                cur->fd_table[i] = NULL;
            } else {
                vfs_close(cur->fd_table[i]);
                cur->fd_table[i] = NULL;
            }
        }
    }

    mm_switch_address_space(pml4);
    if (smp_is_enabled()) smp_tlb_shootdown_sync();
    if (old) mm_destroy_address_space(old);

    *out_stack_ptr = sp;
    return entry;
}
