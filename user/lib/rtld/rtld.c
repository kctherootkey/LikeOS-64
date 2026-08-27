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
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

#define EI_NIDENT 16

typedef struct {
	unsigned char e_ident[EI_NIDENT];
	Elf64_Half e_type, e_machine;
	Elf64_Word e_version;
	Elf64_Addr e_entry;
	Elf64_Off e_phoff, e_shoff;
	Elf64_Word e_flags;
	Elf64_Half e_ehsize, e_phentsize, e_phnum;
	Elf64_Half e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	Elf64_Word p_type, p_flags;
	Elf64_Off p_offset;
	Elf64_Addr p_vaddr, p_paddr;
	Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
	Elf64_Sxword d_tag;
	union {
		Elf64_Xword d_val;
		Elf64_Addr d_ptr;
	} d_un;
} Elf64_Dyn;

typedef struct {
	Elf64_Word st_name;
	unsigned char st_info, st_other;
	Elf64_Half st_shndx;
	Elf64_Addr st_value;
	Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
	Elf64_Addr r_offset;
	Elf64_Xword r_info;
	Elf64_Sxword r_addend;
} Elf64_Rela;

/* Program header types */
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_TLS 7
/* The segment describing an object's exception-handling tables.  The C++
 * unwinder asks the loader for this one by address; see _rtld_find_object. */
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_RELRO 0x6474e552

/* Segment flags */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* ELF types */
#define ET_DYN 3

/* Dynamic tags */
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
/* Reserved by the linker with a value of zero in every dynamic executable, for
 * the loader to fill in with the address of its r_debug.  It is how a debugger
 * finds the object list without knowing anything about this loader. */
#define DT_DEBUG 21
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_BIND_NOW_TAG 24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS 30
#define DT_FLAGS_1 0x6FFFFFFB
#define DT_GNU_HASH 0x6FFFFEF5

/* DT_FLAGS / DT_FLAGS_1 bits */
#define DF_BIND_NOW 0x08
#define DF_1_NOW 0x00000001

/* Symbol binding */
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
/*
 * A GNU extension: a definition of which the process must contain exactly ONE
 * instance, however many objects were loaded and whatever their symbol scope.
 *
 * C++ puts template and inline statics in this binding -- libstdc++ uses it for
 * every locale facet's `id' and for std::string's empty representation -- so a
 * loader that does not recognise it sees no definition at all and reports the
 * symbol as undefined.  That is precisely what happened: libstdc++ loaded with
 * forty-odd "undefined symbol: _ZNSt8numpunctIcE2idE" complaints and the first
 * C++ program died dereferencing one of them.
 *
 * Here it is treated exactly as STB_GLOBAL, which gives the guarantee its name
 * asks for: every object is in one global scope in this loader -- there is no
 * RTLD_LOCAL -- so the first definition found is the only one anything binds
 * to, which is the single instance the binding requires.
 */
#define STB_GNU_UNIQUE 10

/* Symbol type */
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_TLS 6

/* Section indices */
#define SHN_UNDEF 0

/* Relocation types (x86-64) */
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_IRELATIVE 37

/* Macros */
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xFFFFFFFF)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xF)

/* Auxiliary vector types */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_ENTRY 9

/* ================================================================== */
/*  Utility helpers (no libc available)                               */
/* ================================================================== */

static int rtld_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void rtld_memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dd = d;
	const uint8_t *ss = s;
	while (n--)
		*dd++ = *ss++;
}

static void rtld_memset(void *d, int c, size_t n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
}

static char *rtld_strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

static char *rtld_strcat(char *d, const char *s)
{
	char *r = d;
	while (*d)
		d++;
	while ((*d++ = *s++))
		;
	return r;
}

__attribute__((noreturn)) static void rtld_die(const char *msg)
{
	rtld_write_str("ld-likeos.so: fatal: ");
	rtld_write_str(msg);
	rtld_write_str("\n");
	rtld_exit(127);
}

/* ================================================================== */
/*  DSO descriptor                                                    */
/* ================================================================== */

/* Bytes reserved at and above the thread pointer for libc's thread control
 * block.  The TCB lives INSIDE the TLS allocation, at the thread pointer, so
 * that %fs:0 is both the ABI-required self-pointer and the start of libc's
 * struct __pthread.  libc static-asserts its structure against this value
 * (LIKEOS_TCB_RESERVE in user/lib/libc/src/pthread/pthread_internal.h) — keep
 * the two in step. */
#define RTLD_TCB_RESERVE 2048

/* Spare static TLS, handed out to objects that arrive via dlopen() after the
 * block has been laid out.  Without it, a dlopen'd module carrying __thread
 * data has nowhere to live: its offsets would fall outside the allocation.
 * This is the same trick as the conventional loader's static TLS surplus. */
#define RTLD_TLS_SURPLUS 1024

/* An X server loads its client libraries plus a driver module per device
 * and an extension module per protocol extension, so the object table has
 * to be far larger than a typical program needs. */
#define MAX_DSOS 256

/* Object names are at most a basename ("libXfont2.so.2"), so a small fixed
 * buffer is enough and keeps the loader allocation-free. */
#define RTLD_NAME_MAX 96
/* Long enough for the deepest path anything here is loaded from. */
#define RTLD_PATH_MAX 256

/* ---- The debugger rendezvous ------------------------------------------------
 *
 * Declared here to match user/lib/libc/include/link.h exactly; the loader
 * cannot include it, being freestanding and carrying its own ELF types.  The
 * LAYOUT is not ours to choose -- it is the SVR4 arrangement every ELF debugger
 * already reads, which is the entire reason for adopting it rather than
 * inventing something. Changing either copy without the other silently feeds a
 * debugger the wrong fields.
 *
 * See link.h for how a debugger finds and walks this. */
struct link_map {
	Elf64_Addr l_addr; /* load bias */
	char *l_name; /* object path; "" for the main executable */
	Elf64_Dyn *l_ld; /* its PT_DYNAMIC */
	struct link_map *l_next;
	struct link_map *l_prev;
};

#define RT_CONSISTENT 0
#define RT_ADD 1
#define RT_DELETE 2

struct r_debug {
	int r_version;
	struct link_map *r_map;
	Elf64_Addr r_brk;
	int r_state;
	Elf64_Addr r_ldbase;
};

typedef struct dso {
	/* Points at name_buf below, or at a string literal with static
	 * lifetime.  It must NEVER borrow from another object's mapping: a
	 * DT_NEEDED name lives in the *parent's* strtab, and dlclose()ing the
	 * parent would leave every dependency holding a pointer into unmapped
	 * pages — which rtld_find_dso() then walks straight into. */
	const char *name;
	char name_buf[RTLD_NAME_MAX];
	/* Where this object was loaded FROM, as an absolute path.  Published to
	 * a debugger as link_map.l_name; see rtld_load_dso_from_file. */
	const char *path;
	char path_buf[RTLD_PATH_MAX];
	uint64_t base;

	const Elf64_Phdr *phdrs;
	uint16_t phnum;
	const Elf64_Dyn *dynamic;

	/* Symbol tables */
	const char *strtab;
	const Elf64_Sym *symtab;
	uint64_t strtab_size;
	uint64_t syment;

	/* GNU hash */
	const uint32_t *gnu_hash;
	uint32_t gnu_nbuckets;
	uint32_t gnu_symoffset;
	uint32_t gnu_bloom_size;
	uint32_t gnu_bloom_shift;
	const uint64_t *gnu_bloom;
	const uint32_t *gnu_buckets;
	const uint32_t *gnu_chain;

	/* SysV hash */
	const uint32_t *sysv_hash;
	uint32_t sysv_nbuckets;
	uint32_t sysv_nchain;
	const uint32_t *sysv_buckets;
	const uint32_t *sysv_chain_tbl;

	/* Relocations */
	const Elf64_Rela *rela;
	uint64_t rela_size;
	const Elf64_Rela *jmprel;
	uint64_t jmprel_size;

	/* Init / Fini */
	void (*init_fn)(void);
	void (*fini_fn)(void);
	void **init_array;
	uint64_t init_array_sz;
	void **fini_array;
	uint64_t fini_array_sz;

	/* PLT GOT */
	uint64_t *pltgot;

	/* TLS */
	uint64_t tls_image;
	uint64_t tls_filesz;
	uint64_t tls_memsz;
	uint64_t tls_align;
	int tls_modid;
	int64_t tls_offset;
	/* dlopen'd object whose TLS slice is still to be filled from the
	 * (now relocated) image -- see rtld_assign_tls / rtld_dlopen. */
	int tls_needs_init;

	/* Flags */
	int relocated;
	int initialized;
	int is_main;
	int refcount;
	int bind_now;

	/* PT_GNU_RELRO: made read-only after relocation (see
	 * rtld_protect_relro).  relro_done keeps that to once. */
	uint64_t relro_start;
	uint64_t relro_len;
	int relro_done;

	/* For dlclose / munmap */
	uint64_t map_base;
	uint64_t map_size;

	/* This object's node in the debugger's object list.
	 *
	 * Embedded rather than allocated so its address is stable for as long
	 * as the slot is: a debugger reads this list out of the process's
	 * memory while the process is stopped and cannot be asked to re-resolve
	 * a pointer that moved.  Kept in step by rtld_link_map_add/remove. */
	struct link_map lm;
} dso_t;

static dso_t g_dsos[MAX_DSOS];
static int g_ndsos;

/* ================================================================== */
/*  Global state                                                      */
/* ================================================================== */

static int g_tls_next_modid = 1;
static uint64_t g_tls_static_size;
static uint64_t g_tls_static_align = 16;
static int g_tls_initialised;   /* the block is laid out exactly once */
static uint8_t *g_tls_tp;       /* thread pointer of the initial thread */
static uint64_t g_tls_reserved; /* bytes below tp available for TLS */
static uint64_t g_page_size = 4096;

static char g_dlerror_buf[256];
static int g_dlerror_set;

/* Counts of objects added and removed over the process's life, reported through
 * dl_iterate_phdr().  A caller that caches per-object data -- which the C++
 * unwinder does, per address range -- compares these between walks to decide
 * whether its cache is still valid, so they have to move on dlopen and dlclose
 * or a stale entry is trusted after the library behind it is gone. */
static unsigned long long g_dl_adds;
static unsigned long long g_dl_subs;

static void rtld_set_error(const char *msg)
{
	size_t i = 0;
	while (msg[i] && i < sizeof(g_dlerror_buf) - 1) {
		g_dlerror_buf[i] = msg[i];
		i++;
	}
	g_dlerror_buf[i] = '\0';
	g_dlerror_set = 1;
}

/* "<what>: <name>" -- dlerror() has to say WHICH object failed, or a program
 * probing for several optional libraries cannot tell which probe it is. */
static void rtld_set_error2(const char *what, const char *name)
{
	size_t i = 0, j = 0;

	while (what[i] && i < sizeof(g_dlerror_buf) - 3)
		g_dlerror_buf[i] = what[i], i++;
	g_dlerror_buf[i++] = ':';
	g_dlerror_buf[i++] = ' ';
	while (name[j] && i < sizeof(g_dlerror_buf) - 1)
		g_dlerror_buf[i++] = name[j++];
	g_dlerror_buf[i] = '\0';
	g_dlerror_set = 1;
}

/* ================================================================== */
/*  DSO helpers                                                       */
/* ================================================================== */

/* ---- The debugger rendezvous, continued ------------------------------------ */

/* Found by a debugger through DT_DEBUG in the executable's dynamic section,
 * which rtld_publish_r_debug() below points here. */
struct r_debug _r_debug __attribute__((visibility("default"))) = {
	.r_version = 1,
	.r_map = NULL,
	.r_brk = 0,
	.r_state = RT_CONSISTENT,
	.r_ldbase = 0,
};

/* The address a debugger breakpoints to be told the object list is changing.
 *
 * Empty on purpose: its whole value is being a fixed, named place to stop.
 * `noinline` and `used` because an empty function with no callers worth keeping
 * is exactly what a compiler removes -- and removing it removes the only way a
 * debugger learns that a library was loaded. */
void _dl_debug_state(void) __attribute__((visibility("default"), noinline,
					  used));
void _dl_debug_state(void)
{
	__asm__ volatile("" ::: "memory");
}

/* Announce that the list is about to change, and again once it has.
 *
 * Both calls matter.  A debugger stopped at the first sees RT_ADD/RT_DELETE and
 * knows not to trust the list yet; stopped at the second it sees RT_CONSISTENT
 * and reads it.  Reporting only the finished state would leave a debugger that
 * stopped for some other reason mid-edit unable to tell that is what happened. */
static void rtld_debug_state(int state)
{
	_r_debug.r_state = state;
	_dl_debug_state();
}

/* Append `d` to the object list.  Order is load order, and the main executable
 * must be first -- a debugger identifies it by position, having no other way to
 * tell which of the objects is the program. */
static void rtld_link_map_add(dso_t *d)
{
	struct link_map *m = &d->lm;

	m->l_addr = (Elf64_Addr)d->base;
	/* The main executable reports an empty name, as everything walking this
	 * list expects; the loader knows the path but the convention does not
	 * carry it. */
	/* The PATH, not the soname: a debugger opens this string to read the
	 * object's symbols.  The main executable reports an empty name, as
	 * everything walking this list expects. */
	m->l_name = d->is_main	       ? (char *)"" :
		    d->path && d->path[0] ? (char *)d->path :
					    (char *)d->name;
	m->l_ld = (Elf64_Dyn *)d->dynamic;
	m->l_next = NULL;
	m->l_prev = NULL;

	if (!_r_debug.r_map) {
		_r_debug.r_map = m;
		return;
	}
	struct link_map *last = _r_debug.r_map;

	while (last->l_next)
		last = last->l_next;
	last->l_next = m;
	m->l_prev = last;
}

static void rtld_link_map_remove(dso_t *d)
{
	struct link_map *m = &d->lm;

	if (m->l_prev)
		m->l_prev->l_next = m->l_next;
	else if (_r_debug.r_map == m)
		_r_debug.r_map = m->l_next;
	if (m->l_next)
		m->l_next->l_prev = m->l_prev;
	m->l_next = NULL;
	m->l_prev = NULL;
}

/* Store the address of _r_debug where a debugger looks for it: the DT_DEBUG
 * entry of the MAIN executable's dynamic section, which the linker reserves
 * with a value of zero for exactly this.
 *
 * Must run before PT_GNU_RELRO is applied -- the dynamic section is inside the
 * region that gets made read-only, so afterwards this store would fault. */
static void rtld_publish_r_debug(dso_t *main_dso, uint64_t interp_base)
{
	_r_debug.r_brk = (Elf64_Addr)(uint64_t)&_dl_debug_state;
	_r_debug.r_ldbase = (Elf64_Addr)interp_base;

	if (!main_dso || !main_dso->dynamic)
		return;
	for (Elf64_Dyn *e = (Elf64_Dyn *)main_dso->dynamic; e->d_tag != DT_NULL;
	     e++) {
		if (e->d_tag == DT_DEBUG) {
			e->d_un.d_ptr = (Elf64_Addr)(uint64_t)&_r_debug;
			break;
		}
	}
}

static dso_t *rtld_find_dso(const char *name)
{
	for (int i = 0; i < g_ndsos; i++)
		if (g_dsos[i].name && rtld_strcmp(g_dsos[i].name, name) == 0)
			return &g_dsos[i];
	return NULL;
}

static dso_t *rtld_alloc_dso(void)
{
	/* Every object the process gains passes through here, whether at start-up
	 * or through dlopen, so this is where the add counter belongs. */
	g_dl_adds++;
	/* Reuse a previously dlclose'd slot (all fields zeroed) if available. */
	for (int i = 0; i < g_ndsos; i++) {
		if (!g_dsos[i].name && !g_dsos[i].symtab) {
			dso_t *d = &g_dsos[i];
			rtld_memset(d, 0, sizeof(*d));
			d->refcount = 1;
			return d;
		}
	}
	if (g_ndsos >= MAX_DSOS)
		rtld_die("too many shared objects");
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
	if (!d->dynamic)
		return;
	uint64_t b = d->base;

	for (const Elf64_Dyn *e = d->dynamic; e->d_tag != DT_NULL; e++) {
		switch (e->d_tag) {
		case DT_STRTAB:
			d->strtab = (const char *)(b + e->d_un.d_ptr);
			break;
		case DT_SYMTAB:
			d->symtab = (const Elf64_Sym *)(b + e->d_un.d_ptr);
			break;
		case DT_STRSZ:
			d->strtab_size = e->d_un.d_val;
			break;
		case DT_SYMENT:
			d->syment = e->d_un.d_val;
			break;
		case DT_RELA:
			d->rela = (const Elf64_Rela *)(b + e->d_un.d_ptr);
			break;
		case DT_RELASZ:
			d->rela_size = e->d_un.d_val;
			break;
		case DT_JMPREL:
			d->jmprel = (const Elf64_Rela *)(b + e->d_un.d_ptr);
			break;
		case DT_PLTRELSZ:
			d->jmprel_size = e->d_un.d_val;
			break;
		case DT_PLTGOT:
			d->pltgot = (uint64_t *)(b + e->d_un.d_ptr);
			break;
		case DT_INIT:
			d->init_fn = (void (*)(void))(b + e->d_un.d_ptr);
			break;
		case DT_FINI:
			d->fini_fn = (void (*)(void))(b + e->d_un.d_ptr);
			break;
		case DT_INIT_ARRAY:
			d->init_array = (void **)(b + e->d_un.d_ptr);
			break;
		case DT_INIT_ARRAYSZ:
			d->init_array_sz = e->d_un.d_val;
			break;
		case DT_FINI_ARRAY:
			d->fini_array = (void **)(b + e->d_un.d_ptr);
			break;
		case DT_FINI_ARRAYSZ:
			d->fini_array_sz = e->d_un.d_val;
			break;
		case DT_FLAGS:
			if (e->d_un.d_val & DF_BIND_NOW)
				d->bind_now = 1;
			break;
		case DT_FLAGS_1:
			if (e->d_un.d_val & DF_1_NOW)
				d->bind_now = 1;
			break;
		case DT_GNU_HASH:
			d->gnu_hash = (const uint32_t *)(b + e->d_un.d_ptr);
			break;
		case DT_HASH:
			d->sysv_hash = (const uint32_t *)(b + e->d_un.d_ptr);
			break;
		default:
			break;
		}
	}

	if (d->gnu_hash) {
		const uint32_t *h = d->gnu_hash;
		d->gnu_nbuckets = h[0];
		d->gnu_symoffset = h[1];
		d->gnu_bloom_size = h[2];
		d->gnu_bloom_shift = h[3];
		d->gnu_bloom = (const uint64_t *)&h[4];
		d->gnu_buckets =
			(const uint32_t *)&d->gnu_bloom[d->gnu_bloom_size];
		d->gnu_chain = &d->gnu_buckets[d->gnu_nbuckets];
	}
	if (d->sysv_hash) {
		d->sysv_nbuckets = d->sysv_hash[0];
		d->sysv_nchain = d->sysv_hash[1];
		d->sysv_buckets = &d->sysv_hash[2];
		d->sysv_chain_tbl = &d->sysv_buckets[d->sysv_nbuckets];
	}
}

/* ================================================================== */
/*  Symbol lookup                                                     */
/* ================================================================== */

static uint32_t gnu_hash_fn(const char *name)
{
	uint32_t h = 5381;
	for (; *name; name++)
		h = (h << 5) + h + (uint8_t)*name;
	return h;
}

static const Elf64_Sym *gnu_hash_lookup(dso_t *d, const char *name, uint32_t h)
{
	if (!d->gnu_hash)
		return NULL;
	uint64_t bw = d->gnu_bloom[(h / 64) % d->gnu_bloom_size];
	uint64_t mask =
		(1ULL << (h % 64)) | (1ULL << ((h >> d->gnu_bloom_shift) % 64));
	if ((bw & mask) != mask)
		return NULL;

	uint32_t idx = d->gnu_buckets[h % d->gnu_nbuckets];
	if (idx == 0)
		return NULL;
	for (;;) {
		const Elf64_Sym *s = &d->symtab[idx];
		uint32_t ch = d->gnu_chain[idx - d->gnu_symoffset];
		if ((h | 1) == (ch | 1) &&
		    rtld_strcmp(d->strtab + s->st_name, name) == 0)
			return s;
		if (ch & 1)
			break;
		idx++;
	}
	return NULL;
}

static const Elf64_Sym *sysv_hash_lookup(dso_t *d, const char *name)
{
	if (!d->sysv_hash)
		return NULL;
	uint32_t h = 0, g;
	for (const char *p = name; *p; p++) {
		h = (h << 4) + (uint8_t)*p;
		g = h & 0xF0000000;
		if (g)
			h ^= g >> 24;
		h &= ~g;
	}
	uint32_t idx = d->sysv_buckets[h % d->sysv_nbuckets];
	while (idx) {
		const Elf64_Sym *s = &d->symtab[idx];
		if (rtld_strcmp(d->strtab + s->st_name, name) == 0)
			return s;
		idx = d->sysv_chain_tbl[idx];
	}
	return NULL;
}

/* ---- Global lookup across all DSOs ---- */

typedef struct {
	const Elf64_Sym *sym;
	dso_t *dso;
} sym_result_t;

static sym_result_t rtld_lookup_symbol(const char *name, dso_t *skip)
{
	sym_result_t res = { NULL, NULL };
	uint32_t gh = gnu_hash_fn(name);

	for (int i = 0; i < g_ndsos; i++) {
		dso_t *d = &g_dsos[i];
		if (d == skip || !d->symtab || !d->strtab)
			continue;

		const Elf64_Sym *sym = d->gnu_hash ?
					       gnu_hash_lookup(d, name, gh) :
					       sysv_hash_lookup(d, name);

		if (sym && sym->st_shndx != SHN_UNDEF) {
			uint8_t bind = ELF64_ST_BIND(sym->st_info);
			if (bind == STB_GLOBAL || bind == STB_GNU_UNIQUE) {
				res.sym = sym;
				res.dso = d;
				return res;
			}
			if (bind == STB_WEAK && !res.sym) {
				res.sym = sym;
				res.dso = d;
			}
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
			sym_result_t sr = rtld_lookup_symbol(
				d->strtab + sym->st_name, NULL);
			if (sr.sym)
				*tgt = sr.dso->base + sr.sym->st_value;
			else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
				*tgt = 0;
			else {
				/* Name the object too: with a couple of dozen
				 * libraries loaded, the symbol alone does not
				 * say which one failed to resolve. */
				rtld_write_str("ld-likeos.so: ");
				rtld_write_str(d->name ? d->name : "?");
				rtld_write_str(": undefined symbol: ");
				rtld_write_str(d->strtab + sym->st_name);
				rtld_write_str("\n");
				*tgt = 0;
			}
			break;
		}

		case R_X86_64_64: {
			const Elf64_Sym *sym = &d->symtab[sidx];
			sym_result_t sr = rtld_lookup_symbol(
				d->strtab + sym->st_name, NULL);
			if (sr.sym)
				*tgt = sr.dso->base + sr.sym->st_value +
				       rel[i].r_addend;
			break;
		}

		case R_X86_64_COPY: {
			const Elf64_Sym *sym = &d->symtab[sidx];
			sym_result_t sr =
				rtld_lookup_symbol(d->strtab + sym->st_name, d);
			if (sr.sym && sr.dso)
				rtld_memcpy(tgt,
					    (const void *)(sr.dso->base +
							   sr.sym->st_value),
					    sr.sym->st_size);
			break;
		}

		case R_X86_64_DTPMOD64:
			if (sidx == 0) {
				*tgt = d->tls_modid;
				break;
			}
			{
				const Elf64_Sym *sym = &d->symtab[sidx];
				sym_result_t sr = rtld_lookup_symbol(
					d->strtab + sym->st_name, NULL);
				*tgt = (sr.sym && sr.dso) ? sr.dso->tls_modid :
							    d->tls_modid;
			}
			break;

		case R_X86_64_DTPOFF64:
			if (sidx == 0) {
				*tgt = rel[i].r_addend;
				break;
			}
			{
				/* Resolve the symbol, exactly as DTPMOD64 just
				 * above and TPOFF64 just below both do.  The
				 * offset wanted here is the variable's place
				 * inside the TLS block of whichever object
				 * DEFINES it -- and when the reference is to
				 * another library's variable, this object's
				 * own entry for it is UND with st_value 0.
				 *
				 * Reading st_value straight out of d->symtab
				 * therefore yielded 0 for every cross-object
				 * general-dynamic access, silently: the pair
				 * (module, offset) named the right module and
				 * the wrong slot, so a write landed at the
				 * front of that module's block and the owning
				 * library read its variable back as zero.
				 *
				 * WebKit hit this on std::__once_call, which
				 * libstdc++ places at offset 0x10.  WebKit's
				 * webkitInitialize() stores its lambda there
				 * through __tls_get_addr and calls
				 * pthread_once(&flag, __once_proxy); the proxy
				 * reads __once_call back from 0x10, found the
				 * zero this left, and tail-jumped to it.  Both
				 * luakit and Claws Mail died at RIP=0 with
				 * pthread_once's return address on the stack
				 * and nothing to say why. */
				const Elf64_Sym *sym = &d->symtab[sidx];
				sym_result_t sr = rtld_lookup_symbol(
					d->strtab + sym->st_name, NULL);
				uint64_t sv = sr.sym ? sr.sym->st_value :
						       sym->st_value;
				*tgt = sv + rel[i].r_addend;
			}
			break;

		case R_X86_64_TPOFF64:
			if (sidx == 0) {
				*tgt = d->tls_offset + rel[i].r_addend;
				break;
			}
			{
				const Elf64_Sym *sym = &d->symtab[sidx];
				sym_result_t sr = rtld_lookup_symbol(
					d->strtab + sym->st_name, NULL);
				dso_t *td = (sr.sym && sr.dso) ? sr.dso : d;
				uint64_t sv = sr.sym ? sr.sym->st_value :
						       sym->st_value;
				*tgt = td->tls_offset + sv + rel[i].r_addend;
			}
			break;

		/* IRELATIVE: the addend is a resolver function in this object.
		 * Call it and store what it returns.  Skipping these (as the
		 * default case used to) leaves a NULL where a function pointer
		 * belongs, and the crash lands far from the cause. */
		case R_X86_64_IRELATIVE: {
			uint64_t (*resolver)(void) =
				(uint64_t(*)(void))(d->base + rel[i].r_addend);
			*tgt = resolver();
			break;
		}

		case R_X86_64_NONE:
			break;

		default:
			/* Say so rather than silently leaving the slot as it
			 * was: an unhandled relocation type otherwise shows up
			 * as an inexplicable jump to a wild address. */
			rtld_write_str("ld-likeos.so: ");
			rtld_write_str(d->name ? d->name : "?");
			rtld_write_str(": unhandled relocation type\n");
			break;
		}
	}
}

/* ================================================================== */
/*  TLS                                                               */
/* ================================================================== */

/* Hand out a slice of the static TLS block.
 *
 * ORDER IS PART OF THE ABI.  The main executable must be assigned FIRST, so
 * that its block sits immediately below the thread pointer.  Its __thread
 * accesses are compiled local-exec (R_X86_64_TPOFF32), which the *static*
 * linker resolves to constant offsets on exactly that assumption — there is no
 * dynamic relocation left for us to adjust.  Assign a library ahead of the
 * executable and every one of those accesses silently reads the wrong slice.
 * _dl_main() therefore calls this for main_dso before loading any DT_NEEDED
 * library; do not reorder it. */
static void rtld_assign_tls(dso_t *d)
{
	if (!d->tls_memsz)
		return;
	d->tls_modid = g_tls_next_modid++;

	/* Use the object's OWN p_align, never a rounded-up minimum.  For
	 * local-exec accesses the static linker has already burned the offset
	 * into the instruction stream as
	 *
	 *     tpoff = offset_in_segment - align_up(p_memsz, p_align)
	 *
	 * so the slice has to be sized by that same formula.  Substituting a
	 * 16-byte floor here (p_align is commonly 4 or 8) moves the image a few
	 * bytes away from where every one of those instructions expects it, and
	 * the reads land just past the end of the data. */
	uint64_t a = d->tls_align ? d->tls_align : 1;

	/* The BLOCK alignment is a separate question: the thread pointer keeps
	 * a 16-byte floor so the TCB at tp stays suitably aligned. */
	if (a > g_tls_static_align)
		g_tls_static_align = a;

	uint64_t want = (g_tls_static_size + d->tls_memsz + a - 1) & ~(a - 1);

	/* Objects loaded after the block was laid out (dlopen) can only be
	 * given space that was already reserved as surplus — the block cannot
	 * grow, because every thread already has one at a fixed size. */
	if (g_tls_initialised && want > g_tls_reserved) {
		rtld_write_str("ld-likeos.so: ");
		rtld_write_str(d->name ? d->name : "?");
		rtld_write_str(": no static TLS space left for this object\n");
		d->tls_memsz = 0;
		d->tls_offset = 0;
		return;
	}

	g_tls_static_size = want;
	d->tls_offset = -(int64_t)g_tls_static_size; /* variant-II */

	/* Already-running threads have a block whose images were copied at
	 * creation, so a late arrival must initialise its own slice.  Zero it
	 * now, but do NOT copy the image yet: this runs at load time, before
	 * the object is relocated, and a TLS initialiser can carry a dynamic
	 * relocation of its own -- Mesa's libGL starts its per-thread context
	 * pointer at `&dummyContext', which is an R_X86_64_RELATIVE into
	 * .tdata.  Copied here, the slice held the link-time address, every
	 * glX call compared it against the relocated one, missed, and
	 * dereferenced it (GTK's first GL context creation took luakit down).
	 * The copy is made in rtld_dlopen() once the relocation pass is over. */
	if (g_tls_initialised && g_tls_tp) {
		rtld_memset(g_tls_tp + d->tls_offset, 0, d->tls_memsz);
		d->tls_needs_init = 1;
	}
}

/* Lay out the initial thread's TLS block.
 *
 * Variant-II layout, which is what x86-64 __thread accesses assume:
 *
 *     [ static TLS ][ tp ][ TCB ... ]
 *       negative offsets    %fs:0 = self-pointer = libc's struct __pthread
 *
 * The TCB deliberately lives inside this allocation.  libc used to point %fs
 * at a struct in its own .bss instead, which meant whichever of the two ran
 * last won: with libc winning, every __thread access at %fs:-N landed in
 * unrelated .bss.  Reserving RTLD_TCB_RESERVE bytes at the thread pointer lets
 * both live at the same address without fighting over it.
 *
 * Runs exactly once.  dlopen() used to call this again and hand the process a
 * brand-new block, silently discarding every thread's existing TLS. */
static void rtld_init_tls(void)
{
	if (g_tls_initialised)
		return;

	/* Round the static area up, then add the dlopen surplus so late
	 * arrivals have somewhere to go. */
	uint64_t stat_sz = (g_tls_static_size + g_tls_static_align - 1) &
			   ~(g_tls_static_align - 1);
	uint64_t reserved = stat_sz + RTLD_TLS_SURPLUS;
	reserved = (reserved + g_tls_static_align - 1) &
		   ~(g_tls_static_align - 1);
	uint64_t total = reserved + RTLD_TCB_RESERVE;

	void *blk = rtld_mmap(NULL, total + g_page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (blk == MAP_FAILED)
		rtld_die("cannot allocate TLS block");
	rtld_memset(blk, 0, total);

	uint8_t *tp = (uint8_t *)blk + reserved;
	*(uint64_t *)tp = (uint64_t)tp; /* self-pointer */

	g_tls_reserved = reserved;
	g_tls_tp = tp;
	g_tls_initialised = 1;

	for (int i = 0; i < g_ndsos; i++) {
		dso_t *d = &g_dsos[i];
		if (d->tls_memsz && d->tls_filesz && d->tls_image)
			rtld_memcpy(tp + d->tls_offset,
				    (const void *)d->tls_image, d->tls_filesz);
	}
	/* Preserve the bootstrap stack canary so stack-protected rtld frames
     * that saved it in their prologue still pass the epilogue check after
     * FS is switched to the real TLS block. */
	uint64_t bootstrap_canary;
	__asm__ volatile("mov %%fs:0x28, %0" : "=r"(bootstrap_canary));
	*(uint64_t *)(tp + 0x28) = bootstrap_canary;
	rtld_arch_prctl(ARCH_SET_FS, (uint64_t)tp);
}

/* ---- __tls_get_addr ---- */

typedef struct {
	uint64_t ti_module, ti_offset;
} tls_index_t;

void *__tls_get_addr(tls_index_t *ti) __attribute__((visibility("default")));
void *__tls_get_addr(tls_index_t *ti)
{
	uint64_t tp;
	__asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
	for (int i = 0; i < g_ndsos; i++)
		if (g_dsos[i].tls_modid == (int)ti->ti_module)
			return (void *)(tp + g_dsos[i].tls_offset +
					ti->ti_offset);

	/* No module with that id.  Returning NULL here is the worst possible
	 * answer: the caller has asked "where is this thread's copy of that
	 * variable", and a null address is not a diagnosis, it is a booby
	 * trap.  The read or write lands at a low address and the failure
	 * surfaces somewhere else entirely -- and when the variable happens to
	 * hold a function pointer, as libstdc++'s std::__once_call does, the
	 * program does not fault on the access at all: it reads a zero and
	 * jumps to it, arriving at RIP=0 with nothing on the stack to say
	 * which variable was never allocated.
	 *
	 * Say what happened instead.  There is no correct value to return, so
	 * this does not return. */
	rtld_write_str("ld-likeos.so: __tls_get_addr: no TLS block for "
		       "module id\n");
	rtld_die("unallocated TLS module");
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

/*
 * Resolve one PLT entry on first call.
 *
 * A failure here is FATAL and has to be treated as such.  This used to patch
 * the GOT slot with 0 and return 0, so the PLT stub jumped straight to address
 * zero: the process died with RIP=0, no bytes around it to disassemble, no
 * stack that meant anything, and the one line that explained it ("lazy: _Znwm
 * not found") already scrolled past.  A program cannot continue past a call it
 * has no address for, so there is nothing to be gained by returning -- report
 * which object wanted which symbol, and stop.
 */
uint64_t _dl_fixup(dso_t *d, uint64_t reloc_idx)
{
	if (!d || !d->jmprel)
		rtld_die("PLT relocation on an object with no jump slots");

	const Elf64_Rela *rel = &d->jmprel[reloc_idx];
	uint64_t sidx = ELF64_R_SYM(rel->r_info);
	const Elf64_Sym *sym = &d->symtab[sidx];
	const char *name = d->strtab + sym->st_name;

	sym_result_t sr = rtld_lookup_symbol(name, NULL);
	if (!sr.sym || !sr.dso) {
		rtld_write_str("ld-likeos.so: ");
		rtld_write_str(d->name ? d->name : "?");
		rtld_write_str(": symbol lookup error: undefined symbol: ");
		rtld_write_str(name);
		rtld_write_str("\n");
		rtld_exit(127);
	}

	uint64_t addr = sr.dso->base + sr.sym->st_value;

	/* Patch the GOT entry */
	*(uint64_t *)(d->base + rel->r_offset) = addr;
	return addr;
}

/* ---- PLT/GOT setup ---- */

extern void _dl_runtime_resolve(void) __attribute__((visibility("default")));

/*
 * Point PLT0 at the resolver.
 *
 * Done for EVERY object that has a PLT, eagerly bound ones included, and that
 * is deliberately not redundant.  Nothing is supposed to reach PLT0 in a
 * BIND_NOW object -- every slot was bound at load time -- but if anything ever
 * does, the stub pushes GOT[1] and jumps through GOT[2], and those two words
 * are zero unless somebody writes them.  A jump through a zero GOT[2] is the
 * least informative failure this loader can produce: RIP=0, no instruction
 * bytes to disassemble, and a stack whose top word is the zero just pushed.
 * Armed, the very same path resolves the symbol correctly instead, or stops in
 * _dl_fixup with a message naming the object and the symbol.
 */
static void rtld_arm_plt0(dso_t *d)
{
	if (!d->pltgot)
		return;
	d->pltgot[1] = (uint64_t)d;
	d->pltgot[2] = (uint64_t)&_dl_runtime_resolve;
}

/*
 * After eager binding, every jump slot must hold a real address.
 *
 * A slot left at zero does not announce itself: the symbol resolved, no error
 * was printed, and the program runs normally until the first call through that
 * slot.  Checking the slots while the object and its symbol names are still to
 * hand turns that into a message that names the symbol, instead of a null jump
 * somewhere far away with nothing left to identify it.
 */
static void rtld_verify_plt(dso_t *d)
{
	if (!d->jmprel || !d->jmprel_size || !d->symtab || !d->strtab)
		return;

	size_t n = d->jmprel_size / sizeof(Elf64_Rela);
	for (size_t i = 0; i < n; i++) {
		const Elf64_Rela *r = &d->jmprel[i];

		if (ELF64_R_TYPE(r->r_info) != R_X86_64_JUMP_SLOT)
			continue;
		if (*(uint64_t *)(d->base + r->r_offset))
			continue;

		const Elf64_Sym *sym = &d->symtab[ELF64_R_SYM(r->r_info)];

		/* Zero is the right answer for a weak symbol nobody defines;
		 * the caller is required to test it before calling. */
		if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
			continue;

		rtld_write_str("ld-likeos.so: ");
		rtld_write_str(d->name ? d->name : "?");
		rtld_write_str(": jump slot left null after binding: ");
		rtld_write_str(d->strtab + sym->st_name);
		rtld_write_str("\n");
	}
}

static void rtld_setup_pltgot(dso_t *d)
{
	if (!d->pltgot)
		return;
	if (d->bind_now) {
		if (d->jmprel && d->jmprel_size) {
			rtld_apply_relocs(d, d->jmprel, d->jmprel_size);
			rtld_verify_plt(d);
		}
		rtld_arm_plt0(d);
	} else {
		rtld_arm_plt0(d);
		/*
         * For lazy binding, each .got.plt entry initially holds a 0-based
         * file offset pointing back into the PLT stub (the push instruction).
         * Since the DSO is loaded at d->base, we must rebase every entry
         * so the PLT stub jumps to the correct in-memory address.
         */
		if (d->jmprel && d->jmprel_size && d->base) {
			size_t n = d->jmprel_size / sizeof(Elf64_Rela);
			for (size_t i = 0; i < n; i++) {
				uint64_t *slot =
					(uint64_t *)(d->base +
						     d->jmprel[i].r_offset);
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
static void rtld_report_missing(const char *name);

/* Seek+read helper for the header-only load path. */
static int rtld_pread(int fd, void *buf, size_t n, long off)
{
	if (rtld_lseek(fd, off, SEEK_SET) != off)
		return -1;
	uint8_t *dst = (uint8_t *)buf;
	size_t rem = n;
	while (rem) {
		long r = rtld_read(fd, dst, rem);
		if (r <= 0)
			return -1;
		dst += r;
		rem -= (size_t)r;
	}
	return 0;
}

/* Upper bound on a shared library, as a sanity check only: it rejects a file
 * that cannot be an ELF object before any of it is trusted.  It is NOT a
 * statement about how much can be mapped -- segments are mapped straight from
 * the file and paged in on demand, so a large library costs address space, not
 * memory.
 *
 * This was 64 MB, and that was too small for a real one.  libwebkit2gtk-4.1 is
 * 92,113,008 bytes, so every load of it failed here, before the ELF magic was
 * even checked.  The failure is quiet in the worst way: the library is simply
 * never mapped, and the first sign is a screen of "undefined symbol: webkit_*"
 * from the relocation pass, followed by a jump to a null PLT slot and a SIGSEGV
 * at RIP 0.  Nothing names the library that was skipped.
 *
 * One gigabyte instead.  Nothing legitimate approaches it -- the next largest
 * object in this system is libicudata at 33 MB -- while an unstripped or
 * debug-built engine has room to grow.  The value only has to be absurd, not
 * tight; e_type and the ELF magic are what actually decide validity, a few
 * lines below. */
#define RTLD_MAX_LIBRARY_SIZE (1024L * 1024 * 1024)

static dso_t *rtld_load_dso_from_file(const char *path, const char *soname)
{
	int fd = rtld_open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	/* Demand paging: read ONLY the ELF header and program headers.
	 * Read-only/executable segments are mapped straight from the file
	 * (MAP_PRIVATE, kernel pages them in on first touch), so a library's
	 * text costs neither disk reads nor memory until it is executed. */
	long file_size = rtld_lseek(fd, 0, SEEK_END);
	if (file_size <= (long)sizeof(Elf64_Ehdr) ||
	    file_size > RTLD_MAX_LIBRARY_SIZE)
		goto fail;

	Elf64_Ehdr ehdr_buf;
	Elf64_Ehdr *ehdr = &ehdr_buf;
	if (rtld_pread(fd, &ehdr_buf, sizeof(ehdr_buf), 0) != 0)
		goto fail;
	if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
	    ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F' ||
	    ehdr->e_type != ET_DYN) {
		goto fail;
	}

	static Elf64_Phdr phdr_buf[64];
	Elf64_Phdr *phdrs = phdr_buf;
	if (ehdr->e_phnum > 64 ||
	    ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
	    ehdr->e_phoff + (size_t)ehdr->e_phnum * ehdr->e_phentsize >
		    (size_t)file_size) {
		goto fail;
	}
	if (rtld_pread(fd, phdr_buf,
		       (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr),
		       (long)ehdr->e_phoff) != 0)
		goto fail;

	/* Total memory span */
	uint64_t lo = ~0ULL, hi = 0;
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;
		uint64_t s = phdrs[i].p_vaddr & ~0xFFFULL;
		uint64_t e = (phdrs[i].p_vaddr + phdrs[i].p_memsz + 0xFFF) &
			     ~0xFFFULL;
		if (s < lo)
			lo = s;
		if (e > hi)
			hi = e;
	}
	if (lo >= hi)
		goto fail;
	uint64_t span = hi - lo;

	/* Reserve address space */
	uint64_t map = g_lib_mmap_base;
	g_lib_mmap_base = (map + span + g_page_size - 1) & ~(g_page_size - 1);

	dso_t *d = rtld_alloc_dso();
	/* Own the name (see the field comment): `soname` usually points into
	 * the requesting object's string table, which can be unmapped while
	 * this object is still loaded. */
	{
		size_t n = 0;
		if (soname) {
			while (soname[n] && n < RTLD_NAME_MAX - 1) {
				d->name_buf[n] = soname[n];
				n++;
			}
		}
		d->name_buf[n] = '\0';
		d->name = d->name_buf;

		/* The PATH this was opened from, kept separately from the
		 * soname, because a debugger needs to open the same file.
		 *
		 * l_name in the link_map is that path -- not the soname -- and
		 * a debugger takes it literally: it opens the string to read
		 * the library's symbols.  Publishing "libglib-2.0.so.0" gave it
		 * nothing to open, so it reported "Could not load shared
		 * library symbols" and every frame in that library printed as
		 * `?? ()'.  The loader knows the path here and used to discard
		 * it. */
		n = 0;
		if (path) {
			while (path[n] && n < RTLD_PATH_MAX - 1) {
				d->path_buf[n] = path[n];
				n++;
			}
		}
		d->path_buf[n] = '\0';
		d->path = d->path_buf;
	}
	d->base = map - lo;
	d->map_base = map;
	d->map_size = span;

	/* Map each PT_LOAD.
	 *
	 * Read-only / executable segments with page-congruent file offsets
	 * are mapped DIRECTLY from the file (MAP_PRIVATE|MAP_FIXED with fd):
	 * the kernel registers a lazy file-backed region and pages text in
	 * on first execution — no read of the segment happens here at all.
	 * The mapping pins the file in the kernel, so closing fd below is
	 * safe.
	 *
	 * Writable segments (.data — small) are mapped anonymous (lazy
	 * zero-fill) and their file bytes read in eagerly: relocations
	 * touch essentially all of .data anyway, so laziness buys nothing
	 * and the eager read keeps kernel-write-under-lock faults out of
	 * the picture. */
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;

		uint64_t va = phdrs[i].p_vaddr + d->base;
		uint64_t alv = va & ~0xFFFULL;
		uint64_t end = (va + phdrs[i].p_memsz + 0xFFF) & ~0xFFFULL;
		uint64_t len = end - alv;

		int prot = 0;
		if (phdrs[i].p_flags & PF_R)
			prot |= PROT_READ;
		if (phdrs[i].p_flags & PF_W)
			prot |= PROT_WRITE;
		if (phdrs[i].p_flags & PF_X)
			prot |= PROT_EXEC;

		int congruent = ((phdrs[i].p_offset & 0xFFFULL) ==
				 (phdrs[i].p_vaddr & 0xFFFULL));

		if (!(prot & PROT_WRITE) && congruent &&
		    phdrs[i].p_filesz > 0) {
			/* Demand-paged from the file. */
			uint64_t fend = (va + phdrs[i].p_filesz + 0xFFFULL) &
					~0xFFFULL;
			long foff = (long)(phdrs[i].p_offset -
					   (phdrs[i].p_vaddr & 0xFFFULL));
			void *m = rtld_mmap((void *)alv, fend - alv, prot,
					    MAP_PRIVATE | MAP_FIXED, fd, foff);
			if (m == MAP_FAILED)
				goto fail;
			/* Rare RO-BSS tail beyond the file bytes. */
			if (end > fend) {
				m = rtld_mmap((void *)fend, end - fend, prot,
					      MAP_PRIVATE | MAP_ANONYMOUS |
						      MAP_FIXED,
					      -1, 0);
				if (m == MAP_FAILED)
					goto fail;
			}
			continue;
		}

		void *m = rtld_mmap((void *)alv, len, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
				    0);
		if (m == MAP_FAILED)
			goto fail;

		if (phdrs[i].p_filesz) {
			if (phdrs[i].p_offset + phdrs[i].p_filesz >
				    (uint64_t)file_size ||
			    rtld_pread(fd, (void *)va, phdrs[i].p_filesz,
				       (long)phdrs[i].p_offset) != 0)
				goto fail;
		}
		if (!(prot & PROT_WRITE))
			rtld_mprotect((void *)alv, len, prot);
	}
	rtld_close(fd);
	fd = -1;

	/* PT_DYNAMIC, PT_TLS */
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_DYNAMIC)
			d->dynamic =
				(const Elf64_Dyn *)(phdrs[i].p_vaddr + d->base);
		if (phdrs[i].p_type == PT_TLS) {
			d->tls_image = phdrs[i].p_vaddr + d->base;
			d->tls_filesz = phdrs[i].p_filesz;
			d->tls_memsz = phdrs[i].p_memsz;
			d->tls_align = phdrs[i].p_align;
		}
	}
	/* Where this object's program headers live IN THE MAPPED IMAGE.
	 *
	 * phnum was recorded here and phdrs was not, which left every library
	 * loaded from disk with a null table.  Nothing noticed until something
	 * asked: dl_iterate_phdr() then walked past every one of them and
	 * reported only the main executable, and _dl_find_object() could not
	 * resolve an address in a shared library at all -- so a C++ exception
	 * thrown inside one would find no unwind tables and reach
	 * std::terminate instead of its handler.
	 *
	 * `phdrs' cannot simply be stored: it points into phdr_buf, ONE static
	 * buffer reused by every call, so every object would end up describing
	 * whichever library was loaded last.  The mapped copy has to be found
	 * instead -- through PT_PHDR, which states its own address, or by
	 * locating the PT_LOAD whose file range covers e_phoff and translating
	 * that offset into the segment.  Nearly every shared object has the
	 * former; the latter is what the ones without it need.
	 */
	d->phnum = ehdr->e_phnum;
	d->phdrs = NULL;

	/* PT_GNU_RELRO: the part of the writable data that only needs to be
	 * writable WHILE relocating -- the GOT, .data.rel.ro, .init_array.
	 * Recorded now, made read-only once this object is relocated. */
	d->relro_start = 0;
	d->relro_len = 0;
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_GNU_RELRO) {
			d->relro_start = d->base + phdrs[i].p_vaddr;
			d->relro_len = phdrs[i].p_memsz;
			break;
		}
	}
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_PHDR) {
			d->phdrs = (const Elf64_Phdr *)(d->base +
						       phdrs[i].p_vaddr);
			break;
		}
	}
	if (!d->phdrs) {
		uint64_t need = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);

		for (int i = 0; i < ehdr->e_phnum; i++) {
			if (phdrs[i].p_type != PT_LOAD)
				continue;
			if (ehdr->e_phoff < phdrs[i].p_offset)
				continue;
			if (ehdr->e_phoff + need >
			    phdrs[i].p_offset + phdrs[i].p_filesz)
				continue;
			d->phdrs = (const Elf64_Phdr *)(d->base +
					phdrs[i].p_vaddr +
					(ehdr->e_phoff - phdrs[i].p_offset));
			break;
		}
	}
	/* Report nothing rather than a wild pointer if neither route worked. */
	if (!d->phdrs)
		d->phnum = 0;

	rtld_parse_dynamic(d);
	rtld_assign_tls(d);

	/* Recursive DT_NEEDED */
	if (d->dynamic)
		for (const Elf64_Dyn *e = d->dynamic; e->d_tag != DT_NULL; e++)
			if (e->d_tag == DT_NEEDED) {
				const char *need = d->strtab + e->d_un.d_val;
				if (!rtld_find_dso(need))
					if (!rtld_load_library(need))
						rtld_report_missing(need);
			}
	return d;

fail:
	if (fd >= 0)
		rtld_close(fd);
	return NULL;
}

static dso_t *rtld_load_library(const char *name)
{
	dso_t *e = rtld_find_dso(name);
	if (e) {
		e->refcount++;
		return e;
	}

	char path[512];
	dso_t *d;

	rtld_strcpy(path, "/lib/");
	rtld_strcat(path, name);
	d = rtld_load_dso_from_file(path, name);
	if (d)
		return d;

	rtld_strcpy(path, "/");
	rtld_strcat(path, name);
	d = rtld_load_dso_from_file(path, name);
	if (d)
		return d;

	/* Deliberately silent.  Whether a library that cannot be found is worth
	 * a message depends entirely on WHO asked for it: a DT_NEEDED entry is
	 * a hard dependency and its absence is fatal, so those call sites report
	 * it; a dlopen() is a PROBE, and programs probe for things they expect
	 * to be missing.  libepoxy asks for libGL.so.1 and libGLX.so.1 on a
	 * system with no OpenGL, handles the failure perfectly well, and used to
	 * print two lines of alarming nonsense on the way past.  A failed
	 * dlopen() reports through dlerror(), which is what dlerror() is for. */
	return NULL;
}

/* A DT_NEEDED dependency that could not be found: fatal, and worth saying so. */
static void rtld_report_missing(const char *name)
{
	rtld_write_str("ld-likeos.so: cannot find: ");
	rtld_write_str(name);
	rtld_write_str("\n");
}

/* ================================================================== */
/*  Relocate + Initialise                                             */
/* ================================================================== */

/*
 * Make an object's RELRO region read-only, once it has been relocated.
 *
 * Everything in there -- the GOT above all -- is written by the loader and by
 * nothing else afterwards, so leaving it writable for the life of the process
 * only offers a target.  A stray write into the GOT is not a crash where it
 * happens: the program keeps running until it next calls through the damaged
 * slot, and then dies in a PLT stub with nothing to say about who broke it.
 * That is precisely how Claws Mail died -- jumping through a corrupted slot for
 * `stat', minutes after whatever wrote it had moved on.
 *
 * With the pages read-only the write faults where it is made, with the
 * offender's own instruction pointer in the crash dump.  And the crash it used
 * to cause cannot happen at all.
 *
 * Requires the object to be linked -z now as well: with lazy binding the GOT
 * has to stay writable, so anything still resolving on demand is left alone.
 */
static void rtld_protect_relro(dso_t *d)
{
	if (!d || !d->relro_len || d->relro_done)
		return;

	/* Both ends round DOWN.  The end especially: p_memsz is page-aligned by
	 * construction (the linker pads the region out), and rounding it up
	 * instead would freeze the first page of .data on any object where it
	 * is not -- which is the sort of damage that shows up much later as an
	 * inexplicable fault on an ordinary global.
	 *
	 * Rounding the start down is safe for the matching reason: -z relro
	 * makes the linker start the region on a page belonging to the writable
	 * segment alone, never one shared with the text or rodata above it. */
	uint64_t start = d->relro_start & ~(uint64_t)(g_page_size - 1);
	uint64_t end = (d->relro_start + d->relro_len) &
		       ~(uint64_t)(g_page_size - 1);

	/* An object that still binds lazily has its PLT slots written by the
	 * resolver, one per first call, for as long as it runs -- so its
	 * .got.plt cannot be frozen.  Only the three reserved entries at the
	 * front are written once, at load time.
	 *
	 * The stock linker script accounts for that and ends the region at
	 * pltgot+24, so a lazy object can be protected as it stands; ours puts
	 * the whole of .got.plt inside, which is only sound because it is
	 * always paired with -z now.  Deciding from the region itself rather
	 * than from the flag covers both, and costs one comparison. */
	if (!d->bind_now && d->pltgot) {
		uint64_t lazy_from = (uint64_t)d->pltgot + 3 * sizeof(uint64_t);
		if (end > lazy_from)
			return;
	}

	d->relro_done = 1;
	if (start < end)
		rtld_mprotect((void *)start, (size_t)(end - start), PROT_READ);
}

static void rtld_relocate(dso_t *d)
{
	if (d->relocated)
		return;
	d->relocated = 1;
	if (d->rela && d->rela_size)
		rtld_apply_relocs(d, d->rela, d->rela_size);
	if (d->jmprel && d->jmprel_size) {
		if (d->bind_now || d->is_main) {
			rtld_apply_relocs(d, d->jmprel, d->jmprel_size);
			rtld_verify_plt(d);
			/* The main executable took this branch on `is_main'
			 * alone and so never reached rtld_setup_pltgot, which
			 * is the only other place PLT0 gets armed.  gdb is the
			 * first executable here whose PLT is large enough to
			 * make that matter. */
			rtld_arm_plt0(d);
		} else {
			rtld_setup_pltgot(d);
		}
	}
}

static void rtld_init_dso(dso_t *d)
{
	if (d->initialized || d->is_main)
		return;
	d->initialized = 1;
	if (d->init_fn)
		d->init_fn();
	if (d->init_array && d->init_array_sz) {
		size_t n = d->init_array_sz / sizeof(void *);
		for (size_t i = 0; i < n; i++) {
			void (*f)(void) = (void (*)(void))d->init_array[i];
			if (f && (uint64_t)f != (uint64_t)-1)
				f();
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
int _rtld_dlclose(void *handle) __attribute__((visibility("default")));
char *_rtld_dlerror(void) __attribute__((visibility("default")));

/* ------------------------------------------------------------------ *
 * TLS interface for libc.
 *
 * libc creates a thread's stack and control block, so it — not the loader —
 * has to place the per-thread TLS area.  These three calls tell it how much
 * room to leave below the thread pointer and fill that area with the initial
 * images.  Keeping the knowledge of which objects own which slice here means
 * libc never has to walk the object list.
 * ------------------------------------------------------------------ */

uint64_t _rtld_tls_size(void) __attribute__((visibility("default")));
uint64_t _rtld_tls_align(void) __attribute__((visibility("default")));
void _rtld_tls_init(void *tp) __attribute__((visibility("default")));

/* Bytes that must be reserved BELOW the thread pointer. */
uint64_t _rtld_tls_size(void)
{
	return g_tls_reserved;
}

uint64_t _rtld_tls_align(void)
{
	return g_tls_static_align;
}

/* Fill [tp - _rtld_tls_size(), tp) with each object's initial image, zeroing
 * the .tbss remainder.  The caller has already zeroed the block, but do it
 * again per-object so a recycled allocation cannot leak another thread's
 * values. */
void _rtld_tls_init(void *tp)
{
	uint8_t *t = (uint8_t *)tp;

	if (!t)
		return;
	for (int i = 0; i < g_ndsos; i++) {
		dso_t *d = &g_dsos[i];
		if (!d->tls_memsz || !d->tls_offset)
			continue;
		uint8_t *dst = t + d->tls_offset;
		rtld_memset(dst, 0, d->tls_memsz);
		if (d->tls_filesz && d->tls_image)
			rtld_memcpy(dst, (const void *)d->tls_image,
				    d->tls_filesz);
	}
}

void *_rtld_dlopen(const char *filename, int flags)
{
	(void)flags;
	g_dlerror_set = 0;
	if (!filename) {
		for (int i = 0; i < g_ndsos; i++)
			if (g_dsos[i].is_main)
				return &g_dsos[i];
		return NULL;
	}
	dso_t *d = rtld_find_dso(filename);
	if (d) {
		d->refcount++;
		return d;
	}

	dso_t *ld = NULL;
	if (filename[0] == '/') {
		/* Objects are registered under their basename, so an absolute
		 * path has to be matched against that too — otherwise
		 * dlopen("/usr/lib/xorg/modules/libfoo.so") loads a second
		 * copy of an object already open as "libfoo.so". */
		const char *bn = filename;
		for (const char *p = filename; *p; p++)
			if (*p == '/')
				bn = p + 1;
		d = rtld_find_dso(bn);
		if (d) {
			d->refcount++;
			return d;
		}
		ld = rtld_load_dso_from_file(filename, bn);
	} else {
		ld = rtld_load_library(filename);
	}
	if (!ld) {
		rtld_set_error2("cannot load shared library", filename);
		return NULL;
	}

	/* Relocate and initialise everything that is not done yet, not just the
	 * object named above: loading it may have pulled in its own DT_NEEDED
	 * dependencies, and those arrive unrelocated.  Leaving them that way
	 * means calling through GOT slots that were never filled in, which is
	 * how a dlopen'd module with a dependency of its own crashes.  Both
	 * helpers are idempotent, so already-loaded objects are skipped.
	 *
	 * Dependencies are appended to g_dsos after the object that needs them,
	 * so walking backwards relocates and initialises them first. */
	for (int i = g_ndsos - 1; i >= 0; i--)
		rtld_relocate(&g_dsos[i]);
	/* Lock down RELRO for anything newly relocated (see the startup path). */
	for (int i = 0; i < g_ndsos; i++)
		rtld_protect_relro(&g_dsos[i]);
	/* Now that the images are relocated, fill the TLS slice of every
	 * newly assigned object in THIS thread's block (rtld_assign_tls zeroed
	 * it and deferred the copy -- see there).  Before the constructors:
	 * those may already read __thread variables. */
	if (g_tls_tp)
		for (int i = 0; i < g_ndsos; i++) {
			dso_t *d = &g_dsos[i];
			if (!d->tls_needs_init)
				continue;
			d->tls_needs_init = 0;
			if (d->tls_memsz && d->tls_filesz && d->tls_image)
				rtld_memcpy(g_tls_tp + d->tls_offset,
					    (const void *)d->tls_image,
					    d->tls_filesz);
		}
	/* No-op after the first call (the startup path lays the block out). */
	rtld_init_tls();
	for (int i = g_ndsos - 1; i >= 0; i--)
		rtld_init_dso(&g_dsos[i]);

	/* Tell a debugger the object list grew.
	 *
	 * Announced as RT_ADD first and RT_CONSISTENT after, with the list only
	 * edited in between: a debugger stopped at either call can tell from
	 * r_state whether what it is looking at is finished.  Everything newly
	 * loaded is added, not just the object named -- a dlopen pulls in its
	 * own dependencies, and a debugger that never heard about those cannot
	 * resolve a symbol in one. */
	rtld_debug_state(RT_ADD);
	for (int i = 0; i < g_ndsos; i++) {
		dso_t *d = &g_dsos[i];

		/* Not already on the list, and not a dlclose'd empty slot. */
		if ((d->name || d->symtab) && !d->lm.l_ld && d->dynamic)
			rtld_link_map_add(d);
	}
	rtld_debug_state(RT_CONSISTENT);
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
	const Elf64_Sym *sym =
		d->gnu_hash ? gnu_hash_lookup(d, symbol, gnu_hash_fn(symbol)) :
			      sysv_hash_lookup(d, symbol);
	if (sym && sym->st_shndx != SHN_UNDEF)
		return (void *)(d->base + sym->st_value);
	rtld_set_error("symbol not found in object");
	return NULL;
}

int _rtld_dlclose(void *handle)
{
	g_dlerror_set = 0;
	if (!handle)
		return -1;
	dso_t *d = (dso_t *)handle;
	if (d->is_main)
		return 0;
	if (--d->refcount > 0)
		return 0;
	/* Finalizers (reverse order) */
	if (d->fini_array && d->fini_array_sz) {
		size_t n = d->fini_array_sz / sizeof(void *);
		for (size_t i = n; i > 0; i--) {
			void (*f)(void) = (void (*)(void))d->fini_array[i - 1];
			if (f && (uint64_t)f != (uint64_t)-1)
				f();
		}
	}
	if (d->fini_fn)
		d->fini_fn();
	/* Off the debugger's list BEFORE the pages go away, and announced as
	 * RT_DELETE while it happens.  A debugger that read the list after the
	 * unmap but before the removal would follow l_ld into memory that is no
	 * longer mapped -- the list has to stop describing the object strictly
	 * before the object stops existing. */
	rtld_debug_state(RT_DELETE);
	rtld_link_map_remove(d);

	if (d->map_base && d->map_size)
		rtld_munmap((void *)d->map_base, d->map_size);
	/* Zero the entire DSO entry so subsequent rtld_lookup_symbol iterations
     * skip it (symtab/strtab checks will be NULL) and stale pointers into
     * the now-unmapped library pages can never be dereferenced. */
	rtld_memset(d, 0, sizeof(*d));
	rtld_debug_state(RT_CONSISTENT);
	/* Tells anything caching per-object data that its cache is now stale --
	 * the pages this object occupied are unmapped, and an entry still
	 * pointing into them is a fault waiting for the next lookup. */
	g_dl_subs++;
	return 0;
}

char *_rtld_dlerror(void)
{
	if (g_dlerror_set) {
		g_dlerror_set = 0;
		return g_dlerror_buf;
	}
	return NULL;
}

/* ================================================================== */
/*  Object introspection: _dl_find_object, dl_iterate_phdr, dladdr    */
/*                                                                    */
/*  All three answer the same question -- which loaded object covers  */
/*  this address, and what is in its program headers -- so all three  */
/*  are here, where the object list is, and libc wraps them.          */
/*                                                                    */
/*  _dl_find_object is not an optional extra.  It is how the C++      */
/*  exception unwinter finds an object's PT_GNU_EH_FRAME from a       */
/*  program counter: without it every throw ends in std::terminate    */
/*  rather than in the matching catch, and the failure names neither  */
/*  the loader nor the missing call.                                  */
/* ================================================================== */

struct rtld_find_object {
	unsigned long long dlfo_flags;
	void *dlfo_map_start;
	void *dlfo_map_end;
	void *dlfo_link_map;
	void *dlfo_eh_frame;
	unsigned long long __dlfo_reserved[7];
};

struct rtld_phdr_info {
	uint64_t dlpi_addr;
	const char *dlpi_name;
	const Elf64_Phdr *dlpi_phdr;
	uint16_t dlpi_phnum;
	unsigned long long dlpi_adds;
	unsigned long long dlpi_subs;
	size_t dlpi_tls_modid;
	void *dlpi_tls_data;
};

int _rtld_find_object(void *addr, struct rtld_find_object *result)
	__attribute__((visibility("default")));
int _rtld_iterate_phdr(int (*cb)(struct rtld_phdr_info *, size_t, void *),
		       void *data) __attribute__((visibility("default")));
int _rtld_dladdr(const void *addr, const char **fname, void **fbase,
		 const char **sname, void **saddr)
	__attribute__((visibility("default")));

/* The address range an object occupies: from its lowest PT_LOAD to the end of
 * its highest.  Derived from the program headers rather than read from
 * map_base/map_size, because those describe the mapping the loader made and are
 * not set for the main executable, which the kernel mapped. */
static void rtld_dso_bounds(const dso_t *d, uint64_t *start, uint64_t *end)
{
	uint64_t lo = (uint64_t)-1, hi = 0;

	for (int i = 0; i < d->phnum; i++) {
		if (d->phdrs[i].p_type != PT_LOAD)
			continue;
		uint64_t s = d->base + d->phdrs[i].p_vaddr;
		uint64_t e = s + d->phdrs[i].p_memsz;
		if (s < lo)
			lo = s;
		if (e > hi)
			hi = e;
	}
	if (lo == (uint64_t)-1)
		lo = d->base;
	if (hi < lo)
		hi = lo;
	*start = lo;
	*end = hi;
}

static dso_t *rtld_dso_for_addr(uint64_t a)
{
	for (int i = 0; i < g_ndsos; i++) {
		dso_t *d = &g_dsos[i];
		uint64_t lo, hi;

		/* A dlclose()d slot is zeroed, so it has no headers to walk. */
		if (!d->phdrs || !d->phnum)
			continue;
		rtld_dso_bounds(d, &lo, &hi);
		if (a >= lo && a < hi)
			return d;
	}
	return NULL;
}

int _rtld_find_object(void *addr, struct rtld_find_object *result)
{
	dso_t *d = rtld_dso_for_addr((uint64_t)addr);
	uint64_t lo, hi;

	if (!d || !result)
		return -1;
	rtld_dso_bounds(d, &lo, &hi);

	result->dlfo_flags = 0;
	result->dlfo_map_start = (void *)lo;
	result->dlfo_map_end = (void *)hi;
	result->dlfo_link_map = d;
	result->dlfo_eh_frame = NULL;
	for (int i = 0; i < 7; i++)
		result->__dlfo_reserved[i] = 0;

	/* The unwinder's actual question.  An object without the segment is not
	 * an error -- it simply has no exception data, and the caller then knows
	 * to look no further rather than searching a table that is not there. */
	for (int i = 0; i < d->phnum; i++)
		if (d->phdrs[i].p_type == PT_GNU_EH_FRAME) {
			result->dlfo_eh_frame =
				(void *)(d->base + d->phdrs[i].p_vaddr);
			break;
		}
	return 0;
}

int _rtld_iterate_phdr(int (*cb)(struct rtld_phdr_info *, size_t, void *),
		       void *data)
{
	if (!cb)
		return 0;
	for (int i = 0; i < g_ndsos; i++) {
		dso_t *d = &g_dsos[i];
		struct rtld_phdr_info info;
		int r;

		if (!d->phdrs || !d->phnum)
			continue;
		info.dlpi_addr = d->base;
		/* The main executable is reported with an empty name, which is
		 * what every caller of this interface expects to identify it
		 * by; the loader stores a placeholder for its own use. */
		info.dlpi_name = d->is_main ? "" : d->name;
		info.dlpi_phdr = d->phdrs;
		info.dlpi_phnum = d->phnum;
		info.dlpi_adds = g_dl_adds;
		info.dlpi_subs = g_dl_subs;
		info.dlpi_tls_modid = (size_t)d->tls_modid;
		info.dlpi_tls_data = NULL;

		/* A non-zero return ends the walk and is passed back: the
		 * caller uses it to say "found it, stop". */
		r = cb(&info, sizeof(info), data);
		if (r)
			return r;
	}
	return 0;
}

/* The defined symbol with the highest address at or below `a`, within one
 * object.  Walking the symbol table linearly is right here: the hash tables
 * answer name-to-address, and this is the other direction. */
static const Elf64_Sym *rtld_sym_for_addr(const dso_t *d, uint64_t a)
{
	const Elf64_Sym *best = NULL;
	uint32_t n = 0;

	if (!d->symtab || !d->strtab)
		return NULL;

	/* How many symbols there are is not recorded in the dynamic section, so
	 * it is taken from whichever hash table the object carries: the SysV
	 * chain has one entry per symbol, and the GNU table's last bucket chain
	 * ends at the highest index it covers. */
	if (d->sysv_hash) {
		n = d->sysv_nchain;
	} else if (d->gnu_hash && d->gnu_nbuckets) {
		uint32_t last = 0;
		for (uint32_t i = 0; i < d->gnu_nbuckets; i++)
			if (d->gnu_buckets[i] > last)
				last = d->gnu_buckets[i];
		if (last < d->gnu_symoffset)
			return NULL;
		/* Follow that bucket's chain to its terminator. */
		while (!(d->gnu_chain[last - d->gnu_symoffset] & 1))
			last++;
		n = last + 1;
	}

	for (uint32_t i = 0; i < n; i++) {
		const Elf64_Sym *s = &d->symtab[i];
		uint64_t v;

		if (s->st_shndx == SHN_UNDEF || !s->st_value)
			continue;
		v = d->base + s->st_value;
		if (v > a)
			continue;
		/* Prefer the closest match, and among equals the one whose size
		 * actually covers the address. */
		if (!best || v > d->base + best->st_value)
			best = s;
	}
	return best;
}

int _rtld_dladdr(const void *addr, const char **fname, void **fbase,
		 const char **sname, void **saddr)
{
	uint64_t a = (uint64_t)addr;
	dso_t *d = rtld_dso_for_addr(a);
	const Elf64_Sym *s;

	if (!d)
		return 0;
	if (fname)
		*fname = d->name;
	if (fbase)
		*fbase = (void *)d->base;
	if (sname)
		*sname = NULL;
	if (saddr)
		*saddr = NULL;

	s = rtld_sym_for_addr(d, a);
	if (s) {
		if (sname)
			*sname = d->strtab + s->st_name;
		if (saddr)
			*saddr = (void *)(d->base + s->st_value);
	}
	return 1;
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
	const Elf64_Dyn *own_dyn = NULL;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_DYNAMIC) {
			own_dyn = (const Elf64_Dyn *)(own_base + ph[i].p_vaddr);
			break;
		}

	/* ---- Register ourselves ---- */
	dso_t *rtld = rtld_alloc_dso();
	rtld->name = "ld-likeos.so";
	/* And its path, for the same reason every other object has one: a
	 * debugger opens l_name to read symbols, and the loader appears in
	 * every backtrace that starts before main. */
	rtld->path = "/lib/ld-likeos.so";
	rtld->base = own_base;
	rtld->dynamic = own_dyn;
	rtld->relocated = 1;
	rtld->initialized = 1;
	rtld_parse_dynamic(rtld);

	/* ---- Parse auxiliary vector ---- */
	uint64_t argc = sp[0];
	uint64_t *p = sp + 1 + argc + 1; /* past argv + NULL */
	while (*p)
		p++; /* skip envp        */
	p++; /* past envp NULL   */

	uint64_t at_phdr = 0, at_phnum = 0, at_entry = 0, at_pagesz = 0;
	while (p[0] != AT_NULL) {
		switch (p[0]) {
		case AT_PHDR:
			at_phdr = p[1];
			break;
		case AT_PHNUM:
			at_phnum = p[1];
			break;
		case AT_ENTRY:
			at_entry = p[1];
			break;
		case AT_PAGESZ:
			at_pagesz = p[1];
			break;
		}
		p += 2;
	}
	if (at_pagesz)
		g_page_size = at_pagesz;

	/* ---- Register the main executable ---- */
	dso_t *main_dso = rtld_alloc_dso();
	main_dso->name = "<main>";
	main_dso->is_main = 1;
	main_dso->phnum = (uint16_t)at_phnum;

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
	main_dso->base = main_base;
	main_dso->phdrs = mph;

	for (uint64_t i = 0; i < at_phnum; i++) {
		if (mph[i].p_type == PT_DYNAMIC)
			main_dso->dynamic =
				(const Elf64_Dyn *)(main_base + mph[i].p_vaddr);
		if (mph[i].p_type == PT_TLS) {
			main_dso->tls_image = main_base + mph[i].p_vaddr;
			main_dso->tls_filesz = mph[i].p_filesz;
			main_dso->tls_memsz = mph[i].p_memsz;
			main_dso->tls_align = mph[i].p_align;
		}
		if (mph[i].p_type == PT_GNU_RELRO) {
			main_dso->relro_start = main_base + mph[i].p_vaddr;
			main_dso->relro_len = mph[i].p_memsz;
		}
	}
	rtld_parse_dynamic(main_dso);
	rtld_assign_tls(main_dso);

	/* ---- Load DT_NEEDED libraries ---- */
	if (main_dso->dynamic)
		for (const Elf64_Dyn *e = main_dso->dynamic;
		     e->d_tag != DT_NULL; e++)
			if (e->d_tag == DT_NEEDED) {
				const char *need =
					main_dso->strtab + e->d_un.d_val;
				/* Report a failure here exactly as the
				 * recursive loader does.  This return value
				 * used to be discarded, so a library the MAIN
				 * program depends on could fail to load and
				 * say nothing at all: the run then died in the
				 * relocation pass with a screen of "undefined
				 * symbol", none of which named the library
				 * that was missing.  That is how an
				 * over-tight size cap on libwebkit2gtk hid
				 * itself. */
				if (!rtld_find_dso(need))
					if (!rtld_load_library(need))
						rtld_report_missing(need);
			}

	/* ---- Relocate (dependencies first) ---- */
	for (int i = g_ndsos - 1; i >= 0; i--)
		rtld_relocate(&g_dsos[i]);

	/* Publish the debugger rendezvous.
	 *
	 * BEFORE the RELRO lock-down below, and that ordering is the whole
	 * reason it sits here: DT_DEBUG lives in the dynamic section, which is
	 * inside the region about to be made read-only, so the store has to
	 * happen while it is still writable.
	 *
	 * The object list is built in load order with the main executable
	 * first, then announced once as consistent -- there is no debugger
	 * attached yet to see the intermediate states of start-up, and a
	 * program stopped at its entry point wants one complete answer. */
	{
		dso_t *m = NULL;

		for (int i = 0; i < g_ndsos; i++)
			if (g_dsos[i].is_main) {
				m = &g_dsos[i];
				break;
			}
		if (m)
			rtld_link_map_add(m);
		for (int i = 0; i < g_ndsos; i++)
			if (!g_dsos[i].is_main &&
			    (g_dsos[i].name || g_dsos[i].symtab))
				rtld_link_map_add(&g_dsos[i]);
		rtld_publish_r_debug(m, own_base);
		rtld_debug_state(RT_CONSISTENT);
	}

	/* Relocation is done: lock down each object's RELRO region.  After all
	 * of them, not inside the loop -- an object relocated earlier may still
	 * be written by a later one's relocations. */
	for (int i = 0; i < g_ndsos; i++)
		rtld_protect_relro(&g_dsos[i]);

	/* ---- TLS ----
	 * Unconditional: even a program with no __thread data needs the block,
	 * because libc's thread control block lives at the thread pointer. */
	rtld_init_tls();

	/* ---- Initializers ---- */
	for (int i = 0; i < g_ndsos; i++)
		rtld_init_dso(&g_dsos[i]);

	return at_entry;
}
