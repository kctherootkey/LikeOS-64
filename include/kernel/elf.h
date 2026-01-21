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

// Forward declaration for task
struct task;

// ELF loader result structure
typedef struct {
    uint64_t entry_point;         // Program entry point
    uint64_t brk_start;           // End of loaded segments (heap start)
    uint64_t load_base;           // Lowest virtual address loaded
    uint64_t load_end;            // Highest virtual address + 1
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

#endif // _KERNEL_ELF_H_
