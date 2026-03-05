/* ============================================================================
 * ld-likeos.so  —  LikeOS-64 Runtime Dynamic Linker
 *
 * Self-contained (no libc dependency).  The kernel loads this as the ELF
 * interpreter (PT_INTERP) at a fixed high address (0x7F0000000000).
 *
 * _start (in rtld_entry.S) finds our load base, applies R_X86_64_RELATIVE
 * self-relocations, then calls _dl_main() which:
 *   1. Parses the auxiliary vector from the user stack
 *   2. Registers the main executable
 *   3. Recursively loads all DT_NEEDED shared libraries from /lib
 *   4. Relocates everything (supports lazy PLT binding)
 *   5. Initialises TLS and runs DT_INIT/DT_INIT_ARRAY constructors
 *   6. Returns the application entry point
 *
 * _start then restores the original stack pointer and jumps to the app.
 *
 * Also provides:
 *   - __tls_get_addr   (compiler-generated TLS access)
 *   - _dl_fixup        (lazy PLT resolution)
 *   - _rtld_dlopen / _rtld_dlsym / _rtld_dlclose / _rtld_dlerror
 * ========================================================================= */

#include "rtld_syscall.h"

/* ================================================================== */
/*  Minimal ELF structures (self-contained — no kernel headers)       */
/* ================================================================== */

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half  e_type, e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;
    Elf64_Off   e_phoff, e_shoff;
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize, e_phentsize, e_phnum;
    Elf64_Half  e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type, p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info, st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr    r_offset;
    Elf64_Xword   r_info;
    Elf64_Sxword  r_addend;
} Elf64_Rela;

/* Program header types */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_PHDR     6
#define PT_TLS      7

/* Segment flags */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* ELF types */
#define ET_DYN  3

/* Dynamic tags */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ       10
#define DT_SYMENT      11
#define DT_INIT        12
#define DT_FINI        13
#define DT_SONAME      14
#define DT_PLTREL      20
#define DT_JMPREL      23
#define DT_BIND_NOW_TAG 24
#define DT_INIT_ARRAY  25
#define DT_FINI_ARRAY  26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS       30
#define DT_FLAGS_1     0x6FFFFFFB
#define DT_GNU_HASH    0x6FFFFEF5

/* DT_FLAGS / DT_FLAGS_1 bits */
#define DF_BIND_NOW    0x08
#define DF_1_NOW       0x00000001

/* Symbol binding */
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

/* Symbol type */
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2
#define STT_TLS    6

/* Section indices */
#define SHN_UNDEF  0

/* Relocation types (x86-64) */
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_TPOFF64   18

/* Macros */
#define ELF64_R_SYM(i)   ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xFFFFFFFF)
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xF)

/* Auxiliary vector types */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

/* ================================================================== */
/*  Utility helpers (no libc available)                               */
/* ================================================================== */

static int rtld_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void rtld_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}

static void rtld_memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
}

static char *rtld_strcpy(char *d, const char *s)
{
    char *r = d; while ((*d++ = *s++)); return r;
}

static char *rtld_strcat(char *d, const char *s)
{
    char *r = d; while (*d) d++; while ((*d++ = *s++)); return r;
}

static void rtld_die(const char *msg)
{
    rtld_write_str("ld-likeos.so: fatal: ");
    rtld_write_str(msg);
    rtld_write_str("\n");
    rtld_exit(127);
}

/* ================================================================== */
/*  DSO descriptor                                                    */
/* ================================================================== */

#define MAX_DSOS 64

typedef struct dso {
    const char       *name;
    uint64_t          base;

    const Elf64_Phdr *phdrs;
    uint16_t          phnum;
    const Elf64_Dyn  *dynamic;

    /* Symbol tables */
    const char       *strtab;
    const Elf64_Sym  *symtab;
    uint64_t          strtab_size;
    uint64_t          syment;

    /* GNU hash */
    const uint32_t   *gnu_hash;
    uint32_t          gnu_nbuckets;
    uint32_t          gnu_symoffset;
    uint32_t          gnu_bloom_size;
    uint32_t          gnu_bloom_shift;
    const uint64_t   *gnu_bloom;
    const uint32_t   *gnu_buckets;
    const uint32_t   *gnu_chain;

    /* SysV hash */
    const uint32_t   *sysv_hash;
    uint32_t          sysv_nbuckets;
    uint32_t          sysv_nchain;
    const uint32_t   *sysv_buckets;
    const uint32_t   *sysv_chain_tbl;

    /* Relocations */
    const Elf64_Rela *rela;
    uint64_t          rela_size;
    const Elf64_Rela *jmprel;
    uint64_t          jmprel_size;

    /* Init / Fini */
    void             (*init_fn)(void);
    void             (*fini_fn)(void);
    void            **init_array;
    uint64_t          init_array_sz;
    void            **fini_array;
    uint64_t          fini_array_sz;

    /* PLT GOT */
    uint64_t         *pltgot;

    /* TLS */
    uint64_t          tls_image;
    uint64_t          tls_filesz;
    uint64_t          tls_memsz;
    uint64_t          tls_align;
    int               tls_modid;
    int64_t           tls_offset;

    /* Flags */
    int               relocated;
    int               initialized;
    int               is_main;
    int               refcount;
    int               bind_now;

    /* For dlclose / munmap */
    uint64_t          map_base;
    uint64_t          map_size;
} dso_t;

static dso_t  g_dsos[MAX_DSOS];
static int    g_ndsos;

/* ================================================================== */
/*  Global state                                                      */
/* ================================================================== */

static int      g_tls_next_modid  = 1;
static uint64_t g_tls_static_size;
static uint64_t g_tls_static_align = 16;
static uint64_t g_page_size       = 4096;

static char     g_dlerror_buf[256];
static int      g_dlerror_set;

static void rtld_set_error(const char *msg)
{
    size_t i = 0;
    while (msg[i] && i < sizeof(g_dlerror_buf) - 1) {
        g_dlerror_buf[i] = msg[i]; i++;
    }
    g_dlerror_buf[i] = '\0';
    g_dlerror_set = 1;
}

/* ================================================================== */
/*  DSO helpers                                                       */
/* ================================================================== */

static dso_t *rtld_find_dso(const char *name)
{
    for (int i = 0; i < g_ndsos; i++)
        if (g_dsos[i].name && rtld_strcmp(g_dsos[i].name, name) == 0)
            return &g_dsos[i];
    return NULL;
}

static dso_t *rtld_alloc_dso(void)
{
    if (g_ndsos >= MAX_DSOS) rtld_die("too many shared objects");
    dso_t *d = &g_dsos[g_ndsos++];
    rtld_memset(d, 0, sizeof(*d));
    d->refcount = 1;
    return d;
}

/* ================================================================== */
/*  Parse PT_DYNAMIC                                                  */
/* ================================================================== */

static void rtld_parse_dynamic(dso_t *d)
{
    if (!d->dynamic) return;
    uint64_t b = d->base;

    for (const Elf64_Dyn *e = d->dynamic; e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
        case DT_STRTAB:       d->strtab       = (const char *)(b + e->d_un.d_ptr); break;
        case DT_SYMTAB:       d->symtab       = (const Elf64_Sym *)(b + e->d_un.d_ptr); break;
        case DT_STRSZ:        d->strtab_size  = e->d_un.d_val; break;
        case DT_SYMENT:       d->syment       = e->d_un.d_val; break;
        case DT_RELA:         d->rela         = (const Elf64_Rela *)(b + e->d_un.d_ptr); break;
        case DT_RELASZ:       d->rela_size    = e->d_un.d_val; break;
        case DT_JMPREL:       d->jmprel       = (const Elf64_Rela *)(b + e->d_un.d_ptr); break;
        case DT_PLTRELSZ:     d->jmprel_size  = e->d_un.d_val; break;
        case DT_PLTGOT:       d->pltgot       = (uint64_t *)(b + e->d_un.d_ptr); break;
        case DT_INIT:         d->init_fn      = (void (*)(void))(b + e->d_un.d_ptr); break;
        case DT_FINI:         d->fini_fn      = (void (*)(void))(b + e->d_un.d_ptr); break;
        case DT_INIT_ARRAY:   d->init_array   = (void **)(b + e->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: d->init_array_sz = e->d_un.d_val; break;
        case DT_FINI_ARRAY:   d->fini_array   = (void **)(b + e->d_un.d_ptr); break;
        case DT_FINI_ARRAYSZ: d->fini_array_sz = e->d_un.d_val; break;
        case DT_FLAGS:
            if (e->d_un.d_val & DF_BIND_NOW) d->bind_now = 1;
            break;
        case DT_FLAGS_1:
            if (e->d_un.d_val & DF_1_NOW) d->bind_now = 1;
            break;
        case DT_GNU_HASH:     d->gnu_hash  = (const uint32_t *)(b + e->d_un.d_ptr); break;
        case DT_HASH:         d->sysv_hash = (const uint32_t *)(b + e->d_un.d_ptr); break;
        default: break;
        }
    }

    if (d->gnu_hash) {
        const uint32_t *h = d->gnu_hash;
        d->gnu_nbuckets    = h[0];
        d->gnu_symoffset   = h[1];
        d->gnu_bloom_size  = h[2];
        d->gnu_bloom_shift = h[3];
        d->gnu_bloom   = (const uint64_t *)&h[4];
        d->gnu_buckets = (const uint32_t *)&d->gnu_bloom[d->gnu_bloom_size];
        d->gnu_chain   = &d->gnu_buckets[d->gnu_nbuckets];
    }
    if (d->sysv_hash) {
        d->sysv_nbuckets  = d->sysv_hash[0];
        d->sysv_nchain    = d->sysv_hash[1];
        d->sysv_buckets   = &d->sysv_hash[2];
        d->sysv_chain_tbl = &d->sysv_buckets[d->sysv_nbuckets];
    }
}

/* ================================================================== */
/*  Symbol lookup                                                     */
/* ================================================================== */

static uint32_t gnu_hash_fn(const char *name)
{
    uint32_t h = 5381;
    for (; *name; name++) h = (h << 5) + h + (uint8_t)*name;
    return h;
}

static const Elf64_Sym *gnu_hash_lookup(dso_t *d, const char *name, uint32_t h)
{
    if (!d->gnu_hash) return NULL;
    uint64_t bw = d->gnu_bloom[(h / 64) % d->gnu_bloom_size];
    uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> d->gnu_bloom_shift) % 64));
    if ((bw & mask) != mask) return NULL;

    uint32_t idx = d->gnu_buckets[h % d->gnu_nbuckets];
    if (idx == 0) return NULL;
    for (;;) {
        const Elf64_Sym *s = &d->symtab[idx];
        uint32_t ch = d->gnu_chain[idx - d->gnu_symoffset];
        if ((h | 1) == (ch | 1) && rtld_strcmp(d->strtab + s->st_name, name) == 0)
            return s;
        if (ch & 1) break;
        idx++;
    }
    return NULL;
}

static const Elf64_Sym *sysv_hash_lookup(dso_t *d, const char *name)
{
    if (!d->sysv_hash) return NULL;
    uint32_t h = 0, g;
    for (const char *p = name; *p; p++) {
        h = (h << 4) + (uint8_t)*p;
        g = h & 0xF0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    uint32_t idx = d->sysv_buckets[h % d->sysv_nbuckets];
    while (idx) {
        const Elf64_Sym *s = &d->symtab[idx];
        if (rtld_strcmp(d->strtab + s->st_name, name) == 0) return s;
        idx = d->sysv_chain_tbl[idx];
    }
    return NULL;
}

/* ---- Global lookup across all DSOs ---- */

typedef struct { const Elf64_Sym *sym; dso_t *dso; } sym_result_t;

static sym_result_t rtld_lookup_symbol(const char *name, dso_t *skip)
{
    sym_result_t res = { NULL, NULL };
    uint32_t gh = gnu_hash_fn(name);

    for (int i = 0; i < g_ndsos; i++) {
        dso_t *d = &g_dsos[i];
        if (d == skip || !d->symtab || !d->strtab) continue;

        const Elf64_Sym *sym = d->gnu_hash
            ? gnu_hash_lookup(d, name, gh)
            : sysv_hash_lookup(d, name);

        if (sym && sym->st_shndx != SHN_UNDEF) {
            uint8_t bind = ELF64_ST_BIND(sym->st_info);
            if (bind == STB_GLOBAL) { res.sym = sym; res.dso = d; return res; }
            if (bind == STB_WEAK && !res.sym) { res.sym = sym; res.dso = d; }
        }
    }
    return res;
}

/* ================================================================== */
/*  Relocation engine                                                 */
/* ================================================================== */

static void rtld_apply_relocs(dso_t *d, const Elf64_Rela *rel, size_t sz)
{
    size_t n = sz / sizeof(Elf64_Rela);

    for (size_t i = 0; i < n; i++) {
        uint64_t type = ELF64_R_TYPE(rel[i].r_info);
        uint64_t sidx = ELF64_R_SYM(rel[i].r_info);
        uint64_t *tgt = (uint64_t *)(d->base + rel[i].r_offset);

        switch (type) {

        case R_X86_64_RELATIVE:
            *tgt = d->base + rel[i].r_addend;
            break;

        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            const Elf64_Sym *sym = &d->symtab[sidx];
            sym_result_t sr = rtld_lookup_symbol(d->strtab + sym->st_name, NULL);
            if (sr.sym)
                *tgt = sr.dso->base + sr.sym->st_value;
            else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
                *tgt = 0;
            else {
                rtld_write_str("ld-likeos.so: undefined: ");
                rtld_write_str(d->strtab + sym->st_name);
                rtld_write_str("\n");
                *tgt = 0;
            }
            break;
        }

        case R_X86_64_64: {
            const Elf64_Sym *sym = &d->symtab[sidx];
            sym_result_t sr = rtld_lookup_symbol(d->strtab + sym->st_name, NULL);
            if (sr.sym)
                *tgt = sr.dso->base + sr.sym->st_value + rel[i].r_addend;
            break;
        }

        case R_X86_64_COPY: {
            const Elf64_Sym *sym = &d->symtab[sidx];
            sym_result_t sr = rtld_lookup_symbol(d->strtab + sym->st_name, d);
            if (sr.sym && sr.dso)
                rtld_memcpy(tgt, (const void *)(sr.dso->base + sr.sym->st_value),
                            sr.sym->st_size);
            break;
        }

        case R_X86_64_DTPMOD64:
            if (sidx == 0) { *tgt = d->tls_modid; break; }
            { const Elf64_Sym *sym = &d->symtab[sidx];
              sym_result_t sr = rtld_lookup_symbol(d->strtab + sym->st_name, NULL);
              *tgt = (sr.sym && sr.dso) ? sr.dso->tls_modid : d->tls_modid;
            }
            break;

        case R_X86_64_DTPOFF64:
            if (sidx == 0) { *tgt = rel[i].r_addend; break; }
            { const Elf64_Sym *sym = &d->symtab[sidx];
              *tgt = sym->st_value + rel[i].r_addend;
            }
            break;

        case R_X86_64_TPOFF64:
            if (sidx == 0) { *tgt = d->tls_offset + rel[i].r_addend; break; }
            { const Elf64_Sym *sym = &d->symtab[sidx];
              sym_result_t sr = rtld_lookup_symbol(d->strtab + sym->st_name, NULL);
              dso_t *td = (sr.sym && sr.dso) ? sr.dso : d;
              uint64_t sv = sr.sym ? sr.sym->st_value : sym->st_value;
              *tgt = td->tls_offset + sv + rel[i].r_addend;
            }
            break;

        case R_X86_64_NONE:
        default:
            break;
        }
    }
}

/* ================================================================== */
/*  TLS                                                               */
/* ================================================================== */

static void rtld_assign_tls(dso_t *d)
{
    if (!d->tls_memsz) return;
    d->tls_modid = g_tls_next_modid++;
    uint64_t a = d->tls_align < 16 ? 16 : d->tls_align;
    if (a > g_tls_static_align) g_tls_static_align = a;
    g_tls_static_size = (g_tls_static_size + d->tls_memsz + a - 1) & ~(a - 1);
    d->tls_offset = -(int64_t)g_tls_static_size;       /* variant-II */
}

static void rtld_init_tls(void)
{
    if (!g_tls_static_size) return;
    uint64_t total = (g_tls_static_size + 8 + g_tls_static_align - 1)
                     & ~(g_tls_static_align - 1);

    void *blk = rtld_mmap(NULL, total + g_page_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == MAP_FAILED) rtld_die("cannot allocate TLS block");
    rtld_memset(blk, 0, total);

    uint8_t *tp = (uint8_t *)blk + g_tls_static_size;
    *(uint64_t *)tp = (uint64_t)tp;                     /* self-pointer */

    for (int i = 0; i < g_ndsos; i++) {
        dso_t *d = &g_dsos[i];
        if (d->tls_memsz && d->tls_filesz && d->tls_image)
            rtld_memcpy(tp + d->tls_offset,
                        (const void *)d->tls_image, d->tls_filesz);
    }
    rtld_arch_prctl(ARCH_SET_FS, (uint64_t)tp);
}

/* ---- __tls_get_addr ---- */

typedef struct { uint64_t ti_module, ti_offset; } tls_index_t;

void *__tls_get_addr(tls_index_t *ti) __attribute__((visibility("default")));
void *__tls_get_addr(tls_index_t *ti)
{
    uint64_t tp;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(tp));
    for (int i = 0; i < g_ndsos; i++)
        if (g_dsos[i].tls_modid == (int)ti->ti_module)
            return (void *)(tp + g_dsos[i].tls_offset + ti->ti_offset);
    return NULL;
}

/* ================================================================== */
/*  Lazy PLT resolution                                               */
/* ================================================================== */

/*
 * Called from _dl_runtime_resolve (rtld_entry.S).
 * PLT0 pushed *GOT[1] (dso_t *), PLTn pushed reloc_index.
 */
uint64_t _dl_fixup(dso_t *d, uint64_t reloc_idx)
    __attribute__((visibility("default")));

uint64_t _dl_fixup(dso_t *d, uint64_t reloc_idx)
{
    if (!d || !d->jmprel) return 0;

    const Elf64_Rela *rel = &d->jmprel[reloc_idx];
    uint64_t sidx = ELF64_R_SYM(rel->r_info);
    const Elf64_Sym *sym = &d->symtab[sidx];
    const char *name = d->strtab + sym->st_name;

    sym_result_t sr = rtld_lookup_symbol(name, NULL);
    uint64_t addr = 0;
    if (sr.sym && sr.dso)
        addr = sr.dso->base + sr.sym->st_value;
    else {
        rtld_write_str("ld-likeos.so: lazy: ");
        rtld_write_str(name);
        rtld_write_str(" not found\n");
    }

    /* Patch the GOT entry */
    *(uint64_t *)(d->base + rel->r_offset) = addr;
    return addr;
}

/* ---- PLT/GOT setup ---- */

extern void _dl_runtime_resolve(void) __attribute__((visibility("default")));

static void rtld_setup_pltgot(dso_t *d)
{
    if (!d->pltgot) return;
    if (d->bind_now) {
        if (d->jmprel && d->jmprel_size)
            rtld_apply_relocs(d, d->jmprel, d->jmprel_size);
    } else {
        d->pltgot[1] = (uint64_t)d;
        d->pltgot[2] = (uint64_t)&_dl_runtime_resolve;
        /*
         * For lazy binding, each .got.plt entry initially holds a 0-based
         * file offset pointing back into the PLT stub (the push instruction).
         * Since the DSO is loaded at d->base, we must rebase every entry
         * so the PLT stub jumps to the correct in-memory address.
         */
        if (d->jmprel && d->jmprel_size && d->base) {
            size_t n = d->jmprel_size / sizeof(Elf64_Rela);
            for (size_t i = 0; i < n; i++) {
                uint64_t *slot = (uint64_t *)(d->base + d->jmprel[i].r_offset);
                *slot += d->base;
            }
        }
    }
}

/* ================================================================== */
/*  Load a shared library from disk                                   */
/* ================================================================== */

static uint64_t g_lib_mmap_base = 0x7F0001000000ULL;

static dso_t *rtld_load_library(const char *name);

static dso_t *rtld_load_dso_from_file(const char *path, const char *soname)
{
    int fd = rtld_open(path, O_RDONLY);
    if (fd < 0) return NULL;

    Elf64_Ehdr ehdr;
    if (rtld_read(fd, &ehdr, sizeof ehdr) != (long)sizeof ehdr)
        goto fail;
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F' ||
        ehdr.e_type != ET_DYN)
        goto fail;

    Elf64_Phdr phdrs[64];
    if (ehdr.e_phnum > 64) goto fail;
    size_t phsz = ehdr.e_phnum * ehdr.e_phentsize;
    rtld_lseek(fd, ehdr.e_phoff, SEEK_SET);
    if (rtld_read(fd, phdrs, phsz) != (long)phsz) goto fail;

    /* Total memory span */
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t s = phdrs[i].p_vaddr & ~0xFFFULL;
        uint64_t e = (phdrs[i].p_vaddr + phdrs[i].p_memsz + 0xFFF) & ~0xFFFULL;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    if (lo >= hi) goto fail;
    uint64_t span = hi - lo;

    /* Reserve address space */
    uint64_t map = g_lib_mmap_base;
    g_lib_mmap_base = (map + span + g_page_size - 1) & ~(g_page_size - 1);

    dso_t *d   = rtld_alloc_dso();
    d->name     = soname;
    d->base     = map - lo;
    d->map_base = map;
    d->map_size = span;

    /* Map each PT_LOAD */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t va  = phdrs[i].p_vaddr + d->base;
        uint64_t alv = va & ~0xFFFULL;
        uint64_t end = (va + phdrs[i].p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t len = end - alv;

        int prot = 0;
        if (phdrs[i].p_flags & PF_R) prot |= PROT_READ;
        if (phdrs[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (phdrs[i].p_flags & PF_X) prot |= PROT_EXEC;

        void *m = rtld_mmap((void *)alv, len,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                            -1, 0);
        if (m == MAP_FAILED) goto fail;

        if (phdrs[i].p_filesz) {
            rtld_lseek(fd, phdrs[i].p_offset, SEEK_SET);
            size_t rem = phdrs[i].p_filesz;
            uint8_t *dst = (uint8_t *)va;
            while (rem) {
                long c = rem > 4096 ? 4096 : rem;
                long r = rtld_read(fd, dst, c);
                if (r <= 0) break;
                dst += r; rem -= r;
            }
        }
        if (!(prot & PROT_WRITE))
            rtld_mprotect((void *)alv, len, prot);
    }
    rtld_close(fd);

    /* PT_DYNAMIC, PT_TLS */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC)
            d->dynamic = (const Elf64_Dyn *)(phdrs[i].p_vaddr + d->base);
        if (phdrs[i].p_type == PT_TLS) {
            d->tls_image  = phdrs[i].p_vaddr + d->base;
            d->tls_filesz = phdrs[i].p_filesz;
            d->tls_memsz  = phdrs[i].p_memsz;
            d->tls_align  = phdrs[i].p_align;
        }
    }
    d->phnum = ehdr.e_phnum;
    rtld_parse_dynamic(d);
    rtld_assign_tls(d);

    /* Recursive DT_NEEDED */
    if (d->dynamic)
        for (const Elf64_Dyn *e = d->dynamic; e->d_tag != DT_NULL; e++)
            if (e->d_tag == DT_NEEDED) {
                const char *need = d->strtab + e->d_un.d_val;
                if (!rtld_find_dso(need)) rtld_load_library(need);
            }
    return d;

fail:
    rtld_close(fd);
    return NULL;
}

static dso_t *rtld_load_library(const char *name)
{
    dso_t *e = rtld_find_dso(name);
    if (e) { e->refcount++; return e; }

    char path[512];
    dso_t *d;

    rtld_strcpy(path, "/lib/");
    rtld_strcat(path, name);
    d = rtld_load_dso_from_file(path, name);
    if (d) return d;

    rtld_strcpy(path, "/");
    rtld_strcat(path, name);
    d = rtld_load_dso_from_file(path, name);
    if (d) return d;

    rtld_write_str("ld-likeos.so: cannot find: ");
    rtld_write_str(name);
    rtld_write_str("\n");
    return NULL;
}

/* ================================================================== */
/*  Relocate + Initialise                                             */
/* ================================================================== */

static void rtld_relocate(dso_t *d)
{
    if (d->relocated) return;
    d->relocated = 1;
    if (d->rela && d->rela_size)
        rtld_apply_relocs(d, d->rela, d->rela_size);
    if (d->jmprel && d->jmprel_size) {
        if (d->bind_now || d->is_main)
            rtld_apply_relocs(d, d->jmprel, d->jmprel_size);
        else
            rtld_setup_pltgot(d);
    }
}

static void rtld_init_dso(dso_t *d)
{
    if (d->initialized || d->is_main) return;
    d->initialized = 1;
    if (d->init_fn) d->init_fn();
    if (d->init_array && d->init_array_sz) {
        size_t n = d->init_array_sz / sizeof(void *);
        for (size_t i = 0; i < n; i++) {
            void (*f)(void) = (void (*)(void))d->init_array[i];
            if (f && (uint64_t)f != (uint64_t)-1) f();
        }
    }
}

/* ================================================================== */
/*  dlopen / dlsym / dlclose / dlerror  (called from libc wrappers)   */
/* ================================================================== */

#define RTLD_DEFAULT ((void *)0)

void *_rtld_dlopen(const char *filename, int flags)
    __attribute__((visibility("default")));
void *_rtld_dlsym(void *handle, const char *symbol)
    __attribute__((visibility("default")));
int   _rtld_dlclose(void *handle)
    __attribute__((visibility("default")));
char *_rtld_dlerror(void)
    __attribute__((visibility("default")));

void *_rtld_dlopen(const char *filename, int flags)
{
    (void)flags;
    g_dlerror_set = 0;
    if (!filename) {
        for (int i = 0; i < g_ndsos; i++)
            if (g_dsos[i].is_main) return &g_dsos[i];
        return NULL;
    }
    dso_t *d = rtld_find_dso(filename);
    if (d) { d->refcount++; return d; }

    dso_t *ld = NULL;
    if (filename[0] == '/') {
        const char *bn = filename;
        for (const char *p = filename; *p; p++) if (*p == '/') bn = p + 1;
        ld = rtld_load_dso_from_file(filename, bn);
    } else {
        ld = rtld_load_library(filename);
    }
    if (!ld) { rtld_set_error("cannot load shared library"); return NULL; }

    rtld_relocate(ld);
    if (ld->tls_memsz) rtld_init_tls();
    rtld_init_dso(ld);
    return ld;
}

void *_rtld_dlsym(void *handle, const char *symbol)
{
    g_dlerror_set = 0;
    if (handle == RTLD_DEFAULT || !handle) {
        sym_result_t sr = rtld_lookup_symbol(symbol, NULL);
        if (sr.sym && sr.dso)
            return (void *)(sr.dso->base + sr.sym->st_value);
        rtld_set_error("symbol not found");
        return NULL;
    }
    dso_t *d = (dso_t *)handle;
    const Elf64_Sym *sym = d->gnu_hash
        ? gnu_hash_lookup(d, symbol, gnu_hash_fn(symbol))
        : sysv_hash_lookup(d, symbol);
    if (sym && sym->st_shndx != SHN_UNDEF)
        return (void *)(d->base + sym->st_value);
    rtld_set_error("symbol not found in object");
    return NULL;
}

int _rtld_dlclose(void *handle)
{
    g_dlerror_set = 0;
    if (!handle) return -1;
    dso_t *d = (dso_t *)handle;
    if (d->is_main) return 0;
    if (--d->refcount > 0) return 0;
    /* Finalizers (reverse order) */
    if (d->fini_array && d->fini_array_sz) {
        size_t n = d->fini_array_sz / sizeof(void *);
        for (size_t i = n; i > 0; i--) {
            void (*f)(void) = (void (*)(void))d->fini_array[i-1];
            if (f && (uint64_t)f != (uint64_t)-1) f();
        }
    }
    if (d->fini_fn) d->fini_fn();
    if (d->map_base && d->map_size)
        rtld_munmap((void *)d->map_base, d->map_size);
    d->name = NULL; d->refcount = 0; d->base = 0;
    return 0;
}

char *_rtld_dlerror(void)
{
    if (g_dlerror_set) { g_dlerror_set = 0; return g_dlerror_buf; }
    return NULL;
}

/* ================================================================== */
/*  _dl_main  —  C entry called from _start (rtld_entry.S)           */
/*  Returns the application entry point address.                      */
/* ================================================================== */

uint64_t _dl_main(uint64_t *sp, uint64_t own_base)
    __attribute__((visibility("default")));

uint64_t _dl_main(uint64_t *sp, uint64_t own_base)
{
    /* ---- Locate our own PT_DYNAMIC ---- */
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)own_base;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(own_base + eh->e_phoff);
    const Elf64_Dyn  *own_dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) {
            own_dyn = (const Elf64_Dyn *)(own_base + ph[i].p_vaddr);
            break;
        }

    /* ---- Register ourselves ---- */
    dso_t *rtld = rtld_alloc_dso();
    rtld->name        = "ld-likeos.so";
    rtld->base        = own_base;
    rtld->dynamic     = own_dyn;
    rtld->relocated   = 1;
    rtld->initialized = 1;
    rtld_parse_dynamic(rtld);

    /* ---- Parse auxiliary vector ---- */
    uint64_t argc = sp[0];
    uint64_t *p = sp + 1 + argc + 1;       /* past argv + NULL */
    while (*p) p++;                          /* skip envp        */
    p++;                                     /* past envp NULL   */

    uint64_t at_phdr = 0, at_phnum = 0, at_entry = 0, at_pagesz = 0;
    while (p[0] != AT_NULL) {
        switch (p[0]) {
        case AT_PHDR:   at_phdr   = p[1]; break;
        case AT_PHNUM:  at_phnum  = p[1]; break;
        case AT_ENTRY:  at_entry  = p[1]; break;
        case AT_PAGESZ: at_pagesz = p[1]; break;
        }
        p += 2;
    }
    if (at_pagesz) g_page_size = at_pagesz;

    /* ---- Register the main executable ---- */
    dso_t *main_dso   = rtld_alloc_dso();
    main_dso->name    = "<main>";
    main_dso->is_main = 1;
    main_dso->phnum   = (uint16_t)at_phnum;

    const Elf64_Phdr *mph = (const Elf64_Phdr *)at_phdr;
    uint64_t main_base = 0;
    for (uint64_t i = 0; i < at_phnum; i++)
        if (mph[i].p_type == PT_PHDR) {
            main_base = at_phdr - mph[i].p_vaddr;
            break;
        }
    if (!main_base) {
        /* Fallback: ELF header is at AT_PHDR - 0x40 (sizeof Elf64_Ehdr) */
        main_base = at_phdr - 0x40;
    }
    main_dso->base  = main_base;
    main_dso->phdrs = mph;

    for (uint64_t i = 0; i < at_phnum; i++) {
        if (mph[i].p_type == PT_DYNAMIC)
            main_dso->dynamic = (const Elf64_Dyn *)(main_base + mph[i].p_vaddr);
        if (mph[i].p_type == PT_TLS) {
            main_dso->tls_image  = main_base + mph[i].p_vaddr;
            main_dso->tls_filesz = mph[i].p_filesz;
            main_dso->tls_memsz  = mph[i].p_memsz;
            main_dso->tls_align  = mph[i].p_align;
        }
    }
    rtld_parse_dynamic(main_dso);
    rtld_assign_tls(main_dso);

    /* ---- Load DT_NEEDED libraries ---- */
    if (main_dso->dynamic)
        for (const Elf64_Dyn *e = main_dso->dynamic; e->d_tag != DT_NULL; e++)
            if (e->d_tag == DT_NEEDED) {
                const char *need = main_dso->strtab + e->d_un.d_val;
                if (!rtld_find_dso(need)) rtld_load_library(need);
            }

    /* ---- Relocate (dependencies first) ---- */
    for (int i = g_ndsos - 1; i >= 0; i--)
        rtld_relocate(&g_dsos[i]);

    /* ---- TLS ---- */
    if (g_tls_static_size) rtld_init_tls();

    /* ---- Initializers ---- */
    for (int i = 0; i < g_ndsos; i++)
        rtld_init_dso(&g_dsos[i]);

    return at_entry;
}
