// LikeOS-64 ELF64 Loader Definitions
#ifndef _KERNEL_ELF_H_
#define _KERNEL_ELF_H_

#include <kernel/types.h>

// ELF Magic
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

// ELF Class
#define ELFCLASSNONE    0
#define ELFCLASS32      1
#define ELFCLASS64      2

// ELF Data encoding
#define ELFDATANONE     0
#define ELFDATA2LSB     1   // Little endian
#define ELFDATA2MSB     2   // Big endian

// ELF Type
#define ET_NONE         0
#define ET_REL          1   // Relocatable
#define ET_EXEC         2   // Executable
#define ET_DYN          3   // Shared object
#define ET_CORE         4   // Core dump

// ELF Machine
#define EM_X86_64       62

// ELF Version
#define EV_CURRENT      1

// Program header types
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_GNU_STACK    0x6474E551

// Program header flags
#define PF_X            0x1     // Execute
#define PF_W            0x2     // Write
#define PF_R            0x4     // Read

// e_ident indices
#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8
#define EI_PAD          9
#define EI_NIDENT       16

// ELF64 Header
typedef struct {
    uint8_t  e_ident[EI_NIDENT];  // Magic number and other info
    uint16_t e_type;              // Object file type
    uint16_t e_machine;           // Architecture
    uint32_t e_version;           // Object file version
    uint64_t e_entry;             // Entry point virtual address
    uint64_t e_phoff;             // Program header table file offset
    uint64_t e_shoff;             // Section header table file offset
    uint32_t e_flags;             // Processor-specific flags
    uint16_t e_ehsize;            // ELF header size in bytes
    uint16_t e_phentsize;         // Program header table entry size
    uint16_t e_phnum;             // Program header table entry count
    uint16_t e_shentsize;         // Section header table entry size
    uint16_t e_shnum;             // Section header table entry count
    uint16_t e_shstrndx;          // Section header string table index
} __attribute__((packed)) Elf64_Ehdr;

// ELF64 Program Header
typedef struct {
    uint32_t p_type;              // Segment type
    uint32_t p_flags;             // Segment flags
    uint64_t p_offset;            // Segment file offset
    uint64_t p_vaddr;             // Segment virtual address
    uint64_t p_paddr;             // Segment physical address
    uint64_t p_filesz;            // Segment size in file
    uint64_t p_memsz;             // Segment size in memory
    uint64_t p_align;             // Segment alignment
} __attribute__((packed)) Elf64_Phdr;

// ELF64 Section Header
typedef struct {
    uint32_t sh_name;             // Section name (string tbl index)
    uint32_t sh_type;             // Section type
    uint64_t sh_flags;            // Section flags
    uint64_t sh_addr;             // Section virtual addr at execution
    uint64_t sh_offset;           // Section file offset
    uint64_t sh_size;             // Section size in bytes
    uint32_t sh_link;             // Link to another section
    uint32_t sh_info;             // Additional section information
    uint64_t sh_addralign;        // Section alignment
    uint64_t sh_entsize;          // Entry size if section holds table
} __attribute__((packed)) Elf64_Shdr;

// ============================================================================
// DYNAMIC LINKING STRUCTURES
// ============================================================================

// ELF64 Dynamic section entry
typedef struct {
    int64_t  d_tag;               // Dynamic entry type
    union {
        uint64_t d_val;           // Integer value
        uint64_t d_ptr;           // Address value
    } d_un;
} __attribute__((packed)) Elf64_Dyn;

// ELF64 Symbol table entry
typedef struct {
    uint32_t st_name;             // Symbol name (string tbl index)
    uint8_t  st_info;             // Symbol type and binding
    uint8_t  st_other;            // Symbol visibility
    uint16_t st_shndx;            // Section index
    uint64_t st_value;            // Symbol value
    uint64_t st_size;             // Symbol size
} __attribute__((packed)) Elf64_Sym;

// ELF64 Relocation entry with addend
typedef struct {
    uint64_t r_offset;            // Address
    uint64_t r_info;              // Relocation type and symbol index
    int64_t  r_addend;            // Addend
} __attribute__((packed)) Elf64_Rela;

// ELF64 Relocation entry without addend
typedef struct {
    uint64_t r_offset;            // Address
    uint64_t r_info;              // Relocation type and symbol index
} __attribute__((packed)) Elf64_Rel;

// Symbol binding (upper nibble of st_info)
#define ELF64_ST_BIND(i)    ((i) >> 4)
#define ELF64_ST_TYPE(i)    ((i) & 0xF)
#define ELF64_ST_INFO(b,t)  (((b) << 4) + ((t) & 0xF))

// Symbol binding values
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2

// Symbol type values
#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_SECTION     3
#define STT_FILE        4
#define STT_COMMON      5
#define STT_TLS         6

// Symbol visibility
#define STV_DEFAULT     0
#define STV_INTERNAL    1
#define STV_HIDDEN      2
#define STV_PROTECTED   3
#define ELF64_ST_VISIBILITY(o) ((o) & 0x3)

// Special section indices
#define SHN_UNDEF       0
#define SHN_ABS         0xFFF1
#define SHN_COMMON      0xFFF2

// Relocation macros
#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xFFFFFFFFL)
#define ELF64_R_INFO(s,t)  (((uint64_t)(s) << 32) + (uint64_t)(t))

// x86-64 relocation types
#define R_X86_64_NONE       0
#define R_X86_64_64         1   // S + A
#define R_X86_64_PC32       2   // S + A - P
#define R_X86_64_GOT32      3
#define R_X86_64_PLT32      4
#define R_X86_64_COPY       5   // Copy symbol from shared object
#define R_X86_64_GLOB_DAT   6   // S (set GOT entry to symbol addr)
#define R_X86_64_JUMP_SLOT  7   // S (set PLT GOT entry)
#define R_X86_64_RELATIVE   8   // B + A (base + addend)
#define R_X86_64_GOTPCREL   9
#define R_X86_64_32        10
#define R_X86_64_32S       11
#define R_X86_64_16        12
#define R_X86_64_PC16      13
#define R_X86_64_8         14
#define R_X86_64_PC8       15
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_TPOFF64   18
#define R_X86_64_TLSGD     19
#define R_X86_64_TLSLD     20
#define R_X86_64_DTPOFF32  21
#define R_X86_64_GOTTPOFF  22
#define R_X86_64_TPOFF32   23
#define R_X86_64_IRELATIVE 37

// Dynamic section tags
#define DT_NULL         0   // End of dynamic section
#define DT_NEEDED       1   // Name of needed library
#define DT_PLTRELSZ     2   // Size of PLT relocation entries
#define DT_PLTGOT       3   // PLT and/or GOT address
#define DT_HASH         4   // Symbol hash table address
#define DT_STRTAB       5   // String table address
#define DT_SYMTAB       6   // Symbol table address
#define DT_RELA         7   // Rela relocation table
#define DT_RELASZ       8   // Size of Rela relocation table
#define DT_RELAENT      9   // Size of a Rela relocation entry
#define DT_STRSZ       10   // Size of string table
#define DT_SYMENT      11   // Size of symbol table entry
#define DT_INIT        12   // Address of init function
#define DT_FINI        13   // Address of fini function
#define DT_SONAME      14   // Name of shared object
#define DT_RPATH       15   // Library search path (deprecated)
#define DT_SYMBOLIC    16   // Alter symbol resolution algorithm
#define DT_REL         17   // Rel relocation table
#define DT_RELSZ       18   // Size of Rel relocation table
#define DT_RELENT      19   // Size of a Rel relocation entry
#define DT_PLTREL      20   // Type of PLT relocation entries
#define DT_DEBUG       21   // Debugging entry
#define DT_TEXTREL     22   // Relocation might modify .text
#define DT_JMPREL      23   // Address of PLT relocation entries
#define DT_BIND_NOW    24   // Process relocations at load time
#define DT_INIT_ARRAY  25   // Array of init functions
#define DT_FINI_ARRAY  26   // Array of fini functions
#define DT_INIT_ARRAYSZ 27  // Size of DT_INIT_ARRAY
#define DT_FINI_ARRAYSZ 28  // Size of DT_FINI_ARRAY
#define DT_RUNPATH     29   // Library search path
#define DT_FLAGS       30   // Flags
#define DT_PREINIT_ARRAY    32
#define DT_PREINIT_ARRAYSZ  33

// DT_FLAGS values
#define DF_ORIGIN       0x01
#define DF_SYMBOLIC     0x02
#define DF_TEXTREL      0x04
#define DF_BIND_NOW     0x08
#define DF_STATIC_TLS   0x10

// GNU extensions
#define DT_GNU_HASH     0x6FFFFEF5
#define DT_VERSYM       0x6FFFFFF0
#define DT_RELACOUNT    0x6FFFFFF9
#define DT_RELCOUNT     0x6FFFFFFA
#define DT_FLAGS_1      0x6FFFFFFB
#define DT_VERDEF       0x6FFFFFFC
#define DT_VERDEFNUM    0x6FFFFFFD
#define DT_VERNEED      0x6FFFFFFE
#define DT_VERNEEDNUM   0x6FFFFFFF

// DT_FLAGS_1 values
#define DF_1_NOW        0x00000001
#define DF_1_PIE        0x08000000

// TLS-related dynamic tags
#define DT_TLSDESC_PLT  0x6FFFFEF6
#define DT_TLSDESC_GOT  0x6FFFFEF7

// Program header types for TLS
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474E550
#define PT_GNU_RELRO    0x6474E552
#define PT_GNU_PROPERTY 0x6474E553

// Section header types
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_DYNSYM      11
#define SHT_INIT_ARRAY  14
#define SHT_FINI_ARRAY  15
#define SHT_GNU_HASH    0x6FFFFFF6

// Auxiliary vector types (passed on stack to dynamic linker)
#define AT_NULL         0   // End of auxiliary vector
#define AT_IGNORE       1   // Entry should be ignored
#define AT_EXECFD       2   // File descriptor of program
#define AT_PHDR         3   // Program headers for program
#define AT_PHENT        4   // Size of program header entry
#define AT_PHNUM        5   // Number of program headers
#define AT_PAGESZ       6   // System page size
#define AT_BASE         7   // Interpreter base address
#define AT_FLAGS        8   // Flags
#define AT_ENTRY        9   // Entry point of program
#define AT_UID          11  // Real uid
#define AT_EUID         12  // Effective uid
#define AT_GID          13  // Real gid
#define AT_EGID         14  // Effective gid
#define AT_SECURE       23  // Boolean, was exec setuid-like?
#define AT_RANDOM       25  // Address of 16 random bytes
#define AT_EXECFN       31  // Filename of program

// Auxiliary vector entry
typedef struct {
    uint64_t a_type;
    union {
        uint64_t a_val;
    } a_un;
} Elf64_auxv_t;

// ============================================================================
// GNU Hash table structures
// ============================================================================

typedef struct {
    uint32_t nbuckets;
    uint32_t symoffset;          // Index of first symbol in hash
    uint32_t bloom_size;         // Number of bloom filter words
    uint32_t bloom_shift;        // Bloom filter shift count
    // Followed by: uint64_t bloom[bloom_size]
    // Followed by: uint32_t buckets[nbuckets]
    // Followed by: uint32_t chain[] (for each symbol starting at symoffset)
} Elf64_GNU_Hash_Header;

// Forward declaration for task
struct task;

// ELF loader result structure
typedef struct {
    uint64_t entry_point;         // Program entry point
    uint64_t brk_start;           // End of loaded segments (heap start)
    uint64_t load_base;           // Lowest virtual address loaded
    uint64_t load_end;            // Highest virtual address + 1
    uint64_t phdr_addr;           // Address of loaded program headers in memory
    uint16_t phnum;               // Number of program headers
    uint16_t phentsize;           // Size of each program header entry
    uint64_t interp_base;         // Interpreter load base (0 if none)
    uint64_t interp_entry;        // Interpreter entry point (0 if none)
    char     interp_path[256];    // Interpreter path from PT_INTERP
    int      has_interp;          // Whether PT_INTERP was found
    int      is_dynamic;          // Whether this is ET_DYN
} elf_load_result_t;

// Function prototypes
// Validate an ELF64 executable
// Returns 0 on success, negative error code on failure
int elf_validate(const void* data, size_t size);

// Load a static ELF64 executable into user address space
// Parameters:
//   elf_data: Pointer to ELF file data in memory
//   elf_size: Size of ELF file
//   pml4: User page table to map pages into
//   result: Output structure with entry point and memory layout
// Returns 0 on success, negative error code on failure
int elf_load_user(const void* elf_data, size_t elf_size,
                  uint64_t* pml4, elf_load_result_t* result);

// Execute an ELF file from the filesystem
// Parameters:
//   path: Path to ELF file
//   argv: NULL-terminated array of argument strings
//   envp: NULL-terminated array of environment strings
//   out_task: Optional pointer to receive created task
// Returns 0 on success (task created), negative error code on failure
int elf_exec(const char* path, char* const argv[], char* const envp[], struct task** out_task);

// Execute ELF file, replacing current task's memory (for execve semantics)
// Returns: entry point address on success, 0 on failure
// On success, *out_stack_ptr is set to the new user stack pointer
uint64_t elf_exec_replace(const char* path, char* const argv[], char* const envp[], uint64_t* out_stack_ptr);

#endif // _KERNEL_ELF_H_
