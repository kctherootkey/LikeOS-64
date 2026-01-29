#include <efi.h>
#include <efilib.h>

// ELF64 structures
#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

// ELF constants
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64 62
#define PT_LOAD 1

// Kernel entry point type (now takes boot info)
typedef void (*kernel_entry_t)(void* boot_info);

// Trampoline function type
typedef void (*trampoline_func_t)(UINT64 kernel_entry, void* boot_info, UINT64 pml4_addr);

// Framebuffer information structure
typedef struct {
    void* framebuffer_base;
    uint32_t framebuffer_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixels_per_scanline;
    uint32_t bytes_per_pixel;
} framebuffer_info_t;

// UEFI memory map entry (matching EFI_MEMORY_DESCRIPTOR)
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} memory_map_entry_t;

// Memory map information passed to kernel
#define MAX_MEMORY_MAP_ENTRIES 256
typedef struct {
    uint32_t entry_count;
    uint32_t descriptor_size;
    uint64_t total_memory;
    memory_map_entry_t entries[MAX_MEMORY_MAP_ENTRIES];
} memory_map_info_t;

// Boot information passed to kernel
typedef struct {
    framebuffer_info_t fb_info;
    memory_map_info_t mem_info;
} boot_info_t;

// Global boot info to pass to kernel
static boot_info_t g_boot_info;

// Paging structures (assuming 4-level paging)
typedef struct {
    UINT64 entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Higher half kernel constants (must match kernel.lds and memory.h)
#define KERNEL_OFFSET    0xFFFFFFFF80000000ULL
#define KERNEL_START     0x0

// Physical memory locations for page tables
// These will be dynamically allocated from UEFI at low addresses
// to avoid conflicts with VMware's EfiRuntimeServicesData regions.
// All page tables are allocated using AllocateMaxAddress to get low addresses.

// Dynamic page table addresses (set by init_page_tables())
static EFI_PHYSICAL_ADDRESS g_pml4_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pdpt_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pd_addr[4] = {0};    // 4 page directories for identity map (4GB)
static EFI_PHYSICAL_ADDRESS g_pdpt_high_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pd_high_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pt_high_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pdpt_physmap_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pd_physmap_addr[16] = {0}; // 16 page directories for physmap (16GB)

// Trampoline address (will be allocated below kernel)
static EFI_PHYSICAL_ADDRESS trampoline_addr = 0;

// Serial port I/O for debug output (COM1 = 0x3F8)
#define COM1_PORT 0x3F8

static inline void outb(UINT16 port, UINT8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    // Disable interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB
    outb(COM1_PORT + 0, 0x03);    // 38400 baud (low byte)
    outb(COM1_PORT + 1, 0x00);    // (high byte)
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);    // Enable FIFO
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static void serial_putc(char c) {
    while ((inb(COM1_PORT + 5) & 0x20) == 0);  // Wait for transmit buffer empty
    outb(COM1_PORT, c);
}

static void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_puthex(UINT64 val) {
    const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    serial_puts("0x");
    serial_puts(buf);
}

static void serial_putdec(UINT64 val) {
    char buf[21];
    buf[20] = 0;
    int i = 19;
    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0 && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    serial_puts(&buf[i+1]);
}

// Memory type names for debug output
static const char* efi_memory_type_name(UINT32 type) {
    switch (type) {
        case 0: return "EfiReservedMemoryType";
        case 1: return "EfiLoaderCode";
        case 2: return "EfiLoaderData";
        case 3: return "EfiBootServicesCode";
        case 4: return "EfiBootServicesData";
        case 5: return "EfiRuntimeServicesCode";
        case 6: return "EfiRuntimeServicesData";
        case 7: return "EfiConventionalMemory";
        case 8: return "EfiUnusableMemory";
        case 9: return "EfiACPIReclaimMemory";
        case 10: return "EfiACPIMemoryNVS";
        case 11: return "EfiMemoryMappedIO";
        case 12: return "EfiMemoryMappedIOPortSpace";
        case 13: return "EfiPalCode";
        case 14: return "EfiPersistentMemory";
        default: return "Unknown";
    }
}

// Page table entry flags
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_SIZE       (1ULL << 7)    // PS bit for 2MB pages
#define PAGE_NX         (1ULL << 63)   // No-Execute bit

// Common page combinations
#define PAGE_RW         (PAGE_PRESENT | PAGE_WRITABLE)
#define PAGE_RWX        (PAGE_PRESENT | PAGE_WRITABLE)  // Executable (NX=0)
#define PAGE_RW_NX      (PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX)  // Non-executable

// Page table pool allocator - allocates from reserved low memory to avoid conflicts
// We'll allocate a contiguous block from UEFI in low memory during init
static EFI_PHYSICAL_ADDRESS pt_pool_base = 0;
static EFI_PHYSICAL_ADDRESS pt_pool_next = 0;
static UINTN pt_pool_pages = 128;  // 128 pages = 512KB for page tables (increased for all initial tables)

static EFI_STATUS init_page_table_pool(void) {
    // Allocate page table pool in low memory (below 1MB to be safe)
    EFI_PHYSICAL_ADDRESS max_addr = 0x100000 - 1;  // Below 1MB
    
    EFI_STATUS status = uefi_call_wrapper(BS->AllocatePages, 4,
                                          AllocateMaxAddress,
                                          EfiLoaderData,
                                          pt_pool_pages,
                                          &max_addr);
    
    if (EFI_ERROR(status)) {
        // Try anywhere below kernel memory pool start
        max_addr = 0x1b00000 - 1;  // ~27MB, well below memory pool
        status = uefi_call_wrapper(BS->AllocatePages, 4,
                                   AllocateMaxAddress,
                                   EfiLoaderData,
                                   pt_pool_pages,
                                   &max_addr);
    }
    
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate page table pool: %r\r\n", status);
        return status;
    }
    
    pt_pool_base = max_addr;
    pt_pool_next = max_addr;
    
    // Zero the entire pool
    uefi_call_wrapper(BS->SetMem, 3, (VOID*)pt_pool_base, pt_pool_pages * 4096, 0);
    
    Print(L"Page table pool allocated at 0x%lx (%lu pages, %lu KB)\r\n", 
          pt_pool_base, pt_pool_pages, (pt_pool_pages * 4096) / 1024);
    
    return EFI_SUCCESS;
}

static EFI_PHYSICAL_ADDRESS allocate_page_table(void) {
    if (!pt_pool_base) {
        Print(L"ERROR: Page table pool not initialized!\r\n");
        return 0;
    }
    
    if (pt_pool_next >= pt_pool_base + (pt_pool_pages * 4096)) {
        Print(L"ERROR: Page table pool exhausted!\r\n");
        return 0;
    }
    
    EFI_PHYSICAL_ADDRESS addr = pt_pool_next;
    pt_pool_next += 4096;
    
    // Memory is already zeroed during pool init
    return addr;
}

// Allocate all initial page tables from the pool
static EFI_STATUS init_page_tables(void) {
    // First allocate the pool
    EFI_STATUS status = init_page_table_pool();
    if (EFI_ERROR(status)) {
        return status;
    }
    
    // Allocate PML4 (one page)
    g_pml4_addr = allocate_page_table();
    if (!g_pml4_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate PDPT for identity mapping (one page)
    g_pdpt_addr = allocate_page_table();
    if (!g_pdpt_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate 4 page directories for identity mapping (4GB)
    for (int i = 0; i < 4; i++) {
        g_pd_addr[i] = allocate_page_table();
        if (!g_pd_addr[i]) return EFI_OUT_OF_RESOURCES;
    }
    
    // Allocate higher half page tables
    g_pdpt_high_addr = allocate_page_table();
    if (!g_pdpt_high_addr) return EFI_OUT_OF_RESOURCES;
    
    g_pd_high_addr = allocate_page_table();
    if (!g_pd_high_addr) return EFI_OUT_OF_RESOURCES;
    
    g_pt_high_addr = allocate_page_table();
    if (!g_pt_high_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate physmap page tables
    g_pdpt_physmap_addr = allocate_page_table();
    if (!g_pdpt_physmap_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate 16 page directories for physmap (16GB)
    for (int i = 0; i < 16; i++) {
        g_pd_physmap_addr[i] = allocate_page_table();
        if (!g_pd_physmap_addr[i]) return EFI_OUT_OF_RESOURCES;
    }
    
    Print(L"Page tables allocated dynamically:\r\n");
    Print(L"  PML4:         0x%lx\r\n", g_pml4_addr);
    Print(L"  PDPT (low):   0x%lx\r\n", g_pdpt_addr);
    Print(L"  PD (low):     0x%lx - 0x%lx\r\n", g_pd_addr[0], g_pd_addr[3]);
    Print(L"  PDPT (high):  0x%lx\r\n", g_pdpt_high_addr);
    Print(L"  PD (high):    0x%lx\r\n", g_pd_high_addr);
    Print(L"  PT (high):    0x%lx\r\n", g_pt_high_addr);
    Print(L"  PDPT (phys):  0x%lx\r\n", g_pdpt_physmap_addr);
    Print(L"  PD (phys):    0x%lx - 0x%lx\r\n", g_pd_physmap_addr[0], g_pd_physmap_addr[15]);
    
    serial_puts("Page tables allocated:\n");
    serial_puts("  PML4:       "); serial_puthex(g_pml4_addr); serial_puts("\n");
    serial_puts("  PDPT (low): "); serial_puthex(g_pdpt_addr); serial_puts("\n");
    serial_puts("  PDPT (high):"); serial_puthex(g_pdpt_high_addr); serial_puts("\n");
    serial_puts("  PDPT (phys):"); serial_puthex(g_pdpt_physmap_addr); serial_puts("\n");
    
    return EFI_SUCCESS;
}

// External trampoline function from trampoline.S
extern void trampoline_jump(UINT64 kernel_entry, void* boot_info, UINT64 pml4_addr);

// We need the end symbol to calculate the size
extern char trampoline_jump_end[];

// Set up higher half kernel paging with identity mapping for low memory
static void setup_higher_half_paging(UINT64 kernel_phys_addr, UINT64 kernel_size) {
    page_table_t *pml4 = (page_table_t*)g_pml4_addr;
    page_table_t *pdpt_low = (page_table_t*)g_pdpt_addr;
    
    // Higher half page tables
    page_table_t *pdpt_high = (page_table_t*)g_pdpt_high_addr;
    page_table_t *pd_high = (page_table_t*)g_pd_high_addr;
    page_table_t *pt_high = (page_table_t*)g_pt_high_addr;
    
    // Clear main page tables (they should already be zeroed from pool, but be safe)
    for (int i = 0; i < 512; i++) {
        pml4->entries[i] = 0;
        pdpt_low->entries[i] = 0;
        pdpt_high->entries[i] = 0;
        pd_high->entries[i] = 0;
        pt_high->entries[i] = 0;
    }
    
    // Set up PML4 entries
    // Entry 0: Low memory identity mapping (0x0 - 0x7FFFFFFFFF) - EXECUTABLE
    pml4->entries[0] = g_pdpt_addr | PAGE_RWX;
    
    // Entry 511: Higher half mapping (0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF)
    pml4->entries[511] = g_pdpt_high_addr | PAGE_RWX;
    
    // Set up low memory identity mapping for first 4GB
    // 4GB = 4 PDPT entries (each covers 1GB)
    for (int pdpt_i = 0; pdpt_i < 4; pdpt_i++) {
        // Use pre-allocated page directory for this 1GB region
        EFI_PHYSICAL_ADDRESS pd_addr = g_pd_addr[pdpt_i];
        page_table_t *pd = (page_table_t*)pd_addr;
        
        // Clear page directory
        for (int i = 0; i < 512; i++) {
            pd->entries[i] = 0;
        }
        
        // Set PDPT entry to point to this page directory
        pdpt_low->entries[pdpt_i] = pd_addr | PAGE_RWX;
        
        // Each page directory covers 512 * 2MB = 1GB using 2MB pages
        // Set up 2MB page entries for this 1GB region
        for (int pd_i = 0; pd_i < 512; pd_i++) {
            // Calculate physical address for this 2MB page
            UINT64 phys_addr = (UINT64)pdpt_i * 0x40000000ULL + (UINT64)pd_i * 0x200000ULL;
            
            // Create executable 2MB pages for identity mapping
            // CRITICAL: These pages must be executable for trampoline to work
            pd->entries[pd_i] = phys_addr | PAGE_RWX | PAGE_SIZE;
        }
    }
    
    // Set up higher half mapping for kernel
    // KERNEL_OFFSET = 0xFFFFFFFF80000000
    // Virtual address: KERNEL_OFFSET + KERNEL_START = 0xFFFFFFFF80000000
    // Physical address: kernel_phys_addr
    
    // Calculate indices for higher half mapping
    UINT64 kernel_virt = KERNEL_OFFSET + KERNEL_START;
    UINT64 pml4_index = (kernel_virt >> 39) & 0x1FF;  // Should be 511
    UINT64 pdpt_index = (kernel_virt >> 30) & 0x1FF;  // Should be 256 for 0xFFFFFFFF80000000
    UINT64 pd_index = (kernel_virt >> 21) & 0x1FF;    // Should be 0
    UINT64 pt_index = (kernel_virt >> 12) & 0x1FF;    // Should be 0
    
    // Set up higher half page table hierarchy
    pdpt_high->entries[pdpt_index] = g_pd_high_addr | PAGE_RWX;
    pd_high->entries[pd_index] = g_pt_high_addr | PAGE_RWX;
    
    // Map kernel pages in higher half
    UINT64 kernel_pages = (kernel_size + 4095) / 4096;
    
    // Calculate how much virtual memory we need to map
    // We need to cover at least 0xFFFFFFFF80A00000 which is 0xA00000 (10MB) above kernel base
    UINT64 min_virtual_size = 32 * 1024 * 1024; // 32MB
    UINT64 total_pages_needed = min_virtual_size / 4096; // Convert to 4KB pages
    
    Print(L"Mapping %lu MB (%lu pages) of virtual memory starting at 0x%lx...\r\n", 
          min_virtual_size / (1024 * 1024), total_pages_needed, kernel_virt);
    
    // Calculate how many page tables we need
    // Each page table can map 512 pages (2MB of virtual memory)
    UINT64 page_tables_needed = (total_pages_needed + 511) / 512;
    
    Print(L"Allocating %lu additional page tables for extended mapping...\r\n", page_tables_needed - 1);
    
    // First page table is already allocated at PT_HIGH_ADDRESS
    // Map what we can with the first page table
    UINT64 first_pt_pages = (total_pages_needed < 512) ? total_pages_needed : 512;
    
    // Map kernel pages first with real physical addresses
    for (UINT64 i = 0; i < kernel_pages && i < first_pt_pages; i++) {
        UINT64 phys_addr = kernel_phys_addr + (i * 4096);
        pt_high->entries[pt_index + i] = phys_addr | PAGE_RWX;
    }
    
    // For pages beyond the kernel, allocate new physical memory
    for (UINT64 i = kernel_pages; i < first_pt_pages; i++) {
        // Allocate physical memory for this page
        UINTN pages_to_allocate = 1;
        EFI_PHYSICAL_ADDRESS phys_addr = 0;
        
        EFI_STATUS alloc_status = uefi_call_wrapper(BS->AllocatePages, 4, 
                                                    AllocateAnyPages, 
                                                    EfiLoaderData, 
                                                    pages_to_allocate, 
                                                    &phys_addr);
        
        if (EFI_ERROR(alloc_status)) {
            Print(L"ERROR: Could not allocate physical memory for page %lu: %r\r\n", i, alloc_status);
            break;
        }
        
        // Map the allocated physical memory to virtual address
        pt_high->entries[pt_index + i] = phys_addr | PAGE_RWX;
        
        // Zero out the allocated memory
        uefi_call_wrapper(BS->SetMem, 3, (VOID*)phys_addr, 4096, 0);
    }
    
    // If we need more page tables, allocate and set them up
    UINT64 pages_mapped = first_pt_pages;
    
    for (UINT64 pt_i = 1; pt_i < page_tables_needed && pages_mapped < total_pages_needed; pt_i++) {
        // Allocate page table from our reserved pool (not AllocateAnyPages!)
        EFI_PHYSICAL_ADDRESS pt_phys_addr = allocate_page_table();
        
        if (!pt_phys_addr) {
            Print(L"ERROR: Could not allocate page table %lu from pool\r\n", pt_i);
            break;
        }
        
        // Page table is already zeroed by allocate_page_table()
        
        // Set up page directory entry to point to this page table
        UINT64 pd_entry_index = pd_index + pt_i;
        if (pd_entry_index < 512) {
            pd_high->entries[pd_entry_index] = pt_phys_addr | PAGE_RWX;
            
            Print(L"Page table %lu allocated at 0x%lx, mapped to PD[%lu]\r\n", 
                  pt_i, pt_phys_addr, pd_entry_index);
            
            // Get pointer to the new page table
            page_table_t *current_pt = (page_table_t*)pt_phys_addr;
            
            // Map pages in this page table
            UINT64 pages_in_this_pt = (total_pages_needed - pages_mapped < 512) ? 
                                     (total_pages_needed - pages_mapped) : 512;
            
            for (UINT64 i = 0; i < pages_in_this_pt; i++) {
                // Check if this page is still within the kernel's pre-allocated physical memory
                if (pages_mapped < kernel_pages) {
                    // Map to the kernel's physical memory
                    UINT64 phys_addr = kernel_phys_addr + (pages_mapped * 4096);
                    current_pt->entries[i] = phys_addr | PAGE_RWX;
                } else {
                    // Allocate physical memory for this page (beyond kernel)
                    UINTN pages_to_allocate = 1;
                    EFI_PHYSICAL_ADDRESS phys_addr = 0;
                    
                    EFI_STATUS alloc_status = uefi_call_wrapper(BS->AllocatePages, 4, 
                                                                AllocateAnyPages, 
                                                                EfiLoaderData, 
                                                                pages_to_allocate, 
                                                                &phys_addr);
                    
                    if (EFI_ERROR(alloc_status)) {
                        Print(L"ERROR: Could not allocate physical memory for page %lu in PT %lu: %r\r\n", 
                              i, pt_i, alloc_status);
                        break;
                    }
                    
                    // Map the allocated physical memory to virtual address
                    current_pt->entries[i] = phys_addr | PAGE_RWX;
                    
                    // Zero out the allocated memory
                    uefi_call_wrapper(BS->SetMem, 3, (VOID*)phys_addr, 4096, 0);
                }
                
                pages_mapped++;
            }
        }
    }
    
    // ===================================================================
    // Set up DIRECT MAP region at PHYS_MAP_BASE = 0xFFFF880000000000
    // This maps physical memory 0-16GB to virtual 0xFFFF880000000000-0xFFFF880400000000
    // PML4 index 272 = (0xFFFF880000000000 >> 39) & 0x1FF
    // ===================================================================
    {
        page_table_t *pdpt_physmap = (page_table_t*)g_pdpt_physmap_addr;
        
        // Clear the PDPT for direct map
        for (int i = 0; i < 512; i++) {
            pdpt_physmap->entries[i] = 0;
        }
        
        // Set up PML4 entry 272 to point to direct map PDPT
        pml4->entries[272] = g_pdpt_physmap_addr | PAGE_RWX;
        
        // Map first 16GB of physical memory using 2MB pages
        for (int pdpt_i = 0; pdpt_i < 16; pdpt_i++) {
            // Use pre-allocated page directory for this 1GB region
            EFI_PHYSICAL_ADDRESS pd_addr = g_pd_physmap_addr[pdpt_i];
            page_table_t *pd = (page_table_t*)pd_addr;
            
            // Clear page directory
            for (int i = 0; i < 512; i++) {
                pd->entries[i] = 0;
            }
            
            // Set PDPT entry to point to this page directory (supervisor-only, RWX)
            pdpt_physmap->entries[pdpt_i] = pd_addr | PAGE_RWX;
            
            // Map each 2MB page in this 1GB region
            for (int pd_i = 0; pd_i < 512; pd_i++) {
                UINT64 phys_addr = (UINT64)pdpt_i * 0x40000000ULL + (UINT64)pd_i * 0x200000ULL;
                // Use 2MB pages (PAGE_SIZE bit set), supervisor-only (no PAGE_USER)
                pd->entries[pd_i] = phys_addr | PAGE_RWX | PAGE_SIZE;
            }
        }
        
        Print(L"  Direct map: 0xFFFF880000000000 -> phys 0x0 (16GB, 2MB pages)\r\n");
    }
    
    Print(L"Higher half paging configured:\r\n");
    Print(L"  Identity mapped: 0x0 - 0x100000000 (4GB) [for boot only]\r\n");
    Print(L"  Kernel virtual: 0x%lx -> 0x%lx (%lu total pages mapped)\r\n", 
          kernel_virt, kernel_phys_addr, pages_mapped);
    Print(L"  Virtual memory covers: 0x%lx - 0x%lx (%lu MB)\r\n", 
          kernel_virt, kernel_virt + (pages_mapped * 4096), (pages_mapped * 4096) / (1024 * 1024));
}

// Allocate and setup trampoline below kernel space
static EFI_STATUS allocate_trampoline(UINT64 kernel_phys_addr) {
    EFI_STATUS status;
    
    // Calculate the actual size of the trampoline function
    UINTN trampoline_size = (UINTN)trampoline_jump_end - (UINTN)trampoline_jump;
    
    // Ensure we have at least some reasonable size
    if (trampoline_size < 16 || trampoline_size > 4096) {
        trampoline_size = 256; // Safe fallback
    }
    
    // Allocate a page below the kernel's physical address
    // This ensures the trampoline is in low memory and identity-mapped
    EFI_PHYSICAL_ADDRESS max_addr = kernel_phys_addr - 1; // Just below kernel
    UINTN pages = 1; // One page for trampoline
    
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, 
                               EfiLoaderCode, pages, &max_addr);
    if (EFI_ERROR(status)) {
        // If that fails, try to allocate anywhere below 1MB
        max_addr = 0x100000 - 1;
        status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, 
                                   EfiLoaderCode, pages, &max_addr);
        if (EFI_ERROR(status)) {
            Print(L"ERROR: Could not allocate trampoline memory: %r\r\n", status);
            return status;
        }
    }
    
    trampoline_addr = max_addr;
    Print(L"Trampoline allocated at: 0x%lx\r\n", trampoline_addr);
    
    // Copy trampoline function code to allocated memory
    uefi_call_wrapper(BS->CopyMem, 3, (VOID*)trampoline_addr, 
                      (VOID*)trampoline_jump, trampoline_size);
    
    Print(L"Trampoline code copied (%lu bytes)\r\n", trampoline_size);
    
    return EFI_SUCCESS;
}

// Validate ELF64 header
static BOOLEAN validate_elf64(Elf64_Ehdr *elf_header) {
    // Check ELF magic
    if (elf_header->e_ident[0] != ELFMAG0 ||
        elf_header->e_ident[1] != ELFMAG1 ||
        elf_header->e_ident[2] != ELFMAG2 ||
        elf_header->e_ident[3] != ELFMAG3) {
        return FALSE;
    }
    
    // Check 64-bit, little-endian, x86-64
    if (elf_header->e_ident[4] != ELFCLASS64 ||
        elf_header->e_ident[5] != ELFDATA2LSB ||
        elf_header->e_machine != EM_X86_64) {
        return FALSE;
    }
    
    return TRUE;
}

// UEFI bootloader entry point
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *file_system;
    EFI_FILE_PROTOCOL *root_dir, *kernel_file;
    EFI_FILE_INFO *file_info;
    UINTN info_size, kernel_size;
    VOID *kernel_buffer;
    Elf64_Ehdr *elf_header;
    Elf64_Phdr *program_headers;
    kernel_entry_t kernel_entry;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    
    // Initialize GNU-EFI library
    InitializeLib(ImageHandle, SystemTable);
    
    // Initialize serial port for debug output (VMware)
    serial_init();
    serial_puts("\n\n=== LikeOS-64 UEFI Bootloader starting ===\n");
    
    Print(L"LikeOS-64 Enhanced UEFI Bootloader\r\n");
    Print(L"===================================\r\n");
    Print(L"ELF64 Loader with Paging Support\r\n\r\n");

    // Dump UEFI memory map for debugging
    //dump_memory_map();
    //for (;;) {}
    
    // Get loaded image protocol
    status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, 
                               &LoadedImageProtocol, (VOID**)&loaded_image);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get loaded image protocol: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Get file system protocol
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded_image->DeviceHandle, 
                               &FileSystemProtocol, (VOID**)&file_system);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get file system protocol: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Open root directory
    status = uefi_call_wrapper(file_system->OpenVolume, 2, file_system, &root_dir);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not open root directory: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    Print(L"Loading kernel.elf...\r\n");
    
    // Get Graphics Output Protocol for framebuffer info
    status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (VOID**)&gop);
    if (EFI_ERROR(status)) {
        Print(L"WARNING: Could not get Graphics Output Protocol: %r\r\n", status);
        // Set up fallback VGA mode info
        g_boot_info.fb_info.framebuffer_base = (void*)0xB8000;
        g_boot_info.fb_info.framebuffer_size = 4000;
        g_boot_info.fb_info.horizontal_resolution = 80;
        g_boot_info.fb_info.vertical_resolution = 25;
        g_boot_info.fb_info.pixels_per_scanline = 80;
        g_boot_info.fb_info.bytes_per_pixel = 2;
    } else {
        // Get current mode information
        g_boot_info.fb_info.framebuffer_base = (void*)gop->Mode->FrameBufferBase;
        g_boot_info.fb_info.framebuffer_size = (uint32_t)gop->Mode->FrameBufferSize;
        g_boot_info.fb_info.horizontal_resolution = gop->Mode->Info->HorizontalResolution;
        g_boot_info.fb_info.vertical_resolution = gop->Mode->Info->VerticalResolution;
        g_boot_info.fb_info.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
        
        // Calculate bytes per pixel based on pixel format
        switch (gop->Mode->Info->PixelFormat) {
            case PixelRedGreenBlueReserved8BitPerColor:
            case PixelBlueGreenRedReserved8BitPerColor:
                g_boot_info.fb_info.bytes_per_pixel = 4;
                break;
            case PixelBitMask:
                g_boot_info.fb_info.bytes_per_pixel = 4; // Assume 32-bit
                break;
            default:
                g_boot_info.fb_info.bytes_per_pixel = 4; // Safe default
                break;
        }
        
        Print(L"Framebuffer: 0x%lx, Size: %d bytes\r\n", g_boot_info.fb_info.framebuffer_base, g_boot_info.fb_info.framebuffer_size);
        Print(L"Resolution: %dx%d, BPP: %d\r\n", g_boot_info.fb_info.horizontal_resolution, g_boot_info.fb_info.vertical_resolution, g_boot_info.fb_info.bytes_per_pixel);
    }
    
    // Open kernel file (now ELF format)
    status = uefi_call_wrapper(root_dir->Open, 5, root_dir, &kernel_file, 
                               L"kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not open kernel.elf: %r\r\n", status);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Get file size
    info_size = sizeof(EFI_FILE_INFO) + 256;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (VOID**)&file_info);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory for file info: %r\r\n", status);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    status = uefi_call_wrapper(kernel_file->GetInfo, 4, kernel_file, 
                               &GenericFileInfo, &info_size, file_info);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get file info: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    kernel_size = (UINTN)file_info->FileSize;
    Print(L"ELF kernel size: %d bytes\r\n", kernel_size);
    
    // Allocate memory for the entire ELF file
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, kernel_size, &kernel_buffer);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory for ELF file: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Read entire ELF file into memory
    status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &kernel_size, kernel_buffer);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not read kernel file: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Parse ELF header
    elf_header = (Elf64_Ehdr*)kernel_buffer;
    
    if (!validate_elf64(elf_header)) {
        Print(L"ERROR: Invalid ELF64 file\r\n");
        uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return EFI_INVALID_PARAMETER;
    }
    
    Print(L"Valid ELF64 x86-64 kernel\r\n");
    Print(L"Entry point: 0x%lx\r\n", elf_header->e_entry);
    Print(L"Program headers: %d\r\n", elf_header->e_phnum);
    
    // Get program headers
    program_headers = (Elf64_Phdr*)((UINT8*)kernel_buffer + elf_header->e_phoff);
    
    // Track kernel physical location for paging setup
    EFI_PHYSICAL_ADDRESS kernel_phys_addr = 0;
    UINT64 kernel_size_total = 0;
    
    // Load program segments
    for (int i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr *phdr = &program_headers[i];
        
        if (phdr->p_type == PT_LOAD) {
            Print(L"Loading segment %d: vaddr=0x%lx, size=%ld\r\n", 
                  i, phdr->p_vaddr, phdr->p_memsz);
            
            // Allocate memory for this segment using AllocateAnyPages
            UINTN pages = (phdr->p_memsz + 4095) / 4096;
            EFI_PHYSICAL_ADDRESS segment_addr = 0;
            
            status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, 
                                       EfiLoaderCode, pages, &segment_addr);
            if (EFI_ERROR(status)) {
                Print(L"ERROR: Could not allocate memory: %r\r\n", status);
                uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
                uefi_call_wrapper(BS->FreePool, 1, file_info);
                uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
                uefi_call_wrapper(root_dir->Close, 1, root_dir);
                Print(L"System halted. Press any key to continue...\r\n");
                uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
                EFI_INPUT_KEY key;
                uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
                return status;
            }
            
            // Track the kernel's physical location (use first segment)
            if (kernel_phys_addr == 0) {
                kernel_phys_addr = segment_addr;
            }
            
            Print(L"Allocated at physical address: 0x%lx\r\n", segment_addr);
            
            // Calculate total kernel size
            UINT64 segment_end = segment_addr + phdr->p_memsz;
            if (segment_end > (kernel_phys_addr + kernel_size_total)) {
                kernel_size_total = segment_end - kernel_phys_addr;
            }
            
            // Copy segment data
            if (phdr->p_filesz > 0) {
                uefi_call_wrapper(BS->CopyMem, 3, (VOID*)segment_addr, 
                                  (UINT8*)kernel_buffer + phdr->p_offset, phdr->p_filesz);
            }
            
            // Zero out any extra space (BSS)
            if (phdr->p_memsz > phdr->p_filesz) {
                uefi_call_wrapper(BS->SetMem, 3, (VOID*)(segment_addr + phdr->p_filesz), 
                                  phdr->p_memsz - phdr->p_filesz, 0);
            }
        }
    }
    
    Print(L"Kernel physical location: 0x%lx (total size: %ld bytes)\r\n", 
          kernel_phys_addr, kernel_size_total);
    
    Print(L"Kernel loaded successfully!\r\n");
    Print(L"Setting up paging and exiting UEFI...\r\n");
    
    // Clean up
    uefi_call_wrapper(BS->FreePool, 1, file_info);
    uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
    uefi_call_wrapper(root_dir->Close, 1, root_dir);
    
    // Get memory map for exit boot services
    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_key, descriptor_size;
    UINT32 descriptor_version;
    
    // Get memory map size
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                               &map_key, &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"ERROR: Unexpected error getting memory map size: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Allocate buffer for memory map
    memory_map_size += 2 * descriptor_size;  // Add some extra space
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, 
                               memory_map_size, (VOID**)&memory_map);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory map: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Get the actual memory map
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                               &map_key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get memory map: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, memory_map);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Store memory map info for kernel
    {
        UINTN num_entries = memory_map_size / descriptor_size;
        g_boot_info.mem_info.entry_count = 0;
        g_boot_info.mem_info.descriptor_size = (uint32_t)descriptor_size;
        g_boot_info.mem_info.total_memory = 0;
        
        Print(L"Processing %lu memory map entries...\r\n", num_entries);
        
        // Dump full memory map to serial for VMware debugging
        serial_puts("\n=== UEFI MEMORY MAP ===\n");
        serial_puts("Entries: ");
        serial_putdec(num_entries);
        serial_puts(", Descriptor size: ");
        serial_putdec(descriptor_size);
        serial_puts("\n\n");
        
        for (UINTN i = 0; i < num_entries && g_boot_info.mem_info.entry_count < MAX_MEMORY_MAP_ENTRIES; i++) {
            EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)memory_map + i * descriptor_size);
            
            // Dump to serial
            serial_puts("[");
            serial_putdec(i);
            serial_puts("] ");
            serial_puthex(desc->PhysicalStart);
            serial_puts(" - ");
            serial_puthex(desc->PhysicalStart + desc->NumberOfPages * 4096);
            serial_puts(" (");
            serial_putdec(desc->NumberOfPages);
            serial_puts(" pages, ");
            serial_putdec(desc->NumberOfPages * 4096 / 1024);
            serial_puts(" KB) Type=");
            serial_putdec(desc->Type);
            serial_puts(" ");
            serial_puts(efi_memory_type_name(desc->Type));
            serial_puts("\n");
            
            // Copy entry to boot_info
            memory_map_entry_t *entry = &g_boot_info.mem_info.entries[g_boot_info.mem_info.entry_count];
            entry->type = desc->Type;
            entry->physical_start = desc->PhysicalStart;
            entry->virtual_start = desc->VirtualStart;
            entry->number_of_pages = desc->NumberOfPages;
            entry->attribute = desc->Attribute;
            
            // Count usable memory (EfiConventionalMemory type = 7)
            if (desc->Type == 7) {
                g_boot_info.mem_info.total_memory += desc->NumberOfPages * 4096;
            }
            
            g_boot_info.mem_info.entry_count++;
        }
        
        serial_puts("\nTotal usable memory: ");
        serial_putdec(g_boot_info.mem_info.total_memory / (1024 * 1024));
        serial_puts(" MB\n");
        serial_puts("=== END MEMORY MAP ===\n\n");
        
        Print(L"Stored %d memory entries, total usable: %lu MB\r\n", 
              g_boot_info.mem_info.entry_count, 
              g_boot_info.mem_info.total_memory / (1024 * 1024));
    }
    
    // Step 1: Allocate trampoline below kernel space
    Print(L"Allocating trampoline below kernel space...\r\n");
    status = allocate_trampoline(kernel_phys_addr);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, memory_map);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Step 2: Set up higher half paging (including identity mapping for low memory)
    Print(L"Setting up higher half kernel paging...\r\n");
    
    // Initialize and allocate all page tables from UEFI at low addresses
    status = init_page_tables();
    if (EFI_ERROR(status)) {
        Print(L"Failed to initialize page tables\r\n");
        return status;
    }
    
    setup_higher_half_paging(kernel_phys_addr, kernel_size_total);
    
    Print(L"About to exit boot services. Kernel entry: 0x%lx\r\n", elf_header->e_entry);
    Print(L"Trampoline at: 0x%lx\r\n", trampoline_addr);

    // Step 3: Exit boot services
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        // Try one more time in case the memory map changed
        uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                          &map_key, &descriptor_size, &descriptor_version);
        status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
        if (EFI_ERROR(status)) {
            Print(L"ERROR: Could not exit boot services: %r\r\n", status);
            Print(L"System halted. Press any key to continue...\r\n");
            uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
            EFI_INPUT_KEY key;
            uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
            return status;
        }
    }
    
    // After ExitBootServices, UEFI services are no longer available
    
    // Step 4: Call the copied trampoline function in low memory
    // The trampoline will:
    // - Load CR3 with our page tables  
    // - Jump to the kernel's higher half virtual address
    trampoline_func_t trampoline = (trampoline_func_t)trampoline_addr;
    trampoline(elf_header->e_entry, &g_boot_info, g_pml4_addr);
    
    // Should never reach here
    return EFI_SUCCESS;
}
